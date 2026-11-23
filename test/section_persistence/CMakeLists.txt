add_executable(SectionPersistenceTest
  SectionPersistenceTest.cpp
  EpubProgressPersistenceTest.cpp
  SectionPersistenceLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(SectionPersistenceTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(SectionPersistenceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

# This target exercises Section's own shaping-recovery logic (claimShapingRecovery,
# the retry/readiness gating) against the lightweight ComplexShaper stub in
# SectionPersistenceLinkStubs.cpp, not the real shaping engine -- so overriding the
# firmware's own CROSSINK_INDIC_SHAPING=0 default (see platformio.ini) here costs
# nothing in link size and keeps that logic under test even while it's disabled
# in production.
target_compile_definitions(SectionPersistenceTest PRIVATE CROSSINK_INDIC_SHAPING=1)

gtest_discover_tests(SectionPersistenceTest)
