cmake_minimum_required(VERSION 3.24)

set(DURIN_ENGINE_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Engine/Build/Build.version")
file(READ "${DURIN_ENGINE_VERSION_FILE}" _durin_engine_version_json)
string(JSON DURIN_ENGINE_VERSION_MAJOR GET "${_durin_engine_version_json}" Major)
string(JSON DURIN_ENGINE_VERSION_MINOR GET "${_durin_engine_version_json}" Minor)
string(JSON DURIN_ENGINE_VERSION_PATCH GET "${_durin_engine_version_json}" Patch)
string(JSON DURIN_ENGINE_VERSION_CHANNEL GET "${_durin_engine_version_json}" Channel)

foreach(_durin_version_component IN ITEMS MAJOR MINOR PATCH)
	if(NOT DURIN_ENGINE_VERSION_${_durin_version_component} MATCHES "^[0-9]+$")
		message(FATAL_ERROR "Durin engine version ${_durin_version_component} must be a non-negative integer.")
	endif()
endforeach()
if(DURIN_ENGINE_VERSION_CHANNEL AND NOT DURIN_ENGINE_VERSION_CHANNEL MATCHES "^[0-9A-Za-z][0-9A-Za-z.-]*$")
	message(FATAL_ERROR "Durin engine version channel must be empty or a SemVer-compatible prerelease identifier.")
endif()

project(Durin VERSION
	"${DURIN_ENGINE_VERSION_MAJOR}.${DURIN_ENGINE_VERSION_MINOR}.${DURIN_ENGINE_VERSION_PATCH}"
	LANGUAGES CXX
)

set(DURIN_ENGINE_VERSION_STRING "${PROJECT_VERSION}")
if(DURIN_ENGINE_VERSION_CHANNEL)
	string(APPEND DURIN_ENGINE_VERSION_STRING "-${DURIN_ENGINE_VERSION_CHANNEL}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DURIN_ENGINE_VERSION_FILE}")

include(CTest)

set(DURIN_CMAKE_DIR "${PROJECT_SOURCE_DIR}/CMake")
include("${DURIN_CMAKE_DIR}/DurinWorkspaceSetup.cmake")

add_subdirectory(Engine)
add_subdirectory(Sandbox)

if(BUILD_TESTING)
	durin_add_native_test_aggregate_target(DurinNativeTests)
endif()

if(DURIN_PROFILE_NAME STREQUAL "DurinGame")
	durin_assert_target_dependency_closure_excludes(DurinLauncher
		AssetImport
		EngineAssetBuild
		assimp::assimp
		bc7enc_rdo::bc7enc_rdo
		rgbcx
	)
endif()

durin_enforce_code_model_only_build()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DurinLauncher)
