#pragma once

#include <optional>
#include <string_view>

namespace embedded_libraries {

// Returns a view directly into this DLL's read-only resource section.
// The view remains valid until the DLL is unloaded.
[[nodiscard]] std::optional<std::string_view> find(std::string_view name);

}  // namespace embedded_libraries
