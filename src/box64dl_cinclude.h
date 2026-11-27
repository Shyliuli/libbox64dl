/*用于c! 需要获取结构体内部字段*/
#include "box64dl_bindgen.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "auxval.h"
#include "box64cpu.h"
#include "box64cpu_util.h"
#include "debug.h"
#include "guestalloc.h"
#include "x64emu_private.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef size_t usize;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
