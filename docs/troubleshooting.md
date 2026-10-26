# LiteCapture Troubleshooting Guide

## Common Issues & Resolutions

### 1. CMake says "No CMAKE_CXX_COMPILER could be found"
- **Cause**: MSVC compiler tools are not installed or not in environment.
- **Fix**: Launch the Visual Studio Installer, click **Modify**, select **Desktop development with C++**, and ensure **MSVC v143** and **Windows 10/11 SDK** are checked. Alternatively, run commands from **x64 Native Tools Command Prompt for VS 2022**.

### 2. CMake reports "LiteCapture requires 64-bit architecture (x64)"
- **Cause**: 32-bit (x86) generator or toolchain was selected.
- **Fix**: When configuring CMake, specify `-A x64` or select an x64 preset.

### 3. Log File Location
- All application events, warnings, HRESULT failures, and diagnostic logs are automatically written to:
  `%LOCALAPPDATA%\LiteCapture\Logs\litecapture_YYYYMMDD_HHMMSS.log`
- Settings are saved to:
  `%LOCALAPPDATA%\LiteCapture\config.ini`

### 4. Global Hotkey (Ctrl + Shift + R) Not Triggering
- **Cause**: Another background app (e.g., Radeon Software, GeForce Experience, Discord, OBS) may have already claimed the shortcut.
- **Fix**: Check `litecapture_*.log` for "Failed to register global hotkey". The hotkey can be customized in `%LOCALAPPDATA%\LiteCapture\config.ini` under `[Hotkeys]`.
