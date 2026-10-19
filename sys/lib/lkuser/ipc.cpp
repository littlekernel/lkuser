/*
 * Copyright (c) 2026 Travis Geiselbrecht
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */
// Calls to user endpoints. The caller's pending_call is queued on the endpoint, a server
// thread takes it with recv into an inflight slot of its process, reads the request and writes
// the reply in place while the caller waits, and reply ends it. It can also end with the caller
// leaving (a timeout or a kill), the server's process exiting, or the endpoint's last handle
// closing while it is still queued.
//
// What keeps this safe:
//   - The call's state, the slots and the queues are under LKUSER_IPC_LOCK, and the outcome is
//     whatever the state says under it, never what a wait returned: a reply can race a timeout.
//   - A server thread touching the caller's buffers, table or process sets the call busy first,
//     having checked under the lock that the call is still its own, and the caller does not
//     leave the call, however it ends, until busy clears. So the caller's address space, which
//     the reaper frees once its threads are gone, outlives every copy.
//   - A caller leaving a received call cuts the slot's link to its pending_call, which the
//     thread reuses for its next call: the slot's token then fails with ERR_NOT_FOUND, and the
//     slot goes the next time the server uses it.
//   - A handle moving to another table goes in there before it leaves the first, and a moved
//     handle in transit counts as a handle of its object.

#include "ipc.h"

#include <kernel/thread.h>
#include <lk/err.h>
#include <platform.h>
#include <string.h>

#include "endpoint.h"
#include "group.h"
#include "handle.h"
#include "proc.h"
#include "syscalls.h"
#include "thread.h"

