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
box64_call_simple(f, a0, a1, a2, a3, a4, a5, &rax); /* 标量快速路径 */
box64_call_desc(f, &desc, &out);                  /* 浮点/结构体/栈参数 */
void* cb = box64_callback(host_fn);               /* 宿主函数 → guest 可调地址 */
```

引擎寄存器文件的 ABI 类型与容量常量集中在 `include/libbox64_abi.h`；`libbox64.h`
自身不依赖 unilib，生成的 unilib stub 直接使用同一套 descriptor 类型。调用是每线程
一个 emu、各带独立 guest 栈，`DynaCall` 不加全局锁；全局锁只保护 `dlopen` / `dlsym` /
bridge 注册这些改动共享结构的操作。

## 标量入口与 descriptor 入口

参数全是整数/指针、最多六个且不需要浮点或栈参数时，使用标量入口。它按 SysV64 的参数
寄存器顺序接收六个物理槽，未使用的槽传零，并通过独立的 `BOX64_CALL_OK` 状态与 RAX
输出区分“guest 返回零”和“引擎拒绝”：

```c
uint64_t rax;
box64_call_status_t st = box64_call_simple(f, a0, a1, a2, a3, a4, a5, &rax);
```

带浮点、嵌套结构体、大结构体按值或 sret，以及需要 guest 栈数据的签名走 descriptor
入口。descriptor 的掩码点名固定的物理 GPR/SSE 位置，`stack_len` 是栈 blob 的字节数，
三个地址字段指向本次调用的寄存器文件和栈数据；返回值通过 `box64_call_results_t` 写回：

```c
box64_call_desc_t desc = {
    .flags = BOX64_CALL_FLAG_FORCE_FRAME,
    .gpr_mask = gpr_mask, .fpr_mask = fpr_mask,
    .ret_mask = ret_mask, .stack_len = stack_len,
    .gprs = BOX64_PTR_TO_U64(gprs),
    .fprs = BOX64_PTR_TO_U64(fprs),
    .stack = BOX64_PTR_TO_U64(stack),
};
box64_call_results_t out;
box64_call_status_t st = box64_call_desc(f, &desc, &out);
```

**引擎不在运行时解析或归类签名**，只按 descriptor 掩码读写；生成器在编译期完成 ABI
分类。每个生成 stub 还把 descriptor 的 flags、掩码和长度放进 `static const` 模板，
调用时只复制该静态部分并改写本次调用的三个指针字段，避免每次清零并逐字段构造 48 字节
descriptor。`box64_fpr_t` 始终携带一份 SSE 寄存器的完整 16 字节。`stack_len` 是
`uint32_t`，生成期仍会检查其不会溢出 descriptor 字段。

`flags` 只接受两位：`BOX64_CALL_FLAG_FORCE_FRAME`（`stack_len == 0` 也保留一个锚定
frame；非零 `stack_len` 本来就锚定）和 `BOX64_CALL_FLAG_FULL_REGS`（走历史通用
`DynaCall`，正常返回后把调用前的 RDI/RSI/RBX/RBP 额外写回；不设则走 `DynaCall_abi`，
恰好跳过这组恢复，其余 RSP/RIP/eflags/长跳转规则完全相同，其它寄存器两种模式都不
恢复）。它是**调用方可见的后置条件**，不是按 ABI 自动优化：需要这四个寄存器跨调用保持
就设它；只有确认调用后它们已死（例如 SysV64 的 RDI/RSI 返回值不读，或符合 Win64 约定的
被调方已自行保存其非易失 RDI/RSI）才可清除。不要拿一种 ABI 的 clobber 规则套到另一种：
Win64 的 RDI/RSI 是 callee-saved 而 RCX/RDX/R8/R9 易失，与 SysV64 相反，见
`include/libbox64_abi.h`。两位之外的任何 flag、
非零 `reserved0`/`reserved1`、越界掩码（gpr > 0x3f、fpr > 0xff、ret > 0x0f）以及
“掩码点名空数组/空栈”都由 `include/libbox64_abi.h` 的公共 `static inline`
`box64_call_desc_validate()` 统一判定；`box64_call_desc()` 与 winedl 的描述符入口共享
这一个规则，不各自维护检查表，也不新增第三个执行入口。

诊断开关默认**关闭**。`BOX64DL_DIAG` 只在第一次使用时采样一次，`box64_diag_set()` 可以
在运行期覆盖它；错误从不走这个开关，始终经 `box64_dlerror()` 报告。相关回归：

```bash
make -C libbox64dl test-call-arity   # 定参入口 + 重入回调
make -C libbox64dl test-call-plan    # descriptor 掩码与结构体摆放
make -C libbox64dl test-call-fini    # box64_lib_fini() 之后的调用被拒绝
```

**终止与 ready 契约。** phase 由 Rust 独占（`rust/src/phase.rs`）：
`UNINIT → INITIALIZING → READY`，`READY → CLOSING → FINAL`，失败走终态 `FAILED`。
Rust 只导出**短叶**迁移原语（`box64dl_rust_phase_*`：拿锁、改状态、发布、返回），
编排放到 C（`src/libbox64.c`）：`box64_lib_init` 先 `begin_init` 再在 C 帧里 boot；
`box64_lib_fini` 先发布 `CLOSING` 再调 box64 自己的 `endBox64()`（guest fini/destructor
都在 C 帧，Rust 锁与 Rust 帧都不跨 guest）。C 侧还有显式准入计数
（`engine_admit`/`engine_release`，**不是** Rust RAII 票据，foreign longjmp 不会跳过
Drop）：有 guest 调用在飞时 `box64_lib_fini()` **拒绝**并发布终态 `FINAL`，
**不**强 free 活 context，错误槽要求进程退出；此后所有入口（含 cached-emu 快路径）
先查 phase 再拒绝。`box64_is_ready()` 只读 phase。`FAILED`/`FINAL` 都是终态：此后
`box64_lib_init()` 被显式拒绝并给出原因，`box64_dlopen()` 干净失败并置错误。
Rust **不会**自己 free context（`Engine` 没有 freeing `Drop`），所以不可能二次释放；
init 中途失败走 shim 的单独 free 路径并记 FAILED。`my_context` 是 box64 自己的全局，
不是状态源。`tests/test_call_fini.c`、`tests/test_fini_race.c` 与
`tests/consumer_smoke.c` 钉住这条契约；完整所有权/双重释放审计见
[`docs/engine-ownership.md`](docs/engine-ownership.md)。

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
uint64_t st_rax;
int32_t st = (int32_t)box64_call_simple(g.detach_current_thread, 0, 0, 0, 0,
                                         0, 0, &st_rax);
(void)st_rax;
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
| `vendor/box64/build/libbox64.a` | 宿主可执行文件 |
| `vendor/box64/build-pic/libbox64.a` | `.so` / `.node` 消费者 |

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
make pic      # 只构建 PIC（.so / DSO 消费者）
make static   # 只构建静态（进程内静态链接消费者）
make shared   # 引擎 DSO + 版本脚本（build/lib/libbox64dl.so）
make cli      # 打包版 box64 CLI（build/bin/box64，动态链接该 DSO）
make pkgconfig # 生成 build/lib/pkgconfig/libbox64dl.pc
make plugins  # 列出已注册插件
```

