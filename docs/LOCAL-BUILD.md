# 本地能不能编 XNU？—— 实测结论（2026-10-30）

短答：**完整构建不能，文件级检查能。** 完整构建必须 macOS 工具链 + Darwin SDK；
这台是 Windows，WSL 里只有 Linux clang。下面是逐条实测，别重复试。

## 能做的（已经进仓库，几秒钟出结果）

| 工具 | 作用 | 实测 |
|---|---|---|
| `tools/ci/check_series.py` | 按 CI 相同顺序把 `patches/xnu/*.patch` 打进"上游+补丁"的临时树，验证每条都能打上、且没有 BOM/零宽字符混进源码 | 秒级；今晚 3 次编译失败里有 2 次这类错误它能提前抓住（`c2672e4` 的 U+FEFF、`690cb43` 的 `ISB_SY` 属于后一类，靠 `syntax_check` 抓） |
| `tools/ci/syntax_check.sh` | 用 WSL clang 对 `pexpert/arm/pe_fiq.c` 做 `-fsyntax-only` | ok（就是这个文件两次把编译搞炸） |
| `tools/localbuild/local-kernel-check.sh` | 同上，但可指定任意内核源文件、可选 `--emit-obj` | 对 `pe_fiq.c` ok；其余文件报 `n/a` 并说明缺哪个头 |

`local-kernel-check.sh` 对缺 SDK 头的文件会**诚实报 n/a**，不是假绿也不是假红。

## 不能做的，以及为什么

| 文件 | 卡在哪 |
|---|---|
| `bsd/kern/bsd_init.c` | 需要 `TargetConditionals.h`（SDK）与 C11 atomics（`memory_order` 来自 SDK 的 `stdatomic.h` 链） |
| `iokit/bsddev/IOKitBSDInit.cpp` | 需要 `mach/clock_priv.h`、`mach/clock.h`（SDK） |
| `osfmk/arm64/sleh.c` | 需要 `mach_assert.h`、`TargetConditionals.h`，以及 **MIG 生成的** `mach/*_server.h`；MIG 是 macOS 专有工具 |

这些不是"补几个桩头文件"能解决的：补桩之后每缺一个 SDK 声明就再断一次，
最后测的是桩不是代码（今晚试过 3 个头文件的桩，链条立刻延伸到 MIG）。

## 完整构建的两条路，各自的真实成本

1. **macOS 机器**（Mac mini / 黑苹果 / macOS VM）：直接可用。CI 用的就是 `macos-14`
   + Xcode SDK，`tools/ci/build-xnu.sh` 原样能跑。
2. **Linux 上搭 Apple SDK 交叉编译**：需要一个真正的 Xcode SDK。
   `https://github.com/theos/sdks` 那种用户态 SDK **不够**——它没有
   `System/Library/Frameworks/Kernel.framework`，也没有 `usr/include/mach_assert.h`，
   正好是 `sleh.c` 和 IOKit 那层要的东西。此外 `xcrun`/`cctools`/`MIG` 都要凑齐。
   本次会话尝试到一半（`tools/localbuild/fetch-sdk.sh`、`try-build-xnu.sh`、
   `xcrun-shim.sh` 就是那次留下的骨架）没有走通，**没有证据说它可行**。

## 建议

- 日常：改动前跑 `check_series.py` + `syntax_check.sh`，把"打不上/脏字节/`pe_fiq.c` 语法错"这三类挡在推送前。
- 推送：**把多个改动捆成一批再推**。今晚实测 macOS runner 排队 0–10 分钟、构建 4–7 分钟，
  一批一推能把排队次数压到最少。
- 需要快速迭代某个 SDK 依赖的文件（`bsd_init.c`、`sleh.c`、`IOKitBSDInit.cpp`）时，
  一台 macOS 机器是唯一省时的办法；否则就接受 CI 的往返。
