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

# Runtime values and generic storage are allowed below authoring modules;
# source translators and offline build tools may not flow in the opposite direction.
if(TARGET Engine)
	durin_assert_target_dependency_closure_excludes(Engine
		StaticMeshBuild
		SkeletalBuild
		TerrainBuild
		TextureBuild
		AssetForgeBuiltins
		assimp::assimp
	)
endif()

if(TARGET TextureBuild)
	durin_assert_target_dependency_closure_includes(TextureBuild
		DerivedDataCache
		Engine
		bc7enc_rdo::bc7enc_rdo
	)
	durin_assert_target_dependency_closure_excludes(TextureBuild
		StaticMeshBuild
		SkeletalBuild
		TerrainBuild
		AssetForgeBuiltins
		assimp::assimp
	)
endif()

if(TARGET StaticMeshBuild)
	durin_assert_target_dependency_closure_includes(StaticMeshBuild
		DerivedDataCache
		Engine
	)
	durin_assert_target_dependency_closure_excludes(StaticMeshBuild
		SkeletalBuild
		TerrainBuild
		TextureBuild
		AssetForgeBuiltins
		assimp::assimp
		bc7enc_rdo::bc7enc_rdo
		rgbcx
	)
endif()

if(TARGET SkeletalBuild)
	durin_assert_target_dependency_closure_includes(SkeletalBuild
		DerivedDataCache
		Engine
	)
	durin_assert_target_dependency_closure_excludes(SkeletalBuild
		StaticMeshBuild
		TerrainBuild
		TextureBuild
		AssetForgeBuiltins
		assimp::assimp
		bc7enc_rdo::bc7enc_rdo
		rgbcx
	)
endif()

if(TARGET TerrainBuild)
	durin_assert_target_dependency_closure_includes(TerrainBuild
		DerivedDataCache
		Engine
	)
	durin_assert_target_dependency_closure_excludes(TerrainBuild
		StaticMeshBuild
		SkeletalBuild
		TextureBuild
		AssetForgeBuiltins
		assimp::assimp
		bc7enc_rdo::bc7enc_rdo
		rgbcx
	)
endif()

if(TARGET DerivedDataCache)
	durin_assert_target_dependency_closure_includes(DerivedDataCache
		Core
	)
	durin_assert_target_dependency_closure_excludes(DerivedDataCache
		Engine
		StaticMeshBuild
		SkeletalBuild
		TerrainBuild
		TextureBuild
		DurinEd
		RHI
		assimp::assimp
		bc7enc_rdo::bc7enc_rdo
		rgbcx
	)
endif()

if(TARGET AssetForgeBuiltins)
	durin_assert_target_dependency_closure_includes(AssetForgeBuiltins
		StaticMeshBuild
		SkeletalBuild
		TerrainBuild
		TextureBuild
		assimp::assimp
	)
endif()

if(BUILD_TESTING)
	durin_add_native_test_aggregate_target(DurinNativeTests)
	durin_generate_native_test_registry(
		"${CMAKE_BINARY_DIR}/DurinNativeTestRegistry.json")
endif()

if(DURIN_RUNTIME_VARIANT STREQUAL "DurinGame")
	durin_assert_target_dependency_closure_excludes(DurinLauncher
		# Authoring frameworks and hosts.
		AssetImport
		# Derived builders and offline compressors.
		StaticMeshBuild
		SkeletalBuild
		TerrainBuild
		DerivedDataCache
		TextureBuild
		bc7enc_rdo::bc7enc_rdo
		rgbcx
		# Concrete source translators and their SDKs.
		AssetForgeBuiltins
		assimp::assimp
	)
endif()

durin_enforce_code_model_only_build()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT DurinLauncher)
