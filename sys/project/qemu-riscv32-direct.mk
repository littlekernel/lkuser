# The direct profile on riscv32: LK's qemu-virt-riscv target in supervisor mode (sv32), no shell.
SUBARCH := 32
RISCV_MODE := supervisor
LKUSER_LK_PROJECT := project/target/qemu-virt-riscv.mk
include project/profile/direct.mk
