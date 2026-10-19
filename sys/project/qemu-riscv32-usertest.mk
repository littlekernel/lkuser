# The usertest profile on riscv32: the riscv64 project's knobs over LK's sv32 supervisor project.
LKUSER_LK_PROJECT := project/qemu-virt-riscv32-supervisor-test.mk

include project/qemu-riscv-usertest.mk
