#include "swine/windows/tile_layout.hpp"
#include "swine/windows/window_bridge.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {
using namespace swine::windows;

constexpr int kWorkspaceCount = 9;
constexpr int kFirstWorkspaceHotkey = 100;
constexpr int kFirstMoveHotkey = 200;
constexpr int kRescanHotkey = 300;
constexpr int kExitHotkey = 301;
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;

WindowId id_of(HWND hwnd) {
  return static_cast<WindowId>(reinterpret_cast<std::uintptr_t>(hwnd));
}

HWND hwnd_of(WindowId id) {
  return reinterpret_cast<HWND>(static_cast<std::uintptr_t>(id));
}

class NativeShell {
 public:
  NativeShell()
      : bridge_([this](const WindowEvent& event) { on_window_event(event); }) {
    RECT work_area{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
      const TileRect bounds{work_area.left, work_area.top,
                            work_area.right - work_area.left,
                            work_area.bottom - work_area.top};
      for (auto& layout : layouts_) layout.set_bounds(bounds);
    }
    sync_windows();
  }

  int run() {
    register_hotkeys();
    apply_layout(active_workspace_);
    std::wcout << L"sway-hwnd active. Ctrl+Alt+1..9 switches workspace; "
                  L"Ctrl+Alt+Shift+1..9 moves the focused window; "
                  L"Ctrl+Alt+arrows changes focus; Ctrl+Alt+R rescans; "
                  L"Ctrl+Alt+Q exits.\n";

    MSG message{};
    BOOL result;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
      if (message.message == WM_HOTKEY) {
        on_hotkey(static_cast<int>(message.wParam));
        continue;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    unregister_hotkeys();
    return result == -1 ? 1 : 0;
  }

 private:
  void register_one(int id, UINT modifiers, UINT key, const wchar_t* description) {
    if (RegisterHotKey(nullptr, id, modifiers, key)) {
      registered_hotkeys_.push_back(id);
    } else {
      std::wcerr << L"Could not register " << description
                 << L" (another app may own that shortcut).\n";
    }
  }

  void register_hotkeys() {
    for (int i = 0; i < kWorkspaceCount; ++i) {
      const UINT key = static_cast<UINT>('1' + i);
      register_one(kFirstWorkspaceHotkey + i, kHotkeyModifiers, key,
                   L"workspace shortcut");
      register_one(kFirstMoveHotkey + i, kHotkeyModifiers | MOD_SHIFT, key,
                   L"move-to-workspace shortcut");
    }
    register_one(kRescanHotkey, kHotkeyModifiers, 'R', L"rescan shortcut");
    register_one(kExitHotkey, kHotkeyModifiers, 'Q', L"exit shortcut");
    register_one(400, kHotkeyModifiers, VK_LEFT, L"focus-left shortcut");
    register_one(401, kHotkeyModifiers, VK_RIGHT, L"focus-right shortcut");
    register_one(402, kHotkeyModifiers, VK_UP, L"focus-up shortcut");
    register_one(403, kHotkeyModifiers, VK_DOWN, L"focus-down shortcut");
  }

  void unregister_hotkeys() {
    for (int id : registered_hotkeys_) UnregisterHotKey(nullptr, id);
    registered_hotkeys_.clear();
  }

  void sync_windows() {
    for (HWND hwnd : bridge_.enumerate()) {
      if (workspace_by_window_.contains(hwnd)) continue;
      workspace_by_window_[hwnd] = active_workspace_;
      bridge_.assign_workspace(hwnd, static_cast<WorkspaceId>(active_workspace_));
      layouts_[active_workspace_ - 1].add(id_of(hwnd));
      std::wcout << L"[manage] hwnd=" << hwnd << L"\n";
    }
    apply_layout(active_workspace_);
  }

  void apply_layout(int workspace) {
    const auto& layout = layouts_[workspace - 1];
    for (const auto& placement : layout.placements()) {
      const HWND hwnd = hwnd_of(placement.window);
      if (!IsWindow(hwnd)) continue;
      const RECT bounds{placement.bounds.x, placement.bounds.y,
                        placement.bounds.x + placement.bounds.width,
                        placement.bounds.y + placement.bounds.height};
      bridge_.configure(hwnd, bounds);
    }
  }

  void switch_workspace(int workspace) {
    if (workspace < 1 || workspace > kWorkspaceCount) return;
    active_workspace_ = workspace;
    bridge_.show_workspace(static_cast<WorkspaceId>(workspace));
    apply_layout(workspace);
    if (auto focused = layouts_[workspace - 1].focused()) {
      bridge_.activate(hwnd_of(*focused));
    }
    std::wcout << L"[workspace] " << workspace << L"\n";
  }

  void move_focused_to_workspace(int workspace) {
    if (workspace < 1 || workspace > kWorkspaceCount || workspace == active_workspace_) return;
    auto focused = layouts_[active_workspace_ - 1].focused();
    if (!focused) return;
    const HWND hwnd = hwnd_of(*focused);
    layouts_[active_workspace_ - 1].remove(*focused);
    layouts_[workspace - 1].add(*focused);
    workspace_by_window_[hwnd] = workspace;
    bridge_.assign_workspace(hwnd, static_cast<WorkspaceId>(workspace));
    apply_layout(active_workspace_);
    switch_workspace(workspace);
  }

  void focus_direction(Direction direction) {
    auto& layout = layouts_[active_workspace_ - 1];
    const auto target = layout.neighbor(direction);
    if (!target) return;
    layout.focus(*target);
    bridge_.activate(hwnd_of(*target));
  }

  void on_hotkey(int id) {
    if (id >= kFirstWorkspaceHotkey && id < kFirstWorkspaceHotkey + kWorkspaceCount) {
      switch_workspace(id - kFirstWorkspaceHotkey + 1);
    } else if (id >= kFirstMoveHotkey && id < kFirstMoveHotkey + kWorkspaceCount) {
      move_focused_to_workspace(id - kFirstMoveHotkey + 1);
    } else if (id == kRescanHotkey) {
      sync_windows();
    } else if (id == kExitHotkey) {
      PostQuitMessage(0);
    } else if (id == 400) {
      focus_direction(Direction::left);
    } else if (id == 401) {
      focus_direction(Direction::right);
    } else if (id == 402) {
      focus_direction(Direction::up);
    } else if (id == 403) {
      focus_direction(Direction::down);
    }
  }

  void on_window_event(const WindowEvent& event) {
    if (event.kind == WindowEventKind::destroyed) {
      if (const auto it = workspace_by_window_.find(event.hwnd);
          it != workspace_by_window_.end()) {
        layouts_[it->second - 1].remove(id_of(event.hwnd));
        workspace_by_window_.erase(it);
        apply_layout(active_workspace_);
      }
    } else if (event.kind == WindowEventKind::foreground_changed) {
      const auto it = workspace_by_window_.find(event.hwnd);
      if (it != workspace_by_window_.end() && it->second == active_workspace_) {
        layouts_[active_workspace_ - 1].focus(id_of(event.hwnd));
      }
    }
  }

  std::array<TileLayout, kWorkspaceCount> layouts_;
  WindowBridge bridge_;
  std::unordered_map<HWND, int> workspace_by_window_;
  std::vector<int> registered_hotkeys_;
  int active_workspace_ = 1;
};
}  // namespace

int wmain() {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  NativeShell shell;
  return shell.run();
}
