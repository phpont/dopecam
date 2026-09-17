#include <windows.h>
#include <dshow.h>
#include <oleauto.h>

#include <iostream>
#include <string>

int wmain() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::wcerr << L"CoInitializeEx failed: 0x" << std::hex << hr << L"\n";
        return 2;
    }

    ICreateDevEnum* deviceEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&deviceEnum));
    if (FAILED(hr)) {
        std::wcerr << L"CLSID_SystemDeviceEnum failed: 0x" << std::hex << hr << L"\n";
        if (uninitialize) CoUninitialize();
        return 3;
    }

    IEnumMoniker* enumMoniker = nullptr;
    hr = deviceEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMoniker, 0);
    deviceEnum->Release();

    if (hr != S_OK || !enumMoniker) {
        std::wcerr << L"No DirectShow video-input devices were enumerated.\n";
        if (enumMoniker) enumMoniker->Release();
        if (uninitialize) CoUninitialize();
        return 4;
    }

    bool found = false;
    IMoniker* moniker = nullptr;
    ULONG fetched = 0;
    while (enumMoniker->Next(1, &moniker, &fetched) == S_OK) {
        IPropertyBag* bag = nullptr;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag)))) {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR) {
                const std::wstring name(value.bstrVal ? value.bstrVal : L"");
                std::wcout << name << L"\n";
                if (name == L"DopeCam") found = true;
            }
            VariantClear(&value);
            bag->Release();
        }
        moniker->Release();
        moniker = nullptr;
    }

    enumMoniker->Release();
    if (uninitialize) CoUninitialize();

    if (!found) {
        std::wcerr << L"DopeCam was not found in CLSID_VideoInputDeviceCategory.\n";
        return 5;
    }

    std::wcout << L"DOPECAM_DIRECTSHOW_ENUMERATION=PASS\n";
    return 0;
}
