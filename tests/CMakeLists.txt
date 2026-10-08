include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  SYSTEM
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(tsengine_tests
  coding_test.cpp
  crc32c_test.cpp
  line_protocol_test.cpp
  wal_test.cpp
)
target_link_libraries(tsengine_tests PRIVATE tsengine tsengine_options GTest::gtest_main)
gtest_discover_tests(tsengine_tests)
