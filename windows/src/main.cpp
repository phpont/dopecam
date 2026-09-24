#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "MediaPipeline.h"
#include "NetworkClient.h"
#include "Protocol.h"
#include "RtpReceiver.h"

#pragma comment(linker, \
    "\"/manifestdependency:type='win32' " \
    "name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' " \
    "processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' " \
    "language='*'\"")

namespace {
constexpr wchar_t kWindowClass[] = L"DopeCamWindow";
constexpr UINT WM_DOPECAM_FATAL = WM_APP + 47;
constexpr UINT WM_DOPECAM_ZOOM_FAILED = WM_APP + 48;
constexpr UINT_PTR kZoomTimerId = 1001;
constexpr UINT kZoomIntervalMs = 33;
constexpr int kZoomTrackMin = 0;
constexpr int kZoomTrackMax = 1000;
constexpr int kSidebarWidthDip = 324;
constexpr int kWindowMinWidthDip = 920;
constexpr int kWindowMinHeightDip = 640;
constexpr int kAppIconResourceId = 101;
constexpr int kBrandIconResourceId = 102;

constexpr COLORREF kPanelColor = RGB(246, 247, 249);
constexpr COLORREF kPreviewColor = RGB(0, 0, 0);
constexpr COLORREF kTextPrimary = RGB(27, 31, 36);
constexpr COLORREF kTextSecondary = RGB(100, 108, 119);
constexpr COLORREF kTextMuted = RGB(127, 136, 148);
constexpr COLORREF kStateReady = RGB(31, 122, 77);
constexpr COLORREF kStateStreaming = RGB(15, 138, 75);
constexpr COLORREF kStateError = RGB(180, 35, 24);
constexpr COLORREF kPrimaryButton = RGB(28, 32, 38);
constexpr COLORREF kPrimaryButtonPressed = RGB(15, 18, 22);
constexpr COLORREF kPrimaryButtonDisabled = RGB(205, 210, 217);

enum class StateTone {
    Muted,
    Ready,
    Streaming,
    Error
};

enum ControlId : int {
    ID_DISCOVER = 100,
    ID_CONNECT,
    ID_IP,
    ID_CAMERA,
    ID_PRESET,
    ID_START_STOP,
    ID_ZOOM,
    ID_ZOOM_VALUE,
    ID_ROTATE_LEFT,
    ID_ROTATE_RIGHT,
    ID_MIRROR_H,
    ID_MIRROR_V,
    ID_STATUS,
    ID_PREVIEW,
    ID_BRAND_ICON,
    ID_TITLE,
    ID_SUBTITLE,
    ID_STATE,
    ID_CONNECTION_LABEL,
    ID_CAMERA_LABEL,
    ID_QUALITY_LABEL,
    ID_ZOOM_LABEL,
    ID_TRANSFORM_LABEL
};

HWND gMain = nullptr;
HWND gBrandIcon = nullptr;
HWND gTitle = nullptr;
HWND gSubtitle = nullptr;
HWND gState = nullptr;
HWND gConnectionLabel = nullptr;
HWND gCameraLabel = nullptr;
HWND gQualityLabel = nullptr;
HWND gZoomLabel = nullptr;
HWND gTransformLabel = nullptr;
HWND gIp = nullptr;
HWND gCamera = nullptr;
HWND gPreset = nullptr;
HWND gStartStop = nullptr;
HWND gZoom = nullptr;
HWND gZoomValue = nullptr;
HWND gStatus = nullptr;
HWND gPreview = nullptr;
HWND gDiscover = nullptr;
HWND gConnect = nullptr;

NetworkClient gNetwork;
RtpReceiver gReceiver;
std::unique_ptr<MediaPipeline> gPipeline;
std::vector<CameraChoice> gChoices;
std::uint16_t gControlPort = dopecam::protocol::kControlPort;
bool gStreaming = false;
UINT gDpi = 96;
StateTone gStateTone = StateTone::Muted;

HFONT gFontBody = nullptr;
HFONT gFontSemibold = nullptr;
HFONT gFontSmall = nullptr;
HFONT gFontTitle = nullptr;
HBRUSH gPanelBrush = nullptr;
HBRUSH gPreviewBrush = nullptr;

PTP_WORK gZoomWork = nullptr;
std::atomic<float> gPendingZoom{1.0f};
std::atomic<float> gZoomWorkValue{1.0f};
std::atomic<bool> gHasPendingZoom{false};
std::atomic<bool> gZoomWorkInFlight{false};
std::atomic<bool> gZoomShuttingDown{false};
bool gZoomTimerArmed = false;
std::mutex gZoomErrorMutex;
std::string gZoomError;

int Dip(int value) {
    return MulDiv(value, static_cast<int>(gDpi), 96);
}

HFONT CreateUiFont(int points, int weight) {
    LOGFONTW font{};
    font.lfHeight = -MulDiv(points, static_cast<int>(gDpi), 72);
    font.lfWeight = weight;
    font.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&font);
}

