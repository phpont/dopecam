#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "MediaPipeline.h"
#include "NetworkClient.h"
#include "Protocol.h"
#include "RtpReceiver.h"

namespace {
constexpr wchar_t kWindowClass[] = L"DopeCamWindow";
constexpr UINT WM_DOPECAM_FATAL = WM_APP + 47;

enum ControlId : int {
    ID_DISCOVER = 100,
    ID_CONNECT,
    ID_IP,
    ID_CAMERA,
    ID_PRESET,
    ID_START_STOP,
    ID_ZOOM,
    ID_SET_ZOOM,
    ID_ROTATE_LEFT,
    ID_ROTATE_RIGHT,
    ID_MIRROR_H,
    ID_MIRROR_V,
    ID_STATUS,
    ID_PREVIEW
};

HWND gMain = nullptr;
HWND gIp = nullptr;
HWND gCamera = nullptr;
HWND gPreset = nullptr;
HWND gStartStop = nullptr;
HWND gZoom = nullptr;
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
HFONT gFont = nullptr;

void SetStatus(const std::wstring& text) {
    SetWindowTextW(gStatus, text.c_str());
}

void ShowError(const std::string& error) {
    std::wstring wide = Utf8ToWide(error);
    SetStatus(L"Error: " + wide);
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
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
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
        wchar_t zoom[32]{};
        swprintf_s(zoom, L"%.2f", static_cast<double>(gChoices[0].initialZoom));
        SetWindowTextW(gZoom, zoom);
    }
}

bool ConnectAndLoadCameras(std::string& error) {
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
        return false;
    }
    PopulateCameraChoices(cameras);
    return true;
}

void ConnectPhoneManual() {
    if (gStreaming) return;
    gControlPort = dopecam::protocol::kControlPort;
    std::string error;
    if (!ConnectAndLoadCameras(error)) {
        ShowError(error);
        return;
    }
    SetStatus(L"Connected manually to " + GetText(gIp));
}

void DiscoverPhone() {
    if (gStreaming) return;
    SetStatus(L"Discovering armed phone...");
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
    SetStatus(L"Connected to " + Utf8ToWide(phone.model) + L" at " + Utf8ToWide(phone.ip));
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
    gReceiver.Stop();
    gStreaming = false;
    SetWindowTextW(gStartStop, L"Start");
    SetStatus(L"Stopped");
}

void StartStream() {
    if (gStreaming) {
        StopStream(true);
        return;
    }

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

    float zoom = choice.initialZoom;
    try {
        zoom = std::stof(WideToUtf8(GetText(gZoom)));
    } catch (...) {
        zoom = choice.initialZoom;
    }
    zoom = std::clamp(zoom, choice.zoomMin, choice.zoomMax);

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
    SetWindowTextW(gStartStop, L"Stop");
    std::wostringstream status;
    status << L"Streaming " << info.width << L"×" << info.height << L" @ " << info.fps
           << L" fps · " << (info.bitrate / 1'000'000.0) << L" Mbps";
    SetStatus(status.str());
}

void ApplyZoom() {
    if (!gStreaming) {
        SetStatus(L"Start the stream before changing zoom");
        return;
    }
    std::string error;
    float zoom = 1.0f;
    try {
        zoom = std::stof(WideToUtf8(GetText(gZoom)));
    } catch (...) {
        ShowError("Invalid zoom value");
        return;
    }

    LRESULT cameraIndex = SendMessageW(gCamera, CB_GETCURSEL, 0, 0);
    if (cameraIndex != CB_ERR && static_cast<std::size_t>(cameraIndex) < gChoices.size()) {
        const auto& choice = gChoices[static_cast<std::size_t>(cameraIndex)];
        zoom = std::clamp(zoom, choice.zoomMin, choice.zoomMax);
    }
    if (!gNetwork.SetZoom(zoom, error)) {
        ShowError(error);
    }
}

void UpdateTransformButtonText() {
    if (!gPipeline) return;
    SetWindowTextW(GetDlgItem(gMain, ID_MIRROR_H), gPipeline->MirrorHorizontal() ? L"Mirror H: On" : L"Mirror H");
    SetWindowTextW(GetDlgItem(gMain, ID_MIRROR_V), gPipeline->MirrorVertical() ? L"Mirror V: On" : L"Mirror V");
}

