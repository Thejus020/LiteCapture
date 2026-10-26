#include "ui/MainWindow.hpp"
#include "app/RecordingController.hpp"
#include "audio/WasapiAudioCapture.hpp"
#include "core/Logger.hpp"
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <format>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace LiteCapture::UI {

constexpr const wchar_t* WINDOW_CLASS_NAME = L"LiteCaptureMainWindowClass";
constexpr const wchar_t* WINDOW_TITLE      = L"LiteCapture — Native Screen Recorder";

constexpr int ID_BTN_RECORD         = 1001;
constexpr int ID_BTN_STOP           = 1002;
constexpr int ID_BTN_BROWSE         = 1003;
constexpr int ID_CMB_SOURCE         = 1004;
constexpr int ID_CMB_PROFILE        = 1005;
constexpr int ID_CHK_SYSTEM_AUDIO   = 1006;
constexpr int ID_CHK_MICROPHONE     = 1007;
constexpr int ID_CMB_SPEAKER        = 1008;
constexpr int ID_CMB_MIC            = 1009;
constexpr int ID_CHK_MINIMIZE_TRAY  = 1010;
constexpr int ID_BTN_SPEAKER_VOL_DOWN = 1011;
constexpr int ID_BTN_SPEAKER_VOL_UP   = 1012;
constexpr int ID_SLD_SPEAKER_VOL      = 1013;
constexpr int ID_BTN_MIC_VOL_DOWN     = 1014;
constexpr int ID_BTN_MIC_VOL_UP       = 1015;
constexpr int ID_SLD_MIC_VOL          = 1016;
constexpr UINT_PTR TIMER_ID_UI_TICK = 2001;

constexpr int ID_TRAY_TOGGLE_RECORD = 3001;
constexpr int ID_TRAY_SHOW_WINDOW   = 3002;
constexpr int ID_TRAY_OPEN_FOLDER   = 3003;
constexpr int ID_TRAY_EXIT          = 3004;
constexpr UINT ID_TRAY_ICON_UID     = 9001;

static HICON CreateRecordingDotIcon(int size) {
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, size, size);
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);

    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmColor);

    // Dark grey background matching Windows taskbar/tray
    HBRUSH hbrBg = CreateSolidBrush(RGB(24, 24, 24));
    RECT rc{ 0, 0, size, size };
    FillRect(hdcMem, &rc, hbrBg);
    DeleteObject(hbrBg);

    // Antialiased glowing red recording circle in center
    HBRUSH hbrRed = CreateSolidBrush(RGB(245, 45, 45));
    HPEN hpenRed = CreatePen(PS_SOLID, 1, RGB(255, 120, 120));
    SelectObject(hdcMem, hbrRed);
    SelectObject(hdcMem, hpenRed);
    int pad = size / 5;
    Ellipse(hdcMem, pad, pad, size - pad, size - pad);
    DeleteObject(hbrRed);
    DeleteObject(hpenRed);

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;
    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return hIcon;
}

MainWindow::MainWindow(HINSTANCE hInstance, App::AppConfig& config)
    : m_hInstance(hInstance), m_config(config) {
    m_hBgBrush = CreateSolidBrush(RGB(28, 28, 28)); // Dark surface
    int cxSm = GetSystemMetrics(SM_CXSMICON);
    int cySm = GetSystemMetrics(SM_CYSMICON);
    m_hIconApp = static_cast<HICON>(LoadImageW(nullptr, IDI_APPLICATION, IMAGE_ICON, cxSm, cySm, LR_SHARED));
    if (!m_hIconApp) {
        m_hIconApp = LoadIconW(nullptr, IDI_APPLICATION);
    }
    m_hIconRecording = CreateRecordingDotIcon(cxSm);
}

MainWindow::~MainWindow() {
    RemoveTrayIcon();
    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);
    if (m_hBgBrush) DeleteObject(m_hBgBrush);
    if (m_hIconRecording) DestroyIcon(m_hIconRecording);
}

bool MainWindow::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = m_hBgBrush;
    wc.lpszClassName = WINDOW_CLASS_NAME;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            LC_LOG_ERROR(std::format(L"Failed to register window class. Error: {}", err));
            return false;
        }
    }

    // Comfortable 560x640 window for video profile, audio, volume controls, and tray settings
    int baseWidth = 560;
    int baseHeight = 640;

    m_hwnd = CreateWindowExW(
        0,
        WINDOW_CLASS_NAME,
        WINDOW_TITLE,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        baseWidth, baseHeight,
        nullptr, nullptr, m_hInstance, this
    );

    if (!m_hwnd) {
        LC_LOG_ERROR(std::format(L"Failed to create main window. Error: {}", GetLastError()));
        return false;
    }

    ApplyTheme();
    UpdateFonts();
    CreateControls();
    LayoutControls();
    PopulateSources();
    PopulateProfiles();
    PopulateAudioDevices();
    InitTrayIcon();

    LC_LOG_INFO(L"Main window created successfully.");
    return true;
}

void MainWindow::Show(int nCmdShow) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, nCmdShow);
        UpdateWindow(m_hwnd);
    }
}

void MainWindow::ApplyTheme() {
    // Enable Windows 11 / Windows 10 Dark Titlebar
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
}

