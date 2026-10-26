# The full test build with the console served from user space: /bin/consoled starts at boot,
# relaying to the kernel console, and every process the kernel makes after it talks to it
# under LK_TAG_CONSOLE. A qemu-<arch>-consoled project names the LK project underneath in
# LKUSER_LK_PROJECT and includes this.
LKUSER_BOOT := /bin/consoled@console

include project/profile/usertest.mk
