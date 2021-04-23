.section .text.startup
.arm
.globl _start
_start:
    // nothing to do so just branch into C code
    b   _start2
