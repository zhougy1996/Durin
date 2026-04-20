include_guard(GLOBAL)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
add_definitions(-DUNICODE -D_UNICODE)

if(MSVC)
	add_compile_options(/MP)
	add_compile_options($<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:MSVC>>:/utf-8>)
	add_compile_options(/Zc:preprocessor)
	add_compile_options(/FS)
endif()

get_filename_component(DOGE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
message(STATUS "Doge root: ${DOGE_DIR}")

if(MSVC)
	set(DOGE_ARCH "Win64")
elseif(APPLE)
	set(DOGE_ARCH "MacOS")
	set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

message(STATUS "Arch: ${DOGE_ARCH}")

message(STATUS "Generator: ${CMAKE_GENERATOR}")

find_package(Vulkan)
message(STATUS "Vulkan: ${Vulkan_VERSION} (${Vulkan_LIBRARY})")

set(Python_ROOT_DIR "${DOGE_DIR}/.venv")
find_package(Python REQUIRED COMPONENTS Interpreter Development)
message(STATUS "Python: ${Python_VERSION} (${Python_EXECUTABLE})")