## 打包版 box64 CLI

`make cli` 产出 `build/bin/box64`：一个 thin front（`cli/box64_main.c` 就是上游 `main.c`
的 `initialize()` + `emulate()`），它**动态链接**打包树里的 `build/lib/libbox64dl.so`
（`-lbox64dl`，rpath `$ORIGIN/../lib`），不是在可执行文件里再静态链一份引擎。因此 CLI
与嵌入式消费者用的是**同一个 engine、同一套库级信号策略**。它是应用打包布局里的
`runtime/bin/box64`：

- winedl 用它当 `system_box64`：prefix 引导的 wineboot 子进程、以及嵌入式引擎
  的 `box64path`（wineserver / CreateProcess 子进程 re-exec 的落点）都指向它；
- 嵌入相关的补丁在独立运行路径上都是 no-op（0002 只改默认日志级别；0003 的
  host-fault 路由在没有外来宿主线程时永不触发），但这**不等于**与系统 box64 行为
  完全一致——CLI 跑的是本库的引擎与库级信号策略（默认 `fast`）。

wine 的 x86_64-unix `ntdll.so` 需要 libgcc 的 `_Unwind_Find_FDE`（它的
DWARF↔SEH 桥）；系统 box64 走模拟版 `/usr/lib/box64-x86_64-linux-gnu/libgcc_s.so.1`
拿到它，而 libgccs 插件选择 wrap 宿主 libgcc，因此符号表里必须带上——补上之前
wine 在这个引擎下起不来。