void MainWindow::UpdateFonts() {
    if (m_hwnd) {
        // Query DPI
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            auto getDpiForWindow = reinterpret_cast<UINT(WINAPI*)(HWND)>(GetProcAddress(user32, "GetDpiForWindow"));
            if (getDpiForWindow) {
                m_dpi = getDpiForWindow(m_hwnd);
            }
        }
    }

    if (m_hFontRegular) DeleteObject(m_hFontRegular);
    if (m_hFontBold) DeleteObject(m_hFontBold);
    if (m_hFontHeader) DeleteObject(m_hFontHeader);

    int regularSize = -MulDiv(10, m_dpi, 72);
    int boldSize    = -MulDiv(10, m_dpi, 72);
    int headerSize  = -MulDiv(13, m_dpi, 72);

    m_hFontRegular = CreateFontW(regularSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontBold = CreateFontW(boldSize, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    m_hFontHeader = CreateFontW(headerSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void MainWindow::CreateControls() {
    auto createLabel = [&](const wchar_t* text, DWORD style = 0) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, m_hwnd, nullptr, m_hInstance, nullptr);
    };

    m_lblStatusHeader = createLabel(L"● Status: Idle — Ready to record");
    m_lblSource       = createLabel(L"Capture Source:");
    m_cmbSource       = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CMB_SOURCE)), m_hInstance, nullptr);

    m_lblProfile      = createLabel(L"Recording Profile:");
    m_cmbProfile      = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CMB_PROFILE)), m_hInstance, nullptr);

    m_lblOutputDir    = createLabel(L"Save Recordings To:");
    m_txtOutputDir    = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", m_config.outputDirectory.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY, 0, 0, 0, 0, m_hwnd, nullptr, m_hInstance, nullptr);
    m_btnBrowseDir    = CreateWindowExW(0, L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_BROWSE)), m_hInstance, nullptr);

    m_chkSystemAudio  = CreateWindowExW(0, L"BUTTON", L"Record System Audio",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHK_SYSTEM_AUDIO)), m_hInstance, nullptr);
    SendMessageW(m_chkSystemAudio, BM_SETCHECK, m_config.captureSystemAudio ? BST_CHECKED : BST_UNCHECKED, 0);

    m_cmbSpeaker      = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CMB_SPEAKER)), m_hInstance, nullptr);

    int sysVolPct = static_cast<int>(std::round(m_config.systemVolume * 100.0f));
    m_lblSpeakerVol   = createLabel(std::format(L"Speaker Vol: {}%", sysVolPct).c_str());
    m_btnSpeakerVolDown = CreateWindowExW(0, L"BUTTON", L"−", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_SPEAKER_VOL_DOWN)), m_hInstance, nullptr);
    m_btnSpeakerVolUp   = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_SPEAKER_VOL_UP)), m_hInstance, nullptr);
    m_sldSpeakerVol     = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SLD_SPEAKER_VOL)), m_hInstance, nullptr);
    SendMessageW(m_sldSpeakerVol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(m_sldSpeakerVol, TBM_SETPOS, TRUE, sysVolPct);
    SendMessageW(m_sldSpeakerVol, TBM_SETLINESIZE, 0, 5);
    SendMessageW(m_sldSpeakerVol, TBM_SETPAGESIZE, 0, 10);

    m_chkMicrophone   = CreateWindowExW(0, L"BUTTON", L"Record Microphone",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHK_MICROPHONE)), m_hInstance, nullptr);
    SendMessageW(m_chkMicrophone, BM_SETCHECK, m_config.captureMicrophone ? BST_CHECKED : BST_UNCHECKED, 0);

    m_cmbMic          = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CMB_MIC)), m_hInstance, nullptr);

    int micVolPct = static_cast<int>(std::round(m_config.micVolume * 100.0f));
    m_lblMicVol       = createLabel(std::format(L"Mic Vol: {}%", micVolPct).c_str());
    m_btnMicVolDown   = CreateWindowExW(0, L"BUTTON", L"−", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_MIC_VOL_DOWN)), m_hInstance, nullptr);
    m_btnMicVolUp     = CreateWindowExW(0, L"BUTTON", L"+", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_MIC_VOL_UP)), m_hInstance, nullptr);
    m_sldMicVol       = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SLD_MIC_VOL)), m_hInstance, nullptr);
    SendMessageW(m_sldMicVol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(m_sldMicVol, TBM_SETPOS, TRUE, micVolPct);
    SendMessageW(m_sldMicVol, TBM_SETLINESIZE, 0, 5);
    SendMessageW(m_sldMicVol, TBM_SETPAGESIZE, 0, 10);

    m_btnRecord       = CreateWindowExW(0, L"BUTTON", L"Start Recording (Ctrl+Shift+R)", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_RECORD)), m_hInstance, nullptr);
    m_btnStop         = CreateWindowExW(0, L"BUTTON", L"Stop Recording", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        0, 0, 0, 0, m_hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_BTN_STOP)), m_hInstance, nullptr);

    m_lblStats        = createLabel(L"Duration: 00:00:00   |   FPS: 0.0   |   Dropped: 0");
    m_lblEncoderInfo  = createLabel(L"Encoder: Awaiting initialization (MF H.264 + AAC)");
    m_lblGpuInfo      = createLabel(L"GPU: Detecting Direct3D 11 hardware...");

    m_chkMinimizeToTray = CreateWindowExW(0, L"BUTTON", L"Minimize to System Tray on close / minimize",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, m_hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CHK_MINIMIZE_TRAY)), m_hInstance, nullptr);
    SendMessageW(m_chkMinimizeToTray, BM_SETCHECK, m_config.minimizeToTrayOnClose ? BST_CHECKED : BST_UNCHECKED, 0);

    // Apply fonts
    SendMessageW(m_lblStatusHeader,   WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontHeader), TRUE);
    SendMessageW(m_lblSource,         WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_cmbSource,         WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_lblProfile,        WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_cmbProfile,        WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_lblOutputDir,      WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_txtOutputDir,      WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_btnBrowseDir,      WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_chkSystemAudio,    WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_cmbSpeaker,        WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_lblSpeakerVol,     WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_btnSpeakerVolDown, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_btnSpeakerVolUp,   WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_chkMicrophone,     WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_cmbMic,            WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_lblMicVol,         WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_btnMicVolDown,     WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_btnMicVolUp,       WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_btnRecord,         WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_btnStop,           WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontBold), TRUE);
    SendMessageW(m_lblStats,          WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_lblEncoderInfo,    WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_lblGpuInfo,        WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
    SendMessageW(m_chkMinimizeToTray, WM_SETFONT, reinterpret_cast<WPARAM>(m_hFontRegular), TRUE);
}