void LayoutControls(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int margin = 16;
    int panel = 280;
    int row = 32;
    int y = margin;

    auto move = [&](int id, int x, int yy, int w, int h = 28) {
        HWND control = GetDlgItem(hwnd, id);
        if (control) MoveWindow(control, x, yy, w, h, TRUE);
    };

    move(ID_DISCOVER, margin, y, 135); move(ID_CONNECT, margin + 145, y, 135); y += row;
    move(ID_IP, margin, y, panel); y += row + 8;
    move(ID_CAMERA, margin, y, panel, 180); y += row;
    move(ID_PRESET, margin, y, panel, 140); y += row;
    move(ID_ZOOM, margin, y, 170); move(ID_SET_ZOOM, margin + 180, y, 100); y += row + 8;
    move(ID_ROTATE_LEFT, margin, y, 135); move(ID_ROTATE_RIGHT, margin + 145, y, 135); y += row;
    move(ID_MIRROR_H, margin, y, 135); move(ID_MIRROR_V, margin + 145, y, 135); y += row + 8;
    move(ID_START_STOP, margin, y, panel, 34); y += 44;
    move(ID_STATUS, margin, y, panel, std::max(50, height - y - margin));

    int previewX = margin + panel + margin;
    MoveWindow(gPreview, previewX, margin, std::max(1, width - previewX - margin),
               std::max(1, height - margin * 2), TRUE);
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
            gFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            gDiscover = MakeControl(hwnd, L"BUTTON", L"Discover", BS_PUSHBUTTON, ID_DISCOVER);
            gConnect = MakeControl(hwnd, L"BUTTON", L"Connect IP", BS_PUSHBUTTON, ID_CONNECT);
            gIp = MakeControl(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, ID_IP);
            gCamera = MakeControl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, ID_CAMERA);
            gPreset = MakeControl(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST, ID_PRESET);
            gZoom = MakeControl(hwnd, L"EDIT", L"1.00", WS_BORDER | ES_AUTOHSCROLL, ID_ZOOM);
            MakeControl(hwnd, L"BUTTON", L"Set Zoom", BS_PUSHBUTTON, ID_SET_ZOOM);
            MakeControl(hwnd, L"BUTTON", L"Rotate -90°", BS_PUSHBUTTON, ID_ROTATE_LEFT);
            MakeControl(hwnd, L"BUTTON", L"Rotate +90°", BS_PUSHBUTTON, ID_ROTATE_RIGHT);
            MakeControl(hwnd, L"BUTTON", L"Mirror H", BS_PUSHBUTTON, ID_MIRROR_H);
            MakeControl(hwnd, L"BUTTON", L"Mirror V", BS_PUSHBUTTON, ID_MIRROR_V);
            gStartStop = MakeControl(hwnd, L"BUTTON", L"Start", BS_DEFPUSHBUTTON, ID_START_STOP);
            gStatus = MakeControl(hwnd, L"STATIC", L"Arm DopeCam, then Discover or enter the phone IPv4 and Connect IP.", SS_LEFT, ID_STATUS);
            gPreview = MakeControl(hwnd, L"STATIC", L"", SS_BLACKRECT, ID_PREVIEW);

            SendMessageW(gPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Budget · 720p30"));
            SendMessageW(gPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Normal · 1080p30"));
            SendMessageW(gPreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Quality · 1080p30"));
            SendMessageW(gPreset, CB_SETCURSEL, 1, 0);
            LayoutControls(hwnd);
            return 0;
        }
        case WM_SIZE:
            LayoutControls(hwnd);
            return 0;
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case ID_DISCOVER: DiscoverPhone(); break;
                case ID_CONNECT: ConnectPhoneManual(); break;
                case ID_START_STOP: StartStream(); break;
                case ID_SET_ZOOM: ApplyZoom(); break;
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
                            wchar_t zoom[32]{};
                            swprintf_s(zoom, L"%.2f", static_cast<double>(gChoices[static_cast<std::size_t>(index)].initialZoom));
                            SetWindowTextW(gZoom, zoom);
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
                SetStatus(L"Pipeline stopped: " + *text);
                MessageBoxW(hwnd, text->c_str(), L"DopeCam media pipeline", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        case WM_CLOSE:
            StopStream(true);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            gReceiver.Stop();
            gNetwork.Close();
            gPipeline.reset();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int showCommand) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed", L"DopeCam", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        WSACleanup();
        return 1;
    }

    HWND window = CreateWindowExW(
            0, kWindowClass, L"DopeCam v0",
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT, 1180, 720,
            nullptr, nullptr, instance, nullptr);
    if (!window) {
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
