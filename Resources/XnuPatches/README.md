# xnu 移植补丁（线 A，已冻结）

`D:\aios\xnu` 是 Apple 官方 `xnu-8796.141.3` 的一个 detached checkout，**没有可推送的
远端**（`origin` 指向 apple-oss-distributions/xnu）。2026-10-29 我们把里面的 dandelion
移植工作整理成一个提交：

- 分支：`dandelion-port`
- 提交：`5a023ec dandelion (MT6762G / Redmi 9A): XNU port work in progress`
- 内容：37 个文件（`config/MASTER.arm64.dandelion`、`DANDELION.h`、`dandelion_gic.h`、
  `DandelionPlatformExpert.cpp`、`start.s`、`arm_vm_init.c`、`pmap.c`、`serial_console.c` 等）

`0001-xnu-dandelion-port.patch` 就是这个提交的 `git format-patch` 输出，放在这里是为了：

1. 换电脑时**不用拷整个 8 GB 的 xnu 树**也能恢复这部分工作；
2. 让这份工作跟着 `dandelion-xnu` 一起出现在 GitHub 上（那个树自己推不上去）。

## 恢复方法

```powershell
git clone --depth 1 --branch xnu-8796.141.3 https://github.com/apple-oss-distributions/xnu.git xnu
cd xnu
git am ..\dandelion-xnu\Resources\XnuPatches\0001-xnu-dandelion-port.patch
```

## 注意

- **线 A 已冻结在 `sched_startup`**：这份代码只作为存档，不要在这里继续推进；
  当前活跃的是线 B（`dandelion-xnu`：LK → BootShim → UEFI → UniXNU），进展见
  [`docs/RESUME-1029.md`](../../docs/RESUME-1029.md)。
- 如果以后再改 `D:\aios\xnu`，记得重新生成这个 patch（`git -C D:\aios\xnu format-patch -1 --stdout`），
  否则新机器上恢复出来的还是旧版本。
