.PHONY: test clean

X86_64_CC ?= $(if $(filter x86_64,$(shell uname -m)),$(CC),x86_64-linux-gnu-gcc)

test:
	$(X86_64_CC) -shared -fPIC -nostdlib -o libadder.so adder.c
	$(CC) test.c -I../../src -I.. -o test02 -L$(LIB_DIR) -llibbox64dl -Wl,-rpath,$(LIB_DIR)
	./test02

clean:
	rm -f libadder.so test02
