# 2026-10-27 交接（换机器 / 恢复开发）

> 读完这一份就能接着干，不需要翻聊天记录。
> 上一份是 `docs/HANDOVER-DAY2.md`（10-27 凌晨），本文件覆盖它之后的所有变化。

---

## 1. 一句话状态

**技术结论变了：原来的「boot shim + 纯开源 XNU」线路被判定走不通，主线转向 UniXNU/UEFI。**
切换依据见 §4。基础设施（eMMC 调试通道）已经建好并验证。

---

## 2. 今天完成的东西

| 提交 | 内容 |
|---|---|
| `33fe46a` | 内核 eMMC 日志通道（printf.c + arm_vm_init 钩子） |
| `b985c53` / `1021294` | shim 侧 MSDC 轮询 PIO 写日志（`bootshim/msdc.c`） |
| `509ba2c` | 接受全 0x00 空区 + 用备份 GPT 验证高 LBA 读取 |
| `4aa2737` | `docs/EMMC-LOG.md`：通道用法 |
| `46d6e22` | **修掉 `ICC_IGRPEN0_EL1` 非安全态 UNDEFINED**（内核卡死的真凶之一） |
| `363933d` | **修掉内核 MSDC 映射偏移 0x30000**（新线路的核心 bug） |
| `4de3bd7` | CI：push 默认构建 dandelion（不再依赖 token 触发） |
| `0178545` / `519ba2c` | GIC 逐条打点、`native-emmc-serial` 前置改动 |
| `9dfc736` | `docs/FINDINGS-2026-10-27.md`：死点与已排除项 |
| `ded9347` | 跳过 LOCKDOWN 项 + 调用者栈哨兵（实验） |
| `0479ab0` | 修正 `DEAD-ENDS.md` §8；新增 UEFI 路线图 |
| `490652c` | 按真实 vtable 重写 eMMC console 集成方案 |
| `3d5cd90` | **`patches/xnu/native-emmc-serial.patch`（CI 编译通过）** |

### 真机验证过的能力

- eMMC 持久日志：内核逐行写 `flashinfo + 2 MiB`，回 Android 用 `adb` 拉取解码
- 内核启动推进到 `kernel_startup_initialize_upto(LOCKDOWN)` **返回处**
  （即 `bsd_init()` 前最后一条语句）—— 目前卡在这里
- GIC 两安全态的全部寄存器实测值

---

## 3. 当前技术状态

### 内核死点

```
ulock: buckets ready
[step] phase store: before
[step] phase store: after      ← startup_phase = upto; 成功
（返回即死，后面的标记都不出现）
```

已排除：只读段、eMMC 通道静音（framebuffer 的 trace 停在同一点，照片确认）。
最强假设：**栈 / 返回地址被破坏**（加代码后死点会跟着后移）。

### eMMC 日志通道

- 写路径、日志格式、CMD12/整卡重初始化恢复、主机解码器：**全部真机验证**
- 已整理成 UniXNU 形态的补丁（`patches/xnu/native-emmc-serial.patch`），
  **在 CI 的 macOS 构建里编译通过**
- 运行时惰性：只有 ADT 里有 `emmc-log` 节点才注册

---

## 4. 为什么转线路（依据，不是判断）

`mouseos` 在同一类硬件上走过我们这条线并放弃：

| 他的仓库 | 自述 |
|---|---|
| `xnu_bootshim_benesse_a04br3` | “LK-to-XNU boot shim … **watchdog boot loop, OS boot incomplete**” |
| `xnu_kernel_benesse_a04br3` | 同上 |
| `xnu_device_benesse_a04br3` | 同上 |

这三个仓库现在只作为「MT8167 寄存器数值的参考资料」列在 `UniXNU-a04br3` 的 Credits 里。

他后来走通的线路（`UniXNU-a04br3` README）：

> The hardware **reaches the iPadOS home screen** with four CPUs, eMMC DMA and the
> CPU framebuffer renderer.

六层栈：`BootShim → UEFI(Mu-Silicium) → UniXNU.efi → DarwinBoot → XNU + 原版 kext → IPSW 用户态`。
**dandelion 缺的是第 2 层（UEFI）。**

### 但 MTK 手机这条路没有人走通过

`Status.md`（`mouseos/uefi-a04br3`）核对结果：

| 分组 | 设备 | 状态 |
|---|---|---|
| MT8167 | a04br3 平板 | ✅ 唯一成功的 MTK 案例（iPadOS 桌面） |
| P65/G80（MT6768/6769T） | Redmi 9（lancelot） | ❌ Inactive，仅 WinPE，**One Core only** |
| G99（MT6789） | POCO M5（stone） | ❌ Inactive，全 `?` |

