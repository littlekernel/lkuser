# The usertest profile: every kernel service, and init at boot, which runs the apps the kernel
# command line names (lkuser.init); the shell stays for running them by hand. A
# qemu-<arch>-usertest project names the LK project underneath in LKUSER_LK_PROJECT and
# includes this.
MODULES += \
	lib/lkuser

# lib/lkuser owns the user exception hooks the arch user mode tests override
WITH_ARCH_USPACE_TESTS := 0

# the default services plus the echo test service
LKUSER_SERVICES := console process time vm namespace file echo
# consoled sets its own list, with init last
LKUSER_BOOT ?= /init/bin/init

include $(LKUSER_LK_PROJECT)
