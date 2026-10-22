@echo off
REM Dandelion Shim Auto-Runner
REM Flashes shim, waits for auto-reboot to fastboot, restores boot, reads log

set SHIM_IMG=%~dp0out\dandelion-v3.img
set BOOT_DUMP=%~dp0..\out\boot_dump.img

echo === Dandelion Shim Auto-Runner ===
echo.

REM Check device in fastboot
fastboot devices | findstr QS55XCPRCA9LDA5T >nul
if errorlevel 1 (
    echo ERROR: Device not in fastboot mode!
    echo Please boot phone into fastboot (volume down + power)
    pause
    exit /b 1
)

REM Flash shim
echo [1/4] Flashing shim...
fastboot -s QS55XCPRCA9LDA5T flash boot "%SHIM_IMG%"
if errorlevel 1 (
    echo ERROR: Flash failed!
    pause
    exit /b 1
)

REM Reboot to shim
echo [2/4] Rebooting to shim...
fastboot -s QS55XCPRCA9LDA5T reboot
echo.
echo === Watch the phone screen! ===
echo === Shim will auto-reboot to fastboot in ~10 seconds ===
echo.

REM Wait for device to come back to fastboot
echo [3/4] Waiting for shim to finish and reboot to fastboot...
:wait_fastboot
timeout /t 3 /nobreak >nul
fastboot -s QS55XCPRCA9LDA5T devices >nul 2>&1
if errorlevel 1 (
    echo   Still waiting...
    goto wait_fastboot
)
echo   Device in fastboot!

REM Restore original boot
echo [4/4] Restoring original boot...
fastboot -s QS55XCPRCA9LDA5T flash boot "%BOOT_DUMP%"
fastboot -s QS55XCPRCA9LDA5T reboot
echo.

REM Wait for Android to boot
echo Waiting for Android to boot...
:wait_android
timeout /t 5 /nobreak >nul
adb -s QS55XCPRCA9LDA5T get-state >nul 2>&1
if errorlevel 1 (
    echo   Still booting...
    goto wait_android
)
echo   Android ready!

REM Read pstore log
echo.
echo === Reading shim log from pstore ===
echo.
adb -s QS55XCPRCA9LDA5T shell "su -c 'cat /sys/fs/pstore/console-ramoops-0'" 2>nul > %~dp0shim_log.txt

if exist %~dp0shim_log.txt (
    for %%F in (%~dp0shim_log.txt) do if %%~zF GTR 0 (
        echo === SHIM LOG ===
        type %~dp0shim_log.txt
        echo.
        echo === Log saved to shim_log.txt ===
    ) else (
        echo No log found in pstore.
        echo The shim might not have written to pstore correctly.
    )
) else (
    echo Failed to read pstore log.
)

echo.
pause
