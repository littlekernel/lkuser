MODULES += \
	lib/lkuser

# lib/lkuser owns the user exception hooks the arch user mode tests override
WITH_ARCH_USPACE_TESTS := 0

# The mini OS: the console and process services only, a small handle table, and one static
# process started at boot, which gets the table's size to check. The boot file system is gone
# once its image is loaded. A qemu-<arch>-mini project names the LK project underneath in
# LKUSER_LK_PROJECT and includes this.
LKUSER_SERVICES := console process
LKUSER_HANDLES := 16
LKUSER_BOOT := /bin/minitest:$(LKUSER_HANDLES)

include $(LKUSER_LK_PROJECT)
