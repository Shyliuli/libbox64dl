.PHONY: test clean

test:
	x86_64-linux-gnu-gcc -O2 -shared -fPIC -nostdlib guest.c -o libguest.so
	cc gen.c -I.. -o gen -L$(LIB_DIR) -llibbox64dl -Wl,-rpath,$(LIB_DIR)
	cat template.h > stubs.h && ./gen >> stubs.h
	cc test.c -I.. -o test03 -L$(LIB_DIR) -llibbox64dl -Wl,-rpath,$(LIB_DIR)
	./test03

clean:
	rm -f gen test03 libguest.so stubs.h
