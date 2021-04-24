.section .text.startup

.globl _start
_start:
.option push
.option norelax
    la  gp, __global_pointer$
.option pop

    j   _start_c

