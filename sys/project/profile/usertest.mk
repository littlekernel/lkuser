# The usertest profile: every kernel service; the console loads the apps. A
# qemu-<arch>-usertest project names the LK project underneath in LKUSER_LK_PROJECT and
# includes this.
MODULES += \
	lib/lkuser

# lib/lkuser owns the user exception hooks the arch user mode tests override
WITH_ARCH_USPACE_TESTS := 0

# the default services plus the echo test service
LKUSER_SERVICES := console process time vm namespace file echo

include $(LKUSER_LK_PROJECT)
