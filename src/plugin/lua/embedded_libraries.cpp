#include "embedded_libraries.hpp"

#include "embedded_library_resources.h"

#include <array>
#include <windows.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace embedded_libraries {
namespace {

struct ResourceEntry {
  std::string_view name;
  int id;
};

constexpr std::array kResources{
    ResourceEntry{"nl_compat.lua", IDR_LUA_NL_COMPAT},
    ResourceEntry{"offsets.lua", IDR_LUA_OFFSETS},
    ResourceEntry{"entity_offsets.lua", IDR_LUA_ENTITY_OFFSETS},
    ResourceEntry{"entity_compat.lua", IDR_LUA_ENTITY_COMPAT},
    ResourceEntry{"panorama_compat.lua", IDR_LUA_PANORAMA_COMPAT},
};

}  // namespace

std::optional<std::string_view> find(std::string_view name) {
  const auto module = reinterpret_cast<HMODULE>(&__ImageBase);
  for (const auto& entry : kResources) {
    if (entry.name != name) continue;

    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(entry.id), RT_RCDATA);
    if (!resource) return std::nullopt;

    const DWORD size = SizeofResource(module, resource);
    const HGLOBAL loaded = LoadResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || size == 0) return std::nullopt;
    return std::string_view(static_cast<const char*>(bytes), size);
  }
  return std::nullopt;
}

}  // namespace embedded_libraries
