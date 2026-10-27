# The direct profile on riscv64: LK's qemu-virt-riscv target in supervisor mode, no shell.
SUBARCH := 64
RISCV_MODE := supervisor
LKUSER_LK_PROJECT := project/target/qemu-virt-riscv.mk
include project/profile/direct.mk