void DestroyFonts() {
    if (gFontBody) DeleteObject(gFontBody);
    if (gFontSemibold) DeleteObject(gFontSemibold);
    if (gFontSmall) DeleteObject(gFontSmall);
    if (gFontTitle) DeleteObject(gFontTitle);
    gFontBody = nullptr;
    gFontSemibold = nullptr;
    gFontSmall = nullptr;
    gFontTitle = nullptr;
}

void CreateFonts() {
    DestroyFonts();
    gFontBody = CreateUiFont(10, FW_NORMAL);
    gFontSemibold = CreateUiFont(10, 600);
    gFontSmall = CreateUiFont(9, FW_NORMAL);
    gFontTitle = CreateUiFont(18, 600);
}

void SetControlFont(HWND hwnd, HFONT font) {
    if (hwnd && font) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void ApplyFonts() {
    SetControlFont(gTitle, gFontTitle);
    SetControlFont(gSubtitle, gFontSmall);
    SetControlFont(gState, gFontSemibold);
    SetControlFont(gConnectionLabel, gFontSemibold);
    SetControlFont(gCameraLabel, gFontSemibold);
    SetControlFont(gQualityLabel, gFontSemibold);
    SetControlFont(gZoomLabel, gFontSemibold);
    SetControlFont(gTransformLabel, gFontSemibold);
    SetControlFont(gIp, gFontBody);
    SetControlFont(gCamera, gFontBody);
    SetControlFont(gPreset, gFontBody);
    SetControlFont(gZoom, gFontBody);
    SetControlFont(gZoomValue, gFontSemibold);
    SetControlFont(gDiscover, gFontBody);
    SetControlFont(gConnect, gFontBody);
    SetControlFont(GetDlgItem(gMain, ID_ROTATE_LEFT), gFontBody);
    SetControlFont(GetDlgItem(gMain, ID_ROTATE_RIGHT), gFontBody);
    SetControlFont(GetDlgItem(gMain, ID_MIRROR_H), gFontBody);
    SetControlFont(GetDlgItem(gMain, ID_MIRROR_V), gFontBody);
    SetControlFont(gStartStop, gFontSemibold);
    SetControlFont(gStatus, gFontSmall);
}

void ApplyBrandIcon() {
    if (!gBrandIcon) {
        return;
    }

    HICON icon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(kBrandIconResourceId),
            IMAGE_ICON,
            Dip(32),
            Dip(32),
            LR_DEFAULTCOLOR | LR_SHARED));

    if (icon) {
        SendMessageW(gBrandIcon, STM_SETICON, reinterpret_cast<WPARAM>(icon), 0);
    }
}

void SetState(const std::wstring& text, StateTone tone) {
    gStateTone = tone;
    if (gState) {
        SetWindowTextW(gState, text.c_str());
        InvalidateRect(gState, nullptr, TRUE);
    }
}

void SetStatus(const std::wstring& text) {
    if (gStatus) {
        SetWindowTextW(gStatus, text.c_str());
    }
}

void ShowError(const std::string& error) {
    std::wstring wide = Utf8ToWide(error);
    SetState(L"\u25CF Error", StateTone::Error);
    SetStatus(wide);
    MessageBoxW(gMain, wide.c_str(), L"DopeCam", MB_OK | MB_ICONERROR);
}

std::wstring GetText(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(control, value.data(), length + 1);
    }
    value.resize(static_cast<std::size_t>(length));
    return value;
}

void AddControlFont(HWND hwnd) {
    SetControlFont(hwnd, gFontBody);
}

const CameraChoice* SelectedCameraChoice() {
    LRESULT index = SendMessageW(gCamera, CB_GETCURSEL, 0, 0);
    if (index == CB_ERR || static_cast<std::size_t>(index) >= gChoices.size()) {
        return nullptr;
    }
    return &gChoices[static_cast<std::size_t>(index)];
}

int ZoomToTrackPosition(float zoom, float minZoom, float maxZoom) {
    if (!(minZoom > 0.0f) || maxZoom <= minZoom) {
        return kZoomTrackMin;
    }
    double clamped = std::clamp(
            static_cast<double>(zoom),
            static_cast<double>(minZoom),
            static_cast<double>(maxZoom));
    double t = std::log(clamped / minZoom) / std::log(maxZoom / minZoom);
    return static_cast<int>(std::lround(
            kZoomTrackMin + t * static_cast<double>(kZoomTrackMax - kZoomTrackMin)));
}

