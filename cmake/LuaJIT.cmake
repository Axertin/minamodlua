# LuaJIT.cmake - vendored LuaJIT 2.1, built static by its own build script.
#
# Exposes: minamodlua::luajit  (IMPORTED STATIC - headers, the archive, the
#                               platform libs, and the export policy below)
#          MINAMODLUA_MOD_EXPORT_MAP  (path to cmake/mod-exports.map, ELF only)
#
# Three lanes, two backends:
#   Linux / GCC or Clang     -> src/Makefile,      libluajit.a  (ships)
#   Windows / mingw-w64 x-c  -> src/Makefile,      libluajit.a  (compile check)
#   Windows / MSVC           -> src/msvcbuild.bat, lua51.lib    (ships)
#
# Upstream's own build is driven rather than reimplemented as a CMakeLists,
# because a reimplementation would have to keep reproducing two things at every
# pin bump: the host/target split (minilua/DynASM/buildvm run on the *build*
# machine, and their DynASM feature flags come from a -dM dump of lj_arch.h
# compiled for the *target*), and the .eh_frame probe that decides
# -DLUAJIT_UNWIND_EXTERNAL - losing which silently downgrades error handling to
# longjmp. We only hand the scripts a toolchain and ask for the archive.
#
# LuaJIT is MIT-licensed. A shipped mod statically linking it owes the upstream
# copyright notice (src/luajit_rolling.h / COPYRIGHT) in its distribution.

include_guard(GLOBAL)
include(FetchContent)
include(ExternalProject)
include(ProcessorCount)

# Upstream publishes no tags: v2.1 is a rolling branch and the only real version
# is a commit.
set(LUAJIT_GIT_TAG "1edc3e52b67eaf6ce5f809be8e17d6862594b8bc" CACHE STRING
    "LuaJIT commit to build. Branch v2.1; upstream publishes no tags.")

if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows)$")
    message(FATAL_ERROR
        "LuaJIT.cmake: only the Linux and Windows lanes are wired up, got "
        "CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}. Add a TARGET_SYS mapping and a "
        "matching export policy before building for it.")
endif()

# Backend selection
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    set(_lj_backend msvc)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "LuaJIT.cmake: clang-cl is deliberately unsupported. LuaJIT's MSVC build "
        "script only knows how to drive cl.exe, so a clang-cl configure would mix "
        "toolchains inside one DLL. Use MSVC (cl.exe) or the mingw-w64 toolchain "
        "file for Windows.")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_lj_backend makefile)
    set(_lj_cc clang)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(_lj_backend makefile)
    set(_lj_cc gcc)
else()
    message(FATAL_ERROR
        "LuaJIT.cmake: needs MSVC, GCC or Clang. Got ${CMAKE_CXX_COMPILER_ID}.")
endif()

