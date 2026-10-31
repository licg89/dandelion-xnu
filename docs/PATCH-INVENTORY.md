# 补丁清单：哪些是"移植需要的"，哪些是"调试用的"

写于 2026-10-31 20:0x。当前 `patches/xnu/` 共 52 条，其中一部分是**bring-up 期间加的
诊断/开关**，将来移植稳定后应当撤掉，否则会把调试代码当生产代码留着。

## 一、移植必需的（留）

| 补丁 | 作用 |
|---|---|
| `ci-*.patch`（3 条） | CI 编译环境（4 KiB 页、SDK root、arm64 配置、availability） |
| `dandelion-machine.patch` | 板级 machine 配置接线 |
| `dandelion-gic-mode.patch` | pexpert 的 GICv3 bring-up（boot shim 侧 distributor/redistributor） |
| `zz-dandelion-cntp-timer.patch` | 定时器改用 CNTP/PPI 30（本平台非安全态拥有的是物理定时器） |
| `zzz2-dandelion-mockfs-size.patch` | mockfs：`kfree_type` 类型修正 + file node 的 `si_devsize` |
| `zzz5-dandelion-writer-lock.patch` | console/日志写入器加中断保护（**必须留**：tick 一起来就会踩坏） |
| `zzz6-ci-clang-auto-var-init.patch` | 去掉 clang 已删除的 `-enable-trivial-auto-var-init-zero-...` |
| `zzz7-uio-stackbuf-alignment.patch` | clang 17 的 `uio_stackbuf_t` 数组对齐错误 |
| `zzz8-driverkit-iig-kernelcontent.patch` | clang 17 的 DriverKit `IORPC::kernelContent` |
| `zzz9-dandelion-setrootmedia.patch` | 没有 IOStorageFamily kext 时 `setRootMedia()` 不 panic |
| `zzz10-dandelion-secure-bsd-root.patch` | 没有 platform expert 时 secure-root 校验按"不支持"处理 |
| `zzz11-dandelion-corecrypto-sha2.patch` | 无加密 kext 时用内核自带 corecrypto 的 SHA-256 |
| `native-emmc-serial.patch` | eMMC 日志通道（**唯一的日志手段**，留） |
| 其余 `non-apple-*.patch` | 开源树在本平台编译/运行所需（PPL、trustcache、closed-source、tunables 等） |

## 二、调试专用（移植稳定后撤）

| 补丁 | 作用 | 撤掉的条件 |
|---|---|---|
| `xnu-early-probe-ladder.patch` | 早期启动的 DANDELION_TRACE 阶梯 | 能稳定启动后 |
| `xnu-probe-markers.patch` | 面板探针条 | 同上 |
| `xnu-trace-attach.patch` | trace ring 的 attach（当前是空操作） | 恒等映射去掉后需要重写，不是简单撤 |
| `xnu-vmmap-trace.patch` | `[dl] suballoc`/`[dl] del` 打印 | 内存问题解决后 |
| `dandelion-early-trace.patch` | printf.c 的 trace/framebuffer/eMMC 输出基础设施 | **留**（日志通道本体） |
| `zzz-dandelion-timer-probe.patch` | GIC 三个实验 + CNTPCT 忙等 + 根设备等待有界化 | 用户态起来后 |
| `zzz3-dandelion-irq-entry.patch` | IRQ 入口标记 + 风暴熔断 | tick 可信后 |
| `zzz4-dandelion-rtclock-markers.patch` | rtclock_intr 内部标记 | 同上 |
| `zzz12-dandelion-exec-markers.patch` | exec 路径标记 | exec 通后 |
| `zzz16-dandelion-bsdinit-task.patch` | bsdinit_task 六步标记（**XNUPOST 跳过要留**） | 标记撤，跳过留 |
| `zzz17-dandelion-alias-probe.patch` | `AT S1E1R` 属性对比（假设已证伪） | 可立即撤 |
| `zzz18-dandelion-exec-steps.patch` | execve 内部四步标记 | exec 通后 |
| `zzz19-dandelion-mockfs-lookup.patch` | mockfs lookup/getattr 标记 | mockfs 可信后 |
| `zzz20-dandelion-zalloc-report.patch` | UAF 只记录不 panic | **UAF 修好后必须撤** |
| `zzz21-dandelion-skip-pm.patch` | 跳过 `PMInstantiatePowerDomains()`（实验，已证明不是破坏源） | **现在就可以撤** |

## 三、撤的顺序建议

1. 先撤 `zzz21`（已证伪的实验）和 `zzz17`（已证伪的探针）——纯噪声。
2. UAF 定位并修好后，撤 `zzz20`（它现在把真实 bug 变成了"只记录"，撤掉才能让检查重新生效）。
3. 用户态能稳定跑起来后，撤 `zzz12/zzz16(标记部分)/zzz18/zzz19/zzz3/zzz4/zzz-dandelion-timer-probe`。

撤任何一条之后都要跑 `py -3.13 tools/ci/check_series.py`，并注意补丁是按文件名顺序应用的：
`zzz2` < `zzz10` < ... < `zzz21` 的字典序和数字序**不一致**（`zzz10` 排在 `zzz2` 前面），
这也是为什么 `zzz14` 当初应用失败、改名 `zzz15` 才对。
