#include "tray_icon.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>
#include <thread>

#include "runtime_log.h"
#include "runtime_product.h"
#include "settings_overlay.h"
#include "updater.h"
#endif

namespace TrayIcon {
#if defined(_WIN32)
namespace {

#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NINF_KEY
#define NINF_KEY 0x1
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (NIN_SELECT | NINF_KEY)
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif

constexpr UINT kTrayMessage = WM_APP + 1;  // the icon's mouse and keyboard messages, to the hidden window
constexpr UINT kIconId = 1;
enum : UINT { kMenuShow = 1, kMenuOptions, kMenuQuit };

std::atomic<HWND> g_trayWindow{nullptr};  // the hidden window that owns the icon (the tray thread's)
std::atomic<HWND> g_gameWindow{nullptr};
std::atomic<bool> g_added{false};
std::atomic<bool> g_removed{false};
UINT g_taskbarCreated = 0;  // Explorer's broadcast when it (re)starts: its new tray has to be given the icon again
HICON g_icon = nullptr;
std::wstring g_tooltip;

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

// The app icon (the program's icon resource 1, PublicProducts.cmake's app_icon.rc) at the notification area's size,
// so Windows does not shrink the 32 px picture itself. SDL has made the process DPI aware by now, so the small icon
// size is the system DPI's (20 px at 125%, 24 at 150%).
HICON LoadTrayIcon() {
    auto* icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                               LR_DEFAULTCOLOR));
    return icon != nullptr ? icon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION
}

bool AddIcon() {
    if (g_removed.load()) return false;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_trayWindow.load();
    data.uID = kIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = g_icon;
    lstrcpynW(data.szTip, g_tooltip.c_str(), static_cast<int>(sizeof(data.szTip) / sizeof(data.szTip[0])));
    if (!Shell_NotifyIconW(NIM_ADD, &data)) return false;
    data.uVersion = NOTIFYICON_VERSION_4;  // NIN_SELECT for a click, WM_CONTEXTMENU with the anchor point
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    g_added = true;
    if (g_removed.load()) Remove();  // the process began to end while the icon went in
    return true;
}

// The game window to the front, restored if it was minimized. A click on the tray icon lets this process take the
// foreground.
void ShowGame() {
    const HWND game = g_gameWindow.load();
    if (game == nullptr || !IsWindow(game)) return;
    if (IsIconic(game)) ShowWindowAsync(game, SW_RESTORE);
    SetForegroundWindow(game);
}

void ShowMenu(HWND owner, POINT at) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kMenuShow, L"Show the game");
    AppendMenuW(menu, MF_STRING, kMenuOptions, L"Options");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");
    SetMenuDefaultItem(menu, kMenuShow, FALSE);
    // The menu closes on a click elsewhere only while its owner is the foreground window, and the WM_NULL after it
    // lets the next right-click open it again (the documented TrackPopupMenu notes for notification icons).
    SetForegroundWindow(owner);
    const UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    const UINT command = static_cast<UINT>(TrackPopupMenuEx(
        menu, align | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, at.x, at.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (command) {
    case kMenuShow:
        ShowGame();
        break;
    case kMenuOptions:
        RT_LOGF(RT_TAG_RUNTIME, "[tray] Options from the tray menu\n");
        settings_overlay::RequestOptionsOpen();
        ShowGame();
        break;
    case kMenuQuit: {
        // The same way out as closing the window (aurora_events.h ExitForAuroraWindowClose, which removes the icon).
        RT_LOGF(RT_TAG_RUNTIME, "[tray] Quit from the tray menu\n");
        SDL_Event event{};
        event.type = SDL_EVENT_QUIT;
        if (!SDL_PushEvent(&event)) RT_LOGF(RT_TAG_RUNTIME, "[tray] could not ask the game to quit: %s\n", SDL_GetError());
        break;
    }
    default:
        break;
    }
}

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kTrayMessage) {
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowGame();
            break;
        case WM_CONTEXTMENU:
            ShowMenu(window, POINT{static_cast<short>(LOWORD(wParam)), static_cast<short>(HIWORD(wParam))});
            break;
        default:
            break;
        }
        return 0;
    }
    if (g_taskbarCreated != 0 && message == g_taskbarCreated) {
        g_added = false;
        if (AddIcon()) RT_LOGF(RT_TAG_RUNTIME, "[tray] icon added again (the taskbar restarted)\n");
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void TrayThread() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = TrayWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"OtherMRecompiledTray";
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        RT_LOGF(RT_TAG_RUNTIME, "[tray] no icon: the window class failed (error %lu)\n", GetLastError());
        return;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Hidden and never shown; a top-level window rather than a message-only one, because only top-level windows get
    // TaskbarCreated. WS_EX_TOOLWINDOW keeps it off the taskbar and Alt+Tab.
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0,
                                        nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "[tray] no icon: the window failed (error %lu)\n", GetLastError());
        return;
    }
    g_trayWindow = window;
    g_icon = LoadTrayIcon();
    if (AddIcon()) {
        RT_LOGF(RT_TAG_RUNTIME, "[tray] icon added (%dx%d)\n", GetSystemMetrics(SM_CXSMICON),
                GetSystemMetrics(SM_CYSMICON));
    } else if (!g_removed.load()) {
        // Explorer not running yet (a start at sign-in): TaskbarCreated adds it when the taskbar comes up.
        RT_LOGF(RT_TAG_RUNTIME, "[tray] icon not added yet (error %lu); waiting for the taskbar\n", GetLastError());
    }
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

} // namespace

void Start() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    int windowCount = 0;
    if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
        if (windowCount > 0) {
            g_gameWindow = static_cast<HWND>(
                SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        }
        SDL_free(windows);
    }
    if (g_gameWindow.load() == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "[tray] no icon: no game window\n");
        return;
    }
    std::string tooltip(RuntimeProduct::Active().displayName);
    if (const std::string version = Updater::Current(); !version.empty()) tooltip += " " + version;
    g_tooltip = Widen(tooltip);
    std::thread(TrayThread).detach();
}

void Remove() {
    g_removed = true;
    if (!g_added.exchange(false)) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_trayWindow.load();
    data.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}
#else
void Start() {}
void Remove() {}
#endif
} // namespace TrayIcon
