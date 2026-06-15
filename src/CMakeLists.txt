# Version header generated into the build tree and exposed as "common/version.hpp".
configure_file(
  common/version.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/common/version.hpp"
  @ONLY)

# Command-line handling lives in a static library so tests can link it
# without going through main().
add_library(srv_cli STATIC app/cli.cpp)
target_include_directories(srv_cli PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(srv_cli PRIVATE srv_project_options)

# The server executable. Milestone 1: parses arguments and exits.
add_executable(httpd app/main.cpp)
target_link_libraries(httpd PRIVATE srv_cli srv_project_options)
