# LiteCapture Build Guide

## Prerequisites

1. **Operating System**: Windows 10 (version 1903+) or Windows 11 (recommended).
2. **Compiler**: Visual Studio 2022 (Community, Professional, or Enterprise) or Visual Studio Build Tools 2022.
   - Workload: "Desktop development with C++"
   - Components required:
     * MSVC v143 - VS 2022 C++ x64/x86 build tools
     * Windows 10/11 SDK (version 10.0.19041.0 or later, e.g. 10.0.22621.0)
     * C++ CMake tools for Windows
3. **CMake**: Version 3.20 or newer.

## Build Instructions

### Option 1: Visual Studio 2022 GUI
1. Open Visual Studio 2022.
2. Select **File > Open > Folder...** and select the `LiteCapture` repository directory.
3. Visual Studio will detect `CMakeLists.txt` and `CMakePresets.json`.
4. Select configuration `Windows x64 Release` or `Windows x64 Debug` in the top toolbar.
5. Press **Ctrl+Shift+B** or select **Build > Build All**.
6. The executable will be produced in `build/vs-x64/bin/Release/LiteCapture.exe`.

### Option 2: Command Line (Developer PowerShell / Command Prompt)

Open **Developer PowerShell for VS 2022** or **x64 Native Tools Command Prompt for VS 2022**:

```powershell
# 1. Navigate to the project directory
cd "C:\path\to\LiteCapture"

# 2. Configure build with CMake (Visual Studio 2022 Generator)
cmake -B build -G "Visual Studio 17 2022" -A x64

# 3. Build Release configuration
cmake --build build --config Release --parallel

# 4. Run the executable
.\build\bin\Release\LiteCapture.exe
```

### Option 3: Command Line using Ninja

If you have Ninja installed:

```powershell
cmake -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ninja
.\build-ninja\bin\LiteCapture.exe
```
