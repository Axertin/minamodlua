# The shipped mod: embeds LuaJIT, exposes the bound API, and will load Lua mods
# from sibling mods/<id>/ folders.
#
# host.cpp defines MinaMod_Init and is compiled straight into the MODULE rather
# than reached through an archive, because a linker will drop an unreferenced
# object from a static library and leave a mod the game silently never loads.
mml_add_mod(minamodlua
    USES_LUA
    ID minamodlua
    NAME "minamodlua"
    SOURCES
    events.cpp
    host.cpp
    log.cpp
    modinfo.cpp
    modloader.cpp
    sandbox.cpp)

target_include_directories(minamodlua PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(minamodlua PRIVATE mml_bridge)
target_compile_definitions(minamodlua PRIVATE MINAMODLUA_VERSION="${PROJECT_VERSION}")

# The ergonomic Lua layer ships as files beside the binary, so changing it needs
# no C++ rebuild - just a redeploy and a restart.
add_custom_command(TARGET minamodlua POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/lua" "${MML_MODS_DIR}/minamodlua/lua"
    COMMENT "Staging the Lua layer")
