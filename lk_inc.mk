# copy this and makefile to your external root directory and customize
# according to how you want to use a split repository

# the top level directory that all paths are relative to
LKMAKEROOT := .

# paths relative to LKMAKEROOT where additional modules should be searched
LKINC := sys

# the path relative to LKMAKEROOT where the main lk repository lives
LKROOT := lk

# set the directory relative to LKMAKEROOT where output will go
BUILDROOT ?= .

# set the default project if no args are passed
DEFAULT_PROJECT ?= qemu-arm-usertest

#TOOLCHAIN_PREFIX := <relative path to toolchain>
