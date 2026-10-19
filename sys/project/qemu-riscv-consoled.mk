# The full test build with the console served from user space: /bin/consoled starts at boot,
# relaying to the kernel console, and every process made after it talks to it under
# LK_TAG_CONSOLE.
LKUSER_BOOT := /bin/consoled@console

include project/qemu-riscv-usertest.mk
