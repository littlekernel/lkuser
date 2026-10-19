MODULES += \
	lib/lkuser

# lib/lkuser owns the user exception hooks the arch user mode tests override
WITH_ARCH_USPACE_TESTS := 0

# the default services plus the echo test service
LKUSER_SERVICES := console process time vm namespace file echo

# the LK project underneath: riscv64 here, riscv32 when a qemu-riscv32 project includes this one
LKUSER_LK_PROJECT ?= project/qemu-virt-riscv64-supervisor-test.mk
include $(LKUSER_LK_PROJECT)
