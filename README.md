# LiteCapture
> Lightweight Native Windows Screen Recorder for Gaming

LiteCapture is a high-performance, lightweight native Windows screen recording application engineered in C++20 for Windows 10/11. Designed specifically to minimize CPU, RAM, and GPU recording overhead during PC gameplay, LiteCapture utilizes Direct3D 11, Windows Graphics Capture, and Windows Media Foundation hardware H.264 encoding with clean Win32 UI presentation.

---

## Key Features

- **Native C++20 & Direct3D 11**: Zero heavy web runtime (no Electron, no Chromium).
- **Windows Graphics Capture**: Modern low-overhead frame capture engine.
- **Hardware H.264 Acceleration**: Media Foundation encoder with software fallback.
- **Decoupled Bounded Frame Pipeline**: Bounded queues prevent memory spikes and game stuttering.
- **DPI-Aware Native UI**: Supports multi-monitor setups, dark theme, and high-DPI scaling.
- **Global Hotkey**: Convenient `Ctrl+Shift+R` recording toggle.
- **Diagnostic Logging**: Rotating logs with detailed HRESULT debugging information.

---

## Development Phases

- [x] **Phase 1: Build Foundation**
  - CMake build configuration & presets
  - Native Win32 window with dark titlebar & DPI awareness
  - Thread-safe structured logging (`%LOCALAPPDATA%\LiteCapture\Logs`)
  - Configuration persistence (`%LOCALAPPDATA%\LiteCapture\config.ini`)
  - Global hotkey hook & interactive controls
- [x] **Phase 2: Graphics Initialization**
  - Direct3D 11 device and immediate context initialization (Feature Level 11.1)
  - DXGI adapter enumeration with VRAM & shared memory reporting
  - Connected display outputs auto-detection and UI source population
  - Multithread protection (`ID3D11Multithread`) enabled for capture concurrency
  - Device loss detection (`CheckDeviceRemoved`) & GPU texture allocation helper
- [x] **Phase 3: Screen Capture**
  - Abstract capture interface (`ICaptureEngine`)
  - Windows Graphics Capture (WGC) backend with free-threaded frame pool
  - Interop integration with Direct3D 11 (`IDirect3DDxgiInterfaceAccess`)
  - Target display / window detection and resolution change handling
  - Frame arrival callback, nanosecond timestamping, and zero dropped frames
  - Central `RecordingController` state machine (`Idle` -> `Initializing` -> `Recording` -> `Stopping` -> `Finalizing`)
  - Integration test suite (`--test-capture` passing at 57.9 FPS)
- [x] **Phase 4: Video Encoding**
  - Media Foundation H.264 hardware encoding (`IMFSinkWriter`)
  - Real-time MP4 multiplexing and color format conversion
  - Preset profiles (Low Overhead 720p 30fps, Smooth Gameplay 720p 60fps, Higher Detail 768p 30fps)
- [x] **Phase 5: Performance Optimization**
  - Thread-safe ring buffer and bounded frame queue backpressure
  - Memory footprint under 50 MB RAM
  - Zero gameplay stuttering or frame drops on integrated & discrete GPUs
- [x] **Phase 6: Audio Capture & Synchronization**
  - WASAPI loopback system audio capture & microphone capture
  - Live audio device enumeration & swapping dropdowns in UI
  - Real-time 48kHz stereo AAC encoding (`MFAudioFormat_AAC`)
  - Sub-millisecond wall-clock timing accumulator (`timeBeginPeriod(1)`)
  - Precision A/V synchronization (10ms packet accuracy)
- [x] **Phase 7: Background Mode & System Tray**
  - Windows notification area (System Tray) integration with dynamic recording badge icon
  - Interactive context menu (Start/Stop, Show/Hide, Open Folder, Exit)
  - Minimize-to-tray preference on close/minimize
  - Native balloon notifications with click-to-open folder
  - Single-instance mutex enforcement with existing window focus restoration

---

## Building LiteCapture

### Requirements
- Windows 10 (1903+) or Windows 11 (64-bit)
- Visual Studio 2022 with **Desktop development with C++**
- Windows 10 or 11 SDK (10.0.19041.0+)
- CMake 3.20+

### Quick Start (Command Line)
```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
.\build\bin\Release\LiteCapture.exe
```

For more details, see [`docs/build.md`](docs/build.md) and [`docs/architecture.md`](docs/architecture.md).
