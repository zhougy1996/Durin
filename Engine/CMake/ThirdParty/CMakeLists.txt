include_guard(GLOBAL)

include("${DURIN_WORKSPACE_DIR}/CMake/ThirdParty/ThirdPartyUtils.cmake")

if(DURIN_ENABLE_TRACY)
	add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tracy)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/spdlog)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/glfw)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/glm)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/bc7enc_rdo)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/rapidyaml)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/assimp)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/slang)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/yyjson)

if(BUILD_TESTING)
	add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/googletest)
endif()
