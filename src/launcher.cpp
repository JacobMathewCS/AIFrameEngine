#include "launcher_config.h"
#include "capture_target.h"
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <algorithm>
#include <stdexcept>

namespace {
constexpr wchar_t kClass[] = L"AIFrameEngineLauncher";
enum Control { Exe = 100, Browse, Windows, Refresh, Scaler, Precision, Cache, Launch, Status, Log, Help };
struct Item { HWND window; int x, y, width, height; bool heading; };

class Launcher {
public:
    explicit Launcher(launcher::fs::path executable, bool smoke)
        : executable_(std::move(executable)), smoke_(smoke) {}
    ~Launcher() {
        if (child_) CloseHandle(child_);
        if (font_) DeleteObject(font_);
        if (headingFont_) DeleteObject(headingFont_);
        if (background_) DeleteObject(background_);
    }

    int Run(HINSTANCE instance, int show) {
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.hInstance = instance;
        cls.lpfnWndProc = Dispatch;
        cls.lpszClassName = kClass;
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        cls.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        if (!RegisterClassExW(&cls)) return 1;
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"AIFrameEngine — Launcher",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
            CW_USEDEFAULT, CW_USEDEFAULT, 720, 650, nullptr, nullptr, instance, this);
        if (!window_) return 1;
        // Creation-time DPI/size messages may replace the size requested by
        // WM_CREATE. Apply the final client size once creation has completed.
        ResizeForDpi(GetDpiForWindow(window_));
        ShowWindow(window_, show);
        UpdateWindow(window_);
        SetFocus(GetDlgItem(window_, Windows));
        MSG message{};
        BOOL result;
        while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (!IsDialogMessageW(window_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        return result == -1 ? 1 : static_cast<int>(message.wParam);
    }

private:
    HWND Add(const wchar_t* type, const wchar_t* text, DWORD style, int id,
             int x, int y, int width, int height, bool heading = false) {
        HWND control = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
            type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!control) throw std::runtime_error("Could not create launcher controls.");
        items_.push_back({control, x, y, width, height, heading});
        return control;
    }

