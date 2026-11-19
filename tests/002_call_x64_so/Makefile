.PHONY: test clean

test:
	cc -shared -fPIC -o libadder.so adder.c
	cc test.c -I.. -o test02 -L$(LIB_DIR) -llibbox64dl -Wl,-rpath,$(LIB_DIR)
	./test02

clean:
	rm -f libadder.so test02