namespace lkuser {

namespace {

constexpr uintptr_t kSlotBits = 8;
constexpr uintptr_t kSlotMask = (uintptr_t{1} << kSlotBits) - 1;
constexpr uintptr_t kGenMax = UINTPTR_MAX >> kSlotBits;
static_assert(LKUSER_INFLIGHT <= kSlotMask, "a slot index fits its bits");

// recv took a call it could not deliver and the next one is to be tried; never a status
constexpr status_t kRetry = 1;

lk_token_t make_token(uint32_t idx, uintptr_t gen) {
    return (gen << kSlotBits) | idx;
}

// The slot token names in p while it is the same call's, or null.
inflight_slot *slot_for_locked(proc *p, lk_token_t token, uint32_t *idx) {
    const uintptr_t i = token & kSlotMask;
    const uintptr_t gen = token >> kSlotBits;
    if (i >= LKUSER_INFLIGHT || gen == 0) {
        return nullptr;
    }
    inflight_slot &s = p->inflight[i];
    if (!s.used || s.gen != gen) {
        return nullptr;
    }
    *idx = static_cast<uint32_t>(i);
    return &s;
}

// A free slot of p for a call taken from ep, which counts it as active; null when every slot
// is taken or p has exited.
inflight_slot *claim_slot_locked(proc *p, endpoint *ep, uint32_t *idx) {
    if (p->ipc_dead) {
        return nullptr;
    }
    for (uint32_t i = 0; i < LKUSER_INFLIGHT; i++) {
        inflight_slot &s = p->inflight[i];
        if (s.used) {
            continue;
        }
        p->inflight_gen = p->inflight_gen % kGenMax + 1;
        s.used = true;
        s.gen = p->inflight_gen;
        s.pc = nullptr;
        s.nlent = 0;
        ep->add_ref();
        s.ep = ep;
        ep->active++;
        *idx = i;
        return &s;
    }
    return nullptr;
}

// Frees slot idx of p, and the endpoint can hand out another call. Returns the endpoint, whose
// reference the caller drops after unlocking.
endpoint *free_slot_locked(proc *p, uint32_t idx) {
    inflight_slot &s = p->inflight[idx];
    endpoint *ep = s.ep;
    s.used = false;
    s.pc = nullptr;
    s.ep = nullptr;
    s.nlent = 0;
    ep->active--;
    ep->changed_locked();
    if (ep->has_call_locked()) {
        ep->wake_receiver_locked();
    }
    return ep;
}

// The call is over with status: off its slot if a server holds it, and its caller woken.
void finish_locked(pending_call *pc, status_t status) {
    if (pc->state == call_state::RECEIVED) {
        pc->server->inflight[pc->slot].pc = nullptr;
        pc->server = nullptr;
    }
    pc->state = call_state::DONE;
    pc->result = status;
    wait_queue_wake_one(&pc->done, false, NO_ERROR);
}

void clear_busy_locked(pending_call *pc) {
    pc->busy = false;
    wait_queue_wake_all(&pc->idle, false, NO_ERROR);
}

endpoint *endpoint_of(const pending_call *pc) {
    return static_cast<connection *>(pc->conn.get())->ep();
}

// Whether h is one of the handles just put in a table. When the moving process is the
// receiving one, the slot a moved handle left may have been closed by another thread and
// taken again by the move itself, and closing it would close the new handle.
bool is_installed(const installed_handle *hs, size_t n, lk_handle_t h) {
    for (size_t i = 0; i < n; i++) {
        if (hs[i].h == h) {
            return true;
        }
    }
    return false;
}

// Takes handles back out of a table they were put in.
void take_back(proc *p, const installed_handle *hs, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p->handles().close_if(hs[i].h, hs[i].obj);
    }
}

lk_time_t deadline_after(lk_time_t timeout) {
    return timeout == INFINITE_TIME ? INFINITE_TIME : current_time() + timeout;
}

lk_time_t time_left(lk_time_t deadline) {
    if (deadline == INFINITE_TIME) {
        return INFINITE_TIME;
    }
    const int32_t left = static_cast<int32_t>(deadline - current_time());
    return left > 0 ? static_cast<lk_time_t>(left) : 0;
}

// The caller ends a call that has not ended yet, with status. A queued call leaves the queue; a
// received one leaves its slot, and the lent handles go from the server's table: *server (with
// a reference) and revoke[*nrevoke] say what to take back after unlocking.
void leave_locked(pending_call *pc, status_t status, installed_handle *revoke, size_t *nrevoke,
                  proc **server) {
    if (pc->state == call_state::QUEUED) {
        endpoint *ep = endpoint_of(pc);
        lk::list<pending_call>::remove(pc);
        ep->changed_locked();
    } else if (pc->state == call_state::RECEIVED) {
        proc *p = pc->server;
        inflight_slot &s = p->inflight[pc->slot];
        for (size_t i = 0; i < s.nlent; i++) {
            revoke[i] = s.lent[i];
        }
        *nrevoke = s.nlent;
        s.nlent = 0;
        // a process holding a slot has not exited, so it is alive
        p->add_ref();
        *server = p;
    }
    finish_locked(pc, status);
}

// Queues the caller's call on ep and waits for it to end, or leaves it after timeout or at a
// kill. Returns the call's result.
status_t rendezvous(pending_call &pc, endpoint *ep, lk_time_t timeout) {
    installed_handle revoke[LK_MSG_MAX_HANDLES];
    size_t nrevoke = 0;
    proc *server = nullptr;
    status_t status;
    {
        LKUSER_IPC_LOCK(state);
        if (ep->closed_locked()) {
            LKUSER_IPC_UNLOCK(state);
            return ERR_CHANNEL_CLOSED;
        }
        pc.state = call_state::QUEUED;
        pc.busy = false;
        pc.server = nullptr;
        ep->pending.push_back(&pc);
        ep->changed_locked();
        if (ep->has_call_locked()) {
            ep->wake_receiver_locked();
        }
        while (pc.state != call_state::DONE) {
            const status_t err = wait_queue_block_etc(&pc.done, timeout, WAIT_QUEUE_INTERRUPTIBLE);
            if (pc.state == call_state::DONE) {
                break;
            }
            if (err != NO_ERROR) {
                leave_locked(&pc, err, revoke, &nrevoke, &server);
            }
        }
        // a server thread may still be copying to or from this thread's buffers
        while (pc.busy) {
            wait_queue_block(&pc.idle, INFINITE_TIME);
        }
        status = pc.result;
        pc.state = call_state::IDLE;
        LKUSER_IPC_UNLOCK(state);
    }
    if (server) {
        take_back(server, revoke, nrevoke);
        server->release();
    }
    return status;
}

// What a server ends a call it holds with: the caller gets status, the slot goes, and so do the
// lent handles in p's table. False if the call had already ended (its caller left, or p is
// exiting), in which case the caller has its own result.
bool end_received(proc *p, uint32_t idx, uintptr_t gen, pending_call *pc, status_t status) {
    installed_handle revoke[LK_MSG_MAX_HANDLES];
    size_t nrevoke = 0;
    endpoint *drop = nullptr;
    bool live;
    {
        LKUSER_IPC_LOCK(state);
        inflight_slot &s = p->inflight[idx];
        const bool mine = s.used && s.gen == gen;
        live = mine && s.pc == pc && pc->state == call_state::RECEIVED;
        if (live) {
            finish_locked(pc, status);
        }
        if (mine) {
            for (size_t i = 0; i < s.nlent; i++) {
                revoke[i] = s.lent[i];
            }
            nrevoke = s.nlent;
            drop = free_slot_locked(p, idx);
        }
        clear_busy_locked(pc);
        LKUSER_IPC_UNLOCK(state);
    }
    take_back(p, revoke, nrevoke);
    if (drop) {
        drop->release();
    }
    return live;
}

// recv could not deliver a call it took: back at the head of its endpoint's queue for the
// next receiver, unless it has ended meanwhile.
void unreceive(proc *p, endpoint *ep, uint32_t idx, uintptr_t gen, pending_call *pc) {
    endpoint *drop = nullptr;
    {
        LKUSER_IPC_LOCK(state);
        inflight_slot &s = p->inflight[idx];
        if (s.used && s.gen == gen) {
            if (s.pc == pc && pc->state == call_state::RECEIVED) {
                s.pc = nullptr;
                pc->server = nullptr;
                if (ep->closed_locked()) {
                    finish_locked(pc, ERR_CHANNEL_CLOSED);
                } else {
                    pc->state = call_state::QUEUED;
                    ep->pending.push_front(pc);
                }
            }
            drop = free_slot_locked(p, idx);
        }
        clear_busy_locked(pc);
        LKUSER_IPC_UNLOCK(state);
    }
    if (drop) {
        drop->release();
    }
}

// The call token names in p, set busy so the caller stays put: ERR_NOT_FOUND for no such call
// or one whose caller has left (its slot goes now), ERR_BAD_STATE for a handshake where a call
// is wanted or the reverse. Another server thread copying for the same call is waited for.
status_t acquire(proc *p, lk_token_t token, bool connect, pending_call **out, uint32_t *idx,
                 uintptr_t *gen) {
    endpoint *drop = nullptr;
    status_t err;
    {
        LKUSER_IPC_LOCK(state);
        for (;;) {
            inflight_slot *s = slot_for_locked(p, token, idx);
            if (!s) {
                err = ERR_NOT_FOUND;
                break;
            }
            pending_call *pc = s->pc;
            if (!pc) {
                drop = free_slot_locked(p, *idx);
                err = ERR_NOT_FOUND;
                break;
            }
            if (pc->connect != connect) {
                err = ERR_BAD_STATE;
                break;
            }
            if (pc->busy) {
                wait_queue_block(&pc->idle, INFINITE_TIME);
                continue;
            }
            pc->busy = true;
            *out = pc;
            *gen = s->gen;
            err = NO_ERROR;
            break;
        }
        LKUSER_IPC_UNLOCK(state);
    }
    if (drop) {
        drop->release();
    }
    return err;
}

void release_busy(pending_call *pc) {
    LKUSER_IPC_LOCK(state);
    clear_busy_locked(pc);
    LKUSER_IPC_UNLOCK(state);
}

status_t deliver_impulse(proc *p, impulse_node *n, uint64_t ep_cookie, user_ptr<lk_recv_info> uinfo) {
    lk_recv_info info = {};
    info.ep_cookie = ep_cookie;
    info.conn_cookie = n->imp.cookie;
    info.sender_pid = n->sender_pid;
    info.sender_tid = n->sender_tid;
    memcpy(info.impulse, &n->imp, sizeof(n->imp));
    release_impulse(n);
    return copy_to_user(p->aspace(), uinfo, &info, sizeof(info));
}

// Hands a call recv took (busy, in slot idx) to the receiving thread: the start of the request
// into buf, the carried handles into p's table, the token and the rest into *uinfo. A fault on
// the caller's side fails its call and the next one is tried; anything that goes wrong on the
// receiver's side puts the call back.
status_t deliver_call(thread *t, endpoint *ep, pending_call *pc, uint32_t idx, uintptr_t gen,
                      const msg_buffer &buf, user_ptr<lk_recv_info> uinfo) {
    proc *p = t->process();
    // busy keeps the caller in the call, so its process stays
    lk::ref_ptr<proc> caller(pc->owner->process());

    // what sys_call copied already, then the rest from the caller's memory
    const size_t n = MIN(pc->req.len(), buf.len());
    const size_t from_prefix = MIN(n, kRecvPrefix);
    copy_fault fault = copy_fault::NONE;
    status_t err = NO_ERROR;
    if (buf.write(0, pc->prefix, from_prefix) != static_cast<ssize_t>(from_prefix)) {
        err = ERR_FAULT;
        fault = copy_fault::DST;
    } else if (n > from_prefix) {
        err = copy_msg(pc->req, from_prefix, buf, from_prefix, n - from_prefix, &fault);
    }
    if (err < 0) {
        if (fault == copy_fault::SRC) {
            end_received(p, idx, gen, pc, ERR_FAULT);
            return kRetry;
        }
        unreceive(p, ep, idx, gen, pc);
        return err;
    }

    // into this table now, out of the caller's once the call is surely received
    lk_recv_info info = {};
    installed_handle installed[LK_MSG_MAX_HANDLES];
    installed_handle lent[LK_MSG_MAX_HANDLES];
    size_t nlent = 0;
    for (size_t i = 0; i < pc->ncarried; i++) {
        const carried_handle &ch = pc->carried[i];
        err = p->handles().add(ch.obj, ch.rights, &info.handles[i]);
        if (err < 0) {
            take_back(p, installed, i);
            unreceive(p, ep, idx, gen, pc);
            return err;
        }
        installed[i] = {info.handles[i], ch.obj.get()};
        if (ch.lent) {
            lent[nlent++] = installed[i];
        }
    }
    info.nhandles = static_cast<uint32_t>(pc->ncarried);
    info.token = make_token(idx, gen);
    info.op = pc->connect ? LK_OP_CONNECT : pc->hdr.op;
    info.ep_cookie = ep->cookie();
    info.conn_cookie = static_cast<connection *>(pc->conn.get())->cookie();
    info.sender_pid = caller->pid();
    info.sender_tid = pc->owner->tid();
    info.req_len = pc->req.len();
    info.reply_max = pc->reply.len();
    err = copy_to_user(p->aspace(), uinfo, &info, sizeof(info));
    if (err < 0) {
        take_back(p, installed, pc->ncarried);
        unreceive(p, ep, idx, gen, pc);
        return err;
    }

    bool live;
    {
        LKUSER_IPC_LOCK(state);
        inflight_slot &s = p->inflight[idx];
        live = s.used && s.gen == gen && s.pc == pc;
        if (live) {
            for (size_t i = 0; i < nlent; i++) {
                s.lent[i] = lent[i];
            }
            s.nlent = nlent;
            pc->carried_taken = true;
        }
        LKUSER_IPC_UNLOCK(state);
    }
    if (!live) {
        // the caller left, or this process is exiting: the call is over
        take_back(p, installed, pc->ncarried);
        end_received(p, idx, gen, pc, ERR_CHANNEL_CLOSED);
        return kRetry;
    }
    for (size_t i = 0; i < pc->ncarried; i++) {
        const carried_handle &ch = pc->carried[i];
        if (!ch.lent) {
            if (caller.get() != p || !is_installed(installed, pc->ncarried, ch.h)) {
                caller->handles().close_if(ch.h, ch.obj.get());
            }
            ch.obj->handle_removed(ch.rights);
        }
    }
    release_busy(pc);
    return NO_ERROR;
}

// One look for work on ep or grp, blocking until deadline: an impulse first, then a call.
status_t receive(thread *t, endpoint *ep, group *grp, const msg_buffer &buf,
                 user_ptr<lk_recv_info> uinfo, lk_time_t deadline) {
    proc *p = t->process();
    LKUSER_IPC_LOCK(state);
    for (;;) {
        endpoint *from = ep ? (ep->has_work_locked() ? ep : nullptr) : grp->pick_locked();
        if (from) {
            if (impulse_node *n = from->impulses.pop_front()) {
                from->changed_locked();
                const uint64_t cookie = from->cookie();
                LKUSER_IPC_UNLOCK(state);
                return deliver_impulse(p, n, cookie, uinfo);
            }
            uint32_t idx;
            inflight_slot *s = claim_slot_locked(p, from, &idx);
            if (!s) {
                const status_t err = p->ipc_dead ? ERR_BAD_STATE : ERR_NO_RESOURCES;
                LKUSER_IPC_UNLOCK(state);
                return err;
            }
            pending_call *pc = from->pending.pop_front();
            pc->state = call_state::RECEIVED;
            pc->busy = true;
            pc->server = p;
            pc->slot = idx;
            s->pc = pc;
            from->changed_locked();
            const uintptr_t gen = s->gen;
            LKUSER_IPC_UNLOCK(state);
            return deliver_call(t, from, pc, idx, gen, buf, uinfo);
        }
        if ((ep && ep->closed_locked()) || (grp && grp->closed_locked())) {
            LKUSER_IPC_UNLOCK(state);
            return ERR_CHANNEL_CLOSED;
        }
        const lk_time_t left = time_left(deadline);
        if (left == 0) {
            LKUSER_IPC_UNLOCK(state);
            return ERR_TIMED_OUT;
        }
        const status_t err = wait_queue_block_etc(ep ? &ep->receivers : &grp->receivers, left,
                                                  WAIT_QUEUE_INTERRUPTIBLE);
        // after a wakeup or a timeout, the queues are looked at once more
        if (err == ERR_INTERRUPTED || err == ERR_CHANNEL_CLOSED) {
            LKUSER_IPC_UNLOCK(state);
            return err;
        }
    }
}

} // namespace

