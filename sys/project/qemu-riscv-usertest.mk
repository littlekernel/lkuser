MODULES += \
	lib/lkuser

# lib/lkuser owns the user exception hooks the arch user mode tests override
WITH_ARCH_USPACE_TESTS := 0

include project/qemu-virt-riscv64-supervisor-test.mk

