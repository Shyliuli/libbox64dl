# libbox64dl

把 box64 从"接管进程的模拟器程序"改造成"可嵌入的库"：宿主进程在自己的地址空间里
`dlopen` / `dlsym` / 调用 **x86_64 共享库**，宿主本身是 LoongArch 原生代码。

上游 box64 已经能加载任意 x86_64 `.so`（`my_dlopen` / `my_dlsym`），但那条路径只在
box64 作为程序启动、建立 `my_context` 和线程 emu 之后才可达。libbox64dl 补的就是这段：
不接管进程地把运行时初始化起来，并给宿主侧一组 C API。

## API

```c
#include "libbox64.h"

box64_lib_init();                                  /* 起运行时 */
void* h = box64_dlopen("guest.so", RTLD_NOW);      /* 加载 x86_64 .so */
void* f = box64_dlsym(h, "func");                  /* 取 guest 函数 */
box64_call(f, "pp", a, b);                         /* 简单签名调用 */
box64_call_regs(f, gregs, fregs);                  /* 寄存器直通 */
box64_call_marshaled(f, gregs, fregs, stk, len);   /* 带栈参数 */
void* cb = box64_callback(host_fn);                /* 宿主函数 → guest 可调地址 */
```

调用是每线程一个 emu、各带独立 guest 栈，`DynaCall` 不加全局锁；全局锁只保护
`dlopen` / `dlsym` / bridge 注册这些改动共享结构的操作。

## 运行时 trampoline

guest 的函数指针不能交给宿主去调——宿主会当成本地代码跳进去。常规做法是预先静态
生成一张 thunk 表，但那就给"能桥接多少个不同的 guest 回调"设了硬上限。这里改成按需
生成：

```c
#include "box64dl_thunk.h"

/* 调用它时 $t0=data0、$t1=data1，然后尾调 target（入参寄存器不动）。
   按 (target, data0) 去重，所以同一个 guest 函数永远拿到同一个 thunk。*/
void* t = box64dl_thunk_get(shim, (uint64_t)guest_fn, 0);

box64dl_thunk_release_range(lo, hi);   /* 模块卸载时整批回收 */
```

`target` 通常是一小段汇编 shim，把入参整体左移一位、把 `$t0` 补到最前，再进 C 分发函数。

实现上模板由汇编器生成（`thunk_la64.S`）而非手工编码指令，运行时整段 memcpy、只回填
三个数据字；模板用 `pcaddi` 寻址自己的数据，所以拷到任何地址都能跑、不需要重定位。
代码页通过 memfd 双映射（一份 RW 写、一份 RX 执行），避免在别的线程正执行 thunk 时
去 mprotect 同一页。

`box64_dlclose` 会在 ELF 确实卸载后（用 `FindElfAddress` 判定，而不是只看引用计数
归零）回收落在该模块地址区间内的 thunk——指向已 unmap 代码的 thunk 比泄漏更危险。

unilib 和 uninode 都建立在这个原语上：前者用它做 `dlsym` 的 host→guest stub，
后者用它桥接 N-API 回调。

## 两个构建产物

| 目录 | 用途 |
| --- | --- |
| `third_party/box64/build/libbox64.a` | 宿主可执行文件 |
| `third_party/box64/build-pic/libbox64.a` | `.so` / `.node` 消费者 |

两份都开 dynarec。曾经认为 PIC 那份必须关掉，理由是 LoongArch dynarec 会发 TLS_LE
重定位、共享对象里放不下——实际上 TLS_LE 只在按默认的**非 PIC** 方式构建 box64 时才
出现，`-fPIC` 下编译器发的是 TLS_LD，链接没有问题。实测 guest 侧哈希快约 3 倍。

回退有两级，不必都用：

```bash
make pic PIC_DYNAREC=OFF   # 构建期关掉
BOX64_DYNAREC=0 ./your-app # 运行期回退，无需重新构建
```

消费者链接时除了 `libbox64.a` 还要带上同目录的 `libdynarec.a`（CMake 的 STATIC 目标
不会把依赖归档并进来）。

```bash
make          # 两份都构建
make pic      # 只构建 PIC（uninode 需要）
make static   # 只构建静态（unilib 测试需要）
make plugins  # 列出已注册插件
```

## wrapped library 插件

guest 需要的、必须由宿主提供的库（典型如 `libnode.so`），用插件形式注册。
每个 `plugins/<name>/` 提供三个文件：

```
wrapped<lib>.c            box64 wrapper 实现（GO/GOM）
wrapped<lib>_private.h    符号与签名表
library_list.inc          GO("soname", <lib>) 行
```

下游项目把自己的目录软链进 `plugins/` 即完成注册，例如 uninode：

```make
$(LIBBOX64DL)/plugins/libnode:
	ln -sfn $(ROOT)/wrapped $@
```

`make graft` 会把插件软链进 vendored 树，并生成 `library_list_extra.h` 与
`box64dl_plugins.cmake`。

## 对 vendored box64 的改动面

`third_party/box64` 只有两行手改，其余全部是软链或生成物，所以这棵树能直接和上游同步：

1. `src/library_list.h` 末尾 `#include "library_list_extra.h"`
2. `CMakeLists.txt` 里 `include("${BOX64_ROOT}/box64dl_plugins.cmake" OPTIONAL)`

外加 `CMakeLists.txt` 里的 `libbox64` STATIC target 与 `src/libbox64.c` 一行源文件登记。

## 嵌入时的两个坑

- `NewBox64Context` 把 `box64lib` 设为 `dlopen(NULL)`。作为程序时那就是 box64 自己，
  GOM 的 `my_*` 符号都在里面；嵌进宿主后 `dlopen(NULL)` 变成宿主可执行文件，
  `my_*` 却在我们这个 DSO 里。`box64_lib_init` 用 `dladdr` 把 `box64lib` 重绑到自身模块。
- `core.c` 引用 `unittest()`，嵌入时不链接 `test.c`，由 `libbox64.c` 提供 stub。
