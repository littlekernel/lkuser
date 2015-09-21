
lk:
	$(MAKE) -f makefile.lk

clean:
	$(MAKE) -f makefile.lk clean

spotless: clean
	$(MAKE) -f makefile.lk spotless

configure-newlib build-newlib/Makefile:
	mkdir -p build-newlib
	cd build-newlib && ../newlib/configure --target arm-eabi --disable-newlib-supplied-syscalls --prefix=`pwd`/../install-newlib

build-newlib: build-newlib/Makefile
	mkdir -p install-newlib
	$(MAKE) -C build-newlib
	$(MAKE) -C build-newlib install MAKEFLAGS=

clean-newlib:
	rm -rf build-newlib install-newlib

.PHONY: lk clean spotless build-newlib configure-newlib clean-newlib