if(_lj_backend STREQUAL "makefile")
    # LuaJIT names the cross toolchain as a *prefix* ($(CROSS)$(CC), $(CROSS)ar,
    # $(CROSS)strip), not as paths, so derive one from whatever compiler CMake was
    # given: x86_64-w64-mingw32-g++ -> "x86_64-w64-mingw32-", g++ -> "".
    get_filename_component(_lj_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
    if(_lj_cxx_name MATCHES "^(.*-)?(g\\+\\+|c\\+\\+|clang\\+\\+)(-[0-9.]+)?(\\.exe)?$")
        set(_lj_cross "${CMAKE_MATCH_1}")
    else()
        set(_lj_cross "")
    endif()
    set(LUAJIT_CROSS_PREFIX "${_lj_cross}" CACHE STRING
        "Binutils/compiler prefix passed to LuaJIT as CROSS=. Empty means native.")

    # The host tools are plain C programs for the *build* machine, so they must
    # not be built with the cross compiler. CMAKE_FIND_ROOT_PATH_MODE_PROGRAM is
    # NEVER in the mingw toolchain file, so this finds the real host compiler.
    if(CMAKE_CROSSCOMPILING)
        find_program(LUAJIT_HOST_CC NAMES cc gcc clang REQUIRED
            DOC "C compiler for LuaJIT's minilua/buildvm generators; targets the build machine")
    else()
        set(LUAJIT_HOST_CC "${_lj_cc}" CACHE STRING
            "C compiler for LuaJIT's minilua/buildvm generators; targets the build machine")
    endif()

    find_program(LUAJIT_MAKE NAMES gmake make REQUIRED
        DOC "GNU make, used to drive LuaJIT's own build")
    mark_as_advanced(LUAJIT_CROSS_PREFIX LUAJIT_HOST_CC LUAJIT_MAKE)

    execute_process(COMMAND "${LUAJIT_MAKE}" --version
        OUTPUT_VARIABLE _lj_make_ver ERROR_QUIET)
    if(NOT _lj_make_ver MATCHES "GNU Make")
        message(FATAL_ERROR "LuaJIT.cmake: ${LUAJIT_MAKE} is not GNU make, which "
            "LuaJIT's Makefile requires.")
    endif()

    # BUILDMODE=static skips the parallel -fPIC object set that mixed mode builds
    # for libluajit.so, but leaves the archive's objects non-PIC, which an ELF
    # MODULE library cannot link - hence the explicit -fPIC. PE code is position
    # independent by definition and mingw warns if told otherwise.
    #
    # Everything else stays at upstream's defaults, including CCOPT=-O2: LuaJIT is
    # built optimised regardless of the parent's CMAKE_BUILD_TYPE.
    set(_lj_make_args
        BUILDMODE=static
        CC=${_lj_cc}
        CROSS=${LUAJIT_CROSS_PREFIX}
        HOST_CC=${LUAJIT_HOST_CC}
        TARGET_SYS=${CMAKE_SYSTEM_NAME}
    )
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        list(APPEND _lj_make_args TARGET_CFLAGS=-fPIC)
    endif()
    set(_lj_lib_name libluajit.a)

else()
    # msvcbuild.bat must run inside a Visual Studio developer environment: it
    # bails on `if not defined INCLUDE` and calls bare cl/link/lib/mt. Checked
    # here so the failure is one clear message at configure time, rather than a
    # batch file printing a banner and - because `echo` resets ERRORLEVEL -
    # exiting 0.
    if(NOT DEFINED ENV{INCLUDE} OR NOT DEFINED ENV{LIB})
        message(FATAL_ERROR
            "LuaJIT.cmake: no Visual Studio developer environment (INCLUDE/LIB are "
            "not set). LuaJIT is built by src/msvcbuild.bat, which requires one.\n"
            "  local:  run cmake from an 'x64 Native Tools Command Prompt', or call "
            "vcvarsall.bat x64 first\n"
            "  CI:     add ilammy/msvc-dev-cmd (arch: x64) before the configure step")
    endif()

    foreach(_tool cl link lib)
        string(TOUPPER "${_tool}" _tool_uc)
        find_program(LUAJIT_MSVC_${_tool_uc} NAMES ${_tool})
        mark_as_advanced(LUAJIT_MSVC_${_tool_uc})
        if(NOT LUAJIT_MSVC_${_tool_uc})
            message(FATAL_ERROR
                "LuaJIT.cmake: ${_tool}.exe is not on PATH. msvcbuild.bat invokes it "
                "unqualified, so a developer environment must be active for both the "
                "configure and the build step.")
        endif()
    endforeach()

    # Only used by the post-build audit; missing it is a warning, not fatal.
    find_program(LUAJIT_MSVC_DUMPBIN NAMES dumpbin)
    mark_as_advanced(LUAJIT_MSVC_DUMPBIN)

    # CRT agreement. msvcbuild.bat's :STATIC path passes neither /MD nor /MT, so
    # cl falls back to the static release CRT - verified by compiling that exact
    # command line and reading the object's directives: /DEFAULTLIB:libcmt.lib.
    # A mod built /MD would drag two CRTs into one DLL.
    cmake_policy(GET CMP0091 _lj_cmp0091)
    if(NOT _lj_cmp0091 STREQUAL "NEW")
        message(FATAL_ERROR
            "LuaJIT.cmake: policy CMP0091 must be NEW for CMAKE_MSVC_RUNTIME_LIBRARY "
            "to have any effect; otherwise the CRT selection below is silently ignored.")
    endif()
    if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded")
        message(FATAL_ERROR
            "LuaJIT.cmake: the MSVC lane needs the static release CRT, because that "
            "is what msvcbuild.bat's static build produces (/DEFAULTLIB:LIBCMT) and "
            "mixing CRTs inside one DLL is a runtime bug, not a warning.\n"
            "Add to CMakeLists.txt, before any target is defined:\n"
            "    set(CMAKE_MSVC_RUNTIME_LIBRARY \"MultiThreaded\")\n"
            "(the plain form, not the $<CONFIG:Debug> one: msvcbuild.bat builds the "
            "release CRT in every configuration)\n"
            "Currently: '${CMAKE_MSVC_RUNTIME_LIBRARY}'")
    endif()

    # msvcbuild.bat hardcodes /D_CRT_STDIO_INLINE=__declspec(dllexport)__inline, a
    # 2015-era workaround for its *DLL* build. In a static lib embedded in someone
    # else's DLL it is pure leakage: it turns every UCRT inline stdio definition
    # into a /EXPORT: directive inside each LuaJIT object, and link.exe honours
    # directives coming from library members. Confirmed by compiling the same
    # command line; the directives survive archiving.
    #
    # _CL_ appends to cl's command line (CL prepends, which loses to the script's
    # own /D), so this restores the UCRT default. Set to "" to build exactly what
    # upstream does - the audit below will then report what leaks.
    set(LUAJIT_MSVC_CL_APPEND "/D_CRT_STDIO_INLINE=__inline" CACHE STRING
        "Appended to cl's command line via _CL_ when building LuaJIT.")
    option(LUAJIT_MSVC_ALLOW_LIB_EXPORTS
        "Downgrade the /EXPORT: audit of lua51.lib to a warning" OFF)
    mark_as_advanced(LUAJIT_MSVC_CL_APPEND LUAJIT_MSVC_ALLOW_LIB_EXPORTS)

    set(_lj_lib_name lua51.lib)
endif()

# Source. FetchContent rather than ExternalProject's download step, so the tree
# exists at configure time and the checks below can run before a single object is
# built.
FetchContent_Declare(
    luajit
    GIT_REPOSITORY https://github.com/LuaJIT/LuaJIT.git
    GIT_TAG ${LUAJIT_GIT_TAG}
    SOURCE_SUBDIR _not_a_cmake_project
)
FetchContent_MakeAvailable(luajit)

set(_lj_tree "${CMAKE_CURRENT_BINARY_DIR}/luajit-build")
set(_lj_src "${_lj_tree}/src")
set(_lj_ep "${CMAKE_CURRENT_BINARY_DIR}/luajit-ep")
set(_lj_stamp "${_lj_ep}/stamp")
set(_lj_out "${CMAKE_CURRENT_BINARY_DIR}/luajit")
set(_lj_inc "${_lj_out}/include")
set(_lj_lib "${_lj_out}/lib/${_lj_lib_name}")

# Bumping LUAJIT_GIT_TAG changes the sources but not ExternalProject's stamps,
# and msvcbuild.bat is not incremental at all, so tie both to the pin explicitly
# and start clean when it moves.
set(_lj_pin_file "${_lj_ep}/pinned.txt")
set(_lj_pin_now "${LUAJIT_GIT_TAG} ${_lj_backend}")
set(_lj_pin_was "")
if(EXISTS "${_lj_pin_file}")
    file(READ "${_lj_pin_file}" _lj_pin_was)
endif()
if(NOT _lj_pin_was STREQUAL _lj_pin_now)
    file(REMOVE_RECURSE "${_lj_tree}" "${_lj_stamp}" "${_lj_out}")
endif()

# The build runs in-tree and writes generated headers next to the sources, so it
# gets a private copy and the fetched checkout stays pristine - otherwise
# FetchContent sees a dirty tree on the next configure. file(COPY) preserves
# timestamps and skips unchanged files, so reconfiguring does not invalidate
# object files already sitting in the copy.
file(COPY "${luajit_SOURCE_DIR}/src" "${luajit_SOURCE_DIR}/dynasm"
    DESTINATION "${_lj_tree}")
file(MAKE_DIRECTORY "${_lj_inc}")

# Both build scripts derive the rolling-release version from ../.git, falling
# back to ../.relver. Neither is in the copy, so write .relver from the pinned
# commit; without it the build warns and stamps the version string "ROLLING",
# and the luaL_newstate() header/library version check keys off that string.
find_package(Git QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" show -s --format=%ct "${LUAJIT_GIT_TAG}"
    WORKING_DIRECTORY "${luajit_SOURCE_DIR}"
    OUTPUT_VARIABLE _lj_relver OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _lj_relver_rc ERROR_QUIET)
if(_lj_relver_rc EQUAL 0 AND _lj_relver MATCHES "^[0-9]+$")
    file(WRITE "${_lj_tree}/.relver" "${_lj_relver}\n")
else()
    message(WARNING "LuaJIT.cmake: could not read the release version from git; "
        "LUAJIT_VERSION will read 2.1.ROLLING.")
endif()

file(WRITE "${_lj_pin_file}" "${_lj_pin_now}")

# Configure-time assertions on how LuaJIT will actually be built.
#
# LJ_UNWIND_EXT must be 1: on x64 LuaJIT unwinds through the platform mechanism
# (SEH on Windows, DWARF on Linux) instead of longjmp, and that is the only
# reason a C++ destructor between lua_pcall and the throwing frame ever runs.
# Windows gets it from LJ_ABI_WIN unconditionally; on ELF it hangs off
# -DLUAJIT_UNWIND_EXTERNAL, which the Makefile adds only if its probe finds
# .eh_frame in a test object. Silent to lose, expensive to find out about later.
# LJ_GC64 must be 1, the x64 default.
#
# The flags come out of the Makefile itself on the Makefile lanes, so this
# verifies the real build rather than restating an assumption.
file(WRITE "${_lj_src}/mml-config-probe.c"
    "/* Generated by cmake/LuaJIT.cmake. Compiled with LuaJIT's own build flags. */
#include \"lj_arch.h\"
#if LJ_UNWIND_EXT != 1
#error \"LJ_UNWIND_EXT is 0: LuaJIT would unwind with longjmp, skipping C++ destructors\"
#endif
#if LJ_GC64 != 1
#error \"LJ_GC64 is 0: expected 64-bit GC references on x64\"
#endif
int mml_luajit_config_ok;
")

if(_lj_backend STREQUAL "makefile")
    file(WRITE "${_lj_src}/mml-dump-flags.mk"
        "# Generated by cmake/LuaJIT.cmake. Evaluates alongside LuaJIT's own Makefile
# (make -f Makefile -f this) so the flags reported are the real ones.
mml-dump-flags:
\t@echo \"MML_TARGET_CC=$(TARGET_CC)\"
\t@echo \"MML_TARGET_ACFLAGS=$(TARGET_ACFLAGS)\"
")

    execute_process(
        COMMAND "${LUAJIT_MAKE}" -f Makefile -f mml-dump-flags.mk mml-dump-flags ${_lj_make_args}
        WORKING_DIRECTORY "${_lj_src}"
        OUTPUT_VARIABLE _lj_flags_out
        ERROR_VARIABLE _lj_flags_err
        RESULT_VARIABLE _lj_flags_rc)
    if(NOT _lj_flags_rc EQUAL 0)
        message(FATAL_ERROR "LuaJIT.cmake: could not read the build flags out of "
            "LuaJIT's Makefile:\n${_lj_flags_out}${_lj_flags_err}")
    endif()
    if(NOT _lj_flags_out MATCHES "MML_TARGET_CC=([^\n\r]*)[\n\r]+MML_TARGET_ACFLAGS=([^\n\r]*)")
        message(FATAL_ERROR "LuaJIT.cmake: unexpected flag dump:\n${_lj_flags_out}")
    endif()
    set(_lj_probe_cc "${CMAKE_MATCH_1}")
    set(_lj_probe_flags_str "${CMAKE_MATCH_2}")
    separate_arguments(_lj_probe_flags UNIX_COMMAND "${_lj_probe_flags_str}")
    set(_lj_probe_cmd ${_lj_probe_cc} ${_lj_probe_flags}
        -I. -c mml-config-probe.c -o mml-config-probe.o)
    set(_lj_probe_obj "${_lj_src}/mml-config-probe.o")
else()
    # Mirrors msvcbuild.bat's LJCOMPILE for the :STATIC path.
    set(_lj_probe_cc "${LUAJIT_MSVC_CL}")
    set(_lj_probe_flags /nologo /c /O2 /W3 /D_CRT_SECURE_NO_DEPRECATE)
    set(_lj_probe_flags_str "/nologo /c /O2 /W3 /D_CRT_SECURE_NO_DEPRECATE")
    set(_lj_probe_cmd "${_lj_probe_cc}" ${_lj_probe_flags}
        /I. mml-config-probe.c /Fomml-config-probe.obj)
    set(_lj_probe_obj "${_lj_src}/mml-config-probe.obj")
endif()

execute_process(
    COMMAND ${_lj_probe_cmd}
    WORKING_DIRECTORY "${_lj_src}"
    OUTPUT_VARIABLE _lj_probe_out ERROR_VARIABLE _lj_probe_err
    RESULT_VARIABLE _lj_probe_rc)
file(REMOVE "${_lj_probe_obj}")
if(NOT _lj_probe_rc EQUAL 0)
    message(FATAL_ERROR
        "LuaJIT.cmake: the pinned LuaJIT would not be built the way this project "
        "requires.\n"
        "  compiler: ${_lj_probe_cc}\n"
        "  flags:    ${_lj_probe_flags_str}\n"
        "${_lj_probe_out}${_lj_probe_err}")
endif()
message(STATUS "minamodlua: LuaJIT ${LUAJIT_GIT_TAG} (${_lj_backend}) - "
    "LJ_UNWIND_EXT=1, LJ_GC64=1")

# Build
if(_lj_backend STREQUAL "makefile")
    # One goal, libluajit.a: the default goal would also link the luajit
    # executable and jit/vmdef.lua, neither of which a mod embeds. BUILD_ALWAYS is
    # affordable because make is incremental, and it covers edits to the copied
    # tree that the pin check above cannot see.
    ProcessorCount(_lj_jobs)
    if(_lj_jobs EQUAL 0)
        set(_lj_jobs 1)
    endif()
    set(_lj_build_cmd "${LUAJIT_MAKE}" -j${_lj_jobs} libluajit.a ${_lj_make_args})
    set(_lj_build_always ON)
    set(_lj_verify_cmd "")
else()
    # msvcbuild.bat is not incremental (it deletes every object when it
    # finishes), so BUILD_ALWAYS would mean a full LuaJIT rebuild on every
    # `cmake --build`. The pin check above is what invalidates it instead.
    #
    # The script reports failure by printing a banner and falling through to an
    # `echo`, which resets ERRORLEVEL - so it can exit 0 having built nothing.
    # mml-msvc-verify.cmake is what turns that into a real failure, and it also
    # audits the CRT and the export directives baked into the archive.
    file(CONFIGURE OUTPUT "${_lj_src}/mml-msvc-verify.cmake" @ONLY CONTENT [==[
# Generated by cmake/LuaJIT.cmake - do not edit. Runs after msvcbuild.bat.
set(_lib "@_lj_src@/@_lj_lib_name@")
if(NOT EXISTS "${_lib}")
    message(FATAL_ERROR
        "LuaJIT: msvcbuild.bat did not produce @_lj_lib_name@.\n"
        "It signals failure with a printed banner and still exits 0, so the real "
        "error is further up in this log.")
endif()

if(NOT EXISTS "@LUAJIT_MSVC_DUMPBIN@")
    message(WARNING "LuaJIT: dumpbin was not found, so the CRT and export audit of "
                    "@_lj_lib_name@ was skipped.")
    return()
endif()

execute_process(COMMAND "@LUAJIT_MSVC_DUMPBIN@" /nologo /directives "${_lib}"
                OUTPUT_VARIABLE _out ERROR_VARIABLE _err RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "LuaJIT: dumpbin /directives failed:\n${_out}${_err}")
endif()
string(TOLOWER "${_out}" _lower)

# Expected: /DEFAULTLIB:LIBCMT, the static release CRT, matching the
# MultiThreaded runtime this module requires of the parent project.
if(_lower MATCHES "defaultlib:\"?msvcrt" OR _lower MATCHES "defaultlib:\"?libcmtd")
    message(FATAL_ERROR
        "LuaJIT: @_lj_lib_name@ was built against a CRT that does not match "
        "CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded. Linking it into the mod would "
        "put two C runtimes in one DLL. Check whether upstream's msvcbuild.bat "
        "changed its default runtime flags at this pin.")
elseif(NOT _lower MATCHES "defaultlib:\"?libcmt")
    message(WARNING
        "LuaJIT: no /DEFAULTLIB CRT directive found in @_lj_lib_name@; the runtime "
        "match with the mod could not be confirmed.")
endif()

# msvcbuild.bat's /D_CRT_STDIO_INLINE=__declspec(dllexport)__inline turns the
# UCRT's inline stdio definitions into /EXPORT: directives inside every object,
# and link.exe honours directives coming from library members - so they would
# surface as exports of the mod DLL. LUAJIT_MSVC_CL_APPEND undoes it.
string(REGEX MATCHALL "/[Ee][Xx][Pp][Oo][Rr][Tt]:[^ \t\r\n\"]+" _exports "${_out}")
if(_exports)
    list(REMOVE_DUPLICATES _exports)
    list(JOIN _exports "\n    " _exports_txt)
    set(_msg
        "LuaJIT: @_lj_lib_name@ carries linker export directives, which link.exe "
        "will turn into exports of the mod DLL. The mod must export MinaMod_Init "
        "and nothing else - another loaded mod may embed its own LuaJIT or CRT.\n"
        "    ${_exports_txt}\n"
        "Currently LUAJIT_MSVC_CL_APPEND='@LUAJIT_MSVC_CL_APPEND@'. Setting it to "
        "/D_CRT_STDIO_INLINE=__inline restores the UCRT default and removes these. "
        "Set LUAJIT_MSVC_ALLOW_LIB_EXPORTS=ON to accept them.")
    if(@LUAJIT_MSVC_ALLOW_LIB_EXPORTS@)
        message(WARNING ${_msg})
    else()
        message(FATAL_ERROR ${_msg})
    endif()
endif()
]==])

    set(_lj_build_cmd
        "${CMAKE_COMMAND}" -E env "_CL_=${LUAJIT_MSVC_CL_APPEND}"
        cmd /c msvcbuild.bat static)
    set(_lj_verify_cmd COMMAND "${CMAKE_COMMAND}" -P "${_lj_src}/mml-msvc-verify.cmake")
    set(_lj_build_always OFF)
endif()

# The install step is copy_if_different so a no-op build does not retouch the
# archive and force the mod to relink.
set(_lj_headers lua.h lualib.h lauxlib.h luaconf.h lua.hpp luajit.h)
set(_lj_install_cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${_lj_src}/${_lj_lib_name}" "${_lj_lib}")
foreach(_h IN LISTS _lj_headers)
    list(APPEND _lj_install_cmds
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_lj_src}/${_h}" "${_lj_inc}/${_h}")
endforeach()

ExternalProject_Add(minamodlua_luajit_build
    SOURCE_DIR "${_lj_tree}"
    BINARY_DIR "${_lj_src}"
    PREFIX "${_lj_ep}"
    STAMP_DIR "${_lj_stamp}"
    DOWNLOAD_COMMAND "" # FetchContent above
    UPDATE_COMMAND ""
    CONFIGURE_COMMAND "" # no configure step; upstream's script probes for itself
    BUILD_COMMAND ${_lj_build_cmd} ${_lj_verify_cmd}
    BUILD_ALWAYS ${_lj_build_always}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E make_directory "${_lj_out}/lib"
    ${_lj_install_cmds}
    BUILD_BYPRODUCTS "${_lj_lib}"
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON
)

# minamodlua::luajit
#
# The link options are the export policy: another mod in the same process may
# embed a different LuaJIT, and interposed lua_* symbols across two VMs is an
# immediate crash. What leaks differs per toolchain, so what is needed does too.
#
#   ELF     luaconf.h attaches visibility("default") to every LUA_API
#           declaration, so -fvisibility=hidden does nothing for them. Measured:
#           148 exported lua*/luaL* symbols without help. --version-script pins
#           the dynamic symbol table to MinaMod_Init; --exclude-libs,ALL hides
#           archive symbols even if the script is dropped.
#   mingw   ld auto-exports everything when no symbol is explicitly exported.
#           MM_EXPORT's __declspec(dllexport) already suppresses that, but
#           --exclude-all-symbols makes it independent of anyone remembering it
#           (~840 exports without either).
#   MSVC    link.exe never auto-exports, and --exclude-all-symbols is a GNU ld
#           option cl would reject. The one MSVC leak is baked into the archive
#           rather than the link, and is dealt with above.
set(MINAMODLUA_MOD_EXPORT_MAP "${CMAKE_CURRENT_LIST_DIR}/mod-exports.map"
    CACHE FILEPATH "Linker version script listing a mod's exported symbols (ELF only)")

add_library(minamodlua_luajit STATIC IMPORTED GLOBAL)
add_library(minamodlua::luajit ALIAS minamodlua_luajit)
add_dependencies(minamodlua_luajit minamodlua_luajit_build)

set_target_properties(minamodlua_luajit PROPERTIES
    IMPORTED_LOCATION "${_lj_lib}"
    INTERFACE_INCLUDE_DIRECTORIES "${_lj_inc}"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_lj_inc}"
)

set(_lj_is_dso "$<OR:$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>,$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>>")
if(_lj_backend STREQUAL "msvc")
    # Nothing to add. See the note above.
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set_property(TARGET minamodlua_luajit APPEND PROPERTY
        INTERFACE_LINK_OPTIONS "$<${_lj_is_dso}:-Wl,--exclude-all-symbols>")
else()
    # lj_clib.c dlopen()s for the FFI; lj_vmmath.c wants libm.
    set_property(TARGET minamodlua_luajit APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES m ${CMAKE_DL_LIBS})
    set_property(TARGET minamodlua_luajit APPEND PROPERTY
        INTERFACE_LINK_OPTIONS
        "$<${_lj_is_dso}:-Wl,--version-script=${MINAMODLUA_MOD_EXPORT_MAP}>"
        "$<${_lj_is_dso}:-Wl,--exclude-libs,ALL>")
    set_property(TARGET minamodlua_luajit APPEND PROPERTY
        INTERFACE_LINK_DEPENDS "${MINAMODLUA_MOD_EXPORT_MAP}")
endif()
