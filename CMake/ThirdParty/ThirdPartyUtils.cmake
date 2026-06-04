# Shared helpers for locating and importing third-party package artifacts.

include_guard(GLOBAL)

function(durin_third_party_get_platform_name out_var)
	if(WIN32)
		set(${out_var} "Win64" PARENT_SCOPE)
	elseif(APPLE)
		set(${out_var} "MacOS" PARENT_SCOPE)
	elseif(UNIX)
		set(${out_var} "Linux" PARENT_SCOPE)
	else()
		set(${out_var} "${CMAKE_SYSTEM_NAME}" PARENT_SCOPE)
	endif()
endfunction()

function(durin_third_party_find_single_file out_var base_dir)
	foreach(candidate IN LISTS ARGN)
		if(EXISTS "${base_dir}/${candidate}")
			set(${out_var} "${base_dir}/${candidate}" PARENT_SCOPE)
			return()
		endif()
	endforeach()

	set(${out_var} "" PARENT_SCOPE)
endfunction()

function(durin_third_party_get_target_platform out_var)
	if(DEFINED DURIN_TARGET_PLATFORM)
		set(${out_var} "${DURIN_TARGET_PLATFORM}" PARENT_SCOPE)
		return()
	endif()

	durin_third_party_get_platform_name(_durin_platform_name)
	set(${out_var} "${_durin_platform_name}" PARENT_SCOPE)
endfunction()

function(durin_third_party_get_package_config out_var)
	if(CMAKE_BUILD_TYPE STREQUAL "Shipping")
		set(${out_var} "Release" PARENT_SCOPE)
	else()
		set(${out_var} "${CMAKE_BUILD_TYPE}" PARENT_SCOPE)
	endif()
endfunction()

function(durin_third_party_get_install_dir out_var package_name)
	if(NOT CMAKE_BUILD_TYPE)
		message(FATAL_ERROR "Durin requires CMAKE_BUILD_TYPE to import installed third-party packages.")
	endif()

	durin_third_party_get_target_platform(_durin_target_platform)
	durin_third_party_get_package_config(_durin_package_config)
	set(${out_var} "${DURIN_WORKSPACE_DIR}/Engine/External/Install/${_durin_target_platform}/${_durin_package_config}/${package_name}" PARENT_SCOPE)
endfunction()

function(durin_third_party_get_arch out_var)
	durin_third_party_get_target_platform(${out_var})
endfunction()
