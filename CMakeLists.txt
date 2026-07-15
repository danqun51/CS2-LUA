cmake_minimum_required(VERSION 3.24)
project(CS2LuaPluginSkeleton LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

FetchContent_Declare(luajit
  GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
  GIT_TAG v2.1
  GIT_SHALLOW TRUE
)
FetchContent_GetProperties(luajit)
if(NOT luajit_POPULATED)
  FetchContent_Populate(luajit)
endif()
if(NOT MSVC)
  message(FATAL_ERROR "The embedded LuaJIT build currently requires MSVC on Windows")
endif()
if(NOT EXISTS "${luajit_SOURCE_DIR}/src/lua51.lib")
  execute_process(
    COMMAND cmd /c msvcbuild.bat static
    WORKING_DIRECTORY "${luajit_SOURCE_DIR}/src"
    RESULT_VARIABLE LUAJIT_BUILD_RESULT
  )
  if(NOT LUAJIT_BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "LuaJIT static build failed: ${LUAJIT_BUILD_RESULT}")
  endif()
endif()
add_library(lua_static STATIC IMPORTED GLOBAL)
set_target_properties(lua_static PROPERTIES
  IMPORTED_LOCATION "${luajit_SOURCE_DIR}/src/lua51.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${luajit_SOURCE_DIR}/src"
)

FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG v1.3.4
)
FetchContent_MakeAvailable(minhook)
add_library(imgui_static STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui_static PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui_static PUBLIC d3d11 dxgi dwmapi)

set(CS2LUA_EMBEDDED_LIBRARIES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/lua/libraries/nl_compat.lua
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/lua/libraries/offsets.lua
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/lua/libraries/entity_offsets.lua
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/lua/libraries/entity_compat.lua
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/lua/libraries/panorama_compat.lua
)
set_source_files_properties(${CS2LUA_EMBEDDED_LIBRARIES} PROPERTIES HEADER_FILE_ONLY TRUE)

# The restored 2026-07-13 18:16 Lua snapshot is the deployed source of truth.
# panorama_compat.moon is retained as a readable development draft only. The
# Lua file above is embedded as RCDATA, so the DLL needs no external script.
set(CS2LUA_PANORAMA_MOON_SOURCE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/plugin/lua/libraries/panorama_compat.moon
)
set_source_files_properties(${CS2LUA_PANORAMA_MOON_SOURCE} PROPERTIES HEADER_FILE_ONLY TRUE)
set_property(SOURCE src/plugin/lua/embedded_libraries.rc APPEND PROPERTY
  OBJECT_DEPENDS "${CS2LUA_EMBEDDED_LIBRARIES}")

add_library(CS2LuaPlugin SHARED
  src/plugin/dllmain.cpp
  src/plugin/plugin.cpp
  src/plugin/plugin.hpp
  src/plugin/lua/embedded_libraries.cpp
  src/plugin/lua/embedded_libraries.hpp
  src/plugin/lua/embedded_libraries.rc
  src/plugin/lua/embedded_library_resources.h
  ${CS2LUA_EMBEDDED_LIBRARIES}
  ${CS2LUA_PANORAMA_MOON_SOURCE}
  src/plugin/lua/lua_engine.cpp
  src/plugin/lua/lua_engine.hpp
  src/plugin/ui/menu.cpp
  src/plugin/ui/menu.hpp
  src/plugin/events/game_events.cpp
  src/plugin/events/game_events.hpp
)
target_compile_definitions(CS2LuaPlugin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(CS2LuaPlugin PRIVATE src/plugin)
target_link_libraries(CS2LuaPlugin PRIVATE lua_static imgui_static minhook d3d11 dxgi user32 winhttp)

# Build the injector after the plugin and embed both runtime payloads directly
# into the executable. They are extracted to a private temporary directory only
# for the duration of injection, so the distributed package is a single EXE.
set(CS2LUA_DRIVER_PAYLOAD "${CMAKE_CURRENT_SOURCE_DIR}/CS2HexSyncCompatDriver.sys")
set(CS2LUA_PLUGIN_PAYLOAD "${CMAKE_BINARY_DIR}/$<CONFIG>/CS2LuaPlugin.dll")
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/injector_payloads.rc"
  CONTENT "#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/injector/injector_resources.h\"\nIDR_CS2LUA_DRIVER RCDATA \"${CS2LUA_DRIVER_PAYLOAD}\"\nIDR_CS2LUA_PLUGIN RCDATA \"${CS2LUA_PLUGIN_PAYLOAD}\"\n"
)

add_executable(CS2LuaInjector WIN32
  src/injector/main.cpp
  src/injector/hexsync_client.cpp
  src/injector/hexsync_client.hpp
  src/injector/driver_service.cpp
  src/injector/driver_service.hpp
  src/injector/injector_resources.h
  "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/injector_payloads.rc"
)
add_dependencies(CS2LuaInjector CS2LuaPlugin)
target_compile_definitions(CS2LuaInjector PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(CS2LuaInjector PRIVATE imgui_static advapi32 d3d11 dxgi dwmapi shell32)
target_link_options(CS2LuaInjector PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")



