# M6：UniXNU 已经在手机上跑起来了

状态：2026-10-29 22:0x。证据是固件自己写进 `flashinfo` 的日志
[logs/boot-log-2026-10-29-unixnu.txt](logs/boot-log-2026-10-29-unixnu.txt)（3448 字节，checksum ok）。

---

## 1. 「外部产物不存在」这个判断是错的

`docs/UNIXNU-BUILD-NOTES.md` 记的构建顺序是「内核 → 包 → 固件」，而内核那一步需要
macOS + Xcode，于是 M6 被记成「卡在外部依赖」。读源码之后发现这两件事是**解耦**的：

| 步骤 | 真正需要什么 | 状态 |
|---|---|---|
| `UniXNU.efi` + `UniXNUDxe.efi` + `UniXNURamDiskDxe.efi` | Linux + EDK2 | ✅ 已编出并上机 |
| `DarwinBoot.efi` | Linux + EDK2（自带 `DarwinBoot.dsc`） | ✅ 已编出 |
| `kernel.macho` | **macOS + Xcode** | ⬜ 唯一真正的外部依赖 |

`UniXNU/UniXNU` 的 `package/UniXNUPkg` 是普通 EDK2 包，只依赖 MdePkg
（含 `BaseFdtLib/libfdt`、`MipiSysTLib/mipisyst`），`tools/build_unixnu.sh` 一条命令重建。

> 坑：`make -C BaseTools/Source/C` 在这棵树上会因为 **brotli submodule 缺失**而失败
> （只有 `BrotliCompress` 用得到它）。非 FV 包只需要 `GenFw` 和 `GenSec`：
> `make -C $EDK2/BaseTools/Source/C -j$(nproc) GenFw GenSec`。

## 2. 上机结果

安装方式没有改一行固件 —— 用 M6 设计好的 payload 槽位（`flashinfo + 2 MiB`）：

```powershell
python tools\put_flash_payload.py Resources\UniXNU\UniXNU.efi -o out\payload.bin
adb push out\payload.bin /sdcard/payload.bin
adb shell su -c "dd if=/sdcard/payload.bin of=/dev/block/by-name/flashinfo bs=512 seek=4096 conv=notrunc"
```

刷 `boot-uefi-fdt.img` 之后，日志里出现（原文是被我们的 90 列控制台挤成一行的，
这里按 UniXNU 的 UI 语义还原）：

```
PayloadBoot: using the payload from flashinfo
UniXNU  (UniXNU 0.1.0)
        built-in configuration
        no boot entry found
        drivers 0 loaded / 0 failed
        kits 0    acpi 0
  > 1. Return to firmware  [action]
    2. Restart  [action]
    3. Power off  [action]
    4. Tools  [action]
  Up/Down: select    Enter: boot    1-9: boot entry    Esc: stop timer
  Booting Return to firmware in 10 s   ... 1 s
PayloadBoot: StartImage -> Success
```

读出来的四件事：

1. **`UniXNU 0.1.0` 真的执行了** —— 版本号、菜单、倒计时、按键提示都是它自己打的；
2. **`built-in configuration`** —— 它没有找到 `UniXNU.toml`，用了内建默认配置；
3. **`no boot entry found`** —— 没有任何启动项，因为没有内核、没有 payload 卷；
4. **`Return to firmware` 之后 `StartImage -> Success`** —— 它是**自己正常退出**的，
   不是崩溃。这顺带证明了我们这条 `LoadImage` + `StartImage` 路径对真实应用是通的。

## 3. 日志为什么能记下这些

`PayloadBootDxe` 现在刷两次日志：第一次在 payload 之前（payload 可能卡住，那次必须写），
第二次在 `StartImage` 返回之后。这一次两行都在，正好说明 UniXNU 返回了。
如果哪天它卡在菜单里不返回，日志会停在 `using the payload from flashinfo`——
那本身也是一个明确的信号。

## 4. 已知的呈现问题：80 列 vs 90 列

UniXNU 的 UI 假定的控制台宽度是 **80 列**，每行补齐到 80 个字符再输出，靠终端自动换行。
我们的 `DandelionConsoleDxe` 是 **90 列**（用满 720 px），所以永远不会在 80 处换行，
整屏就被挤成一行。

两个修法，各有代价：

| 方案 | 代价 |
|---|---|
| 把 `PcdConOutColumn` / `QueryMode` 改成 80 | UniXNU 排版正常；我们自己的日志每行少 10 个字符 |
| 保持 90，让 UniXNU 读 `QueryMode` 自适应 | 需要确认 UniXNU 是否真的读 `Mode` 再决定列数 |

