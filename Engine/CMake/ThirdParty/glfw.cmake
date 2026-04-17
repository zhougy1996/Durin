include_guard(GLOBAL)

set(target_name Doge_Glfw3)
message(STATUS "  ThirdParty: ${target_name}")

add_library(${target_name} INTERFACE)

set(GLFW_ROOT "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/glfw/glfw-3.4.bin.WIN64")

target_include_directories(${target_name} INTERFACE "${GLFW_ROOT}/include")
if (WIN32)
    target_link_directories(${target_name} INTERFACE "${GLFW_ROOT}/lib-vc2022")
    target_link_libraries(${target_name} INTERFACE glfw3)
endif()