void MainWindow::LayoutControls() {
    auto scale = [this](int px) { return MulDiv(px, m_dpi, 96); };

    int margin = scale(20);
    int labelHeight = scale(18);
    int controlHeight = scale(28);
    int currentY = scale(16);

    RECT clientRect;
    GetClientRect(m_hwnd, &clientRect);
    int clientWidth = clientRect.right - clientRect.left;
    int contentWidth = clientWidth - (margin * 2);

    // Header Status
    SetWindowPos(m_lblStatusHeader, nullptr, margin, currentY, contentWidth, scale(24), SWP_NOZORDER);
    currentY += scale(34);

    // Source selection
    SetWindowPos(m_lblSource, nullptr, margin, currentY, contentWidth, labelHeight, SWP_NOZORDER);
    currentY += labelHeight + scale(4);
    SetWindowPos(m_cmbSource, nullptr, margin, currentY, contentWidth, scale(160), SWP_NOZORDER);
    currentY += controlHeight + scale(12);

    // Profile selection
    SetWindowPos(m_lblProfile, nullptr, margin, currentY, contentWidth, labelHeight, SWP_NOZORDER);
    currentY += labelHeight + scale(4);
    SetWindowPos(m_cmbProfile, nullptr, margin, currentY, contentWidth, scale(160), SWP_NOZORDER);
    currentY += controlHeight + scale(12);

    // Output Directory
    SetWindowPos(m_lblOutputDir, nullptr, margin, currentY, contentWidth, labelHeight, SWP_NOZORDER);
    currentY += labelHeight + scale(4);
    int browseBtnWidth = scale(90);
    int txtDirWidth = contentWidth - browseBtnWidth - scale(8);
    SetWindowPos(m_txtOutputDir, nullptr, margin, currentY, txtDirWidth, controlHeight, SWP_NOZORDER);
    SetWindowPos(m_btnBrowseDir, nullptr, margin + txtDirWidth + scale(8), currentY, browseBtnWidth, controlHeight, SWP_NOZORDER);
    currentY += controlHeight + scale(12);

    // Audio configuration: two-column layout (System Audio left, Microphone right)
    int colGap = scale(14);
    int colWidth = (contentWidth - colGap) / 2;
    int chkHeight = scale(22);

    // Row 1: Audio Source Checkboxes
    SetWindowPos(m_chkSystemAudio, nullptr, margin, currentY, colWidth, chkHeight, SWP_NOZORDER);
    SetWindowPos(m_chkMicrophone, nullptr, margin + colWidth + colGap, currentY, colWidth, chkHeight, SWP_NOZORDER);
    currentY += chkHeight + scale(4);

    // Row 2: Device Dropdowns
    SetWindowPos(m_cmbSpeaker, nullptr, margin, currentY, colWidth, scale(180), SWP_NOZORDER);
    SetWindowPos(m_cmbMic, nullptr, margin + colWidth + colGap, currentY, colWidth, scale(180), SWP_NOZORDER);
    currentY += controlHeight + scale(10);

    // Row 3: Volume Controls (Labels, [-] [+] buttons, and Trackbar sliders)
    int rightColX = margin + colWidth + colGap;
    int btnVolWidth = scale(26);
    int btnVolHeight = scale(22);
    int lblVolWidth = colWidth - (btnVolWidth * 2) - scale(4);

    // Sub-row 3A: Volume Label and [-] [+] buttons
    SetWindowPos(m_lblSpeakerVol, nullptr, margin, currentY + scale(2), lblVolWidth, btnVolHeight, SWP_NOZORDER);
    SetWindowPos(m_btnSpeakerVolDown, nullptr, margin + lblVolWidth, currentY, btnVolWidth, btnVolHeight, SWP_NOZORDER);
    SetWindowPos(m_btnSpeakerVolUp, nullptr, margin + lblVolWidth + btnVolWidth + scale(4), currentY, btnVolWidth, btnVolHeight, SWP_NOZORDER);

    SetWindowPos(m_lblMicVol, nullptr, rightColX, currentY + scale(2), lblVolWidth, btnVolHeight, SWP_NOZORDER);
    SetWindowPos(m_btnMicVolDown, nullptr, rightColX + lblVolWidth, currentY, btnVolWidth, btnVolHeight, SWP_NOZORDER);
    SetWindowPos(m_btnMicVolUp, nullptr, rightColX + lblVolWidth + btnVolWidth + scale(4), currentY, btnVolWidth, btnVolHeight, SWP_NOZORDER);
    currentY += btnVolHeight + scale(4);

    // Sub-row 3B: Trackbar Sliders
    int sliderHeight = scale(24);
    SetWindowPos(m_sldSpeakerVol, nullptr, margin, currentY, colWidth, sliderHeight, SWP_NOZORDER);
    SetWindowPos(m_sldMicVol, nullptr, rightColX, currentY, colWidth, sliderHeight, SWP_NOZORDER);
    currentY += sliderHeight + scale(14);

    // Action buttons (Record & Stop)
    int btnWidth = (contentWidth - scale(12)) / 2;
    int btnHeight = scale(38);
    SetWindowPos(m_btnRecord, nullptr, margin, currentY, btnWidth, btnHeight, SWP_NOZORDER);
    SetWindowPos(m_btnStop, nullptr, margin + btnWidth + scale(12), currentY, btnWidth, btnHeight, SWP_NOZORDER);
    currentY += btnHeight + scale(16);

    // Stats & Encoder Labels
    SetWindowPos(m_lblStats, nullptr, margin, currentY, contentWidth, labelHeight, SWP_NOZORDER);
    currentY += labelHeight + scale(4);
    SetWindowPos(m_lblEncoderInfo, nullptr, margin, currentY, contentWidth, labelHeight, SWP_NOZORDER);
    currentY += labelHeight + scale(4);
    SetWindowPos(m_lblGpuInfo, nullptr, margin, currentY, contentWidth, labelHeight, SWP_NOZORDER);
    currentY += labelHeight + scale(10);

    // Tray preference
    SetWindowPos(m_chkMinimizeToTray, nullptr, margin, currentY, contentWidth, chkHeight, SWP_NOZORDER);
}

