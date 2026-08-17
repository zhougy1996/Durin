cmake_minimum_required(VERSION 3.24)
project(DurinNativeTestLauncherProbe LANGUAGES CXX)

include(CTest)
include(GoogleTest)

add_executable(LauncherProbe Probe.cpp)
add_executable(LauncherAdapter Launcher.cpp)
target_compile_features(LauncherProbe PRIVATE cxx_std_20)
target_compile_features(LauncherAdapter PRIVATE cxx_std_20)
set_property(TARGET LauncherProbe PROPERTY TEST_LAUNCHER
	"$<TARGET_FILE:LauncherAdapter>"
	"--log"
	"${CMAKE_BINARY_DIR}/launcher.log"
	"--")
gtest_discover_tests(LauncherProbe DISCOVERY_MODE PRE_TEST)
add_test(NAME LauncherProbeWholeTarget COMMAND LauncherProbe --durin-whole-target)