    void CreateControls() {
        background_ = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
        Add(L"STATIC", L"AIFrameEngine", SS_LEFT, 0, 24, 18, 620, 40, true);
        Add(L"STATIC", L"Frame generation & spatial upscaling", SS_LEFT | SS_NOPREFIX, 0, 26, 62, 630, 24);
        Add(L"STATIC", L"&Compositor executable", SS_LEFT, 0, 24, 102, 610, 22);
        Add(L"EDIT", executable_.c_str(), WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
            Exe, 24, 128, 548, 28);
        Add(L"BUTTON", L"&Browse…", WS_TABSTOP | BS_PUSHBUTTON, Browse, 582, 127, 94, 30);
        Add(L"STATIC", L"&Game window", SS_LEFT, 0, 24, 178, 610, 22);
        Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            Windows, 24, 204, 548, 230);
        SendDlgItemMessageW(window_, Windows, CB_SETDROPPEDWIDTH, 640, 0);
        SendDlgItemMessageW(window_, Windows, CB_SETCUEBANNER, 0,
            reinterpret_cast<LPARAM>(L"Select a running game window"));
        Add(L"BUTTON", L"&Refresh", WS_TABSTOP | BS_PUSHBUTTON, Refresh, 582, 203, 94, 30);
        Add(L"STATIC", L"&Upscaling", SS_LEFT, 0, 24, 254, 250, 22);
        Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Scaler, 24, 280, 310, 130);
        SendDlgItemMessageW(window_, Scaler, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FSR 1 — EASU + RCAS"));
        SendDlgItemMessageW(window_, Scaler, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bicubic + sharpening"));
        SendDlgItemMessageW(window_, Scaler, CB_SETCURSEL, 0, 0);
        Add(L"STATIC", L"Inference &precision", SS_LEFT, 0, 354, 254, 300, 22);
        Add(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST, Precision, 354, 280, 322, 130);
        SendDlgItemMessageW(window_, Precision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FP16 — default"));
        SendDlgItemMessageW(window_, Precision, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"FP32"));
        SendDlgItemMessageW(window_, Precision, CB_SETCURSEL, 0, 0);
        Add(L"BUTTON", L"&Cache repeated frames", WS_TABSTOP | BS_AUTOCHECKBOX, Cache, 24, 324, 330, 26);
        SendDlgItemMessageW(window_, Cache, BM_SETCHECK, BST_CHECKED, 0);
        Add(L"BUTTON", L"&Launch compositor", WS_TABSTOP | BS_DEFPUSHBUTTON, Launch, 24, 365, 652, 42);
        Add(L"STATIC", L"Select a game window to begin.", SS_LEFT | SS_NOPREFIX, Status, 24, 421, 652, 48);
        Add(L"STATIC", L"Windowed games • 720p best tested • Native 1080p FG has known issues.\r\n"
            L"F9: toggle FG   |   Ctrl+Shift+F6: in-game status   |   Ctrl+Shift+F12: exit",
            SS_LEFT | SS_NOPREFIX, 0, 24, 481, 652, 46);
        Add(L"BUTTON", L"Open &log", WS_TABSTOP | BS_PUSHBUTTON, Log, 24, 545, 112, 30);
        Add(L"BUTTON", L"&Help", WS_TABSTOP | BS_PUSHBUTTON, Help, 146, 545, 96, 30);
        Add(L"STATIC", L"Closing this launcher does not stop the compositor.", SS_RIGHT, 0, 254, 550, 422, 24);
        ResizeForDpi(GetDpiForWindow(window_));
        RefreshWindows();
        if (smoke_ && !SetTimer(window_, 2, 250, nullptr)) throw std::runtime_error("Smoke-test timer failed.");
    }

    void ResizeForDpi(UINT dpi) {
        const auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
        HFONT normal = CreateFontW(-scale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        HFONT heading = CreateFontW(-scale(30), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        for (const auto& item : items_) {
            SendMessageW(item.window, WM_SETFONT, reinterpret_cast<WPARAM>(item.heading ? heading : normal), TRUE);
            MoveWindow(item.window, scale(item.x), scale(item.y), scale(item.width), scale(item.height), TRUE);
        }
        if (font_) DeleteObject(font_);
        if (headingFont_) DeleteObject(headingFont_);
        font_ = normal; headingFont_ = heading;
        RECT size{0, 0, scale(700), scale(595)};
        AdjustWindowRectExForDpi(&size, static_cast<DWORD>(GetWindowLongPtrW(window_, GWL_STYLE)),
            FALSE, WS_EX_CONTROLPARENT, dpi);
        SetWindowPos(window_, nullptr, 0, 0, size.right - size.left, size.bottom - size.top,
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    static BOOL CALLBACK Enumerate(HWND window, LPARAM parameter) {
        auto self = reinterpret_cast<Launcher*>(parameter);
        launcher::Target target;
        if (!launcher::Identify(window, target) || target.pid == GetCurrentProcessId() ||
            (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        DWORD cloaked = 0;
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        launcher::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target.pid));
        wchar_t path[32768] = {}; DWORD length = 32768;
        if (!process.value || !QueryFullProcessImageNameW(process.value, 0, path, &length)) return TRUE;
        if (_wcsicmp(launcher::fs::path(path).filename().c_str(), L"AIFrameEngineCompositor.exe") == 0) return TRUE;
        self->targets_.push_back(std::move(target));
        return TRUE;
    }

    void RefreshWindows() {
        HWND previous = pinned_.Valid() ? pinned_.Window() : nullptr;
        targets_.clear();
        SendDlgItemMessageW(window_, Windows, CB_RESETCONTENT, 0, 0);
        if (!EnumWindows(Enumerate, reinterpret_cast<LPARAM>(this))) {
            SetStatus(L"Could not list windows. Try Refresh again."); return;
        }
        std::sort(targets_.begin(), targets_.end(), [](const auto& a, const auto& b) { return a.title < b.title; });
        int selected = -1;
        for (size_t i = 0; i < targets_.size(); ++i) {
            const auto& target = targets_[i];
            auto label = target.title + L"  (PID " + std::to_wstring(target.pid) + L")";
            SendDlgItemMessageW(window_, Windows, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (target.window == previous) selected = static_cast<int>(i);
        }
        SendDlgItemMessageW(window_, Windows, CB_SETCURSEL, selected, 0);
        SelectTarget();
    }

    void SelectTarget() {
        int index = static_cast<int>(SendDlgItemMessageW(window_, Windows, CB_GETCURSEL, 0, 0));
        bool valid = index >= 0 && static_cast<size_t>(index) < targets_.size();
        if (valid) {
            const auto& target = targets_[index];
            valid = pinned_.Bind(target.window, target.pid, target.created);
        }
        EnableWindow(GetDlgItem(window_, Launch), valid && !child_);
        if (child_) return;
        SetStatus(valid ? L"Ready to launch. Model readiness and FG state appear in the in-game panel."
            : L"Select a running, restored game window. If it is missing, restore it and Refresh.");
    }

    void SetStatus(const std::wstring& text) { SetDlgItemTextW(window_, Status, text.c_str()); }
    void Fail(const std::wstring& text) {
        SetStatus(L"Launch did not start. Review the message, then try again.");
        MessageBoxW(window_, text.c_str(), L"AIFrameEngine", MB_OK | MB_ICONWARNING);
    }

    void BrowseExecutable() {
        wchar_t path[32768] = {};
        wcsncpy_s(path, executable_.c_str(), _TRUNCATE);
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = window_;
        dialog.lpstrFilter = L"AIFrameEngine compositor\0AIFrameEngineCompositor.exe\0\0";
        dialog.lpstrFile = path; dialog.nMaxFile = 32768;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&dialog)) {
            if (_wcsicmp(launcher::fs::path(path).filename().c_str(), L"AIFrameEngineCompositor.exe") != 0) {
                Fail(L"Select AIFrameEngineCompositor.exe from your build or release folder."); return;
            }
            executable_ = path;
            SetDlgItemTextW(window_, Exe, path);
        }
    }

    void Start() {
        if (smoke_) return; // UI smoke tests must never start capture or inference.
        if (child_) return;
        launcher::LaunchGuard lock;
        if (!lock.acquired) {
            Fail(L"Another launcher is starting a session. Please try again."); return;
        }
        std::wstring error;
        bool running = false;
        if (!launcher::CompositorRunning(running, error)) { Fail(error); return; }
        if (running) { Fail(L"A compositor is already running. Close it with Ctrl+Shift+F12 first."); return; }
        int index = static_cast<int>(SendDlgItemMessageW(window_, Windows, CB_GETCURSEL, 0, 0));
        if (index < 0 || static_cast<size_t>(index) >= targets_.size() || !pinned_.Valid() ||
            !launcher::StillSameTarget(targets_[index])) {
            Fail(L"The selected game window closed, changed, or was minimized. Restore it and Refresh."); return;
        }
        launcher::Options options;
        options.fsr1 = SendDlgItemMessageW(window_, Scaler, CB_GETCURSEL, 0, 0) == 0;
        options.fp16 = SendDlgItemMessageW(window_, Precision, CB_GETCURSEL, 0, 0) == 0;
        options.cache = SendDlgItemMessageW(window_, Cache, BM_GETCHECK, 0, 0) == BST_CHECKED;
        auto missing = launcher::MissingFiles(executable_, options);
        if (!missing.empty()) {
            std::wstring message = L"Missing or empty runtime files:\r\n";
            for (const auto& file : missing) message += L"\r\n" + file.wstring();
            Fail(message + L"\r\n\r\nChoose a complete compositor build with Browse. No files were changed."); return;
        }
        auto inherited = GetEnvironmentStringsW();
        if (!inherited) { Fail(launcher::ErrorText(GetLastError())); return; }
        auto environment = launcher::ReadEnvironment(inherited);
        FreeEnvironmentStringsW(inherited);
        auto block = launcher::ChildEnvironment(std::move(environment), targets_[index], options);
        PROCESS_INFORMATION process{};
        if (!launcher::StartChild(executable_, block, process, error)) { Fail(error); return; }
        CloseHandle(process.hThread);
        child_ = process.hProcess;
        for (int id : {Browse, Windows, Refresh, Scaler, Precision, Cache, Launch})
            EnableWindow(GetDlgItem(window_, id), FALSE);
        SetStatus(L"Compositor process started — FG readiness is not yet confirmed.\r\n"
            L"Check Ctrl+Shift+F6; once ready, press F9 to enable frame generation.");
        if (!SetTimer(window_, 1, 500, nullptr))
            SetStatus(L"Compositor started. Exit it with Ctrl+Shift+F12. Reopen the launcher for another session.");
        ShowWindow(window_, SW_MINIMIZE);
    }

    void PollChild() {
        if (!child_ || WaitForSingleObject(child_, 0) == WAIT_TIMEOUT) return;
        DWORD code = 0;
        GetExitCodeProcess(child_, &code);
        CloseHandle(child_); child_ = nullptr; KillTimer(window_, 1);
        for (int id : {Browse, Windows, Refresh, Scaler, Precision, Cache})
            EnableWindow(GetDlgItem(window_, id), TRUE);
        RefreshWindows();
        SetStatus(L"Compositor exited (code " + std::to_wstring(code) + L"). " +
            (code ? L"Open the log for details." : L"You can select a window and launch again."));
    }

    void OpenLog() {
        constexpr auto path = L"C:\\Temp\\aiframe_compositor.log";
        if (!launcher::NonemptyFile(path)) {
            MessageBoxW(window_, L"No compositor log is available yet at C:\\Temp\\aiframe_compositor.log.",
                L"AIFrameEngine", MB_OK | MB_ICONINFORMATION); return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", path, nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            MessageBoxW(window_, L"Could not open the log. Open C:\\Temp\\aiframe_compositor.log manually.",
                L"AIFrameEngine", MB_OK | MB_ICONWARNING);
    }

    bool ControlsFit() const {
        RECT client{};
        if (!GetClientRect(window_, &client)) return false;
        for (const auto& item : items_) {
            RECT rect{};
            if (!GetWindowRect(item.window, &rect)) return false;
            MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&rect), 2);
            if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom)
                return false;
        }
        return true;
    }

    static LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        try {
            switch (message) {
            case WM_CREATE: self->CreateControls(); return 0;
            case WM_CTLCOLORSTATIC:
                SetTextColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(reinterpret_cast<HDC>(wparam), GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(self->background_);
            case WM_DPICHANGED: {
                const auto rect = reinterpret_cast<RECT*>(lparam);
                SetWindowPos(window, nullptr, rect->left, rect->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                self->ResizeForDpi(HIWORD(wparam)); return 0;
            }
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                case Browse: self->BrowseExecutable(); break;
                case Refresh: self->RefreshWindows(); break;
                case Windows: if (HIWORD(wparam) == CBN_SELCHANGE) self->SelectTarget(); break;
                case Launch: case IDOK: if (IsWindowEnabled(GetDlgItem(window, Launch))) self->Start(); break;
                case Log: self->OpenLog(); break;
                case Help:
                    MessageBoxW(window,
                        L"1. Start your game in windowed mode and choose it in the list.\r\n"
                        L"2. Select the compositor executable from a complete build folder.\r\n"
                        L"3. Launch, then check the in-game panel with Ctrl+Shift+F6.\r\n"
                        L"4. When the model is ready, press F9 to enable frame generation.\r\n\r\n"
                        L"F10: scaler comparison   F11: sharpening presets\r\n"
                        L"Ctrl+Shift+Up/Down: adjust sharpening\r\n"
                        L"Ctrl+Shift+F12: exit the compositor\r\n\r\n"
                        L"720p is best tested. Native 1080p FG remains unreliable. GPU headroom matters.\r\n"
                        L"The GUI clears inherited AIFRAME_* overrides for its child process.\r\n"
                        L"Use the PowerShell launcher for custom diagnostics or adapter overrides.\r\n"
                        L"Closing this launcher does not stop an active compositor.",
                        L"Using AIFrameEngine", MB_OK | MB_ICONINFORMATION); break;
                }
                return 0;
            case WM_TIMER:
                if (wparam == 2) {
                    self->exitCode_ = self->ControlsFit() ? 0 : 2;
                    DestroyWindow(window); return 0;
                }
                self->PollChild(); return 0;
            case WM_CLOSE: DestroyWindow(window); return 0;
            case WM_DESTROY: PostQuitMessage(self->exitCode_); return 0;
            }
        } catch (const std::exception&) {
            MessageBoxW(window, L"The launcher encountered an error. No compositor was intentionally stopped.",
                L"AIFrameEngine", MB_OK | MB_ICONERROR);
            if (message == WM_CREATE) return -1;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HWND window_ = nullptr;
    HFONT font_ = nullptr, headingFont_ = nullptr;
    HBRUSH background_ = nullptr;
    HANDLE child_ = nullptr;
    launcher::fs::path executable_;
    std::vector<Item> items_;
    std::vector<launcher::Target> targets_;
    CaptureTarget pinned_;
    bool smoke_ = false;
    int exitCode_ = 0;
};
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 1;
    wchar_t module[32768] = {};
    DWORD size = GetModuleFileNameW(nullptr, module, 32768);
    if (!size || size == 32768) return 1;
    auto executable = launcher::fs::path(module).parent_path() / L"AIFrameEngineCompositor.exe";
    bool smoke = false, invalid = false;
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--smoke-test") == 0) smoke = true;
        else if (wcscmp(argv[i], L"--compositor") == 0 && i + 1 < argc) executable = argv[++i];
        else invalid = true;
    }
    LocalFree(argv);
    if (invalid || !executable.is_absolute() ||
        _wcsicmp(executable.filename().c_str(), L"AIFrameEngineCompositor.exe") != 0) {
        MessageBoxW(nullptr, L"Usage: AIFrameEngineLauncher.exe [--compositor <absolute path to AIFrameEngineCompositor.exe>]",
            L"AIFrameEngine", MB_OK | MB_ICONERROR); return 1;
    }
    Launcher app(executable, smoke);
    return app.Run(instance, show);
}