void MainWindow::SetGpuInfo(const std::wstring& gpuInfo) {
    if (m_lblGpuInfo) {
        SetWindowTextW(m_lblGpuInfo, gpuInfo.c_str());
    }
}

void MainWindow::SetDisplaySources(const std::vector<Graphics::DisplayOutputInfo>& displays) {
    m_displays = displays;
    SendMessageW(m_cmbSource, CB_RESETCONTENT, 0, 0);
    if (displays.empty()) {
        SendMessageW(m_cmbSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Display 1: Primary Monitor (Full Screen)"));
    } else {
        for (const auto& disp : displays) {
            std::wstring item = disp.GetDisplayName();
            SendMessageW(m_cmbSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }
    }
    SendMessageW(m_cmbSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Active Application Window (Auto-detected)"));
    SendMessageW(m_cmbSource, CB_SETCURSEL, m_config.captureSourceIndex, 0);
}

void MainWindow::PopulateSources() {
    SendMessageW(m_cmbSource, CB_RESETCONTENT, 0, 0);
    SendMessageW(m_cmbSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Display 1: Primary Monitor (Full Screen)"));
    SendMessageW(m_cmbSource, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Active Application Window (Auto-detected)"));
    SendMessageW(m_cmbSource, CB_SETCURSEL, m_config.captureSourceIndex, 0);
}

void MainWindow::PopulateProfiles() {
    SendMessageW(m_cmbProfile, CB_RESETCONTENT, 0, 0);

    auto pA = App::AppConfig::GetPresetProfile(App::ProfilePreset::LowOverhead);
    auto pB = App::AppConfig::GetPresetProfile(App::ProfilePreset::SmoothGameplay);
    auto pC = App::AppConfig::GetPresetProfile(App::ProfilePreset::HigherDetail);

    SendMessageW(m_cmbProfile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pA.name.c_str()));
    SendMessageW(m_cmbProfile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pB.name.c_str()));
    SendMessageW(m_cmbProfile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(pC.name.c_str()));

    SendMessageW(m_cmbProfile, CB_SETCURSEL, static_cast<int>(m_config.profilePreset), 0);
}

void MainWindow::OnProfileChanged() {
    int curSel = static_cast<int>(SendMessageW(m_cmbProfile, CB_GETCURSEL, 0, 0));
    if (curSel >= 0 && curSel <= 2) {
        m_config.profilePreset = static_cast<App::ProfilePreset>(curSel);
        m_config.activeProfile = App::AppConfig::GetPresetProfile(m_config.profilePreset);
        m_config.Save();
        LC_LOG_INFO(std::format(L"Profile selected: {}", m_config.activeProfile.name));
    }
}

void MainWindow::PopulateAudioDevices() {
    m_playbackDevices = Audio::WasapiAudioCapture::EnumeratePlaybackDevices();
    m_recordingDevices = Audio::WasapiAudioCapture::EnumerateRecordingDevices();

    // Populate Playback Devices ComboBox
    SendMessageW(m_cmbSpeaker, CB_RESETCONTENT, 0, 0);
    int selSpeakerIdx = 0;
    for (size_t i = 0; i < m_playbackDevices.size(); ++i) {
        SendMessageW(m_cmbSpeaker, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m_playbackDevices[i].name.c_str()));
        if (!m_config.systemAudioDeviceId.empty() && m_playbackDevices[i].id == m_config.systemAudioDeviceId) {
            selSpeakerIdx = static_cast<int>(i);
        }
    }
    SendMessageW(m_cmbSpeaker, CB_SETCURSEL, selSpeakerIdx, 0);

    // Populate Recording Devices ComboBox
    SendMessageW(m_cmbMic, CB_RESETCONTENT, 0, 0);
    int selMicIdx = 0;
    for (size_t i = 0; i < m_recordingDevices.size(); ++i) {
        SendMessageW(m_cmbMic, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m_recordingDevices[i].name.c_str()));
        if (!m_config.micDeviceId.empty() && m_recordingDevices[i].id == m_config.micDeviceId) {
            selMicIdx = static_cast<int>(i);
        }
    }
    SendMessageW(m_cmbMic, CB_SETCURSEL, selMicIdx, 0);

    EnableWindow(m_cmbSpeaker, m_config.captureSystemAudio ? TRUE : FALSE);
    EnableWindow(m_btnSpeakerVolDown, m_config.captureSystemAudio ? TRUE : FALSE);
    EnableWindow(m_btnSpeakerVolUp, m_config.captureSystemAudio ? TRUE : FALSE);
    EnableWindow(m_sldSpeakerVol, m_config.captureSystemAudio ? TRUE : FALSE);

    EnableWindow(m_cmbMic, m_config.captureMicrophone ? TRUE : FALSE);
    EnableWindow(m_btnMicVolDown, m_config.captureMicrophone ? TRUE : FALSE);
    EnableWindow(m_btnMicVolUp, m_config.captureMicrophone ? TRUE : FALSE);
    EnableWindow(m_sldMicVol, m_config.captureMicrophone ? TRUE : FALSE);

    LC_LOG_INFO(std::format(L"Audio devices enumerated: {} playback devices, {} recording devices.",
        m_playbackDevices.size(), m_recordingDevices.size()));
    for (size_t i = 0; i < m_playbackDevices.size(); ++i) {
        LC_LOG_INFO(std::format(L"  Playback [{}]: '{}' (ID: {})", i, m_playbackDevices[i].name, m_playbackDevices[i].id.empty() ? L"[Default]" : m_playbackDevices[i].id));
    }
    for (size_t i = 0; i < m_recordingDevices.size(); ++i) {
        LC_LOG_INFO(std::format(L"  Recording [{}]: '{}' (ID: {})", i, m_recordingDevices[i].name, m_recordingDevices[i].id.empty() ? L"[Default]" : m_recordingDevices[i].id));
    }
}

void MainWindow::OnSpeakerDeviceChanged() {
    int curSel = static_cast<int>(SendMessageW(m_cmbSpeaker, CB_GETCURSEL, 0, 0));
    if (curSel >= 0 && curSel < static_cast<int>(m_playbackDevices.size())) {
        m_config.systemAudioDeviceId = m_playbackDevices[curSel].id;
        m_config.Save();
        LC_LOG_INFO(std::format(L"Selected playback device: {} (id: {})",
            m_playbackDevices[curSel].name,
            m_config.systemAudioDeviceId.empty() ? L"Default" : m_config.systemAudioDeviceId));
    }
}

void MainWindow::OnMicDeviceChanged() {
    int curSel = static_cast<int>(SendMessageW(m_cmbMic, CB_GETCURSEL, 0, 0));
    if (curSel >= 0 && curSel < static_cast<int>(m_recordingDevices.size())) {
        m_config.micDeviceId = m_recordingDevices[curSel].id;
        m_config.Save();
        LC_LOG_INFO(std::format(L"Selected microphone: {} (id: {})",
            m_recordingDevices[curSel].name,
            m_config.micDeviceId.empty() ? L"Default" : m_config.micDeviceId));
    }
}

void MainWindow::OnSpeakerVolumeChanged(int pos) {
    pos = std::clamp(pos, 0, 200);
    SendMessageW(m_sldSpeakerVol, TBM_SETPOS, TRUE, pos);
    m_config.systemVolume = static_cast<float>(pos) / 100.0f;
    m_config.Save();
    if (m_lblSpeakerVol) {
        SetWindowTextW(m_lblSpeakerVol, std::format(L"Speaker Vol: {}%", pos).c_str());
    }
    if (m_controller) {
        m_controller->SetAudioVolume(m_config.systemVolume, m_config.micVolume);
    }
    LC_LOG_INFO(std::format(L"Speaker volume updated: {}%", pos));
}

void MainWindow::OnMicVolumeChanged(int pos) {
    pos = std::clamp(pos, 0, 200);
    SendMessageW(m_sldMicVol, TBM_SETPOS, TRUE, pos);
    m_config.micVolume = static_cast<float>(pos) / 100.0f;
    m_config.Save();
    if (m_lblMicVol) {
        SetWindowTextW(m_lblMicVol, std::format(L"Mic Vol: {}%", pos).c_str());
    }
    if (m_controller) {
        m_controller->SetAudioVolume(m_config.systemVolume, m_config.micVolume);
    }
    LC_LOG_INFO(std::format(L"Mic volume updated: {}%", pos));
}

void MainWindow::AdjustSpeakerVolume(int delta) {
    int curPos = static_cast<int>(SendMessageW(m_sldSpeakerVol, TBM_GETPOS, 0, 0));
    OnSpeakerVolumeChanged(curPos + delta);
}

void MainWindow::AdjustMicVolume(int delta) {
    int curPos = static_cast<int>(SendMessageW(m_sldMicVol, TBM_GETPOS, 0, 0));
    OnMicVolumeChanged(curPos + delta);
}

void MainWindow::OnAudioCheckboxChanged() {
    m_config.captureSystemAudio = (SendMessageW(m_chkSystemAudio, BM_GETCHECK, 0, 0) == BST_CHECKED);
    m_config.captureMicrophone = (SendMessageW(m_chkMicrophone, BM_GETCHECK, 0, 0) == BST_CHECKED);

    EnableWindow(m_cmbSpeaker, m_config.captureSystemAudio ? TRUE : FALSE);
    EnableWindow(m_btnSpeakerVolDown, m_config.captureSystemAudio ? TRUE : FALSE);
    EnableWindow(m_btnSpeakerVolUp, m_config.captureSystemAudio ? TRUE : FALSE);
    EnableWindow(m_sldSpeakerVol, m_config.captureSystemAudio ? TRUE : FALSE);

    EnableWindow(m_cmbMic, m_config.captureMicrophone ? TRUE : FALSE);
    EnableWindow(m_btnMicVolDown, m_config.captureMicrophone ? TRUE : FALSE);
    EnableWindow(m_btnMicVolUp, m_config.captureMicrophone ? TRUE : FALSE);
    EnableWindow(m_sldMicVol, m_config.captureMicrophone ? TRUE : FALSE);

    m_config.Save();
    LC_LOG_INFO(std::format(L"Audio configuration updated: SystemAudio={}, Microphone={}",
        m_config.captureSystemAudio ? L"Enabled" : L"Disabled",
        m_config.captureMicrophone ? L"Enabled" : L"Disabled"));
}

void MainWindow::OnBrowseOutputDir() {
    IFileOpenDialog* pFileOpen = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileOpen));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions = 0;
        pFileOpen->GetOptions(&dwOptions);
        pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pFileOpen->SetTitle(L"Select Output Directory for LiteCapture Recordings");

        hr = pFileOpen->Show(m_hwnd);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = nullptr;
            hr = pFileOpen->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath = nullptr;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr)) {
                    m_config.outputDirectory = pszFilePath;
                    m_config.Save();
                    SetWindowTextW(m_txtOutputDir, pszFilePath);
                    LC_LOG_INFO(std::format(L"Output directory updated to: {}", pszFilePath));
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }
}

