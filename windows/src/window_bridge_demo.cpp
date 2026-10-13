#include "swine/windows/window_bridge.hpp"

#include <iostream>
#include <iterator>

namespace {
using swine::windows::WindowEventKind;
DWORD demo_thread_id = 0;

const wchar_t* title_of(HWND hwnd) {
  static thread_local wchar_t title[512];
  const int length = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
  title[length > 0 ? length : 0] = L'\0';
  return title;
}

const wchar_t* event_name(WindowEventKind kind) {
  switch (kind) {
    case WindowEventKind::created: return L"created";
    case WindowEventKind::destroyed: return L"destroyed";
    case WindowEventKind::shown: return L"shown";
    case WindowEventKind::hidden: return L"hidden";
    case WindowEventKind::moved_or_resized: return L"moved/resized";
    case WindowEventKind::title_changed: return L"title changed";
    case WindowEventKind::foreground_changed: return L"foreground changed";
  }
  return L"unknown";
}

BOOL WINAPI on_console_control(DWORD) {
  PostThreadMessageW(demo_thread_id, WM_QUIT, 0, 0);
  return TRUE;
}
}  // namespace

int wmain() {
  using namespace swine::windows;
  WindowBridge bridge([](const WindowEvent& event) {
    std::wcout << L"[event] " << event_name(event.kind) << L" hwnd=" << event.hwnd
               << L" title=\"" << title_of(event.hwnd) << L"\"\n";
  });

  for (HWND hwnd : bridge.enumerate()) {
    bridge.assign_workspace(hwnd, 1);
    std::wcout << L"[window] hwnd=" << hwnd << L" title=\"" << title_of(hwnd) << L"\"\n";
  }

  demo_thread_id = GetCurrentThreadId();
  SetConsoleCtrlHandler(on_console_control, TRUE);
  std::wcout << L"Bridge active. Ctrl+Alt+1..9 switches visibility groups; close this window to exit.\n";
  for (int key = '1'; key <= '9'; ++key) {
    RegisterHotKey(nullptr, key, MOD_CONTROL | MOD_ALT, static_cast<UINT>(key));
  }

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_HOTKEY) {
      const auto workspace = static_cast<WorkspaceId>(message.wParam - '0');
      bridge.show_workspace(workspace);
      std::wcout << L"[workspace] " << workspace << L"\n";
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  for (int key = '1'; key <= '9'; ++key) UnregisterHotKey(nullptr, key);
  return 0;
}