float TrackPositionToZoom(int position, float minZoom, float maxZoom) {
    if (!(minZoom > 0.0f) || maxZoom <= minZoom) {
        return minZoom;
    }
    double t = static_cast<double>(position - kZoomTrackMin)
            / static_cast<double>(kZoomTrackMax - kZoomTrackMin);
    t = std::clamp(t, 0.0, 1.0);
    return static_cast<float>(minZoom * std::exp(std::log(maxZoom / minZoom) * t));
}

void UpdateZoomLabel(float zoom) {
    wchar_t text[32]{};
    swprintf_s(text, L"%.2fx", static_cast<double>(zoom));
    SetWindowTextW(gZoomValue, text);
}

void ConfigureZoomForChoice(std::size_t index) {
    if (index >= gChoices.size()) {
        return;
    }

    const CameraChoice& choice = gChoices[index];
    int position = ZoomToTrackPosition(choice.initialZoom, choice.zoomMin, choice.zoomMax);
    SendMessageW(gZoom, TBM_SETPOS, TRUE, position);
    UpdateZoomLabel(choice.initialZoom);
}

void UpdateControlStates() {
    const bool connected = gNetwork.IsConnected();
    const CameraChoice* choice = SelectedCameraChoice();
    const bool hasCamera = connected && choice != nullptr;
    const bool hasVariableZoom = hasCamera && choice->zoomMax > choice->zoomMin;

    EnableWindow(gIp, !gStreaming);
    EnableWindow(gDiscover, !gStreaming);
    EnableWindow(gConnect, !gStreaming);
    EnableWindow(gCamera, connected && !gStreaming);
    EnableWindow(gPreset, connected && !gStreaming);
    EnableWindow(gZoom, hasVariableZoom);
    EnableWindow(GetDlgItem(gMain, ID_ROTATE_LEFT), gStreaming);
    EnableWindow(GetDlgItem(gMain, ID_ROTATE_RIGHT), gStreaming);
    EnableWindow(GetDlgItem(gMain, ID_MIRROR_H), gStreaming);
    EnableWindow(GetDlgItem(gMain, ID_MIRROR_V), gStreaming);
    EnableWindow(gStartStop, gStreaming || hasCamera);

    if (gStartStop) {
        InvalidateRect(gStartStop, nullptr, TRUE);
    }
}

float CurrentSliderZoom() {
    const CameraChoice* choice = SelectedCameraChoice();
    if (!choice) {
        return 1.0f;
    }

    int position = static_cast<int>(SendMessageW(gZoom, TBM_GETPOS, 0, 0));
    return TrackPositionToZoom(position, choice->zoomMin, choice->zoomMax);
}

void CALLBACK ZoomWorkCallback(PTP_CALLBACK_INSTANCE, PVOID, PTP_WORK) {
    float zoom = gZoomWorkValue.load();
    std::string error;
    bool ok = !gZoomShuttingDown.load() && gNetwork.SetZoom(zoom, error);

    if (!ok) {
        gHasPendingZoom.store(false);
    }
    gZoomWorkInFlight.store(false);

    if (!ok && !gZoomShuttingDown.load()) {
        {
            std::lock_guard lock(gZoomErrorMutex);
            gZoomError = error.empty() ? "Zoom command failed" : error;
        }
        PostMessageW(gMain, WM_DOPECAM_ZOOM_FAILED, 0, 0);
    }
}

void QueueZoomUpdate(float zoom) {
    if (!gStreaming || gZoomWork == nullptr || gZoomShuttingDown.load()) {
        return;
    }

    gPendingZoom.store(zoom);
    gHasPendingZoom.store(true);

    if (!gZoomTimerArmed) {
        if (SetTimer(gMain, kZoomTimerId, kZoomIntervalMs, nullptr) != 0) {
            gZoomTimerArmed = true;
        } else {
            SetStatus(L"Could not schedule zoom updates");
        }
    }
}

void PumpZoomWork() {
    if (!gStreaming || gZoomShuttingDown.load()) {
        gHasPendingZoom.store(false);
        if (gZoomTimerArmed) {
            KillTimer(gMain, kZoomTimerId);
            gZoomTimerArmed = false;
        }
        return;
    }

    if (gZoomWorkInFlight.load()) {
        return;
    }

    if (!gHasPendingZoom.exchange(false)) {
        if (gZoomTimerArmed) {
            KillTimer(gMain, kZoomTimerId);
            gZoomTimerArmed = false;
        }
        return;
    }

    gZoomWorkValue.store(gPendingZoom.load());
    gZoomWorkInFlight.store(true);
    SubmitThreadpoolWork(gZoomWork);
}