void MainWindow::OnMinimizeToTrayChanged() {
    m_config.minimizeToTrayOnClose = (SendMessageW(m_chkMinimizeToTray, BM_GETCHECK, 0, 0) == BST_CHECKED);
    m_config.Save();
    LC_LOG_INFO(std::format(L"Minimize to tray setting updated: {}", m_config.minimizeToTrayOnClose ? L"Enabled" : L"Disabled"));
}

void MainWindow::InitTrayIcon() {
    ZeroMemory(&m_nid, sizeof(NOTIFYICONDATAW));
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = ID_TRAY_ICON_UID;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_APP_TRAY_NOTIFICATION;
    m_nid.hIcon = m_hIconApp;
    wcscpy_s(m_nid.szTip, L"LiteCapture — Ready to record");

    // Clean up any stale icon before registering
    Shell_NotifyIconW(NIM_DELETE, &m_nid);

    if (Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        m_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
        m_trayIconCreated = true;
        LC_LOG_INFO(L"System tray icon initialized.");
    } else {
        DWORD err = GetLastError();
        LC_LOG_WARN(std::format(L"System tray icon not available (desktop taskbar not present or non-interactive session). Error={}", err));
    }
}

void MainWindow::RemoveTrayIcon() {
    if (m_trayIconCreated) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_trayIconCreated = false;
        LC_LOG_INFO(L"System tray icon removed.");
    }
}

