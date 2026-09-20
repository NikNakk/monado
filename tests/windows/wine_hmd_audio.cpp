// SPDX-License-Identifier: BSL-1.0
// Pin Wine's default render endpoints to a selected physical MMDevice.
//
// This is intentionally a small Windows-side helper so selection follows
// Wine's own IMMDevice enumeration and registry conventions rather than
// reimplementing endpoint-ID generation on macOS.

#include <windows.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cwchar>
#include <cstdio>
#include <string>

static bool contains_ci(const wchar_t *haystack, const wchar_t *needle)
{
    if (!haystack || !needle || !*needle) return false;
    std::wstring h(haystack), n(needle);
    for (auto &c : h) c = towlower(c);
    for (auto &c : n) c = towlower(c);
    return h.find(n) != std::wstring::npos;
}

static bool read_reg_string(HKEY root, const std::wstring &path, const wchar_t *name, std::wstring &out)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    DWORD type = 0, bytes = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return false;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(value.data()), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return false;
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    out = value;
    return true;
}

static bool write_reg_string(HKEY root, const std::wstring &path, const wchar_t *name, const wchar_t *value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc;
    if (value && *value) {
        DWORD bytes = DWORD((wcslen(value) + 1) * sizeof(wchar_t));
        rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value), bytes);
    } else {
        rc = RegDeleteValueW(key, name);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        fwprintf(stderr, L"usage:\n");
        fwprintf(stderr, L"  wine-hmd-audio.exe list\n");
        fwprintf(stderr, L"  wine-hmd-audio.exe select <friendly-name-substring>\n");
        fwprintf(stderr, L"  wine-hmd-audio.exe restore <driver> <output-or--> <voice-output-or-->\n");
        return 2;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 3;

    IMMDeviceEnumerator *enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator));
    if (FAILED(hr) || !enumerator) {
        CoUninitialize();
        return 4;
    }

    if (!wcscmp(argv[1], L"restore")) {
        if (argc != 5) {
            fwprintf(stderr, L"restore requires driver, output, voice-output\n");
            enumerator->Release();
            CoUninitialize();
            return 2;
        }
        std::wstring path = L"Software\\Wine\\Drivers\\";
        path += argv[2];
        const wchar_t *out = wcscmp(argv[3], L"-") ? argv[3] : L"";
        const wchar_t *voice = wcscmp(argv[4], L"-") ? argv[4] : L"";
        bool ok = write_reg_string(HKEY_CURRENT_USER, path, L"DefaultOutput", out) &&
                  write_reg_string(HKEY_CURRENT_USER, path, L"DefaultVoiceOutput", voice);
        enumerator->Release();
        CoUninitialize();
        return ok ? 0 : 5;
    }

    IMMDeviceCollection *collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) {
        enumerator->Release();
        CoUninitialize();
        return 6;
    }

    UINT count = 0;
    collection->GetCount(&count);
    bool select = !wcscmp(argv[1], L"select");
    const wchar_t *match = (select && argc >= 3) ? argv[2] : nullptr;
    int result = select ? 7 : 0;

    for (UINT i = 0; i < count; ++i) {
        IMMDevice *device = nullptr;
        LPWSTR id = nullptr;
        IPropertyStore *props = nullptr;
        PROPVARIANT friendly, driver;
        PropVariantInit(&friendly);
        PropVariantInit(&driver);

        if (FAILED(collection->Item(i, &device))) continue;
        device->GetId(&id);
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            props->GetValue(PKEY_Device_FriendlyName, &friendly);
            props->GetValue(PKEY_Device_Driver, &driver);
        }

        const wchar_t *name = friendly.vt == VT_LPWSTR ? friendly.pwszVal : L"";
        const wchar_t *drv = driver.vt == VT_LPWSTR ? driver.pwszVal : L"";

        if (!select) {
            wprintf(L"%u\t%ls\t%ls\t%ls\n", i, drv, id ? id : L"", name);
        } else if (id && *drv && contains_ci(name, match)) {
            std::wstring path = L"Software\\Wine\\Drivers\\";
            path += drv;
            std::wstring old_out, old_voice;
            bool had_out = read_reg_string(HKEY_CURRENT_USER, path, L"DefaultOutput", old_out);
            bool had_voice = read_reg_string(HKEY_CURRENT_USER, path, L"DefaultVoiceOutput", old_voice);

            if (write_reg_string(HKEY_CURRENT_USER, path, L"DefaultOutput", id) &&
                write_reg_string(HKEY_CURRENT_USER, path, L"DefaultVoiceOutput", id)) {
                wprintf(L"SELECTED\t%ls\t%ls\t%ls\t%ls\t%ls\n",
                        drv,
                        id,
                        had_out ? old_out.c_str() : L"-",
                        had_voice ? old_voice.c_str() : L"-",
                        name);
                result = 0;
            } else {
                result = 8;
            }

            if (props) props->Release();
            if (id) CoTaskMemFree(id);
            PropVariantClear(&friendly);
            PropVariantClear(&driver);
            device->Release();
            break;
        }

        if (props) props->Release();
        if (id) CoTaskMemFree(id);
        PropVariantClear(&friendly);
        PropVariantClear(&driver);
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    CoUninitialize();
    return result;
}