## wrapped library 插件（本仓 `libgccs`）与显式 native-module seam

guest 需要的、必须由宿主提供、且能在**构建期**命名的库，用 box64 的
wrapped-library 机制注册。每个 `plugins/<name>/` 提供三个文件：

```
wrapped<lib>.c            box64 wrapper 实现（GO/GOM）
wrapped<lib>_private.h    符号与签名表
library_list.inc          GO("soname", <lib>) 行
```

本仓的 plugin 集是**本仓库内的 `plugins/libgccs`**：`libgcc_s.so.1`
的 unwinding/quad-math 符号必须由宿主提供。`make graft` 把 plugin 的
`wrapped*.c` / `wrapped*_private.h` 软链进 vendored 树，并生成
`library_list_extra.h` 与 `box64dl_plugins.cmake`。

**显式 native-module seam（ABI 2，已实现）。** 另一个仓库的 provider（uninode 的
Node/N-API 宿主面）**不在构建期进入本仓**：provider 在首次 `box64_dlopen()` 之前调用
`box64_register_native_module(descriptor, BOX64_NATIVE_MODULE_ABI)` 登记一个固定
descriptor（模块名、guest sonames、`host_library`、bridge 表）。引擎在
wrapped-library 装载路径上先问 seam；命中时由 `src/native_module_lib.c` 构造运行时
wrapped library：

- **普通 entry**（无 `BOX64_BRIDGE_GOM`）：引擎对 `host_library`（NULL = 当前进程）
  做 `dlsym`，用 entry 的 signature token 对应的 box64 生成 wrapper 桥回 guest；
- **GOM entry**：provider 提供 handler，引擎用 E-form wrapper 调用
  `handler(emu, <guest args...>)`，由 handler 自己调宿主并用
  `box64_call_simple`/`box64_call_desc` + `box64dl_thunk_*` 重写 guest 回调；
- 引擎没有的 signature、或解析不到的宿主符号，**fail-closed**（符号解析失败，不静默
  fallback）。

引擎的 signature→wrapper 查表不是第二套 ABI 规则：`src/wrappedbox64dnative_private.h`
(uninode-agnostic 的 SysV token 列表) 作为 WRAPPEDS 输入交给 box64 自己的
`rebuild_wrappers.py`，`src/wrappedbox64dnative.c` 用同一份列表建表，实现
`box64_native_module_wrapper()`。provider 用它做 guest 回调重写的 bridge 地址查询时，
用公开的 `box64_native_function()`（内部 `GetNativeFnc` 的公开形态）。

因此本仓**既**能 engine-only 独立构建/测试（`check-no-node` 只看当前 graft 与产物，
`plugins/` 只允许 `libgccs`），**也**能在运行时装载兄弟仓的 provider，而不再需要
`libbox64dl/plugins/libnode` 软链。描述符布局、注入点与运行时构造见
`include/libbox64_native_module.h`、`patches/0010-native-module-seam.patch` 与
`src/native_module_lib.c`。

**Node 反向依赖仍然被禁。** 本仓不包含、不编译、不软链任何 Node/uninode/libnode
源：`tools/check_no_node.sh` 在构建输入层面断言这一点（活动 graft/provider 无 libnode：
`plugins/libnode` 不存在、无活跃 symlink 指向 uninode；plugin 集只有 `libgccs`、
生成的 cmake/list 无 Node 名、Makefile 不引用兄弟仓库、源码不 include Node/uv 头；
vendored 树里 upstream 自带的 generated 头与旧 build cache 不属于当前 graft，不判失败）。`make graft` 只把 `VENDOR_SRC` 列出的 C/asm 软链进
vendored 树；`src/core_shim.c` 与 `src/pt_mutex/*` 由 Rust crate 的 build script
编译，**不**进 engine archive。seam 的引擎侧源（`src/native_module*.c`、
`src/wrappedbox64dnative*`）只含 SysV signature token，不含任何 Node 符号。