不是 M6 的阻塞项，但一旦开始看 XNU 的输出就必须先解决——XNU 的控制台也是按列宽排版的。

## 5. 还差什么（按顺序）

1. ✅ `UniXNU.efi` 能跑（本文）；
2. ✅ `DarwinBoot.efi` 已编出（26,112 B，`tools/build_darwinboot.sh`）。坑：`DarwinBoot.inf` 依赖 `UniXNUPkg.dec`，`PACKAGES_PATH` 要带上 `UniXNU/package`；
3. ⬜ **给它一个启动项**：`UniXNU.toml` 里写一条 `boot entry`，指向 DarwinBoot + 内核；
4. ⬜ **payload 卷**：`UniXNURamDiskDxe` 可以从内嵌镜像做 RAM 盘，
   或者我们在 `flashinfo` 里建 FAT 卷（见 `docs/RESUME-1029.md` 的待办）；
5. ⬜ **`kernel.macho`**：唯一真正需要 macOS 的东西。

第 3、4 步可以先用一个**假内核**验证：DarwinBoot 收到控制权后会打
`DARWINBOOT: command line:` 并在顶部画一条**绿条**再跳转。只要看到绿条，
就说明「UniXNU → DarwinBoot → 跳转」整条链路是通的，内核可以最后再补。

---

## 6. 第二个里程碑：FAT 挂上了，UniXNU 读到了配置，DarwinBoot 起来了

上一节说 `no boot entry found` 是因为**没有卷可读**。这一轮把 1 MiB 的 FAT12 卷写进
`flashinfo` 开头（`tools/mk_unixnu_esp.sh`），里面放 `\EFI\UniXNU\` 四个文件，
于是同一次启动里三件事一起成立：

日志（`flashinfo+1 MiB`，[logs/boot-log-2026-10-29-fat-ok.txt](logs/boot-log-2026-10-29-fat-ok.txt)）：

```
INHERIT OK
BLOCK OK n=44
FAT OK                                    ← M4 最初的判据，第一次诚实地成立
PayloadBoot: device tree 127044 bytes published
PayloadBoot: firmware log published as an EFI configuration table
```

面板（[images/m6-unixnu-menu.png](images/m6-unixnu-menu.png)）：

```
        ... dandelion   (UniXNU 0.1.0)
        config: \EFI\UniXNU\UniXNU.toml          ← 我们的配置被读到了
      > 1. DarwinBoot (dandelion)  [efi]        ← 我们的启动项，3 秒后自动选中
        2. Return to firmware  [action]
        3. Restart  [action]
        ...
        "DarwinBoot (dandelion)" returned: Success   ← DarwinBoot 被加载、执行、返回
        drivers 0 loaded / 0 failed    kits 0    acpi 0
```

所以链路现在是：

```
固件 → DandelionEmmcDxe BlockIo → 44 个句柄 → 分区 → FAT（flashinfo 里的 1 MiB）
   → PayloadBootDxe 从 flashinfo 槽加载 UniXNU.efi
   → UniXNU 读到 \EFI\UniXNU\UniXNU.toml
   → 选中 DarwinBoot.efi → LoadImage/StartImage → 返回 Success
```

**M6 的「加载并启动 UniXNU」和「给它一个启动项」两步都成立。** 日志只有 686 字节
（第一次 flush），因为 UniXNU 起完 DarwinBoot 后回到自己的菜单等人按键，
`StartImage` 还没返回 —— 面板底部那条**橙色** payload 横条就是证据：
`PayloadBootDxe` 还停在「正在找 payload」的颜色里。

### 6.1 DarwinBoot 为什么是 returned 而不是跳转

`DarwinBoot.efi` 的职责是「接住参数 → 打 `DARWINBOOT: command line:` → 画绿条 →
关 MMU → 跳内核」。没有内核它就没有可跳的目标，于是正常返回。
所以**绿条不是失败**，它是「真的走到跳转那一刻」的标志，而下一次需要给它一个内核。

### 6.2 顺带暴露的呈现问题（已经写在 §4，现在更明显）

面板上图里标题行是 `...U dandelion` / `...nfig:` —— 左边被吃掉了。
原因还是 §4 那一条：UniXNU 按 **80 列**排版并补齐到 80，我们的控制台是 **90 列**，
于是它的行被拆到了下一行开头。菜单项本身（不带补齐）是正常的，所以这个问题
只影响 UniXNU 的标题/状态行。

修法二选一，必须在看 XNU 输出之前定：

- `PcdConOutColumn` / `QueryMode()` 改成 80 —— UniXNU 与 XNU 排版都对，我们日志每行少 10 字符；
- 保持 90，确认 UniXNU 是否读 `Mode->CursorColumn` / `QueryMode` 来决定列数。