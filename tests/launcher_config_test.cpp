#include "../src/launcher_config.h"
#include "../src/capture_target.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct TemporaryDirectory {
    launcher::fs::path path;
    TemporaryDirectory() {
        wchar_t temp[MAX_PATH] = {}, name[MAX_PATH] = {};
        Check(GetTempPathW(MAX_PATH, temp) != 0, "temporary path");
        Check(GetTempFileNameW(temp, L"afe", 0, name) != 0, "unique temporary name");
        DeleteFileW(name);
        path = launcher::fs::path(name) / L"Launcher fixture with spaces \u03a9";
        launcher::fs::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        launcher::fs::remove_all(path.parent_path(), error);
    }
};

static void Touch(const launcher::fs::path& path) {
    launcher::fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "fixture";
}

static int ChildProbe() {
    wchar_t module[32768] = {}, cwd[32768] = {}, value[100] = {};
    if (!GetModuleFileNameW(nullptr, module, 32768) || !GetCurrentDirectoryW(32768, cwd)) return 20;
    if (launcher::fs::path(module).parent_path() != launcher::fs::path(cwd)) return 21;
    if (!GetEnvironmentVariableW(L"AIFRAME_RIFE_FP16", value, 100) || wcscmp(value, L"1")) return 22;
    if (GetEnvironmentVariableW(L"AIFRAME_ORT_PROFILE", value, 100)) return 23;
    if (!GetEnvironmentVariableW(L"AFE_UNICODE", value, 100) || wcscmp(value, L"\u03a9")) return 24;
    return 0;
}

int main() {
    wchar_t marker[8] = {};
    if (GetEnvironmentVariableW(L"AFE_LAUNCHER_TEST_CHILD", marker, 8)) return ChildProbe();
    try {
        const wchar_t inherited[] = L"=C:=C:\\work\0Path=original\0aiframe_ort_profile=1\0"
            L"AIFRAME_TARGET_PID=999\0aiframe_dml_device=7\0AFE_UNICODE=\u03a9\0";
        auto environment = launcher::ReadEnvironment(inherited);
        Check(environment.at(L"PATH") == L"original", "case-insensitive environment");
        Check(environment.at(L"=C:") == L"C:\\work", "preserve drive environment");
        launcher::Target target{reinterpret_cast<HWND>(123), 456, 789, L"fixture"};
        auto block = launcher::ChildEnvironment(environment, target, {});
        Check(block.size() >= 2 && !block.back() && !block[block.size()-2], "double-null block");
        auto child = launcher::ReadEnvironment(block.data());
        Check(child.at(L"AIFRAME_RIFE_FP16") == L"1" && child.at(L"AIFRAME_FSR1") == L"1" &&
            child.at(L"AIFRAME_DISPLAY_CACHE") == L"1", "GUI defaults");
        Check(child.at(L"AIFRAME_TARGET_HWND") == L"123" && child.at(L"AIFRAME_TARGET_PID") == L"456" &&
            child.at(L"AIFRAME_TARGET_CREATED") == L"789", "target identity");
        Check(!child.count(L"AIFRAME_ORT_PROFILE") && !child.count(L"AIFRAME_DML_DEVICE"), "clear stale overrides");
        Check(environment.at(L"AIFRAME_TARGET_PID") == L"999", "parent environment unchanged");
        auto alternative = launcher::ChildEnvironment(environment, target, {false, false, false});
        auto alt = launcher::ReadEnvironment(alternative.data());
        Check(alt.at(L"AIFRAME_RIFE_FP16") == L"0" && alt.at(L"AIFRAME_FSR1") == L"0" &&
            alt.at(L"AIFRAME_DISPLAY_CACHE") == L"0", "alternative settings");

        TemporaryDirectory temporary;
        auto exe = temporary.path / L"AIFrameEngineCompositor.exe";
        Check(launcher::MissingFiles(exe, {}).size() == 8, "default required file inventory");
        Check(launcher::MissingFiles(exe, {false,false,false}).size() == 4, "FP32 bicubic requirements");
        for (const auto& missing : launcher::MissingFiles(exe, {})) Touch(missing);
        Check(launcher::MissingFiles(exe, {}).empty(), "complete fixture");
        std::ofstream(exe, std::ios::trunc).close();
        Check(launcher::MissingFiles(exe, {}).size() == 1, "reject empty file");
        launcher::fs::remove(temporary.path / L"models/RIFE_flow_576x320.onnx");
        Check(launcher::MissingFiles(exe, {}).size() == 2, "FP16 also requires flow gate file");

        HWND window = CreateWindowExW(0, L"STATIC", L"Launcher target fixture", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            0, 0, 200, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(window != nullptr, "create target fixture");
        launcher::Target real;
        Check(launcher::Identify(window, real), "identify target");
        CaptureTarget pinned;
        Check(pinned.Bind(real.window, real.pid, real.created), "pin target lifetime");
        Check(launcher::StillSameTarget(real), "revalidate identity");
        auto wrong = real; ++wrong.created;
        Check(!launcher::StillSameTarget(wrong), "reject changed process lifetime");
        wrong = real; ++wrong.pid;
        Check(!launcher::StillSameTarget(wrong), "reject changed pid");
        ShowWindow(window, SW_MINIMIZE);
        Check(!launcher::StillSameTarget(real), "reject minimized target");
        DestroyWindow(window);
        Check(!launcher::StillSameTarget(real) && !pinned.Valid(), "reject closed target");

        { launcher::LaunchGuard first; Check(first.acquired, "first launch guard");
          launcher::LaunchGuard second; Check(!second.acquired, "reject simultaneous launch guard"); }
        { launcher::LaunchGuard next; Check(next.acquired, "guard released after launch attempt"); }

        auto parentBlock = GetEnvironmentStringsW();
        Check(parentBlock != nullptr, "read actual environment");
        auto actual = launcher::ReadEnvironment(parentBlock);
        FreeEnvironmentStringsW(parentBlock);
        actual[L"AFE_LAUNCHER_TEST_CHILD"] = L"1";
        actual[L"AFE_UNICODE"] = L"\u03a9";
        actual[L"AIFRAME_ORT_PROFILE"] = L"1";
        auto actualBlock = launcher::ChildEnvironment(actual, target, {});
        wchar_t module[32768] = {};
        Check(GetModuleFileNameW(nullptr, module, 32768) > 0, "test executable path");
        // Launch this harmless test executable, never the real compositor.
        auto probe = temporary.path / L"Child probe.exe";
        launcher::fs::copy_file(module, probe);
        PROCESS_INFORMATION process{}; std::wstring error;
        Check(launcher::StartChild(probe, actualBlock, process, error), "spawn fixture in Unicode spaced path");
        launcher::Handle processHandle(process.hProcess), threadHandle(process.hThread);
        Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0, "fixture child exited");
        DWORD code = 999;
        Check(GetExitCodeProcess(process.hProcess, &code) && code == 0, "child environment and working directory");
        Check(!launcher::StartChild(temporary.path / L"missing.exe", actualBlock, process, error) && !error.empty(),
            "process creation failure is reported");
        std::cout << "PASS: environment, model preflight, target identity, launch guard, and real child-process contract\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
    return 0;
}