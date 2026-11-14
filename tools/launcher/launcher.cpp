// CoherencyLatency single-file launcher for Windows x86 / x64 / ARM64.
// Embeds the three native builds as RCDATA and starts the matching one.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kResX86 = 101;
constexpr int kResX64 = 102;
constexpr int kResArm64 = 103;

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
    path += L"CoherencyLatency-";
    path += arch_name(arch);
    path += L"-";
    path += std::to_wstring(GetCurrentProcessId());
    path += L"-";
    path += std::to_wstring(GetTickCount());
    path += L".exe";
    return path;
}

void print_error(const wchar_t* message) {
    std::fwprintf(stderr, L"CoherencyLatency: %ls\n", message);
}

}  // namespace

int main() {
    const int arch = native_arch();
    std::vector<unsigned char> payload;
    if (!load_resource(resource_id(arch), payload)) {
        print_error(L"embedded executable is missing");
        return 1;
    }

    const std::wstring path = make_temp_path(arch);
    if (path.empty() || !write_file(path, payload)) {
        print_error(L"cannot write temporary executable");
        return 1;
    }

    std::wstring command_line = GetCommandLineW();
    std::vector<wchar_t> command_buffer(command_line.begin(), command_line.end());
    command_buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), command_buffer.data(), nullptr, nullptr,
                        TRUE, 0, nullptr, nullptr, &startup, &process)) {
        DeleteFileW(path.c_str());
        print_error(L"cannot start the benchmark executable");
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
