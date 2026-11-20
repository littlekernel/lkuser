/*
 * Copyright (c) 2015-2022 Travis Geiselbrecht
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "proc.h"

#include <arch/mmu.h>
#include <assert.h>
#include <kernel/thread.h>
#include <lk/err.h>
#include <lk/init.h>
#include <lktl/auto_call.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lk_abi.h>

#include "boot.h"
#include "copy.h"
#include "endpoint.h"
#include "memobj.h"
#include "svc/svc.h"

namespace lkuser {

namespace {

// the startup block may take a quarter of the main stack
constexpr size_t kMaxArgsSize = proc::kMainStackSize / 4;

// started processes that are not yet reaped; each holds a reference
lk::list<proc> proc_list;
Mutex proc_list_lock;

event_t reap_event = EVENT_INITIAL_VALUE(reap_event, false, EVENT_FLAG_AUTOUNSIGNAL);

uint32_t next_pid = 1;

} // namespace

// a memory object mapped at [va, va + len)
struct proc::mapping : lk::list_hook<> {
    mapping(size_t l, lk::ref_ptr<memobj> m) : len(l), obj(std::move(m)) {}

    vaddr_t va = 0;
    const size_t len;
    const lk::ref_ptr<memobj> obj;
};

proc::proc(uint32_t pid) : object(kKind), pid_(pid) {
    event_init(&exit_event_, false, 0);
    for (impulse_node &n : impulse_pool) {
        n.pool = this;
        free_impulses.push_back(&n);
    }
}

proc::~proc() {
    DEBUG_ASSERT(threads_.is_empty());
    free_aspace();
    drop_mappings();
    free(args_);
    event_destroy(&exit_event_);
    // a queued impulse holds a reference to its pool's process, so every one is back
    free_impulses.clear();
    // never started, so never notified
    if (exit_notify_) {
        exit_notify_->handle_removed(exit_notify_rights_);
    }
}

status_t proc::create(int argc, const char *const *argv, bool kernel_services,
                      const startup_handle *extra, size_t nextra, lk::ref_ptr<proc> *out) {
    if (argc < 1 || nextra > LK_MSG_MAX_HANDLES) {
        return ERR_INVALID_ARGS;
    }

    lk::ref_ptr<proc> p =
        object::make<proc>(__atomic_fetch_add(&next_pid, 1, __ATOMIC_RELAXED));
    if (!p) {
        return ERR_NO_MEMORY;
    }

    // the image's file name, for messages
    const char *slash = strrchr(argv[0], '/');
    strlcpy(p->name_, slash ? slash + 1 : argv[0], sizeof(p->name_));

    lk_proc_handle handles[kMaxServices + LK_MSG_MAX_HANDLES];
    size_t nhandles = 0;
    if (kernel_services) {
        size_t nservices;
        const service *svcs = lkuser::services(&nservices);
        for (size_t i = 0; i < nservices; i++) {
            if (svcs[i].tag == kNoTag) {
                continue;
            }
            handles[nhandles].tag = svcs[i].tag;
            status_t err = connect(p.get(), svcs[i].ep, &handles[nhandles].handle);
            if (err < 0) {
                return err;
            }
            nhandles++;
        }
    }
    for (size_t j = 0; j < nextra; j++) {
        handles[nhandles].tag = extra[j].tag;
        status_t err = p->handles_.add(extra[j].obj, extra[j].rights, &handles[nhandles].handle);
        if (err < 0) {
            return err;
        }
        nhandles++;
    }

    status_t err = p->build_args(argc, argv, handles, nhandles);
    if (err < 0) {
        return err;
    }

    err = vmm_create_aspace(&p->aspace_, p->name_, 0);
    if (err < 0) {
        p->aspace_ = nullptr;
        return err;
    }

    *out = std::move(p);
    return NO_ERROR;
}

// The startup block: header, argument offsets, handles, then the strings.
status_t proc::build_args(int argc, const char *const *argv, const lk_proc_handle *handles,
                          size_t nhandles) {
    const size_t header =
        sizeof(lk_proc_args) + argc * sizeof(uint32_t) + nhandles * sizeof(lk_proc_handle);
    size_t len = header;
    for (int i = 0; i < argc; i++) {
        len += strlen(argv[i]) + 1;
        if (len > kMaxArgsSize) {
            return ERR_TOO_BIG;
        }
    }

    uint8_t *buf = static_cast<uint8_t *>(malloc(len));
    if (!buf) {
        return ERR_NO_MEMORY;
    }

    auto *a = reinterpret_cast<lk_proc_args *>(buf);
    a->nargs = argc;
    a->nhandles = nhandles;
    auto *offsets = reinterpret_cast<uint32_t *>(a + 1);
    memcpy(offsets + argc, handles, nhandles * sizeof(lk_proc_handle));
    size_t pos = header;
    for (int i = 0; i < argc; i++) {
        const size_t n = strlen(argv[i]) + 1;
        offsets[i] = pos;
        memcpy(buf + pos, argv[i], n);
        pos += n;
    }

    args_ = buf;
    args_len_ = len;
    return NO_ERROR;
}

status_t proc::start(vaddr_t entry) {
    {
        AutoLock guard(lock_);
        if (state_ != state::CREATED || !args_) {
            return ERR_BAD_STATE;
        }
    }

    void *stack = nullptr;
    status_t err = vmm_alloc(aspace_, "main stack", kMainStackSize, &stack, PAGE_SIZE_SHIFT, 0,
                             ARCH_MMU_FLAG_PERM_USER | ARCH_MMU_FLAG_PERM_READ |
                                 ARCH_MMU_FLAG_PERM_WRITE);
    if (err < 0) {
        return err;
    }
    auto free_stack =
        lk::make_auto_call([this, stack]() { vmm_free_region(aspace_, (vaddr_t)stack); });
    err = zero_user_pages(aspace_, (vaddr_t)stack, kMainStackSize);
    if (err < 0) {
        return err;
    }

    // sp points at the startup block, 16-byte aligned as the calling convention asks
    const vaddr_t sp = ROUNDDOWN((vaddr_t)stack + kMainStackSize - args_len_, 16);
    err = copy_to_user(aspace_, sp, args_, args_len_);
    if (err < 0) {
        return err;
    }

    lk::ref_ptr<thread> t;
    err = thread::create(this, entry, sp, &t);
    if (err < 0) {
        return err;
    }
    {
        AutoLock guard(lock_);
        // a kill since the check above has closed the handle table
        if (state_ != state::CREATED) {
            return ERR_BAD_STATE;
        }
        err = t->create_lk_thread();
        if (err < 0) {
            return err;
        }
        // nothing fails past here: a created LK thread has to run
        free_stack.cancel();
        main_stack_ = (vaddr_t)stack;
        main_tid_ = t->tid();
        state_ = state::RUNNING;
        live_threads_ = 1;
        t->add_ref();
        threads_.push_back(t.get());
    }
    free(args_);
    args_ = nullptr;
    {
        AutoLock guard(proc_list_lock);
        add_ref();
        proc_list.push_back(this);
    }

    t->resume();
    return NO_ERROR;
}

status_t proc::start_thread(thread *t) {
    {
        AutoLock guard(lock_);
        if (state_ != state::RUNNING) {
            return ERR_BAD_STATE;
        }
        status_t err = t->create_lk_thread();
        if (err < 0) {
            return err;
        }
        live_threads_++;
        t->add_ref();
        threads_.push_back(t);
    }
    // an exit from here on has killed it, and it leaves before entering user space
    t->resume();
    return NO_ERROR;
}

void proc::exit(int code) {
    {
        AutoLock guard(lock_);
        if (state_ == state::DEAD) {
            return;
        }
        state_ = state::DEAD;
        exit_code_ = code;
        for (thread &t : threads_) {
            t.kill();
        }
    }

    // before the handles go, so anyone who sees the endpoints close has the line too
    printf("lkuser: process %u (%s) exited with code %d\n", pid_, name_, code);
    cancel_inflight(this);
    handles_.close_all();
    event_signal(&exit_event_, true);
    event_signal(&reap_event, true);
}

void proc::set_exit_notify(lk::ref_ptr<object> conn, uint32_t rights) {
    DEBUG_ASSERT(!exit_notify_);
    conn->handle_added(rights);
    exit_notify_ = std::move(conn);
    exit_notify_rights_ = rights;
}

// The PROCESS_EXITED impulse, then the held handle goes, which may be the connection's last
// client view and send CONN_CLOSED behind it.
void proc::notify_exit() {
    if (!exit_notify_) {
        return;
    }
    auto *conn = static_cast<connection *>(exit_notify_.get());
    int code = 0;
    exited(&code);
    lk_impulse_process_exited payload = {.pid = pid_, .exit_code = code, .reserved = {}};
    {
        LKUSER_IPC_LOCK(lock_state);
        exit_impulse_.imp = {
            .type = LK_IMPULSE_PROCESS_EXITED, .flags = 0, .cookie = conn->cookie(), .payload = {}};
        memcpy(exit_impulse_.imp.payload, &payload, sizeof(payload));
        // a closed endpoint has nobody to tell
        queue_impulse_locked(conn->ep(), &exit_impulse_, this);
        LKUSER_IPC_UNLOCK(lock_state);
    }
    lk::ref_ptr<object> held = std::move(exit_notify_);
    held->handle_removed(exit_notify_rights_);
}

bool proc::thread_exiting() {
    AutoLock guard(lock_);
    DEBUG_ASSERT(live_threads_ > 0);
    return --live_threads_ == 0;
}

int proc::wait() {
    event_wait(&exit_event_);
    // written before the event was signaled
    return exit_code_;
}

bool proc::exited(int *code) {
    AutoLock guard(lock_);
    if (state_ != state::DEAD) {
        return false;
    }
    *code = exit_code_;
    return true;
}

bool proc::is_dead() {
    AutoLock guard(lock_);
    return state_ == state::DEAD;
}

status_t proc::map(lk::ref_ptr<memobj> m, size_t off, size_t len, uint arch_mmu_flags,
                   vaddr_t *va) {
    const paddr_t pa = m->paddr() + off;
    auto *mp = new (std::nothrow) mapping(len, std::move(m));
    if (!mp) {
        return ERR_NO_MEMORY;
    }

    void *ptr = nullptr;
    status_t err;
    {
        AutoLock guard(mappings_lock_);
        err = vmm_alloc_physical(aspace_, "memobj", len, &ptr, PAGE_SIZE_SHIFT, pa, 0,
                                 arch_mmu_flags);
        if (err == NO_ERROR) {
            mp->va = reinterpret_cast<vaddr_t>(ptr);
            mappings_.push_back(mp);
            *va = mp->va;
            return NO_ERROR;
        }
    }
    // the memory object's reference goes outside the lock
    delete mp;
    return err;
}

status_t proc::unmap(vaddr_t va) {
    mapping *found = nullptr;
    status_t err;
    {
        AutoLock guard(mappings_lock_);
        for (mapping &m : mappings_) {
            if (va >= m.va && va - m.va < m.len) {
                found = &m;
                break;
            }
        }
        err = vmm_free_region(aspace_, va);
        if (found && err == NO_ERROR) {
            mappings_.remove(found);
        } else {
            found = nullptr;
        }
    }
    delete found;
    return err;
}

void proc::drop_mappings() {
    for (;;) {
        mapping *m;
        {
            AutoLock guard(mappings_lock_);
            m = mappings_.pop_front();
        }
        if (!m) {
            return;
        }
        delete m;
    }
}

void proc::reap_exited() {
    for (;;) {
        proc *found = nullptr;
        {
            AutoLock guard(proc_list_lock);
            for (proc &p : proc_list) {
                if (p.is_dead()) {
                    found = &p;
                    break;
                }
            }
            if (found) {
                proc_list.remove(found);
            }
        }
        if (!found) {
            return;
        }

        found->teardown();
        found->signal(LK_SIGNAL_TERMINATED);
        found->notify_exit();
        const bool boot = found->boot_;
        // proc_list's reference
        found->release();
        if (boot) {
            boot_reaped();
        }
    }
}

void proc::teardown() {
    // Every thread has called thread_exit() or is about to. The lkuser thread holds the LK
    // thread's storage, so it is dropped only once joined.
    for (;;) {
        thread *t;
        {
            AutoLock guard(lock_);
            t = threads_.pop_front();
        }
        if (!t) {
            break;
        }
        t->join();
        // threads_'s reference
        t->release();
    }

    free_aspace();
    drop_mappings();
}

void proc::free_aspace() {
    if (!aspace_) {
        return;
    }

    // A joined thread's cpu may still be switching away from the aspace, which reports ERR_BUSY.
    status_t err;
    while ((err = vmm_free_aspace(aspace_)) == ERR_BUSY) {
        thread_sleep(1);
    }
    if (err < 0) {
        printf("lkuser: freeing the address space of process %u failed: %d\n", pid_, err);
    }
    aspace_ = nullptr;
}

namespace {

int reaper(void *arg) {
    for (;;) {
        event_wait(&reap_event);
        proc::reap_exited();
    }
    return 0;
}

void lkuser_init(uint level) {
    thread_t *t = thread_create("lkuser reaper", &reaper, nullptr, HIGH_PRIORITY, DEFAULT_STACK_SIZE);
    if (!t) {
        printf("lkuser: no reaper thread, exited processes will not be freed\n");
        return;
    }
    thread_detach_and_resume(t);
}

} // namespace

LK_INIT_HOOK(lkuser, lkuser_init, LK_INIT_LEVEL_THREADING);

} // namespace lkuser
