cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED DURIN_WORKSPACE_DIR)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR is required.")
endif()

include("${DURIN_WORKSPACE_DIR}/CMake/Project/PlatformSources.cmake")
file(GLOB_RECURSE _fixture_sources LIST_DIRECTORIES FALSE
	"${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/PlatformSources/*")

function(assert_contains values expected)
	if(NOT "${expected}" IN_LIST values)
		message(FATAL_ERROR "Expected source '${expected}' in '${values}'.")
	endif()
endfunction()

function(assert_excludes values expected)
	if("${expected}" IN_LIST values)
		message(FATAL_ERROR "Unexpected source '${expected}' in '${values}'.")
	endif()
endfunction()

set(_common "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/PlatformSources/Common.cpp")
set(_windows "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/PlatformSources/Windows/Windows.cpp")
set(_macos "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/PlatformSources/MacOS/MacOS.cpp")
set(_windows_header "${DURIN_WORKSPACE_DIR}/CMake/Tests/Fixtures/PlatformSources/Windows/Windows.h")

durin_select_platform_sources(_win64_sources _win64_foreign_headers "Win64" ${_fixture_sources})
assert_contains("${_win64_sources}" "${_common}")
assert_contains("${_win64_sources}" "${_windows}")
assert_excludes("${_win64_sources}" "${_macos}")

durin_select_platform_sources(_macos_sources _macos_foreign_headers "MacOS" ${_fixture_sources})
assert_contains("${_macos_sources}" "${_common}")
assert_contains("${_macos_sources}" "${_macos}")
assert_excludes("${_macos_sources}" "${_windows}")
assert_contains("${_macos_foreign_headers}" "${_windows_header}")

execute_process(
	COMMAND "${CMAKE_COMMAND}"
		"-DDURIN_WORKSPACE_DIR=${DURIN_WORKSPACE_DIR}"
		-P "${DURIN_WORKSPACE_DIR}/CMake/Tests/PlatformSourceSelectionFailureProbe.cmake"
	RESULT_VARIABLE _failure_result
	ERROR_VARIABLE _failure_error)
if(_failure_result EQUAL 0 OR NOT _failure_error MATCHES "Unsupported DURIN_TARGET_PLATFORM 'Plan9'")
	message(FATAL_ERROR "Unknown-platform probe did not fail with the expected diagnostic: ${_failure_error}")
endif()

# Freeze the complete Win64 module compilation graph. Changes require an
# explicit baseline review rather than silently entering every module target.
file(GLOB_RECURSE _module_descriptors LIST_DIRECTORIES FALSE
	"${DURIN_WORKSPACE_DIR}/Engine/Source/*.dmodule")
set(_win64_module_compilation_sources)
foreach(_module_descriptor IN LISTS _module_descriptors)
	get_filename_component(_module_dir "${_module_descriptor}" DIRECTORY)
	file(GLOB_RECURSE _module_sources LIST_DIRECTORIES FALSE
		"${_module_dir}/Public/*.cpp"
		"${_module_dir}/Public/*.cc"
		"${_module_dir}/Public/*.cxx"
		"${_module_dir}/Private/*.cpp"
		"${_module_dir}/Private/*.cc"
		"${_module_dir}/Private/*.cxx")
	durin_select_platform_sources(_selected_sources _foreign_headers
		"Win64" ${_module_sources})
	foreach(_selected_source IN LISTS _selected_sources)
		file(RELATIVE_PATH _relative_source
			"${DURIN_WORKSPACE_DIR}" "${_selected_source}")
		cmake_path(CONVERT "${_relative_source}" TO_CMAKE_PATH_LIST _relative_source)
		list(APPEND _win64_module_compilation_sources "${_relative_source}")
	endforeach()
endforeach()
list(REMOVE_DUPLICATES _win64_module_compilation_sources)
list(SORT _win64_module_compilation_sources)
set(_stage0_sources ${_win64_module_compilation_sources})
list(REMOVE_ITEM _stage0_sources
	"Engine/Source/Runtime/Launch/Private/Windows/WindowsProcessCrashServices.cpp"
	"Engine/Source/Runtime/VulkanRHI/Private/Windows/VulkanWindowsPresentationSupport.cpp")
list(LENGTH _stage0_sources _stage0_source_count)
string(JOIN "\n" _stage0_source_manifest ${_stage0_sources})
string(APPEND _stage0_source_manifest "\n")
string(SHA256 _stage0_source_sha256 "${_stage0_source_manifest}")
if(NOT _stage0_source_count EQUAL 445
	OR NOT _stage0_source_sha256 STREQUAL
		"629b43982d80809b713002bc9accb0bec02b55e3f745852f4ea98fa8de9f564e")
	message(FATAL_ERROR
		"Stage 0 Win64 module source baseline changed: "
		"count=${_stage0_source_count}, sha256=${_stage0_source_sha256}.")
endif()
list(LENGTH _win64_module_compilation_sources _win64_source_count)
string(JOIN "\n" _win64_source_manifest ${_win64_module_compilation_sources})
string(APPEND _win64_source_manifest "\n")
string(SHA256 _win64_source_sha256 "${_win64_source_manifest}")
if(NOT _win64_source_count EQUAL 447
	OR NOT _win64_source_sha256 STREQUAL
		"06f47a7091b27d147f6a8e4999549c78cc3267d414bdbd87fd8d8788aa27ad7e")
	message(FATAL_ERROR
		"Win64 module compilation source baseline changed: "
		"count=${_win64_source_count}, sha256=${_win64_source_sha256}.")
endif()