void HandleZoomSliderChanged() {
    const CameraChoice* choice = SelectedCameraChoice();
    if (!choice) {
        return;
    }

    int position = static_cast<int>(SendMessageW(gZoom, TBM_GETPOS, 0, 0));
    float zoom = TrackPositionToZoom(position, choice->zoomMin, choice->zoomMax);
    UpdateZoomLabel(zoom);
    QueueZoomUpdate(zoom);
}

void PopulateCameraChoices(const std::vector<CameraDesc>& cameras) {
    gChoices.clear();
    SendMessageW(gCamera, CB_RESETCONTENT, 0, 0);

    int rearIndex = 0;
    int frontIndex = 0;
    for (const CameraDesc& camera : cameras) {
        if (camera.lensFacing == 1) {
            CameraChoice main;
            main.cameraId = camera.id;
            main.initialZoom = std::clamp(1.0f, camera.zoomMin, camera.zoomMax);
            main.zoomMin = camera.zoomMin;
            main.zoomMax = camera.zoomMax;
            main.label = rearIndex == 0 ? L"Rear Main" : L"Rear Camera " + std::to_wstring(rearIndex + 1);
            gChoices.push_back(main);

            if (camera.zoomMin < 0.95f) {
                CameraChoice ultra = main;
                ultra.initialZoom = camera.zoomMin;
                std::wostringstream label;
                label.setf(std::ios::fixed);
                label.precision(1);
                label << L"Rear Ultra-wide (" << camera.zoomMin << L"x)";
                ultra.label = label.str();
                gChoices.push_back(ultra);
            }
            ++rearIndex;
        } else if (camera.lensFacing == 0) {
            CameraChoice front;
            front.cameraId = camera.id;
            front.initialZoom = std::clamp(1.0f, camera.zoomMin, camera.zoomMax);
            front.zoomMin = camera.zoomMin;
            front.zoomMax = camera.zoomMax;
            front.label = frontIndex == 0 ? L"Front" : L"Front Camera " + std::to_wstring(frontIndex + 1);
            gChoices.push_back(front);
            ++frontIndex;
        } else {
            CameraChoice external;
            external.cameraId = camera.id;
            external.initialZoom = std::clamp(1.0f, camera.zoomMin, camera.zoomMax);
            external.zoomMin = camera.zoomMin;
            external.zoomMax = camera.zoomMax;
            external.label = L"External Camera";
            gChoices.push_back(external);
        }
    }

    for (const CameraChoice& choice : gChoices) {
        SendMessageW(gCamera, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(choice.label.c_str()));
    }
    if (!gChoices.empty()) {
        SendMessageW(gCamera, CB_SETCURSEL, 0, 0);
        ConfigureZoomForChoice(0);
    }
    UpdateControlStates();
}

bool ConnectAndLoadCameras(std::string& error) {
    gChoices.clear();
    SendMessageW(gCamera, CB_RESETCONTENT, 0, 0);
    UpdateControlStates();

    std::string ip = WideToUtf8(GetText(gIp));
    if (ip.empty()) {
        error = "Enter the phone IPv4 address or use Discover";
        return false;
    }

    if (!gNetwork.Connect(ip, gControlPort, error)) {
        return false;
    }
    std::vector<CameraDesc> cameras;
    if (!gNetwork.GetCameras(cameras, error)) {
        if (error.empty()) error = "Phone exposed no usable cameras";
        gNetwork.Close();
        UpdateControlStates();
        return false;
    }
    PopulateCameraChoices(cameras);
    return true;
}

void ConnectPhoneManual() {
    if (gStreaming) return;
    SetState(L"\u25CF Connecting\u2026", StateTone::Muted);
    SetStatus(L"Connecting to phone\u2026");
    gControlPort = dopecam::protocol::kControlPort;
    std::string error;
    if (!ConnectAndLoadCameras(error)) {
        ShowError(error);
        return;
    }
    SetState(L"\u25CF Connected", StateTone::Ready);
    SetStatus(L"Connected to " + GetText(gIp) + L" \u00B7 ready to start");
    UpdateControlStates();
}

void DiscoverPhone() {
    if (gStreaming) return;
    SetState(L"\u25CF Discovering\u2026", StateTone::Muted);
    SetStatus(L"Looking for an armed phone on the local network\u2026");
    DiscoveredPhone phone;
    std::string error;
    if (!NetworkClient::Discover(phone, error)) {
        ShowError(error);
        return;
    }

    SetWindowTextW(gIp, Utf8ToWide(phone.ip).c_str());
    gControlPort = phone.controlPort;
    if (!ConnectAndLoadCameras(error)) {
        ShowError(error);
        return;
    }
    SetState(L"\u25CF Connected", StateTone::Ready);
    SetStatus(Utf8ToWide(phone.model) + L" \u00B7 " + Utf8ToWide(phone.ip) + L" \u00B7 ready");
    UpdateControlStates();
}

