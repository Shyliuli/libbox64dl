.PHONY: test clean

test:
	cc test.c -I../../src -I.. -o test01 -L$(LIB_DIR) -llibbox64dl -Wl,-rpath,$(LIB_DIR)
	./test01

clean:
	rm -f test01
