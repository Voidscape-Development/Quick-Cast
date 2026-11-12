find_package(GTest REQUIRED)

add_executable(
  quick-cast-tests
  test-crypto.cpp
  test-data-messages.cpp
  test-identity.cpp
  test-links.cpp
  test-messages.cpp
  test-reorder-buffer.cpp
  test-rtcp.cpp
  test-sdp-utils.cpp
  test-sync-clock.cpp
  test-video-depacketizer.cpp
)
target_link_libraries(quick-cast-tests PRIVATE quick-cast-core GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(quick-cast-tests)

# Developer tool: joins a show from the command line and reports what it receives. Used for end-to-end checks
# against real browser guests without needing OBS.
add_executable(quick-cast-probe ../tools/quick-cast-probe.cpp)
target_link_libraries(quick-cast-probe PRIVATE quick-cast-core)