void StopStream(bool notifyPhone) {
    if (!gStreaming && !notifyPhone) {
        gReceiver.Stop();
        return;
    }

    if (notifyPhone && gNetwork.IsConnected()) {
        std::string ignored;
        gNetwork.StopStream(ignored);
    }
    gHasPendingZoom.store(false);
    if (gZoomTimerArmed) {
        KillTimer(gMain, kZoomTimerId);
        gZoomTimerArmed = false;
    }

    gReceiver.Stop();
    gStreaming = false;
    SetWindowTextW(gStartStop, L"Start camera");
    if (gNetwork.IsConnected()) {
        SetState(L"\u25CF Connected", StateTone::Ready);
        SetStatus(L"Camera stopped \u00B7 ready to start again");
    } else {
        SetState(L"\u25CF Not connected", StateTone::Muted);
        SetStatus(L"Arm DopeCam, then discover the phone or enter its IPv4 address.");
    }
    UpdateControlStates();
}

void StartStream() {
    if (gStreaming) {
        StopStream(true);
        return;
    }

    SetState(L"\u25CF Starting\u2026", StateTone::Muted);
    SetStatus(L"Starting camera stream\u2026");

    std::string error;
    if (!gNetwork.IsConnected() || gChoices.empty()) {
        if (!ConnectAndLoadCameras(error)) {
            ShowError(error);
            return;
        }
    }

    LRESULT cameraIndex = SendMessageW(gCamera, CB_GETCURSEL, 0, 0);
    LRESULT presetIndex = SendMessageW(gPreset, CB_GETCURSEL, 0, 0);
    if (cameraIndex == CB_ERR || static_cast<std::size_t>(cameraIndex) >= gChoices.size()) {
        ShowError("Select a camera");
        return;
    }

    const CameraChoice& choice = gChoices[static_cast<std::size_t>(cameraIndex)];
    const char* preset = presetIndex == 0 ? "budget" : (presetIndex == 2 ? "quality" : "normal");

    float zoom = std::clamp(CurrentSliderZoom(), choice.zoomMin, choice.zoomMax);

    if (!gReceiver.Bind(dopecam::protocol::kVideoPort, error)) {
        ShowError(error);
        return;
    }

    StartInfo info;
    if (!gNetwork.StartStream(choice.cameraId, preset, dopecam::protocol::kVideoPort, zoom, info, error)) {
        gReceiver.Stop();
        ShowError(error);
        return;
    }

    if (!gPipeline) {
        gPipeline = std::make_unique<MediaPipeline>(gPreview);
    }
    auto fatal = [](const std::string& message) {
        auto* text = new std::wstring(Utf8ToWide(message));
        if (!PostMessageW(gMain, WM_DOPECAM_FATAL, 0, reinterpret_cast<LPARAM>(text))) {
            delete text;
        }
    };
    if (!gReceiver.Start(info.width, info.height, info.fps, gPipeline.get(), &gNetwork, fatal, error)) {
        std::string ignored;
        gNetwork.StopStream(ignored);
        ShowError(error);
        return;
    }

    gStreaming = true;
    SetState(L"\u25CF Streaming", StateTone::Streaming);
    SetWindowTextW(gStartStop, L"Stop camera");
    UpdateControlStates();
    std::wostringstream status;
    status << L"Streaming " << info.width << L"\u00D7" << info.height << L" @ " << info.fps
           << L" fps \u00B7 " << (info.bitrate / 1'000'000.0) << L" Mbps";
    SetStatus(status.str());
}

void UpdateTransformButtonText() {
    if (!gPipeline) return;
    SetWindowTextW(GetDlgItem(gMain, ID_MIRROR_H), gPipeline->MirrorHorizontal() ? L"Flip H \u00B7 On" : L"Flip H");
    SetWindowTextW(GetDlgItem(gMain, ID_MIRROR_V), gPipeline->MirrorVertical() ? L"Flip V \u00B7 On" : L"Flip V");
}

