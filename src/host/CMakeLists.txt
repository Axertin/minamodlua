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