`plugins.conf` / `PLUGIN_CONFIG` / `make plugin-manifest` / `make check-plugins` 仍
可用于显式固定 graft 的目录集合（只锁位置，不是版本锁）；`plugins.conf.example`
保留了这一机制的说明。

## 信号掩码策略（库级）

策略默认 `fast`，分**全局默认**和**每库覆盖**两级，优先级是
**显式 per-library > 环境库规则 > 全局默认**：

| 层级 | 配置 |
| --- | --- |
| 全局默认 | `BOX64DL_SIGNAL_POLICY=fast|compat`（默认 `fast`）；`box64_signal_policy_set()` / `box64_signal_policy_get()` |
| 每库覆盖 | `BOX64DL_SIGNAL_LIBRARY_RULES='libfoo.so=compat;/opt/bar/libbaz.so=fast'`：`;` 分隔的 `MATCH=policy`，MATCH 含 `/` 比完整路径/realpath、否则比最后一段，**精确匹配、无子串/前缀/glob、首条命中生效**；规则在任何库构造函数之前绑定，格式错误让 `box64_lib_init()` 直接失败 |
| 显式 API | `box64_signal_policy_set_library(void* handle, int policy)` / `box64_signal_policy_get_library(void* handle)` |

配置是**冷操作、必须顺序执行**：没有热路径锁，改动经 epoch 失效每线程 O(1) 策略缓存，
调用方不得让策略改动与其它线程上的 guest call、或 `dlopen`/`dlclose` 并发（与
`box64_lib_fini()` 同一份 sequential-config 契约）。`handle` 只能对已 `dlopen` 的库设置，
其构造函数已按环境规则/全局默认跑过，所以显式覆盖只作用于之后的新宿主 entry；
`get_library` 返回 override/规则/全局默认里的有效值。

作用域是**宿主入口**：一次宿主进入的策略覆盖该次调用链（含其直接依赖），guest 内部
自己发起的调用不切换策略——不保证覆盖所有 guest PC。宿主重入时，只有策略不同的嵌套
才强制建立新 frame，同策略可以共享一个 frame。

`fast` 省掉每次 `EmuRun()` 的 `rt_sigprocmask`（`sigsetjmp(buf, 0)`），改由信号路径按
**被投递信号的 ucontext 掩码**恢复；`compat` 则在**建立恢复点**时保存当时的入口掩码，
信号 `siglongjmp` 回恢复点时用这份保存值恢复。二者的差异来自 call 中途**任何来源**改过
掩码——guest、host 或引擎自身都算，不只是 guest 的最后一次 `sigprocmask`：`fast` 用
最近一次实际掩码，`compat` 回到恢复点保存的入口掩码。需要严格“恢复点入口掩码”语义的
库用 `compat`，例如：

```sh
BOX64DL_SIGNAL_POLICY=fast \
BOX64DL_SIGNAL_LIBRARY_RULES='libspecial.so=compat' \
  ./your-app
```

## fault 路由（src/signal_route.c）

与掩码策略分开的一件事：先决定 fault 归不归 box64。当前实现只有一条判定：

- Wine guest 线程（GS base≠0，TEB）→ guest。
- 其余 → 转发给 init 前的 host action（V8 等）；没有 host action 就带诊断按默认
  处置退出。

这条路径不查表、不读取环境变量，也不依赖任何未应用的外部 patch。

回归：`make test-signal-route`（真信号：host raise 归 host、bridge 回调 host fault
仍归 host）。

## 对 vendored box64 的改动面

引擎是**上游** box64 仓库的 git submodule `vendor/box64`，锁在 tag `v0.4.2`
（commit `7eeb5016493dab4e143d53da50dd47bfb44a9509`）。本仓不在 submodule 里提交任何
东西：`make patch` 把 `patches/0001`–`0018` 按序打进它的工作树，`make graft` 再把本仓
的源软链进去，构建落在 `vendor/box64/build{,-pic}`。所以 submodule 工作树**预期是脏的**
（`.gitmodules` 用 `ignore = untracked` 只隐藏它的 build/graft/生成物，指针本身仍可见）。

队列里与嵌入直接相关的改动：