status_t call_user(thread *t, lk::ref_ptr<connection> conn, const lk_msg_hdr &hdr,
                   carried_handle *carried, size_t ncarried, uint64_t timeout_ns) {
    pending_call &pc = t->call();
    endpoint *ep = conn->ep();
    pc.hdr = hdr;
    pc.connect = false;
    pc.reply_len = 0;
    pc.nreply_handles = 0;
    pc.carried_taken = false;
    pc.ncarried = ncarried;
    for (size_t i = 0; i < ncarried; i++) {
        pc.carried[i] = std::move(carried[i]);
        if (!pc.carried[i].lent) {
            pc.carried[i].obj->handle_added(pc.carried[i].rights);
        }
    }
    pc.conn = std::move(conn);

    const status_t status = rendezvous(pc, ep, to_lk_time(timeout_ns));

    // no server took the moved handles: they stay with the caller, out of transit
    for (size_t i = 0; i < pc.ncarried; i++) {
        if (!pc.carried[i].lent && !pc.carried_taken) {
            pc.carried[i].obj->handle_removed(pc.carried[i].rights);
        }
        pc.carried[i].obj.reset();
    }
    pc.ncarried = 0;
    pc.conn.reset();
    return status;
}

status_t connect_notify(thread *t, lk::ref_ptr<connection> conn) {
    pending_call &pc = t->call();
    endpoint *ep = conn->ep();
    pc.req.reset();
    pc.reply.reset();
    pc.hdr = {};
    pc.connect = true;
    pc.reply_len = 0;
    pc.nreply_handles = 0;
    pc.reply_handles_cap = 0;
    pc.ncarried = 0;
    pc.carried_taken = false;
    pc.conn = std::move(conn);

    const status_t status = rendezvous(pc, ep, INFINITE_TIME);

    pc.conn.reset();
    pc.connect = false;
    return status;
}

