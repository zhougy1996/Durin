# Shared precompiled-header and common compile-definition helpers.

include_guard(GLOBAL)

function(durin_target_apply_runtime_variant_definitions target)
	target_compile_definitions(${target} PRIVATE
		$<$<CONFIG:Debug>:DURIN_BUILD_DEBUG=1>
		$<$<CONFIG:Release>:DURIN_BUILD_RELEASE=1>
		$<$<CONFIG:Shipping>:DURIN_BUILD_SHIPPING=1>
		DURIN_WITH_EDITOR=${DURIN_WITH_EDITOR}
		DURIN_WITH_TRACY=${DURIN_WITH_TRACY}
		DURIN_RUNTIME_VARIANT="${DURIN_RUNTIME_VARIANT}"
	)
endfunction()

function(durin_target_apply_common_definitions target module_name)
	durin_target_apply_runtime_variant_definitions(${target})

	target_compile_definitions(${target} PRIVATE
		MODULE_NAME="${module_name}"
	)

	if(DURIN_ENABLE_TRACY)
		target_link_libraries(${target} PRIVATE Tracy::TracyClient)
	endif()
endfunction()

function(add_durin_shared_pch target_name)
	set(options)
	set(one_value_args HEADER)
	set(multi_value_args INCLUDE_DIRECTORIES LINK_LIBRARIES)
	cmake_parse_arguments(DURIN_SHARED_PCH "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

	if(NOT DURIN_SHARED_PCH_HEADER)
		message(FATAL_ERROR "add_durin_shared_pch(${target_name}) requires HEADER.")
	endif()

	set_property(GLOBAL PROPERTY "DURIN_SHARED_PCH_HEADER_${target_name}" "${DURIN_SHARED_PCH_HEADER}")

	if(NOT DURIN_ENABLE_PCH)
		return()
	endif()

	set(_durin_shared_pch_dummy_src "${DURIN_PROJECT_ROOT_DIR}/CMake/SharedPCH/SharedPCHDummy.cpp")
	add_library(${target_name} STATIC "${_durin_shared_pch_dummy_src}")

	durin_target_apply_runtime_variant_definitions(${target_name})

	if(DURIN_SHARED_PCH_INCLUDE_DIRECTORIES)
		target_include_directories(${target_name} PUBLIC ${DURIN_SHARED_PCH_INCLUDE_DIRECTORIES})
	endif()

	if(DURIN_SHARED_PCH_LINK_LIBRARIES)
		target_link_libraries(${target_name} PUBLIC ${DURIN_SHARED_PCH_LINK_LIBRARIES})
	endif()

	target_precompile_headers(${target_name} PUBLIC
		"$<$<COMPILE_LANGUAGE:CXX>:${DURIN_SHARED_PCH_HEADER}>"
	)
endfunction()