void MainWindow::UpdateTrayIcon(bool isRecording) {
    if (!m_trayIconCreated) return;
    m_nid.uFlags = NIF_ICON | NIF_TIP;
    m_nid.hIcon = isRecording ? m_hIconRecording : m_hIconApp;
    if (isRecording) {
        wcscpy_s(m_nid.szTip, L"LiteCapture — 🔴 Recording in progress (Ctrl+Shift+R)");
    } else {
        wcscpy_s(m_nid.szTip, L"LiteCapture — Idle (Ctrl+Shift+R to record)");
    }
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void MainWindow::ShowTrayNotification(const std::wstring& title, const std::wstring& message, DWORD infoFlags) {
    if (!m_trayIconCreated) return;
    m_nid.uFlags = NIF_INFO;
    m_nid.dwInfoFlags = infoFlags;
    wcsncpy_s(m_nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(m_nid.szInfo, message.c_str(), _TRUNCATE);
    m_nid.uTimeout = 3000;
    Shell_NotifyIconW(NIM_MODIFY, &m_nid);
}

void MainWindow::ShowTrayContextMenu() {
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    if (m_isRecording) {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_RECORD, L"⏹ Stop Recording (Ctrl+Shift+R)");
    } else {
        AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE_RECORD, L"⏺ Start Recording (Ctrl+Shift+R)");
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    bool isVisible = IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_WINDOW, isVisible ? L"Hide Window" : L"Show Window");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_OPEN_FOLDER, L"Open Recordings Folder");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit LiteCapture");

    SetForegroundWindow(m_hwnd);
    TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

void MainWindow::MinimizeToTray() {
    ShowWindow(m_hwnd, SW_HIDE);
    static bool s_shownTrayTip = false;
    if (!s_shownTrayTip) {
        ShowTrayNotification(L"LiteCapture", L"Running in background. Press Ctrl+Shift+R anytime to record, or click the tray icon.", NIIF_INFO);
        s_shownTrayTip = true;
    }
}

void MainWindow::RestoreFromTray() {
    ShowWindow(m_hwnd, SW_SHOW);
    ShowWindow(m_hwnd, SW_RESTORE);
    SetForegroundWindow(m_hwnd);
}