void cancel_inflight(proc *p) {
    endpoint *drop[LKUSER_INFLIGHT];
    size_t ndrop = 0;
    {
        LKUSER_IPC_LOCK(state);
        p->ipc_dead = true;
        for (uint32_t i = 0; i < LKUSER_INFLIGHT; i++) {
            inflight_slot &s = p->inflight[i];
            if (!s.used) {
                continue;
            }
            if (s.pc) {
                finish_locked(s.pc, ERR_CHANNEL_CLOSED);
            }
            // the lent handles go with the rest of the table
            drop[ndrop++] = free_slot_locked(p, i);
        }
        LKUSER_IPC_UNLOCK(state);
    }
    for (size_t i = 0; i < ndrop; i++) {
        drop[i]->release();
    }
}

// Waits for an impulse or a call on an endpoint or a group. buf gets the start of a call's
// request; the receiving table must have room for the handles it carries, or the call stays
// queued and recv returns ERR_NO_RESOURCES.
status_t sys_recv(lk_handle_t h, user_ptr<lk_recv_args> uargs) {
    thread *t = thread::current();
    proc *p = t->process();

    lk_recv_args args;
    status_t err = copy_from_user(p->aspace(), &args, uargs, sizeof(args));
    if (err < 0) {
        return err;
    }
    lk::ref_ptr<object> src;
    err = p->handles().get(h, LK_RIGHT_RECV, &src);
    if (err < 0) {
        return err;
    }
    endpoint *ep = nullptr;
    group *grp = nullptr;
    if (src->kind() == obj_kind::ENDPOINT) {
        ep = static_cast<endpoint *>(src.get());
    } else if (src->kind() == obj_kind::GROUP) {
        grp = static_cast<group *>(src.get());
    } else {
        return ERR_BAD_HANDLE;
    }
    msg_buffer buf;
    err = buf.load(p->aspace(), user_ptr<const lk_iov>(reinterpret_cast<vaddr_t>(args.buf)),
                   args.nbuf);
    if (err < 0) {
        return err;
    }
    // where the result goes must be writable before anything is taken off a queue
    const user_ptr<lk_recv_info> uinfo(uargs.addr() + offsetof(lk_recv_args, info));
    const lk_recv_info none = {};
    err = copy_to_user(p->aspace(), uinfo, &none, sizeof(none));
    if (err < 0) {
        return err;
    }

    const lk_time_t deadline = deadline_after(to_lk_time(args.timeout_ns));
    do {
        err = receive(t, ep, grp, buf, uinfo, deadline);
    } while (err == kRetry);
    return err;
}

