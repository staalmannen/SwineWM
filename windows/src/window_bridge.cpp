#include "swine/windows/window_bridge.hpp"

namespace swine::windows {
namespace {
bool is_managed_window(HWND hwnd) {
  if (!IsWindow(hwnd) || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
    return false;
  }
  const auto style = static_cast<LONG_PTR>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
  if ((style & WS_EX_TOOLWINDOW) != 0) return false;
  DWORD process_id = 0;
  GetWindowThreadProcessId(hwnd, &process_id);
  return process_id != GetCurrentProcessId();
}
}  // namespace

WindowBridge* WindowBridge::active_ = nullptr;

WindowBridge::WindowBridge(EventCallback on_event) : on_event_(std::move(on_event)) {
  // WinEvent hooks are process-wide callbacks. This bridge is intended to be
  // owned as a singleton by the shell's UI thread.
  active_ = this;
  hook_ = SetWinEventHook(EVENT_MIN, EVENT_MAX, nullptr, &WindowBridge::on_win_event,
                         0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

WindowBridge::~WindowBridge() {
  if (hook_) UnhookWinEvent(hook_);
  if (active_ == this) active_ = nullptr;
}

std::vector<HWND> WindowBridge::enumerate() const {
  std::vector<HWND> result;
  EnumWindows([](HWND hwnd, LPARAM data) -> BOOL {
    if (is_managed_window(hwnd)) {
      reinterpret_cast<std::vector<HWND>*>(data)->push_back(hwnd);
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&result));
  return result;
}

bool WindowBridge::dispatch_one() {
  MSG message{};
  const BOOL result = GetMessageW(&message, nullptr, 0, 0);
  if (result <= 0) return false;
  TranslateMessage(&message);
  DispatchMessageW(&message);
  return true;
}

bool WindowBridge::configure(HWND hwnd, const RECT& bounds) const {
  return SetWindowPos(hwnd, nullptr, bounds.left, bounds.top,
                      bounds.right - bounds.left, bounds.bottom - bounds.top,
                      SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER) != FALSE;
}

bool WindowBridge::activate(HWND hwnd) const {
  if (!IsWindow(hwnd)) return false;
  if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
  SetForegroundWindow(hwnd);
  return GetForegroundWindow() == hwnd;
}

bool WindowBridge::minimize(HWND hwnd) const {
  if (!IsWindow(hwnd)) return false;
  ShowWindowAsync(hwnd, SW_MINIMIZE);
  return true;
}

bool WindowBridge::restore(HWND hwnd) const {
  if (!IsWindow(hwnd)) return false;
  ShowWindowAsync(hwnd, SW_RESTORE);
  return true;
}

bool WindowBridge::close(HWND hwnd) const {
  return IsWindow(hwnd) && PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
}

void WindowBridge::assign_workspace(HWND hwnd, WorkspaceId workspace) {
  if (IsWindow(hwnd)) workspaces_[hwnd] = workspace;
}

bool WindowBridge::show_workspace(WorkspaceId workspace) const {
  bool succeeded = true;
  for (const auto& [hwnd, assigned] : workspaces_) {
    if (!IsWindow(hwnd)) {
      succeeded = false;
      continue;
    }
    const auto command = assigned == workspace ? SW_SHOWNOACTIVATE : SW_HIDE;
    ShowWindowAsync(hwnd, command);
  }
  return succeeded;
}

void CALLBACK WindowBridge::on_win_event(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                        LONG object_id, LONG child_id, DWORD, DWORD) {
  if (!active_ || !hwnd || object_id != OBJID_WINDOW || child_id != CHILDID_SELF) return;
  WindowEventKind kind;
  switch (event) {
    case EVENT_OBJECT_CREATE: kind = WindowEventKind::created; break;
    case EVENT_OBJECT_DESTROY: kind = WindowEventKind::destroyed; active_->workspaces_.erase(hwnd); break;
    case EVENT_OBJECT_SHOW: kind = WindowEventKind::shown; break;
    case EVENT_OBJECT_HIDE: kind = WindowEventKind::hidden; break;
    case EVENT_OBJECT_LOCATIONCHANGE: kind = WindowEventKind::moved_or_resized; break;
    case EVENT_OBJECT_NAMECHANGE: kind = WindowEventKind::title_changed; break;
    case EVENT_SYSTEM_FOREGROUND: kind = WindowEventKind::foreground_changed; break;
    default: return;
  }
  active_->emit(kind, hwnd);
}

void WindowBridge::emit(WindowEventKind kind, HWND hwnd) const {
  if (on_event_) on_event_({kind, hwnd});
}

}  // namespace swine::windows
