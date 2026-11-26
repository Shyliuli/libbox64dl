# ifndef TEST_H_
# define TEST_H_
#include<stdio.h>
#include<stdlib.h>
// TODO: 拓展测试框架
#define eprintf(...) fprintf(stderr, __VA_ARGS__)
#define FAIL(...) do { eprintf(__VA_ARGS__); exit(-1); } while (0)
#define BOX64DL_RTLD_NOW 2
#define BOX64DL_RTLD_GLOBAL 0x100

# endif