// More of a received call's request, at off from its start (header included). The range must
// lie inside the request: ERR_OUT_OF_RANGE.
status_t sys_msg_read(lk_token_t token, size_t off, user_ptr<const lk_iov> uiov, size_t n) {
    proc *p = thread::current()->process();
    msg_buffer mine;
    status_t err = mine.load(p->aspace(), uiov, n);
    if (err < 0) {
        return err;
    }
    pending_call *pc;
    uint32_t idx;
    uintptr_t gen;
    err = acquire(p, token, false, &pc, &idx, &gen);
    if (err < 0) {
        return err;
    }
    if (off > pc->req.len() || mine.len() > pc->req.len() - off) {
        release_busy(pc);
        return ERR_OUT_OF_RANGE;
    }
    copy_fault fault;
    err = copy_msg(pc->req, off, mine, 0, mine.len(), &fault);
    if (err < 0 && fault == copy_fault::SRC) {
        end_received(p, idx, gen, pc, ERR_FAULT);
        return err;
    }
    release_busy(pc);
    return err;
}

// Part of a received call's reply, at off from its start, past the header, which is reply's
// to write. ERR_NOT_ENOUGH_BUFFER past the caller's reply capacity.
status_t sys_msg_write(lk_token_t token, size_t off, user_ptr<const lk_iov> uiov, size_t n) {
    proc *p = thread::current()->process();
    msg_buffer mine;
    status_t err = mine.load(p->aspace(), uiov, n);
    if (err < 0) {
        return err;
    }
    pending_call *pc;
    uint32_t idx;
    uintptr_t gen;
    err = acquire(p, token, false, &pc, &idx, &gen);
    if (err < 0) {
        return err;
    }
    if (off < sizeof(lk_msg_hdr)) {
        release_busy(pc);
        return ERR_INVALID_ARGS;
    }
    if (off > pc->reply.len() || mine.len() > pc->reply.len() - off) {
        release_busy(pc);
        return ERR_NOT_ENOUGH_BUFFER;
    }
    copy_fault fault;
    err = copy_msg(mine, 0, pc->reply, off, mine.len(), &fault);
    if (err < 0 && fault == copy_fault::DST) {
        end_received(p, idx, gen, pc, ERR_FAULT);
        return err;
    }
    if (err == NO_ERROR && mine.len() > 0) {
        pc->reply_len = MAX(pc->reply_len, off + mine.len());
    }
    release_busy(pc);
    return err;
}

