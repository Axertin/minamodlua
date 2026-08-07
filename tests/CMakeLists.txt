# Links mml_events, not the mod: these tests run on a bare lua_State with no
# MinaModAPI at all (main() sets g_api to nullptr), so they need no game and no
# MinaMod_Init. The flip side is that events.cpp's protected_dispatch, which
# goes through g_api, is not reachable from them - see the header comment in
# events_test.cpp.
#
# mml_bridge is listed explicitly alongside mml_events, not left to propagate
# through mml_events's PUBLIC dependency on it: CMake does not forward one
# OBJECT library's objects through a second OBJECT library to a target two
# links away, only the usage requirements (include dirs etc). minamodlua's own
# CMakeLists hits the same thing, which is why its link line names both too.
add_executable(mml_events_test events_test.cpp)
target_link_libraries(mml_events_test PRIVATE mml_events mml_bridge)
add_test(NAME events COMMAND mml_events_test)

# mml_events/mml_bridge are OBJECT libraries compiled once; the bug this is
# for (eventctx.cpp writing past a caller-owned array) is a store this
# executable's own instrumentation cannot see unless the object libraries it
# links carry it too. Both are still shared with the shipped minamodlua
# target, but that is fine: this option is never set for a build that ships.
if(MML_SANITIZE)
    foreach(tgt mml_events_test mml_events mml_bridge)
        target_compile_options(${tgt} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(mml_events_test PRIVATE -fsanitize=address,undefined)
endif()