1. `0012`：`src/library_list.h` 末尾 `#include "library_list_extra.h"`
2. `0011`：`CMakeLists.txt` 里 `include("${BOX64_ROOT}/box64dl_core.cmake" OPTIONAL)`
   与 `include("${BOX64_ROOT}/box64dl_plugins.cmake" OPTIONAL)`
3. `0010`：`src/librarian/library.c` 加一行 include，
   把 `initWrappedLib()` 改成三态返回（0=不是我们的 soname，>0=已构建，<0=被
   descriptor 认领但构建失败），并在它的两个调用点处理失败：认领后失败必须
   中止本次加载（不落到 emulated 路径、不加载同名 x86_64 客体、不留半初始化
   LIB_WRAPPED），让 `box64_dlopen()` 返回 NULL。seam 逻辑全在引擎自己的
   `src/native_module_lib.c`

`0011` 还包含 `libbox64` STATIC target、`main.c`/`test.c` 的 `BOX64_MAIN_SRC` 拆分与
`-rdynamic`；`0013`–`0018` 是上一版 `third_party/box64` 里以手改形式存在、无法从干净
上游重建的行为差异（x87 tag word/FXSAVE、LA64 FSQRT 标志与未对齐 16 位原子、LinkNext
帧标记、printer 签名、解释器 FSQRT、include 可移植性）。每条的动机见 patch 头。

验证整条链（submodule commit、patch 哈希、干净上游可应用、工作树无漂移）：

```bash
tools/verify_box64_patch.sh --check
```

未纳入队列的旧本地差异（从未定义的 `WINE_BOX64CPU_*` 开关、`_WIN32` 分支、CMake 不编译
的两个 Wine `src/os` 文件、`wine_step/`、`Makefile.in`、`#if 0` 调试块、由
`rebuild_wrappers.py` 重新生成的 wrapper 头）逐条列在
[`patches/README.md`](patches/README.md)；迁移审计见
[`docs/vendor-provenance.md`](docs/vendor-provenance.md)。

## 嵌入时的三个坑

- `NewBox64Context` 把 `box64lib` 设为 `dlopen(NULL)`。作为程序时那就是 box64 自己，
  GOM 的 `my_*` 符号都在里面；嵌进宿主后 `dlopen(NULL)` 变成宿主可执行文件，
  `my_*` 却在我们这个 DSO 里。`box64_lib_init` 用 `dladdr` 把 `box64lib` 重绑到自身模块。
- vendor 的 `librarian/library.c` 用 `dlsym(my_context->box64lib, "my_"+name)` 解析
  wrapped libc；对一个 dlopen 句柄做 dlsym 只看该模块的**动态**符号表。所以
  `src/libbox64dl.map` 必须把内部 `my*` 表（`my_`/`my2_`/`my32_`/各 altmy 前缀，
  约 2200 个 wrapper 符号）设为 global，同时把 Rust 运行时与私有
  `box64dl_core_*` shim 保持 local。只导出 `box64_*` 会让所有 DT_NEEDED libc guest
  的 `printf`/`fopen`/`bsearch` 解析失败。`tests/consumer_dso_guest.c` 用**只链最终
  DSO** 的 consumer 加载真实带 libc DT_NEEDED 的 guest 并观察
  printf/fopen/bsearch/destructor；`tools/check_exports.py` 分别校验公共 ABI 与内部
  wrapper 两类动态符号。
- `core.c` 引用 `unittest()`，嵌入时不链接 `test.c`，由 `libbox64.c` 提供 stub。

## 许可证

MIT，见 [`LICENSE`](LICENSE)。

`vendor/box64/` 是 [box64](https://github.com/ptitSeb/box64) 的上游 git submodule
（tag `v0.4.2`，commit `7eeb5016493dab4e143d53da50dd47bfb44a9509`），同为 MIT，版权归
ptitSeb（见该目录下的 `LICENSE`）。本仓库对它的本地改动是 `patches/0001`–`0018` 的编号
补丁；除此之外只有 submodule 工作树里的 graft 软链、生成物与构建目录，都不提交。来源、
上游 commit、队列明细与"哪些旧本地差异被判定为不可达"的审计见
[`docs/vendor-provenance.md`](docs/vendor-provenance.md) 与
[`patches/README.md`](patches/README.md)。