// Ends a received call with the reply in args: its payload after the header, the header's
// status, and the handles, which move into the caller's table, or stay here if the reply
// fails. Nothing else is written if the caller has left (ERR_NOT_FOUND).
status_t sys_reply(lk_token_t token, user_ptr<const lk_reply_args> uargs) {
    proc *p = thread::current()->process();
    vmm_aspace_t *aspace = p->aspace();

    lk_reply_args args;
    status_t err = copy_from_user(aspace, &args, uargs, sizeof(args));
    if (err < 0) {
        return err;
    }
    if (args.nhandles > LK_MSG_MAX_HANDLES) {
        return ERR_INVALID_ARGS;
    }
    msg_buffer mine;
    err = mine.load(aspace, user_ptr<const lk_iov>(reinterpret_cast<vaddr_t>(args.iov)), args.n);
    if (err < 0) {
        return err;
    }
    if (mine.len() < sizeof(lk_msg_hdr)) {
        return ERR_INVALID_ARGS;
    }
    lk_msg_hdr own;
    if (mine.read(0, &own, sizeof(own)) != sizeof(own)) {
        return ERR_FAULT;
    }
    lk_handle_t hs[LK_MSG_MAX_HANDLES];
    err = copy_from_user(aspace, hs, reinterpret_cast<vaddr_t>(args.handles),
                         args.nhandles * sizeof(lk_handle_t));
    if (err < 0) {
        return err;
    }
    carried_handle give[LK_MSG_MAX_HANDLES];
    for (size_t i = 0; i < args.nhandles; i++) {
        for (size_t j = 0; j < i; j++) {
            if (hs[j] == hs[i]) {
                return ERR_INVALID_ARGS;
            }
        }
        err = p->handles().get(hs[i], LK_RIGHT_TRANSFER, &give[i].obj, &give[i].rights);
        if (err < 0) {
            return err;
        }
    }

    pending_call *pc;
    uint32_t idx;
    uintptr_t gen;
    err = acquire(p, token, false, &pc, &idx, &gen);
    if (err < 0) {
        return err;
    }
    if (mine.len() > pc->reply.len() || args.nhandles > pc->reply_handles_cap) {
        release_busy(pc);
        return ERR_NOT_ENOUGH_BUFFER;
    }
    lk::ref_ptr<proc> caller(pc->owner->process());

    copy_fault fault;
    err = copy_msg(mine, sizeof(lk_msg_hdr), pc->reply, sizeof(lk_msg_hdr),
                   mine.len() - sizeof(lk_msg_hdr), &fault);
    if (err < 0) {
        if (fault == copy_fault::DST) {
            end_received(p, idx, gen, pc, ERR_FAULT);
        } else {
            release_busy(pc);
        }
        return err;
    }

    installed_handle installed[LK_MSG_MAX_HANDLES];
    for (size_t i = 0; i < args.nhandles; i++) {
        err = caller->handles().add(give[i].obj, give[i].rights, &installed[i].h);
        if (err < 0) {
            take_back(caller.get(), installed, i);
            release_busy(pc);
            return err;
        }
        installed[i].obj = give[i].obj.get();
    }

    const size_t written = MAX(pc->reply_len, mine.len());
    const lk_msg_hdr hdr = {
        .op = pc->hdr.op,
        .len = static_cast<uint32_t>(written - sizeof(lk_msg_hdr)),
        .nhandles = static_cast<uint32_t>(args.nhandles),
        .status = own.status,
    };
    if (pc->reply.write(0, &hdr, sizeof(hdr)) != sizeof(hdr)) {
        take_back(caller.get(), installed, args.nhandles);
        end_received(p, idx, gen, pc, ERR_FAULT);
        return ERR_FAULT;
    }
    for (size_t i = 0; i < args.nhandles; i++) {
        pc->reply_handles[i] = {installed[i].h, installed[i].obj};
    }
    pc->nreply_handles = args.nhandles;
    pc->reply_len = written;

    if (!end_received(p, idx, gen, pc, NO_ERROR)) {
        take_back(caller.get(), installed, args.nhandles);
        return ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < args.nhandles; i++) {
        if (caller.get() != p || !is_installed(installed, args.nhandles, hs[i])) {
            p->handles().close_if(hs[i], give[i].obj.get());
        }
    }
    return NO_ERROR;
}