void LayoutControls(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int sidebar = std::min(Dip(kSidebarWidthDip), width);
    const int pad = Dip(20);
    const int content = std::max(1, sidebar - pad * 2);
    const int gap = Dip(8);

    auto move = [&](int id, int x, int y, int w, int h) {
        HWND control = GetDlgItem(hwnd, id);
        if (control) {
            MoveWindow(control, x, y, std::max(1, w), std::max(1, h), TRUE);
        }
    };

    move(ID_BRAND_ICON, pad, Dip(18), Dip(32), Dip(32));
    const int titleX = pad + Dip(44);
    move(ID_TITLE, titleX, Dip(14), content - Dip(44), Dip(28));
    move(ID_SUBTITLE, titleX, Dip(43), content - Dip(44), Dip(18));
    move(ID_STATE, pad, Dip(69), content, Dip(22));

    move(ID_CONNECTION_LABEL, pad, Dip(103), content, Dip(16));
    const int connectWidth = Dip(88);
    move(ID_IP, pad, Dip(122), content - connectWidth - gap, Dip(34));
    move(ID_CONNECT, pad + content - connectWidth, Dip(122), connectWidth, Dip(34));
    move(ID_DISCOVER, pad, Dip(163), content, Dip(32));

    move(ID_CAMERA_LABEL, pad, Dip(211), content, Dip(16));
    move(ID_CAMERA, pad, Dip(230), content, Dip(160));

    move(ID_QUALITY_LABEL, pad, Dip(276), content, Dip(16));
    move(ID_PRESET, pad, Dip(295), content, Dip(140));

    move(ID_ZOOM_LABEL, pad, Dip(341), content - Dip(72), Dip(18));
    move(ID_ZOOM_VALUE, pad + content - Dip(72), Dip(339), Dip(72), Dip(20));
    move(ID_ZOOM, pad, Dip(363), content, Dip(30));

    move(ID_TRANSFORM_LABEL, pad, Dip(405), content, Dip(16));
    const int half = (content - gap) / 2;
    move(ID_ROTATE_LEFT, pad, Dip(425), half, Dip(32));
    move(ID_ROTATE_RIGHT, pad + half + gap, Dip(425), content - half - gap, Dip(32));
    move(ID_MIRROR_H, pad, Dip(465), half, Dip(32));
    move(ID_MIRROR_V, pad + half + gap, Dip(465), content - half - gap, Dip(32));

    const int statusHeight = Dip(42);
    const int statusY = std::max(Dip(570), height - pad - statusHeight);
    const int actionHeight = Dip(42);
    const int actionY = statusY - Dip(12) - actionHeight;
    move(ID_START_STOP, pad, actionY, content, actionHeight);
    move(ID_STATUS, pad, statusY, content, statusHeight);

    if (gPreview) {
        MoveWindow(gPreview, sidebar, 0, std::max(1, width - sidebar), std::max(1, height), TRUE);
    }
}

