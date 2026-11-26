.PHONY: test clean

test:
	x86_64-linux-gnu-gcc -O2 -shared -fPIC -nostdlib guest.c -o libguest.so
	cc test.c -I.. -o test02 -L$(LIB_DIR) -llibbox64dl -Wl,-rpath,$(LIB_DIR)
	./test02

clean:
	rm -f test02 libguest.so