// The server's answers to a CONNECT_NOTIFY handshake. Accepting sets the connection's cookie
// and gives the server a peer handle; the client's connect then returns its own.
status_t sys_connection_accept(lk_token_t token, uintptr_t cookie, user_ptr<lk_handle_t> peer_out) {
    proc *p = thread::current()->process();
    pending_call *pc;
    uint32_t idx;
    uintptr_t gen;
    status_t err = acquire(p, token, true, &pc, &idx, &gen);
    if (err < 0) {
        return err;
    }
    lk::ref_ptr<object> conn = pc->conn;
    {
        LKUSER_IPC_LOCK(state);
        static_cast<connection *>(conn.get())->set_cookie_locked(cookie);
        LKUSER_IPC_UNLOCK(state);
    }
    lk_handle_t peer;
    err = p->handles().add(conn, kPeerRights, &peer);
    if (err == NO_ERROR) {
        err = copy_to_user(p->aspace(), peer_out, &peer, sizeof(peer));
        if (err < 0) {
            p->handles().close_if(peer, conn.get());
        }
    }
    if (err < 0) {
        release_busy(pc);
        return err;
    }
    if (!end_received(p, idx, gen, pc, NO_ERROR)) {
        p->handles().close_if(peer, conn.get());
        return ERR_NOT_FOUND;
    }
    return NO_ERROR;
}

status_t sys_connection_refuse(lk_token_t token) {
    proc *p = thread::current()->process();
    pending_call *pc;
    uint32_t idx;
    uintptr_t gen;
    status_t err = acquire(p, token, true, &pc, &idx, &gen);
    if (err < 0) {
        return err;
    }
    return end_received(p, idx, gen, pc, ERR_NOT_ALLOWED) ? NO_ERROR : ERR_NOT_FOUND;
}

} // namespace lkuser
