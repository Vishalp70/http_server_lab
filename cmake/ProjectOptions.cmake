# Warning flags are carried by an INTERFACE target that our own targets link
# against. Third-party code (GoogleTest) is deliberately not linked to it, so
# -Werror never breaks on code we do not own.

add_library(srv_project_options INTERFACE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(srv_project_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion)
  if(SRV_WARNINGS_AS_ERRORS)
    target_compile_options(srv_project_options INTERFACE -Werror)
  endif()
else()
  message(WARNING "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}': no warning flags applied.")
endif()
