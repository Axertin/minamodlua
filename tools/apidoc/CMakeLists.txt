# Writes docs/api-reference.md into the source tree, so it is deliberately not
# part of ALL: a plain build never dirties a tracked file.
mml_generate_api_comments(
    "${MINAMODAPI_SOURCE_DIR}/MinaModAPI.h"
    "${CMAKE_CURRENT_BINARY_DIR}/api_comments.inc")

add_executable(mml_apidoc EXCLUDE_FROM_ALL apidoc.cpp)
target_link_libraries(mml_apidoc PRIVATE minamodlua::minamodapi)
target_include_directories(mml_apidoc PRIVATE
    "${CMAKE_SOURCE_DIR}/src/bridge"
    "${CMAKE_BINARY_DIR}/src/bridge"   # api_list.inc, api_args.inc
    "${CMAKE_CURRENT_BINARY_DIR}")     # api_comments.inc

add_custom_target(apidoc
    COMMAND mml_apidoc "${CMAKE_SOURCE_DIR}/docs/api-reference.md"
    DEPENDS mml_apidoc
    COMMENT "Regenerating docs/api-reference.md")
