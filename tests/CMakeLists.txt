include(GoogleTest)

# GoogleTest: a pinned copy is fetched by default so that it is compiled with
# the same flags (including sanitizers) as our code. Use
# -DSRV_USE_SYSTEM_GTEST=ON to use an installed copy (offline builds).
if(SRV_USE_SYSTEM_GTEST)
  find_package(GTest REQUIRED)
else()
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE) # only GoogleTest itself is used
  FetchContent_MakeAvailable(googletest)
endif()

# --- Unit tests -------------------------------------------------------------
add_executable(srv_unit_tests unit/cli_test.cpp)
target_link_libraries(srv_unit_tests PRIVATE srv_cli srv_project_options GTest::gtest_main)
gtest_discover_tests(srv_unit_tests)

# --- Smoke tests of the real executable -------------------------------------
add_test(NAME httpd_help COMMAND httpd --help)
set_tests_properties(httpd_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: httpd")

add_test(NAME httpd_version COMMAND httpd --version)
set_tests_properties(httpd_version PROPERTIES
  PASS_REGULAR_EXPRESSION "httpd ${PROJECT_VERSION}")

add_test(NAME httpd_starts COMMAND httpd --port 9090)
set_tests_properties(httpd_starts PROPERTIES
  PASS_REGULAR_EXPRESSION "configuration OK \\(port 9090\\)")

# With PASS_REGULAR_EXPRESSION CTest ignores the exit status, so this checks
# the diagnostic text; the exit status (2) is checked by hand in the README.
add_test(NAME httpd_rejects_bad_port COMMAND httpd --port 70000)
set_tests_properties(httpd_rejects_bad_port PROPERTIES
  PASS_REGULAR_EXPRESSION "invalid port '70000'")