**没有任何 MTK GICv3 手机被验证过能正常拿中断。**
`lancelot` 的「One Core only」与我们在 dandelion 上撞到的墙高度一致。

→ **R1（GIC 两安全态）是唯一可能让整个项目归零的风险，现在是主线。**

---

## 5. 换机器怎么恢复

### 5.1 本机需要带走的东西

代码全在 git，`git clone` 即可。**不在 git 里的**：

| 文件 | 大小 | 说明 |
|---|---|---|
| `out\boot_backup.img` | 64 MiB | 原厂 boot 分区，恢复 Android 的唯一手段 |
| `out\stock_boot.dtb` | 277 KiB | 从 `boot_backup.img` 提取（偏移 `0xF3B000`） |

校验值：

```
boot_backup.img  SHA256 98F5ED0863EC4CF3D07C5982169E92E3B2111BBD7672B72ABD4BFDBAE9CD1E4
stock_boot.dtb   SHA256 757F0318FF2F7166DBD010825B6753074D3BF4318E106E935BC7BBEEDFB8699C
```

### 5.2 可选的外部检出（都能重新 clone）

| 路径 | 来源 | 用途 |
|---|---|---|
| `D:\aios\unixnu` | `https://github.com/UniXNU/UniXNU.git` | 上游 UniXNU（10 MB） |
| `D:\aios\unixnu-m` | `https://github.com/mouseos/UniXNU.git` | fork，含未合并的 `native-*` 补丁 |
| `D:\aios\uefi-status.md` | `raw.githubusercontent.com/mouseos/uefi-a04br3/main/Status.md` | 设备支持状态表（230 KB） |

### 5.3 XNU 树

```powershell
cd D:\aios\xnu
git checkout -- .
git clean -fd
"C:\Program Files\Git\bin\bash.exe" -lc "cd /d/aios/dandelion-xnu && tools/ci/apply-patches.sh /d/aios/xnu && tools/ci/apply-board.sh /d/aios/xnu"
```

补丁数已从 26 增加到 **27**（多了 `native-emmc-serial.patch`）。

### 5.4 CI

push 到 `main` 且改到 `patches/**`、`pexpert/**`、`tools/ci/**`、`config/**`、
`iokit/**`、`.github/workflows/build-xnu.yml` 就会自动构建 dandelion。
手动触发见 `docs/HANDOVER-DAY2.md` §5。

> GitHub token 存在 Windows 凭据管理器里；重启后若 `Get-GHToken` 报
> `no stored github credential`，在自己的终端里跑一次
> `git ls-remote https://github.com/licg89/dandelion-xnu.git HEAD` 重新登录。

### 5.5 设备

- 手机当前是**原厂 Android**，可正常使用
- fastboot：`C:\Users\li\AppData\Local\Android\Sdk\platform-tools\fastboot.exe`
- 序列号 `QS55XCPRCA9LDA5T`
- `adb reboot bootloader` 可用；`fastboot boot` 不可用（MTK LK 限制）
- 内核挂住后**不会自己回 fastboot**，需要手动：长按电源 10 秒 → 音量下+电源

---

## 6. 下一步优先级

1. **R1：解开 GIC 两安全态**（唯一主线）。三条候选路径见
   `docs/UEFI-DANDELION-PLAN.md` §R1：
   - 确认中断在哪一层被关（EL3 / preloader / ATF）
   - patch LK（公开源码已确认 LK 写 `GICD_CTLR=0x16`，而我们实测是 `0x12`）
   - 换中断入口：`sysirq` / MTK GPT（`0x10017000`）走 SPI
2. 把 `patches/xnu/native-emmc-serial.patch` 提到 `mouseos/UniXNU`
   （他正是维护 `XNU_NS16550_UART` 那条 PR 线的人，补丁形状一致）
3. R1 有结论后再动 `MT6765Pkg`（从 `MT6768Pkg` 复制改，注意它是未验证脚手架）

---

## 7. 不要重复的坑

见 `docs/DEAD-ENDS.md`。今天新增/更正的两条：

- **§8 已更正**：UniXNU 确实能启动 iOS；但那条路径需要 UEFI，而且 MTK 手机尚未成功
- **`ICC_IGRPEN0_EL1` 在非安全态是 UNDEFINED**：读它会让内核静默卡死。
  两安全态的 GIC 上，Group 0 的所有系统寄存器都不要碰
