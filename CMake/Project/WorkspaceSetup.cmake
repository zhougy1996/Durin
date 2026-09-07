include_guard(GLOBAL)

function(add_durin_workspace)

	set(_loader "${CMAKE_SOURCE_DIR}/Tools/DurinDevTool/durin_dev_tool/build/workspace_manifest.py")
	set(_generated "${CMAKE_BINARY_DIR}/DurinWorkspace.cmake")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
		"${CMAKE_SOURCE_DIR}/Durin.dworkspace" "${_loader}"
		"${CMAKE_SOURCE_DIR}/Tools/DurinDevTool/durin_dev_tool/build/descriptors.py")
	execute_process(COMMAND "${Python_EXECUTABLE}" "${_loader}" "${CMAKE_SOURCE_DIR}" "${_generated}"
		RESULT_VARIABLE _result)
	if(NOT _result EQUAL 0)
		message(FATAL_ERROR "Failed to load Durin.dworkspace.")
	endif()
	include("${_generated}")
endfunction()
