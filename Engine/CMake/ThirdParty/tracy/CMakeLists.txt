set(_durin_tracy_source_dir "${DURIN_WORKSPACE_DIR}/Engine/External/Source/tracy")
set(_durin_tracy_required_files
	"${_durin_tracy_source_dir}/CMakeLists.txt"
	"${_durin_tracy_source_dir}/LICENSE"
	"${_durin_tracy_source_dir}/public/TracyClient.cpp"
	"${_durin_tracy_source_dir}/public/tracy/Tracy.hpp"
)
foreach(_durin_tracy_required_file IN LISTS _durin_tracy_required_files)
	if(NOT EXISTS "${_durin_tracy_required_file}")
		message(FATAL_ERROR
			"Tracy profiling was requested, but its pinned source is missing or incomplete. "
			"Run Engine\\Scripts\\Bootstrap\\Setup_tracy.bat and configure again."
		)
	endif()
endforeach()

set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
set(TRACY_ONLY_LOCALHOST ON CACHE BOOL "" FORCE)
set(TRACY_NO_CALLSTACK ON CACHE BOOL "" FORCE)
set(TRACY_NO_SAMPLING ON CACHE BOOL "" FORCE)
set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
set(TRACY_STATIC OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON)
add_subdirectory("${_durin_tracy_source_dir}" "${CMAKE_BINARY_DIR}/ThirdParty/tracy")

set(_durin_tracy_output_config "$<CONFIG>")
if(DURIN_BUILD_IDENTIFIER)
	string(APPEND _durin_tracy_output_config "-${DURIN_BUILD_IDENTIFIER}")
endif()
set_target_properties(TracyClient PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY
		"${DURIN_WORKSPACE_DIR}/Engine/Binaries/${DURIN_TARGET_PLATFORM}/${_durin_tracy_output_config}/Runtime/${DURIN_RUNTIME_VARIANT}"
)
