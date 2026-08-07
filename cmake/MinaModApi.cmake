# MinaModApi.cmake - the game's native mod API headers, via FetchContent.
#
# Header-only: MinaModAPI.h, MinaModTypes.h, MinaModEnums.h, MinaModHooks.h.
# Pinned to an exact commit; the upstream-drift CI job reports when that pin
# goes stale - keep this list in step with the one in upstream-drift.yml, which
# is what says which headers that job actually watches.
#
# Exposes: minamodlua::minamodapi (INTERFACE, system include dir only)
#          MINAMODAPI_SOURCE_DIR   (for the name extraction)

include_guard(GLOBAL)
include(FetchContent)

set(MINAMODAPI_GIT_TAG "aebbb9191f1432dc9f81238dcdc9b76acb2208b0" CACHE STRING
    "MinaModAPI commit to build against. Bump deliberately; CI diffs the result.")

# SOURCE_SUBDIR points at a path with no CMakeLists.txt so MakeAvailable
# populates but does NOT add_subdirectory - we want the headers, not its
# example-mod build.
FetchContent_Declare(
    minamodapi
    GIT_REPOSITORY https://github.com/YachtClubGames/MinaModAPI.git
    GIT_TAG ${MINAMODAPI_GIT_TAG}
    SOURCE_SUBDIR _headers_only
)
FetchContent_MakeAvailable(minamodapi)

set(MINAMODAPI_SOURCE_DIR "${minamodapi_SOURCE_DIR}" CACHE INTERNAL "")

add_library(minamodlua_minamodapi INTERFACE)
add_library(minamodlua::minamodapi ALIAS minamodlua_minamodapi)
target_include_directories(minamodlua_minamodapi SYSTEM INTERFACE "${minamodapi_SOURCE_DIR}")
