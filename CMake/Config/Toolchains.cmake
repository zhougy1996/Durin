# Toolchain and host-platform discovery shared by the workspace build.

include_guard(GLOBAL)

get_filename_component(DURIN_WORKSPACE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(DURIN_DIR "${DURIN_WORKSPACE_DIR}")
message(STATUS "Durin root: ${DURIN_WORKSPACE_DIR}")

if(MSVC)
	set(DURIN_TARGET_PLATFORM "Win64")
elseif(APPLE)
	set(DURIN_TARGET_PLATFORM "MacOS")
	set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

set(DURIN_ARCH "${DURIN_TARGET_PLATFORM}")

message(STATUS "Platform: ${DURIN_TARGET_PLATFORM}")
message(STATUS "Generator: ${CMAKE_GENERATOR}")

find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_VERSION} (${Vulkan_LIBRARY})")

set(Python_ROOT_DIR "${DURIN_WORKSPACE_DIR}/.venv")
find_package(Python REQUIRED COMPONENTS Interpreter Development)
message(STATUS "Python: ${Python_VERSION} (${Python_EXECUTABLE})")