void MainWindow::OnToggleRecording() {
    LC_LOG_INFO(std::format(L"=== OnToggleRecording ENTERED (isRecording={}) ===", m_isRecording));
    if (!m_isRecording) {
        Capture::CaptureSource src{};
        int curSel = static_cast<int>(SendMessageW(m_cmbSource, CB_GETCURSEL, 0, 0));
        if (curSel >= 0 && curSel < static_cast<int>(m_displays.size())) {
            src.type = Capture::CaptureSourceType::Monitor;
            src.hMonitor = m_displays[curSel].hMonitor;
            src.name = m_displays[curSel].deviceName;
            src.width = m_displays[curSel].width;
            src.height = m_displays[curSel].height;
        } else {
            HMONITOR hMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
            src.type = Capture::CaptureSourceType::Monitor;
            src.hMonitor = hMon;
            src.name = L"Primary Monitor";
        }

        if (m_controller) {
            auto status = m_controller->StartRecording(m_config, src);
            if (status.IsError()) {
                MessageBoxW(m_hwnd, status.Message().c_str(), L"LiteCapture — Capture Error", MB_ICONERROR | MB_OK);
                return;
            }
        }

        // Transition to Recording state
        m_isRecording = true;
        m_recordDurationSec = 0;
        EnableWindow(m_btnRecord, FALSE);
        EnableWindow(m_btnStop, TRUE);
        EnableWindow(m_cmbSource, FALSE);
        EnableWindow(m_cmbProfile, FALSE);
        EnableWindow(m_btnBrowseDir, FALSE);
        EnableWindow(m_chkSystemAudio, FALSE);
        EnableWindow(m_cmbSpeaker, FALSE);
        EnableWindow(m_chkMicrophone, FALSE);
        EnableWindow(m_cmbMic, FALSE);

        SetWindowTextW(m_lblStatusHeader, L"🔴 Status: Recording in progress (WGC Live)...");
        SetTimer(m_hwnd, TIMER_ID_UI_TICK, 1000, nullptr);
        UpdateTrayIcon(true);
        ShowTrayNotification(L"LiteCapture — Recording Started", L"Capturing video and audio. Press Ctrl+Shift+R to stop.", NIIF_INFO);
        LC_LOG_INFO(L"Recording started by user.");
    } else {
        if (m_controller) {
            m_controller->StopRecording();
        }

        // Transition to Idle state
        m_isRecording = false;
        KillTimer(m_hwnd, TIMER_ID_UI_TICK);
        EnableWindow(m_btnRecord, TRUE);
        EnableWindow(m_btnStop, FALSE);
        EnableWindow(m_cmbSource, TRUE);
        EnableWindow(m_cmbProfile, TRUE);
        EnableWindow(m_btnBrowseDir, TRUE);
        EnableWindow(m_chkSystemAudio, TRUE);
        EnableWindow(m_cmbSpeaker, m_config.captureSystemAudio ? TRUE : FALSE);
        EnableWindow(m_chkMicrophone, TRUE);
        EnableWindow(m_cmbMic, m_config.captureMicrophone ? TRUE : FALSE);

        SetWindowTextW(m_lblStatusHeader, L"● Status: Idle — Ready to record");
        UpdateTrayIcon(false);
        ShowTrayNotification(L"LiteCapture — Recording Saved", std::format(L"Saved recording (duration: {}s). Click to open folder.", m_recordDurationSec), NIIF_INFO);
        LC_LOG_INFO(std::format(L"Recording stopped by user. Total duration: {}s", m_recordDurationSec));
    }
}

void MainWindow::SetStatusText(const std::wstring& status) {
    if (m_lblStatusHeader) {
        SetWindowTextW(m_lblStatusHeader, status.c_str());
    }
}

void MainWindow::UpdateRecordingStats(uint64_t durationSec, double fps, uint64_t droppedFrames, const std::wstring& encoderName) {
    uint64_t hours = durationSec / 3600;
    uint64_t mins  = (durationSec % 3600) / 60;
    uint64_t secs  = durationSec % 60;

    std::wstring statsStr = std::format(L"Duration: {:02d}:{:02d}:{:02d}   |   FPS: {:.1f}   |   Dropped: {}",
        hours, mins, secs, fps, droppedFrames);
    SetWindowTextW(m_lblStats, statsStr.c_str());

    if (!encoderName.empty()) {
        std::wstring encStr = std::format(L"Encoder: {}", encoderName);
        SetWindowTextW(m_lblEncoderInfo, encStr.c_str());
    }
}

