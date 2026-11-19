.PHONY: build

build:
	mkdir -p ./target/c/
	cc -c src/init.c -Ithird_party/box64/src/include -Ithird_party/box64/src/emu -fPIC -o target/c/init.o
