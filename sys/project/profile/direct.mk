# The direct boot: no shell and no console commands. The kernel's only process is init, loaded
# from an rofs image linked into the kernel and mounted at /init (beside a memfs at /tmp);
# init runs the apps the kernel command line names (lkuser.init), and with lkuser.halt the
# kernel powers off once init is done. The makefile builds the image from FS_LIST and passes
# its path as ROFS_EMBED_IMAGE. A qemu-<arch>-direct project names the shell-less LK target
# underneath in LKUSER_LK_PROJECT and includes this.
MODULES += \
	lib/fs \
	lib/fs/rofs \
	lib/fs/rofs/embed \
	lib/lkuser

# the usertest services, so init runs the same apps
LKUSER_SERVICES := console process time vm namespace file echo
LKUSER_BOOT := /init/bin/init
LKUSER_BOOT_FS := rofs
LKUSER_BOOT_DEV := rofs0

# the image's files are at page boundaries (the makefile's ROFS_ALIGN, which it passes as
# ROFS_EMBED_ALIGN), so a file's address in it is a run of whole pages
ROFS_EMBED_NAME := rofs0

include $(LKUSER_LK_PROJECT)
