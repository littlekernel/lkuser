MODULES += \
	lib/lkuser

# lib/lkuser owns the user exception hooks the arch user mode tests override
WITH_ARCH_USPACE_TESTS := 0

# The mini OS: the console and process services only, a small handle table, and one static
# process started at boot, which gets the table's size to check. The boot file system is gone
# once its image is loaded.
LKUSER_SERVICES := console process
LKUSER_HANDLES := 16
LKUSER_BOOT := /bin/minitest:$(LKUSER_HANDLES)

# the LK project underneath: riscv64 here, riscv32 when a qemu-riscv32 project includes this one
LKUSER_LK_PROJECT ?= project/qemu-virt-riscv64-supervisor-test.mk
include $(LKUSER_LK_PROJECT)
