# M3：让 GOP 出现（当前唯一的阻塞）

## 结论先说

`SimpleFbDxe` **已经在构建里**了：

```
Platforms/Xiaomi/dandelionPkg/dandelion.dsc:41   USE_CUSTOM_DISPLAY_DRIVER = 0
Platforms/Xiaomi/dandelionPkg/Include/DXE.inc:42 !if $(USE_CUSTOM_DISPLAY_DRIVER) == 0
                                                   INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf
```

所以 M3 大概率**不是"要写一个 GOP 驱动"**，而是"`SimpleFbDxe` 找不到面板"。

## 证据

`dandelion.dsc` 只给了分辨率和色深，**没有给 framebuffer 基址**：

```
gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|720
gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|1600
gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32
```

而实测可用的面板参数是（`bootshim/fb.h`，每天都在用）：

| 项 | 值 |
|---|---|
| base | `0x7EC50000` |
| stride | `2944` 字节（736 像素 × 4） |
| 分辨率 | 720 × 1600 |
| 格式 | 32bpp，BGRA（`0x00RRGGBB` 直写可用） |

`SimpleFbDxe` 要么读一个 base PCD，要么按名字从 `MemoryMapLib` 里找
"Display OVL"/"Framebuffer" 一类的区域。两种可能：

1. **缺 base PCD** → 在 DSC 里补上（名字要去上游
   `SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf` 里确认，别猜）
2. **找不到内存区域** → 在 `MemoryMapLib.c` 里加一条 720×1600 的 framebuffer
   条目（`0x7EC50000`，`2944 * 1600 = 4710400` 字节）

## 验证方法（不用猜）

在 `PlatformProgressDxe` 里加三行诊断，直接把结果画在屏幕上：

```c
EFI_STATUS  St;
EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop = NULL;

St = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
DandelionDrawString (16, 152, EFI_ERROR (St) ? "GOP NONE" : "GOP FOUND", ...);
if (!EFI_ERROR (St)) {
  // Gop->Mode->Info->HorizontalResolution / VerticalResolution / PixelsPerScanLine
}
```

- `GOP NONE` → `SimpleFbDxe` 没有产出协议，问题在上面 1 或 2
- `GOP FOUND` 但 ConOut 仍不显示 → 是 `GraphicsConsoleDxe` 与 `ConSplitter` 的
  连接顺序问题（把 `PlatformProgressDxe` 放到 `GraphicsConsoleDxe` 之后即可）

## 现在可用的能力（不依赖 GOP）

`PlatformProgressDxe.c` 里的 `DandelionDrawString()` + `DandelionFont.h`
（8×16 点阵，从 `bootshim/font.h` 生成）在 SEC/PEI/DXE 都能直接用。
想在哪一级看状态，就把这两个文件复制过去调一行。

## 顺序

1. 加 GOP 诊断三行 → 刷机 → 看是 NONE 还是 FOUND
2. NONE：按 1/2 补配置；FOUND：调 DXE 顺序
3. GOP 通了 → ConOut 自然可用 → M4（`MsdcDxe` + FAT）→ M5（`UniXNU.efi`）

## Measured: GOP FOUND + GOP 720 OK (2026-10-28)

The panel IS behind a GOP and the resolution is right (720x1600). So M3 is
NOT a missing graphics driver: SimpleFbDxe works.

What failed is ordering: PlatformProgressDxe has Depex TRUE, so it runs
before GraphicsConsoleDxe has connected to the GOP handle. ConSplitter then
has no console child yet and OutputString goes nowhere.

Fix (next change): in PlatformProgressDxe, locate the GOP handle and call
gBS->ConnectController (GopHandle, NULL, NULL, TRUE) before writing to
ConOut, or move the driver after GraphicsConsoleDxe in the FVDXE list.



## Measured: the GOP is drawing into the wrong memory (same day, later)

On-screen dump from PlatformProgressDxe:

    MODE 0
    FB     7D9B0000      <- not our panel (0x7EC50000)
    STRIDE 02D0 (720)    <- real panel pitch is 02E0 (736)
    COLS 0050 ROWS 0019  <- 80x25 text console, GraphicsConsoleDxe never attached
    CUR  0000 0014

Root cause, from upstream SimpleFb.c
(Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c):

    LocateMemoryRegionByName ("Display Reserved", &FrameBufferRegion);
    LocateMemoryRegionByName ("Display_Reserved", &FrameBufferRegion);
    if (!FrameBufferRegion.Address) { ...fall back to an allocation... }

dandelion's memory map has no region with either name, so SimpleFbDxe
allocates a scratch buffer (0x7D9B0000) and reports it as the framebuffer.
Every pixel the UEFI console draws lands in DRAM.  That is why
"CONOUT REACHES PANEL" never appears and why ConOut falls back to an empty
80x25 mode: GraphicsConsoleDxe does attach, but its output goes nowhere
visible.

Second problem in the same file:

    Info->PixelsPerScanLine = Width;

i.e. it assumes pitch == width.  dandelion's panel pitch is 736 px
(2944 bytes), so even with the right base the console output would be sheared
by 16 px per row.

Fix plan (both small, neither is a driver port):

1. Add a memory region named "Display Reserved" at 0x7EC50000, size 4710400
   (736 * 1600 * 4) to the MT6765 MemoryMapLib that dandelion uses.
2. Patch SimpleFb.c through Resources/MuPatches (the CI already applies these
   with absolute paths) so PixelsPerScanLine is 736 instead of Width, e.g. via
   a new PcdFrameBufferStride in SiliciumPkg.dec + dandelion.dsc.

After both, ConOut will draw on the real panel and M3 is done.
