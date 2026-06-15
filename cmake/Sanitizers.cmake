# Sanitizers must instrument every translation unit and every link step
# consistently (including GoogleTest), so unlike warnings they are applied
# globally instead of per target.
#
#   -DSRV_SANITIZER=none        (default)
#   -DSRV_SANITIZER=asan-ubsan  AddressSanitizer + UndefinedBehaviorSanitizer
#   -DSRV_SANITIZER=tsan        ThreadSanitizer (cannot be combined with ASan)

if(SRV_SANITIZER STREQUAL "none")
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "SRV_SANITIZER requires GCC or Clang (found ${CMAKE_CXX_COMPILER_ID}).")
endif()

if(SRV_SANITIZER STREQUAL "asan-ubsan")
  # -fno-sanitize-recover makes UBSan findings abort, so tests fail loudly.
  set(_srv_san_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined)
elseif(SRV_SANITIZER STREQUAL "tsan")
  set(_srv_san_flags -fsanitize=thread)
else()
  message(FATAL_ERROR
    "Unknown SRV_SANITIZER='${SRV_SANITIZER}'. Valid values: none, asan-ubsan, tsan.")
endif()

message(STATUS "Sanitizer configuration: ${SRV_SANITIZER}")
add_compile_options(${_srv_san_flags} -fno-omit-frame-pointer)
add_link_options(${_srv_san_flags})
unset(_srv_san_flags)
