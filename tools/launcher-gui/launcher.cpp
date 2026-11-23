// CoherencyLatency GUI single-file launcher for Windows.
//
// Embeds the three native GUI builds as RCDATA and starts the matching one.
// Unlike the console launcher this one is a windowed subsystem binary, so
// double-clicking it never flashes a console window, and it hands the child a
// hidden STARTUPINFO so the GUI program owns the only window on screen.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kResX86 = 201;
constexpr int kResX64 = 202;
constexpr int kResArm64 = 203;

const wchar_t* arch_name(int arch) {
    switch (arch) {
        case 1: return L"x64";
        case 2: return L"ARM64";
        default: return L"x86";
    }
}

int resource_id(int arch) {
    switch (arch) {
        case 1: return kResX64;
        case 2: return kResArm64;
        default: return kResX86;
    }
}

int native_arch() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            return 1;
        case PROCESSOR_ARCHITECTURE_ARM64:
            return 2;
        default:
            return 0;
    }
}

bool load_resource(int id, std::vector<unsigned char>& data) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (resource == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(module, resource);
    HGLOBAL handle = LoadResource(module, resource);
    if (handle == nullptr || size == 0) {
        return false;
    }
    const void* pointer = LockResource(handle);
    if (pointer == nullptr) {
        return false;
    }
    data.assign(static_cast<const unsigned char*>(pointer),
                static_cast<const unsigned char*>(pointer) + size);
    return true;
}

bool write_file(const std::wstring& path, const std::vector<unsigned char>& data) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data.data(),
                              static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(file);
    return ok != FALSE && written == data.size();
}

std::wstring make_temp_path(int arch) {
    wchar_t directory[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    if (length == 0 || length > MAX_PATH) {
        return {};
    }
    std::wstring path(directory);
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path += L"CoherencyLatencyGUI-";
    path += arch_name(arch);
    path += L"-";
    path += std::to_wstring(GetCurrentProcessId());
    path += L"-";
    path += std::to_wstring(GetTickCount());
    path += L".exe";
    return path;
}

void show_error(const wchar_t* message) {
    MessageBoxW(nullptr, message, L"CoherencyLatency", MB_OK | MB_ICONERROR);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const int arch = native_arch();
    std::vector<unsigned char> payload;
    if (!load_resource(resource_id(arch), payload)) {
        show_error(L"内嵌的图形界面程序缺失，启动器文件可能已损坏。");
        return 1;
    }

    const std::wstring path = make_temp_path(arch);
    if (path.empty() || !write_file(path, payload)) {
        show_error(L"无法写入临时文件，请检查磁盘空间或杀毒软件设置。");
        return 1;
    }

    std::wstring command_line = GetCommandLineW();
    std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
    command_buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command_buffer.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup, &process)) {
        DeleteFileW(path.c_str());
        show_error(L"无法启动图形界面程序。");
        return 1;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    DeleteFileW(path.c_str());
    return static_cast<int>(exit_code);
}
