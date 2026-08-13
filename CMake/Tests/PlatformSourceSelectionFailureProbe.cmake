cmake_minimum_required(VERSION 3.25)

include("${DURIN_WORKSPACE_DIR}/CMake/Project/PlatformSources.cmake")
durin_select_platform_sources(_sources _headers "Plan9" "${CMAKE_CURRENT_LIST_FILE}")
