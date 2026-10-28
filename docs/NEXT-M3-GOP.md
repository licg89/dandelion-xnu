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
