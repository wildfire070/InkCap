add_executable(Ao3ReceiveUtilsTest
  Ao3ReceiveUtilsTest.cpp
  DependencyStubs.cpp
  ${REPO_ROOT}/src/util/Ao3ReceiveUtils.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
)

target_include_directories(Ao3ReceiveUtilsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/.pio/libdeps/simulator/ArduinoJson/src
)

target_link_libraries(Ao3ReceiveUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Ao3ReceiveUtilsTest)
