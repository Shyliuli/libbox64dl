#include "stubs.h"

int main(void) {
    stub_init();
    int r = add(13, 29);
    if (r != 42) FAIL("add got %d, want 42\n", r);
    double d = twice(1.75);
    if (d != 3.5) FAIL("twice got %f, want 3.5\n", d);
    return 0;
}
