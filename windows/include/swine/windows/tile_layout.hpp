#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace swine::windows {

using WindowId = std::uint64_t;

struct TileRect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

enum class Direction { left, right, up, down };

struct TilePlacement {
  WindowId window;
  TileRect bounds;
};

// Platform-independent binary split tree. It computes policy and geometry;
// the caller applies placements to HWNDs or Wayland surfaces.
class TileLayout {
 public:
  struct Node;  // Opaque tree node used by the implementation.

  TileLayout();
  ~TileLayout();
  TileLayout(TileLayout&&) noexcept;
  TileLayout& operator=(TileLayout&&) noexcept;
  TileLayout(const TileLayout&) = delete;
  TileLayout& operator=(const TileLayout&) = delete;

  // Add a window by splitting the focused tile. The first window occupies the
  // full output. Re-adding an existing ID focuses it without duplicating it.
  void add(WindowId window);
  bool remove(WindowId window);
  bool focus(WindowId window);
  [[nodiscard]] std::optional<WindowId> focused() const;
  [[nodiscard]] std::optional<WindowId> neighbor(Direction direction) const;

  void set_bounds(TileRect bounds);
  [[nodiscard]] std::vector<TilePlacement> placements() const;
  [[nodiscard]] bool empty() const;

 private:
  std::unique_ptr<Node> root_;
  TileRect bounds_{};
  std::optional<WindowId> focused_;
};

}  // namespace swine::windows
