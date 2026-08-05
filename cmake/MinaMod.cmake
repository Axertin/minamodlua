include_guard(GLOBAL)

# The game loads a mod as <mods>/<id>/mod.dll (mod.so on Linux) beside a mod.yc
# manifest, so a manifest exists for Lua-only mods too - a folder with no binary
# is still registered.
#
# 0 for min/maxGameVersion means no version constraint.
function(mml_write_mod_manifest DIR ID NAME)
    file(WRITE "${DIR}/mod.yc"
"[YCD Version: 1]
MinaModDef
{
\tid: \"${ID}\",
\tname: \"${NAME}\",
\tmodVersion: 1,
\tminGameVersion: 0,
\tmaxGameVersion: 0,
\tloadPriority: 0,
}
")
endfunction()

# mml_add_mod(<target> ID <mod-id> NAME <display name> [USES_LUA] SOURCES <files...>)
function(mml_add_mod TARGET)
    cmake_parse_arguments(ARG "USES_LUA" "ID;NAME" "SOURCES" ${ARGN})

    set(_dir "${MML_MODS_DIR}/${ARG_ID}")

    add_library(${TARGET} MODULE ${ARG_SOURCES})
    target_link_libraries(${TARGET} PRIVATE minamodlua::minamodapi)

    # The export policy rides on the luajit target because LuaJIT is what would
    # otherwise leak: luaconf.h marks LUA_API default-visibility per declaration,
    # so the project-wide `hidden` preset does nothing for it. Measured: 148
    # exported lua* symbols without the policy, 0 with.
    if(ARG_USES_LUA)
        target_link_libraries(${TARGET} PRIVATE minamodlua::luajit)
    endif()

    set_target_properties(${TARGET} PROPERTIES
        OUTPUT_NAME mod
        PREFIX ""
        # $<1:...> stops multi-config generators appending Debug/Release
        LIBRARY_OUTPUT_DIRECTORY "$<1:${_dir}>")

    mml_write_mod_manifest("${_dir}" "${ARG_ID}" "${ARG_NAME}")
endfunction()
