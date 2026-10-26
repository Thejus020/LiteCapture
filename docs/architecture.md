# LiteCapture Architecture Specification

LiteCapture is designed as an ultra-lightweight, native Windows screen recorder specifically tuned for gaming on modest hardware (e.g., AMD Ryzen 5 mobile APU with integrated Radeon graphics, 8 GB RAM, 1366x768 display).

## 1. Architectural Principles

1. **Zero Heavyweight Web Runtimes**: No Electron, WebView2, or heavy GUI frameworks. The UI is built using clean native Win32 controls, Segoe UI, DWM immersive dark mode, and Common Controls v6.
2. **Decoupled Pipeline**: Capture, frame queuing, encoding, and UI operate on separate threads with bounded synchronization. The capture callback must never wait on disk I/O or encoder throughput.
3. **GPU-Centric Path**: Direct3D 11 textures captured via Windows Graphics Capture (`IDirect3DSurface`) stay on the GPU whenever possible and feed directly into Media Foundation Hardware MFT encoders via `IMFDXGIDeviceManager`.
4. **Bounded Queues & Drop Policy**: Under heavy game load, excess frames are dropped or skipped using a strict backpressure policy rather than growing system RAM.
5. **No Silent Failures**: Every HRESULT and Win32 error code is checked, logged, and surfaced to the user.

## 2. Component Diagram

```
+-------------------------------------------------------------+
|                        Win32 UI                             |
|  - DPI-Aware MainWindow (Segoe UI, Dark Theme)              |
|  - Profile & Source Selection                               |
|  - Real-time Timer Tick (Duration, FPS, Dropped Frames)     |
+-------------------------------------------------------------+
                              | Status Query & Commands
                              v
+-------------------------------------------------------------+
|                   Recording Controller                      |
|  - State Machine: Idle -> Initializing -> Recording -> ...  |
|  - Manages Lifecycles of Capture, Encoder, and Audio        |
+-------------------------------------------------------------+
           |                                       |
           v                                       v
+-----------------------+              +----------------------+
|  Graphics & Capture   |              |   MF Video Encoder   |
|  - D3D11 Device / MGR |              |  - Hardware MFT H.264|
|  - Windows Graphics   |              |  - Software Fallback |
|    Capture (WGC)      |              |  - IMFSinkWriter MP4 |
|  - Direct3D11Surface  |              +----------------------+
+-----------------------+                          ^
           |                                       |
           +----> [ Bounded GPU Texture Queue ] ---+
                       (Max 3-5 frames)
```

## 3. Profiles

- **Profile A (Low Overhead)**: 1280x720, 30 FPS, H.264, 4 Mbps.
- **Profile B (Smooth Gameplay)**: 1280x720, 60 FPS, H.264, 6 Mbps.
- **Profile C (Higher Detail)**: 1366x768, 30 FPS, H.264, 8 Mbps.
- **Custom Profile**: User-configurable resolution, FPS, and bitrate.
