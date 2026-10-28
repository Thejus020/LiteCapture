#include "app/App.hpp"

int WINAPI wWinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE /*hPrevInstance*/,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow
) {
    (void)lpCmdLine;
    LPWSTR cmdLine = GetCommandLineW();
    bool testCapture = (cmdLine && wcsstr(cmdLine, L"--test-capture") != nullptr);

    // Single-instance enforcement: activate existing instance if already running in background/tray
    HANDLE hMutex = nullptr;
    if (!testCapture) {
        hMutex = CreateMutexW(nullptr, TRUE, L"Local\\LiteCapture_SingleInstance_Mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existingWnd = FindWindowW(L"LiteCaptureMainWindowClass", nullptr);
            if (existingWnd) {
                ShowWindow(existingWnd, SW_SHOW);
                ShowWindow(existingWnd, SW_RESTORE);
                SetForegroundWindow(existingWnd);
            }
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
    }

    LiteCapture::App::App app(hInstance);

    if (!app.Initialize(nCmdShow, testCapture)) {
        MessageBoxW(
            nullptr,
            L"LiteCapture failed to initialize. Please check the logs in %LOCALAPPDATA%\\LiteCapture\\Logs.",
            L"LiteCapture — Initialization Error",
            MB_ICONERROR | MB_OK
        );
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return 1;
    }

    int exitCode = app.Run();

    if (hMutex) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
    }

    return exitCode;
}