LRESULT CALLBACK MainWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            int wmEvent = HIWORD(wParam);
            LC_LOG_INFO(std::format(L"WM_COMMAND received: id={}, event={}", wmId, wmEvent));
            if (wmId == ID_BTN_RECORD && wmEvent == BN_CLICKED) {
                OnToggleRecording();
                return 0;
            } else if (wmId == ID_BTN_STOP && wmEvent == BN_CLICKED) {
                OnToggleRecording();
                return 0;
            } else if (wmId == ID_BTN_BROWSE && wmEvent == BN_CLICKED) {
                OnBrowseOutputDir();
                return 0;
            } else if (wmId == ID_CMB_PROFILE && wmEvent == CBN_SELCHANGE) {
                OnProfileChanged();
                return 0;
            } else if (wmId == ID_CHK_SYSTEM_AUDIO || wmId == ID_CHK_MICROPHONE) {
                OnAudioCheckboxChanged();
                return 0;
            } else if (wmId == ID_CMB_SPEAKER && wmEvent == CBN_SELCHANGE) {
                OnSpeakerDeviceChanged();
                return 0;
            } else if (wmId == ID_CMB_MIC && wmEvent == CBN_SELCHANGE) {
                OnMicDeviceChanged();
                return 0;
            } else if (wmId == ID_BTN_SPEAKER_VOL_DOWN) {
                AdjustSpeakerVolume(-10);
                return 0;
            } else if (wmId == ID_BTN_SPEAKER_VOL_UP) {
                AdjustSpeakerVolume(+10);
                return 0;
            } else if (wmId == ID_BTN_MIC_VOL_DOWN) {
                AdjustMicVolume(-10);
                return 0;
            } else if (wmId == ID_BTN_MIC_VOL_UP) {
                AdjustMicVolume(+10);
                return 0;
            } else if (wmId == ID_CHK_MINIMIZE_TRAY) {
                OnMinimizeToTrayChanged();
                return 0;
            } else if (wmId == ID_TRAY_TOGGLE_RECORD) {
                OnToggleRecording();
                return 0;
            } else if (wmId == ID_TRAY_SHOW_WINDOW) {
                if (IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd)) {
                    ShowWindow(m_hwnd, SW_HIDE);
                } else {
                    RestoreFromTray();
                }
                return 0;
            } else if (wmId == ID_TRAY_OPEN_FOLDER) {
                ShellExecuteW(nullptr, L"open", m_config.outputDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            } else if (wmId == ID_TRAY_EXIT) {
                if (m_isRecording) {
                    OnToggleRecording();
                }
                DestroyWindow(m_hwnd);
                return 0;
            }
            break;
        }

        case WM_HSCROLL: {
            HWND hScroll = reinterpret_cast<HWND>(lParam);
            if (hScroll == m_sldSpeakerVol) {
                int pos = static_cast<int>(SendMessageW(m_sldSpeakerVol, TBM_GETPOS, 0, 0));
                OnSpeakerVolumeChanged(pos);
                return 0;
            } else if (hScroll == m_sldMicVol) {
                int pos = static_cast<int>(SendMessageW(m_sldMicVol, TBM_GETPOS, 0, 0));
                OnMicVolumeChanged(pos);
                return 0;
            }
            break;
        }

        case WM_CLOSE: {
            if (m_isRecording) {
                int res = MessageBoxW(m_hwnd, L"Recording is currently active. Do you want to stop and exit LiteCapture?",
                    L"LiteCapture — Confirm Exit", MB_ICONQUESTION | MB_YESNO);
                if (res != IDYES) {
                    return 0;
                }
                OnToggleRecording();
            }
            if (m_config.minimizeToTrayOnClose) {
                MinimizeToTray();
                return 0;
            }
            DestroyWindow(m_hwnd);
            return 0;
        }

        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_MINIMIZE && m_config.minimizeToTrayOnClose) {
                MinimizeToTray();
                return 0;
            }
            break;
        }

        case WM_APP_TRAY_NOTIFICATION: {
            UINT notificationEvent = LOWORD(lParam);
            if (notificationEvent == WM_LBUTTONUP || notificationEvent == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(m_hwnd) && !IsIconic(m_hwnd)) {
                    ShowWindow(m_hwnd, SW_HIDE);
                } else {
                    RestoreFromTray();
                }
                return 0;
            } else if (notificationEvent == WM_RBUTTONUP || notificationEvent == WM_CONTEXTMENU) {
                ShowTrayContextMenu();
                return 0;
            } else if (notificationEvent == NIN_BALLOONUSERCLICK) {
                ShellExecuteW(nullptr, L"open", m_config.outputDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            break;
        }

        case WM_TIMER: {
            if (wParam == TIMER_ID_UI_TICK && m_isRecording) {
                m_recordDurationSec++;
                double fps = 0.0;
                uint64_t dropped = 0;
                if (m_controller) {
                    auto stats = m_controller->GetStats();
                    fps = stats.captureFps;
                    dropped = stats.framesDropped;
                }
                UpdateRecordingStats(m_recordDurationSec, fps, dropped, L"Windows Graphics Capture (Live)");
                return 0;
            }
            break;
        }

        case WM_DPICHANGED: {
            m_dpi = HIWORD(wParam);
            UpdateFonts();
            auto prcNew = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(m_hwnd, nullptr, prcNew->left, prcNew->top,
                prcNew->right - prcNew->left, prcNew->bottom - prcNew->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutControls();
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND hStatic = reinterpret_cast<HWND>(lParam);

            // Dark UI theme text colors
            SetBkColor(hdc, RGB(28, 28, 28));
            if (hStatic == m_lblStatusHeader) {
                SetTextColor(hdc, m_isRecording ? RGB(255, 90, 90) : RGB(100, 220, 100));
            } else if (hStatic == m_lblStats || hStatic == m_lblEncoderInfo || hStatic == m_lblGpuInfo) {
                SetTextColor(hdc, RGB(180, 180, 180));
            } else if (hStatic == m_lblSpeakerVol || hStatic == m_lblMicVol) {
                SetTextColor(hdc, RGB(210, 210, 210));
            } else {
                SetTextColor(hdc, RGB(230, 230, 230));
            }
            return reinterpret_cast<LRESULT>(m_hBgBrush);
        }

        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

} // namespace LiteCapture::UI
