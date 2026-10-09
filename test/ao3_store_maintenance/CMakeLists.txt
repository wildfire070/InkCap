add_executable(Ao3StoreMaintenanceTest
  Ao3StoreMaintenanceTest.cpp
  ${REPO_ROOT}/src/Ao3StoreMaintenance.cpp
  ${REPO_ROOT}/src/Ao3MarkedForLaterStore.cpp
  ${REPO_ROOT}/src/Ao3NewChaptersStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
)

target_include_directories(Ao3StoreMaintenanceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/.pio/libdeps/simulator/ArduinoJson/src
)

target_link_libraries(Ao3StoreMaintenanceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(Ao3StoreMaintenanceTest)
