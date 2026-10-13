#include "swine/windows/tile_layout.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace swine::windows {

struct TileLayout::Node {
  std::optional<WindowId> window;
  bool split_left_to_right = true;
  std::unique_ptr<Node> first;
  std::unique_ptr<Node> second;

  [[nodiscard]] bool leaf() const { return window.has_value(); }
};

namespace {

TileLayout::Node* find_leaf(TileLayout::Node* node, WindowId window) {
  if (!node) return nullptr;
  if (node->leaf()) return node->window == window ? node : nullptr;
  if (auto* found = find_leaf(node->first.get(), window)) return found;
  return find_leaf(node->second.get(), window);
}

TileLayout::Node* first_leaf(TileLayout::Node* node) {
  if (!node || node->leaf()) return node;
  if (auto* first = first_leaf(node->first.get())) return first;
  return first_leaf(node->second.get());
}

bool remove_leaf(std::unique_ptr<TileLayout::Node>& node, WindowId window) {
  if (!node || node->leaf()) return false;
  if (node->first->leaf() && node->first->window == window) {
    node = std::move(node->second);
    return true;
  }
  if (node->second->leaf() && node->second->window == window) {
    node = std::move(node->first);
    return true;
  }
  return remove_leaf(node->first, window) || remove_leaf(node->second, window);
}

void collect(const TileLayout::Node* node, TileRect bounds,
             std::vector<TilePlacement>& result) {
  if (!node) return;
  if (node->leaf()) {
    result.push_back({*node->window, bounds});
    return;
  }
  if (node->split_left_to_right) {
    const int first_width = bounds.width / 2;
    collect(node->first.get(), {bounds.x, bounds.y, first_width, bounds.height}, result);
    collect(node->second.get(), {bounds.x + first_width, bounds.y,
                                 bounds.width - first_width, bounds.height}, result);
  } else {
    const int first_height = bounds.height / 2;
    collect(node->first.get(), {bounds.x, bounds.y, bounds.width, first_height}, result);
    collect(node->second.get(), {bounds.x, bounds.y + first_height, bounds.width,
                                 bounds.height - first_height}, result);
  }
}

void find_rect(const TileLayout::Node* node, TileRect bounds, WindowId window,
               std::optional<TileRect>& result) {
  if (!node) return;
  if (node->leaf()) {
    if (node->window == window) result = bounds;
    return;
  }
  if (node->split_left_to_right) {
    const int first_width = bounds.width / 2;
    find_rect(node->first.get(), {bounds.x, bounds.y, first_width, bounds.height}, window, result);
    find_rect(node->second.get(), {bounds.x + first_width, bounds.y,
                                   bounds.width - first_width, bounds.height}, window, result);
  } else {
    const int first_height = bounds.height / 2;
    find_rect(node->first.get(), {bounds.x, bounds.y, bounds.width, first_height}, window, result);
    find_rect(node->second.get(), {bounds.x, bounds.y + first_height, bounds.width,
                                   bounds.height - first_height}, window, result);
  }
}

}  // namespace

TileLayout::TileLayout() = default;
TileLayout::~TileLayout() = default;
TileLayout::TileLayout(TileLayout&&) noexcept = default;
TileLayout& TileLayout::operator=(TileLayout&&) noexcept = default;

void TileLayout::add(WindowId window) {
  if (auto* existing = find_leaf(root_.get(), window)) {
    (void)existing;
    focused_ = window;
    return;
  }
  auto added = std::make_unique<Node>();
  added->window = window;
  if (!root_) {
    root_ = std::move(added);
  } else {
    auto* target = focused_ ? find_leaf(root_.get(), *focused_) : first_leaf(root_.get());
    if (!target) return;
    std::optional<TileRect> target_bounds;
    find_rect(root_.get(), bounds_, *target->window, target_bounds);
    const auto old_window = target->window;
    target->window.reset();
    const auto area = target_bounds.value_or(bounds_);
    target->split_left_to_right = area.width >= area.height;
    target->first = std::make_unique<Node>();
    target->first->window = old_window;
    target->second = std::move(added);
  }
  focused_ = window;
}

bool TileLayout::remove(WindowId window) {
  if (!root_) return false;
  if (root_->leaf()) {
    if (root_->window != window) return false;
    root_.reset();
    focused_.reset();
    return true;
  }
  if (!remove_leaf(root_, window)) return false;
  if (focused_ == window) {
    std::vector<TilePlacement> current;
    collect(root_.get(), bounds_, current);
    focused_ = current.empty() ? std::nullopt : std::optional<WindowId>(current.front().window);
  }
  return true;
}

bool TileLayout::focus(WindowId window) {
  if (!find_leaf(root_.get(), window)) return false;
  focused_ = window;
  return true;
}

std::optional<WindowId> TileLayout::focused() const { return focused_; }

std::optional<WindowId> TileLayout::neighbor(Direction direction) const {
  if (!focused_) return std::nullopt;
  std::optional<TileRect> current;
  find_rect(root_.get(), bounds_, *focused_, current);
  if (!current) return std::nullopt;
  const int cx = current->x + current->width / 2;
  const int cy = current->y + current->height / 2;
  long long best_score = 0;
  std::optional<WindowId> best;
  for (const auto& tile : placements()) {
    if (tile.window == *focused_) continue;
    const int tx = tile.bounds.x + tile.bounds.width / 2;
    const int ty = tile.bounds.y + tile.bounds.height / 2;
    const int dx = tx - cx;
    const int dy = ty - cy;
    const bool in_direction = direction == Direction::left ? dx < 0
        : direction == Direction::right ? dx > 0
        : direction == Direction::up ? dy < 0 : dy > 0;
    if (!in_direction) continue;
    const long long primary = direction == Direction::left || direction == Direction::right
        ? std::abs(dx) : std::abs(dy);
    const long long secondary = direction == Direction::left || direction == Direction::right
        ? std::abs(dy) : std::abs(dx);
    const long long score = primary * 4 + secondary;
    if (!best || score < best_score) {
      best = tile.window;
      best_score = score;
    }
  }
  return best;
}

void TileLayout::set_bounds(TileRect bounds) {
  bounds.width = std::max(0, bounds.width);
  bounds.height = std::max(0, bounds.height);
  bounds_ = bounds;
}

std::vector<TilePlacement> TileLayout::placements() const {
  std::vector<TilePlacement> result;
  collect(root_.get(), bounds_, result);
  return result;
}

bool TileLayout::empty() const { return !root_; }

}  // namespace swine::windows
