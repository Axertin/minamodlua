# The generated surface: every MinaModAPI member that binds without a
# hand-written wrapper. The "generator" only extracts member names - C++ deduces
# the types - so this is the whole of it.
mml_generate_api_list(
    "${MINAMODAPI_SOURCE_DIR}/MinaModAPI.h"
    "${CMAKE_CURRENT_BINARY_DIR}/api_list.inc")

add_library(mml_bridge OBJECT
    bindings.cpp
    handle.cpp)

target_include_directories(mml_bridge PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(mml_bridge PUBLIC minamodlua::minamodapi minamodlua::luajit)

target_compile_definitions(mml_bridge PUBLIC MINAMODLUA_API_COUNT=${MML_API_COUNT})
