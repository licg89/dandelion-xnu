# 下一次开工从这里开始

写完时间：2026-10-30 03:2x。手机已刷回 Android，仓库已推送。

上一轮的最新提交：`117f0b7`。

---

## 1. 已经确认的事实（不要再重新验证）

| 事实 | 证据 |
|---|---|
| 整条链走通了：LK → BootShim → UEFI → eMMC BlockIo → FAT → UniXNU → UniXNUDxe → DarwinBoot → 256 MiB 窗口 → ExitBootServices → 跳转 | [logs/boot-log-2026-10-30-handoff.txt](logs/boot-log-2026-10-30-handoff.txt) |
| 内核被放到 `pa 0x4D004000`，窗口 `0x10000000`，`CurrentEL 1`，`PARange 2 (40-bit)`，4 KiB granule | 同上 |
| `UniXNU.toml` 的 `args` 原样进了内核命令行 | 同上 |
| 固件把交接记录写对了，**内核全部前置条件满足** | [logs/boot-log-2026-10-30-emmc-handoff.txt](logs/boot-log-2026-10-30-emmc-handoff.txt) |

交接记录那一行的实际值（这是关键，别再猜）：

```
PayloadBoot: emmc handoff lba 122139919 sectors 512 div 26;
             msdc ver 0x20170426 (want 0x20170426) cfg 0x02210499 ckstb 1
```

`MSDC 版本寄存器 0x114 = 0x20170426` —— 和内核要求**逐位相同**。

## 2. 唯一的未解问题

**`flashinfo` 的 sector 30000 里没有 `DLXLOG01`。**

内核的 `dandelion_emmc_enable()` 在 `arm_vm_init` 里、`pmap_bootstrap` 之后才被调用。
它没写 → **XNU 在更早的地方就停了**（不是通道的问题，前置条件已经全满足）。

## 3. 下一轮的三个动作，按顺序

### 动作 1（最便宜）：读内核自己的面板探针

`patches/xnu/dandelion-early-trace.patch` 会在面板上画 240 px 宽的横条，
**x 从 480 到 720**：

| y | 颜色 | 含义 |
|---|---|---|
| 990 | 白 | 另一处标记 |
| 1000 | 蓝 | before the call |
| 1020 | 绿 | after the call |
| 1040 | 洋红 | mark returned |
| 1060 | 橙 | debug inherit ok |
| 1080 | **紫** | **startup→kprintf ok** |

这些 y 位置在 UniXNU 菜单文字下方、固件指示条（y 1092）上方，**应该是空白的**。

操作：刷 `artifacts/boot-uefi-redband.img`（或下次重建的最新镜像）→ 等横条变绿 →
**拍一张** → 只看 y 990~1080 这一段。

| 看到 | 结论 | 去动作 |
|---|---|---|
| 一条都没有 | 内核在 `arm_init` 之前就死了 | 2 |
| 有 1000/1020、没 1080 | 进了 `arm_init`，`kprintf` 还没活 | 2 |
| 有 1080 紫条 | `kprintf` 活了 | 3 |

### 动作 2：内核在早期就死了怎么办

`dandelion-early-trace.patch` 里 `arm_init.c` 的那个钩子在
`kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF)` 之前。它之前的东西包括
`start.s` 的 bootstrap pmap、`arm_vm_init` 的前半段。要查的对象：

- `osfmk/arm64/start.s` 的 `DANDELION_KEEP_IDENTITY_MAP` 段——它把 MSDC0 的
  2 MiB 块（物理 `0x11200000`，虚地址 `0x41030000`）装进 V=P 表；这个映射错了，
  后面所有早期写入都会踩空；
- `arm_vm_init.c` 里那句 `panic("Unsupported memory configuration %lx", mem_size)`
  的判据——我们的 `mem_size` 是 `0xC0000000`（3 GiB），要确认它落在允许集合里；
- ADT 的 `/chosen` 是否给了它要的东西（`dram-base` / `dram-size` / `memory-map`
  的 `Kernel-Dandelion` 段——这些我们都有）。

### 动作 3：`kprintf` 活了但通道没起怎么办

那就是 `dandelion_emmc_enable()` 里 `printf` 之后的分支。已知能过的检查：
`handoff` 三个字段、`0x114 == 0x20170426`、`CKSTB`。剩下可能失败的是
`dl_emmc_write_header()`（第一次 CMD24 写失败）—— 那时候日志区会是**全零但通道
"应该"起来了**，和现在的现象一样，所以需要用探针区分。

---

## 4. 工具（都用这次学到的教训加固过）

| 工具 | 用法 | 注意 |
|---|---|---|
| `tools/extract_log_block.py` | `--lba 30512` | **必须给 `--lba`**：分区里有两个合法块（30512 是新的、32512 是旧布局残留），它现在会列出来并要求选择 |
| `tools/extract_xnu_log.py` | `python tools/extract_xnu_log.py out/fi.img` | 解内核的 `DLXLOG01` header（version/boot/slots/text_bytes/uptime） |
| `tools/patch_darwinboot.py` | 由 `build_darwinboot.sh` 自动调用 | 让 DarwinBoot 的诊断同时进 ConOut |
| `tools/patch_adt_emmc_log.py` | 幂等 | ADT 的 `emmc-log` 节点 + flashinfo 布局 |
| `tools/patch_emmc_handoff.py` | 幂等 | 交接记录（`PayloadBootDxe`） |

flashinfo 布局（三处必须一致：`bootshim/adt.c`、`tools/mk_unixnu_esp.sh`、`PayloadBootDxe.c`）：

```
0      .. 30000   FAT 卷（UniXNU.efi / EFI/UniXNU/* / kernel.macho 14.8 MB）
30000  .. 30512   XNU 日志通道，256 KiB
30512  .. 30640   固件日志块（DDNLOG01）
```

## 5. 值得先做的一个小加固

**把旧日志块（sector 32512..32640）在第一次使用时清零。** 它现在仍然
checksum 有效，任何一个只看 "找到的第一个/最后一个块" 的工具都会拿错它 ——
今晚因此浪费了三轮刷机，并且得出过一个错误结论（见 M6-UNIXNU.md §13）。

## 6. 三个反复踩过的坑

1. **复位的屏幕不清** → 「停在 UEFI 面板」既可能是旧画面也可能是新固件卡住。
   判据要用**像素**（`PaintBar` 的颜色）、不能用日志（DEBUG 只在之后有 flush 才落盘）。
2. **日志块残留** → 见 §5。
3. **同一个仓库里已有能用的实现时不要再写一份** —— `FindFlashInfo` 就是这么错的，
   而 `FlushLogToFlash` 里那份一直是对的。

## 7. 参考实现（别忘了它）

`Dracen-W/UniXNU-merlinx`（Redmi Note 9，MT6768/MT6769Z）已经跑到
`initproc 和 launchd exec 完成`。它的 [docs/devices/merlinx.md](https://github.com/Dracen-W/UniXNU-merlinx/blob/merlinx-work/docs/devices/merlinx.md) 有完整的 build/启动条件，
对照表在 [MERLINX-COMPARISON.md](MERLINX-COMPARISON.md)。
**注意：它的 `payload` 步骤需要 board trust cache 和 platform identity，上游不分发。**
