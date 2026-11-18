#include <test.h>
void box64dl_init(void);
int main(void) {
    void (*p)(void) = box64dl_init;
    if (p == 0){
        eprintf("error: can not find box64dl_init!\n");
        return -1;
    }
    else{
        return 0;
    }
}