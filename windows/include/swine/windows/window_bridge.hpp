#pragma once

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace swine::windows {

using WorkspaceId = std::uint32_t;

enum class WindowEventKind {
  created,
  destroyed,
  shown,
  hidden,
  moved_or_resized,
  title_changed,
  foreground_changed,
};

struct WindowEvent {
  WindowEventKind kind;
  HWND hwnd;
};

// Thin HWND adapter. It deliberately leaves layout policy to its caller and
// does not subclass or reparent application windows.
class WindowBridge {
 public:
  using EventCallback = std::function<void(const WindowEvent&)>;

  explicit WindowBridge(EventCallback on_event);
  ~WindowBridge();
  WindowBridge(const WindowBridge&) = delete;
  WindowBridge& operator=(const WindowBridge&) = delete;

  // Enumerates ordinary top-level windows, excluding this process and
  // tool-only windows. Call again after startup to obtain the initial set.
  [[nodiscard]] std::vector<HWND> enumerate() const;

  // Dispatch this on the owning UI thread to deliver WinEvent notifications.
  // Returns false when WM_QUIT is received.
  bool dispatch_one();

  [[nodiscard]] bool configure(HWND hwnd, const RECT& bounds) const;
  [[nodiscard]] bool activate(HWND hwnd) const;
  [[nodiscard]] bool minimize(HWND hwnd) const;
  [[nodiscard]] bool restore(HWND hwnd) const;
  [[nodiscard]] bool close(HWND hwnd) const;

  // Workspaces are implemented as visibility groups; HWND ownership remains
  // with the application and DWM continues composing each native window.
  void assign_workspace(HWND hwnd, WorkspaceId workspace);
  [[nodiscard]] bool show_workspace(WorkspaceId workspace) const;

 private:
  static void CALLBACK on_win_event(HWINEVENTHOOK, DWORD, HWND, LONG, LONG,
                                    DWORD, DWORD);
  void emit(WindowEventKind kind, HWND hwnd) const;

  EventCallback on_event_;
  HWINEVENTHOOK hook_ = nullptr;
  std::unordered_map<HWND, WorkspaceId> workspaces_;
  static WindowBridge* active_;
};

}  // namespace swine::windows
