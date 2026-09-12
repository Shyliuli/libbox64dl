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
box64_call_regs(f, gregs, fregs);                  /* 寄存器直通 */
box64_call_marshaled(f, gregs, fregs, stk, len);   /* 带栈参数 */
void* cb = box64_callback(host_fn);                /* 宿主函数 → guest 可调地址 */
```

调用是每线程一个 emu、各带独立 guest 栈，`DynaCall` 不加全局锁；全局锁只保护
`dlopen` / `dlsym` / bridge 注册这些改动共享结构的操作。

## 从 guest 状态回到 host（Wine detach 安全）

宿主 pthread 被 Wine attach（`__wine_attach_current_thread` / `__wine_adopt_thread`）后，
是靠 PE bridge 的 `longjmp()` 回到宿主栈的；这条路径绕过了 `DynaCall` 自己的寄存器恢复
（`dynarec.c`：只有正常返回才写回保存的 `old_rsp`）。于是该线程的 emu 会留下两个指向
Wine 映射的脏字段：

- `R_RSP` 仍在 Wine 的 TEB PE 栈上；
- guest GS（`emu->segs_offs[_GS]`）仍指向 Wine TEB。

而 Wine 的 `__wine_detach_current_thread()` 恰好要释放这两个映射，并且它的 stack guard
会拒绝在“模拟 SP 仍位于 Wine 栈内”时执行。所以完整的一次 Wine detach 是三步，顺序不能颠倒：

1. **先跑 PE 侧**：在 TEB / guest GS 还有效时，通过引擎在该线程上调用 PE
   `ntdll.dll!LdrShutdownThread`（DLL_THREAD_DETACH / FLS / PE TLS 释放）；
2. **再切 Box64 状态**：就是下面这两步里的 prepare——把 RSP 切到 Box64 自己的 emu 栈、
   清掉 guest GS；
3. **最后调 Unix 侧**：`__wine_detach_current_thread()` 释放 TEB / thread_data /
   server thread object / GS 绑定。

`LdrShutdownThread` 不是 `__wine_detach_current_thread()` 的一部分，必须在第 2 步切栈之前
跑完。下面是 Box64 侧第 2 步的用法：

```c
#include "libbox64_ext.h"

box64_guest_state_t guest;
/* 完整序列的第 2 步（宿主侧）：此时 PE LdrShutdownThread 已经跑完。把 RSP 切到
   Box64 自己的 emu 栈、清掉 guest GS；失败（返回 -1）就不要调用下面的 unix
   detach，线程保持原状。 */
if (box64_detach_current_thread_guest(&guest) != 0) return -1;

/* 第 3 步：通过引擎调 Unix 侧的 detach 导出；此时 SP 已不在 Wine 栈内。 */
int32_t st = (int32_t)box64_call_regs(g.detach_current_thread, eg, ef);
if (st != 0) {
  /* unix detach 失败。PE 侧已经跑过就无法还原，restore 只能回滚切栈、补不回
     PE 状态：线程按 spent 处理，不要再 restore/retry。 */
  return -1;
}
/* st == 0：线程已经 FOREIGN，保留快照但不要再 restore。 */
```

`box64_detach_current_thread_guest_restore()` 只在 detach 尚未跑过 PE
`LdrShutdownThread` 时才有意义：它回滚的是这一次切栈（把 guest RSP/GS 放回去），
不能撤销已经发生的 PE 线程清理。

`box64_prepare_detach_current_thread()` 是同一调用的正式名字（`box64_detach_current_thread_guest()`
是兼容别名）。切换点不是猜出来的地址：用的是 `emu->init_stack + emu->size_stack`，即
`internalX64Setup()` 为该 emu 记录的栈，`box64_ensure_emu()` 下就是 `new_x64emu()` 分配并
`setProtection_stack()` 登记的 8 MiB 映射；函数按引擎入口同样的 16 字节对齐取栈顶，并用
`getProtection_fast()` 校验该区域是活的、可写的 Box64 映射，不满足就 fail-closed 返回 -1、
不改任何字段。整个过程不分配、不释放任何栈（Wine 的栈由 Wine 的 detach 导出释放，Box64
的 emu 栈跟随 emu 到线程析构），Wine 侧 stack guard 一行未动。

验证：`make -C libbox64dl test-detach-guest`。它不需要 Wine，直接建真实 context/emu，
把 emu 摆成 attach 之后的脏状态，断言切换、回滚、无 emu 线程 no-op 与 fail-closed 路径。

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
make cli      # 打包版 box64 CLI（build/bin/box64）
make plugins  # 列出已注册插件
```

## 打包版 box64 CLI

`make cli` 产出 `build/bin/box64`：一个 LoongArch 可执行文件，`cli/box64_main.c`
就是上游 `main.c` 那两句 `initialize()` + `emulate()`，链接到本项目的静态引擎
（whole-archive `libbox64.a` + `libdynarec.a` + Rust facade，与 unilib 测试相同的
链接形状）。它是应用打包布局里的 `runtime/bin/box64`：

- winedl 用它当 `system_box64`：prefix 引导的 wineboot 子进程、以及嵌入式引擎
  的 `box64path`（wineserver / CreateProcess 子进程 re-exec 的落点）都指向它；
- 行为与系统 box64 完全一致（嵌入用的三张补丁对独立运行路径都是 no-op：
  0003 的 host-fault 路由在没有外来宿主线程时永不触发，0002 只改默认日志级别）。

wine 的 x86_64-unix `ntdll.so` 需要 libgcc 的 `_Unwind_Find_FDE`（它的
DWARF↔SEH 桥）；系统 box64 走模拟版 `/usr/lib/box64-x86_64-linux-gnu/libgcc_s.so.1`
拿到它，而 libgccs 插件选择 wrap 宿主 libgcc，因此符号表里必须带上——补上之前
wine 在这个引擎下起不来。

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

## 许可证

MIT，见 [`LICENSE`](LICENSE)。

`third_party/box64/` 是 [box64](https://github.com/ptitSeb/box64) 的 vendored 副本，
同为 MIT，版权归 ptitSeb（见该目录下的 `LICENSE`）。本仓库对它的改动只有两行 hook，
其余是软链与生成物。
