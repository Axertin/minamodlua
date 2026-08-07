# mml-smoketest is a Lua-only mod: no binary, so there is no target to hang a
# POST_BUILD staging step off of the way src/host/CMakeLists.txt stages lua/.
# A standalone ALL target that copies main.lua and writes the manifest is the
# equivalent for a source-less mod - it runs on every default build, same as
# the real mod's staging step, so `deploy` always finds a current copy.
set(_dir "${MML_MODS_DIR}/mml-smoketest")

mml_write_mod_manifest("${_dir}" "mml-smoketest" "MinaModLua Smoke Test")

add_custom_target(mml-smoketest ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/main.lua" "${_dir}/main.lua"
    SOURCES main.lua
    COMMENT "Staging mml-smoketest")
