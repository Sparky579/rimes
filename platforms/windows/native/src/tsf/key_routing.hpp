#pragma once

#include <Windows.h>

#include <cstdint>

#include "../core/broker_protocol.hpp"

namespace rimes::windows::tsf {

// Claiming an idle digit in OnTestKeyDown and later rejecting it in OnKeyDown
// can lose the native character in dialog EDIT controls. Keep ordinary digits
// on the host path from the first callback. Composition needs them for
// candidate selection, and Buffer capture needs them even without preedit.
inline bool ShouldPassIdleNumberKeyToHost(WPARAM virtual_key,
                                          std::uint32_t modifiers,
                                          bool composing,
                                          bool capturing) noexcept {
  if (composing || capturing) return false;
  const bool shifted =
      (modifiers & static_cast<std::uint32_t>(core::KeyModifiers::kShift)) != 0;
  // Shift+number-row produces punctuation, which still belongs to Rime's
  // punctuator. Shift does not change a numpad digit into punctuation.
  return (!shifted && virtual_key >= '0' && virtual_key <= '9') ||
         (virtual_key >= VK_NUMPAD0 && virtual_key <= VK_NUMPAD9);
}

}  // namespace rimes::windows::tsf
