@echo off
REM Read dandelion shim log from pstore after reboot
REM Usage: read_log.bat

echo === Dandelion Shim Log Reader ===
echo.

REM Wait for device
adb wait-for-device
echo Device connected.
echo.

REM Try pstore first
echo Reading pstore...
adb shell "su -c 'cat /sys/fs/pstore/console-ramoops-0 2>nul'" > shim_log.txt 2>&1

if exist shim_log.txt (
    for %%F in (shim_log.txt) do if %%~zF GTR 0 (
        echo === Log found ===
        type shim_log.txt
        echo.
        echo === Saved to shim_log.txt ===
        goto :done
    )
)

echo No log in pstore. Trying raw memory read...
echo.

REM Try reading pstore area directly via /proc/iomem
adb shell "su -c 'dd if=/proc/iomem bs=1 skip=1204330496 count=4096 2>nul | xxd'" > shim_log_raw.txt 2>&1

echo Check shim_log_raw.txt for raw memory dump.

:done
echo.
pause