HWND MakeControl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
    HWND hwnd = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                0, 0, 10, 10, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    AddControlFont(hwnd);
    return hwnd;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            gMain = hwnd;
            gDpi = GetDpiForWindow(hwnd);
            CreateFonts();
            gPanelBrush = CreateSolidBrush(kPanelColor);
            gPreviewBrush = CreateSolidBrush(kPreviewColor);

            gBrandIcon = MakeControl(
                    hwnd, L"STATIC", L"",
                    SS_ICON | SS_CENTERIMAGE | SS_REALSIZECONTROL,
                    ID_BRAND_ICON);
            gTitle = MakeControl(hwnd, L"STATIC", L"DopeCam", SS_LEFT | SS_NOPREFIX, ID_TITLE);
            gSubtitle = MakeControl(hwnd, L"STATIC", L"Wireless camera bridge", SS_LEFT | SS_NOPREFIX, ID_SUBTITLE);
            gState = MakeControl(hwnd, L"STATIC", L"\u25CF Not connected", SS_LEFT | SS_NOPREFIX, ID_STATE);
            gConnectionLabel = MakeControl(hwnd, L"STATIC", L"CONNECTION", SS_LEFT | SS_NOPREFIX, ID_CONNECTION_LABEL);
            gCameraLabel = MakeControl(hwnd, L"STATIC", L"CAMERA", SS_LEFT | SS_NOPREFIX, ID_CAMERA_LABEL);
            gQualityLabel = MakeControl(hwnd, L"STATIC", L"QUALITY", SS_LEFT | SS_NOPREFIX, ID_QUALITY_LABEL);
            gZoomLabel = MakeControl(hwnd, L"STATIC", L"ZOOM", SS_LEFT | SS_NOPREFIX, ID_ZOOM_LABEL);
            gTransformLabel = MakeControl(hwnd, L"STATIC", L"TRANSFORM", SS_LEFT | SS_NOPREFIX, ID_TRANSFORM_LABEL);

            gDiscover = MakeControl(hwnd, L"BUTTON", L"Discover on LAN", BS_PUSHBUTTON, ID_DISCOVER);
            gConnect = MakeControl(hwnd, L"BUTTON", L"Connect", BS_PUSHBUTTON, ID_CONNECT);
            gIp = MakeControl(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_IP);
            SendMessageW(gIp, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Phone IPv4"));
            gCamera = MakeControl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_CAMERA);
            gPreset = MakeControl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST, ID_PRESET);
            gZoom = MakeControl(hwnd, TRACKBAR_CLASSW, L"",
                                TBS_HORZ | TBS_NOTICKS | TBS_TRANSPARENTBKGND, ID_ZOOM);
            SendMessageW(gZoom, TBM_SETRANGE, TRUE, MAKELPARAM(kZoomTrackMin, kZoomTrackMax));
            gZoomValue = MakeControl(hwnd, L"STATIC", L"1.00x", SS_RIGHT | SS_CENTERIMAGE, ID_ZOOM_VALUE);
            MakeControl(hwnd, L"BUTTON", L"Rotate -90\u00B0", BS_PUSHBUTTON, ID_ROTATE_LEFT);
            MakeControl(hwnd, L"BUTTON", L"Rotate +90\u00B0", BS_PUSHBUTTON, ID_ROTATE_RIGHT);
            MakeControl(hwnd, L"BUTTON", L"Flip H", BS_PUSHBUTTON, ID_MIRROR_H);
            MakeControl(hwnd, L"BUTTON", L"Flip V", BS_PUSHBUTTON, ID_MIRROR_V);
            gStartStop = MakeControl(hwnd, L"BUTTON", L"Start camera", BS_OWNERDRAW, ID_START_STOP);
            gStatus = MakeControl(
                    hwnd, L"STATIC",
                    L"Arm DopeCam, then discover the phone or enter its IPv4 address.",
                    SS_LEFT | SS_NOPREFIX, ID_STATUS);
            gPreview = MakeControl(hwnd, L"STATIC", L"", SS_BLACKRECT, ID_PREVIEW);

            SendMessageW(gPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Budget \u00B7 720p30"));
            SendMessageW(gPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal \u00B7 1080p30"));
            SendMessageW(gPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Quality \u00B7 1080p30"));
            SendMessageW(gPreset, CB_SETCURSEL, 1, 0);

            ApplyFonts();
            ApplyBrandIcon();
            UpdateControlStates();
            LayoutControls(hwnd);
            return 0;
        }
        case WM_SIZE:
            LayoutControls(hwnd);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Dip(kWindowMinWidthDip);
            info->ptMinTrackSize.y = Dip(kWindowMinHeightDip);
            return 0;
        }
        case WM_DPICHANGED: {
            gDpi = HIWORD(wParam);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd, nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            CreateFonts();
            ApplyFonts();
            ApplyBrandIcon();
            LayoutControls(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            RECT client{};
            GetClientRect(hwnd, &client);
            if (gPreviewBrush) {
                FillRect(dc, &client, gPreviewBrush);
            }
            RECT panel = client;
            panel.right = std::min<LONG>(client.right, static_cast<LONG>(Dip(kSidebarWidthDip)));
            if (gPanelBrush) {
                FillRect(dc, &panel, gPanelBrush);
            }
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);

            if (control == gPreview) {
                SetBkColor(dc, kPreviewColor);
                return reinterpret_cast<LRESULT>(gPreviewBrush);
            }

            SetBkMode(dc, TRANSPARENT);
            if (control == gTitle || control == gZoomValue) {
                SetTextColor(dc, kTextPrimary);
            } else if (control == gState) {
                COLORREF color = kTextMuted;
                if (gStateTone == StateTone::Ready) color = kStateReady;
                if (gStateTone == StateTone::Streaming) color = kStateStreaming;
                if (gStateTone == StateTone::Error) color = kStateError;
                SetTextColor(dc, color);
            } else {
                SetTextColor(dc, kTextSecondary);
            }
            return reinterpret_cast<LRESULT>(gPanelBrush);
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (!item || item->CtlID != ID_START_STOP) {
                break;
            }

            RECT rect = item->rcItem;
            COLORREF background = kPrimaryButton;
            COLORREF foreground = RGB(255, 255, 255);

            if (item->itemState & ODS_DISABLED) {
                background = kPrimaryButtonDisabled;
                foreground = RGB(119, 126, 136);
            } else if (item->itemState & ODS_SELECTED) {
                background = kPrimaryButtonPressed;
            }

            HBRUSH brush = CreateSolidBrush(background);
            HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
            HGDIOBJ oldPen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
            RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, Dip(9), Dip(9));
            SelectObject(item->hDC, oldPen);
            SelectObject(item->hDC, oldBrush);
            DeleteObject(brush);

            wchar_t text[64]{};
            GetWindowTextW(item->hwndItem, text, static_cast<int>(_countof(text)));
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, foreground);
            HGDIOBJ oldFont = SelectObject(item->hDC, gFontSemibold);
            DrawTextW(item->hDC, text, -1, &rect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(item->hDC, oldFont);

            if (item->itemState & ODS_FOCUS) {
                RECT focus = rect;
                InflateRect(&focus, -Dip(4), -Dip(4));
                DrawFocusRect(item->hDC, &focus);
            }
            return TRUE;
        }
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == gZoom) {
                HandleZoomSliderChanged();
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == kZoomTimerId) {
                PumpZoomWork();
                return 0;
            }
            break;
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_DISCOVER: DiscoverPhone(); break;
                case ID_CONNECT: ConnectPhoneManual(); break;
                case ID_START_STOP: StartStream(); break;
                case ID_ROTATE_LEFT:
                    if (gPipeline) gPipeline->SetRotation(gPipeline->Rotation() - 90);
                    break;
                case ID_ROTATE_RIGHT:
                    if (gPipeline) gPipeline->SetRotation(gPipeline->Rotation() + 90);
                    break;
                case ID_MIRROR_H:
                    if (gPipeline) gPipeline->ToggleMirrorHorizontal();
                    UpdateTransformButtonText();
                    break;
                case ID_MIRROR_V:
                    if (gPipeline) gPipeline->ToggleMirrorVertical();
                    UpdateTransformButtonText();
                    break;
                case ID_CAMERA:
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        LRESULT index = SendMessageW(gCamera, CB_GETCURSEL, 0, 0);
                        if (!gStreaming && index != CB_ERR && static_cast<std::size_t>(index) < gChoices.size()) {
                            ConfigureZoomForChoice(static_cast<std::size_t>(index));
                            UpdateControlStates();
                        }
                    }
                    break;
            }
            return 0;
        }
        case WM_DOPECAM_FATAL: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
            StopStream(true);
            if (text) {
                SetState(L"\u25CF Pipeline error", StateTone::Error);
                SetStatus(*text);
                MessageBoxW(hwnd, text->c_str(), L"DopeCam media pipeline", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case WM_DOPECAM_ZOOM_FAILED: {
            std::string error;
            {
                std::lock_guard lock(gZoomErrorMutex);
                error = gZoomError;
                gZoomError.clear();
            }
            if (gStreaming) {
                StopStream(false);
                gNetwork.Close();
                SetState(L"\u25CF Control error", StateTone::Error);
                SetStatus(L"Zoom control failed \u00B7 " + Utf8ToWide(error));
                UpdateControlStates();
            }
            return 0;
        }
        case WM_CLOSE:
            StopStream(true);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            gZoomShuttingDown.store(true);
            gHasPendingZoom.store(false);
            if (gZoomTimerArmed) {
                KillTimer(hwnd, kZoomTimerId);
                gZoomTimerArmed = false;
            }
            if (gZoomWork != nullptr) {
                WaitForThreadpoolWorkCallbacks(gZoomWork, TRUE);
                CloseThreadpoolWork(gZoomWork);
                gZoomWork = nullptr;
            }
            gReceiver.Stop();
            gNetwork.Close();
            gPipeline.reset();
            DestroyFonts();
            if (gPanelBrush) {
                DeleteObject(gPanelBrush);
                gPanelBrush = nullptr;
            }
            if (gPreviewBrush) {
                DeleteObject(gPreviewBrush);
                gPreviewBrush = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed", L"DopeCam", MB_OK | MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES;
    if (!InitCommonControlsEx(&commonControls)) {
        MessageBoxW(nullptr, L"Could not initialize Windows common controls", L"DopeCam", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    gZoomShuttingDown.store(false);
    gZoomWork = CreateThreadpoolWork(ZoomWorkCallback, nullptr, nullptr);
    if (gZoomWork == nullptr) {
        MessageBoxW(nullptr, L"Could not initialize zoom control worker", L"DopeCam", MB_OK | MB_ICONERROR);
        WSACleanup();
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(
            instance,
            MAKEINTRESOURCEW(kAppIconResourceId),
            IMAGE_ICON,
            0,
            0,
            LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
            instance,
            MAKEINTRESOURCEW(kAppIconResourceId),
            IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON),
            GetSystemMetrics(SM_CYSMICON),
            LR_SHARED));
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        CloseThreadpoolWork(gZoomWork);
        gZoomWork = nullptr;
        WSACleanup();
        return 1;
    }

    HWND window = CreateWindowExW(
            0, kWindowClass, L"DopeCam",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 720,
            nullptr, nullptr, instance, nullptr);
    if (!window) {
        CloseThreadpoolWork(gZoomWork);
        gZoomWork = nullptr;
        WSACleanup();
        return 1;
    }

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return static_cast<int>(msg.wParam);
}
