# Deterministic traversal of concrete CMake target link dependencies.

include_guard(GLOBAL)

function(durin_normalize_target_alias out_var target_name)
	if(NOT TARGET "${target_name}")
		message(FATAL_ERROR "Cannot normalize missing target ${target_name}.")
	endif()

	get_target_property(_durin_aliased_target "${target_name}" ALIASED_TARGET)
	if(_durin_aliased_target)
		set(${out_var} "${_durin_aliased_target}" PARENT_SCOPE)
	else()
		set(${out_var} "${target_name}" PARENT_SCOPE)
	endif()
endfunction()

function(_durin_active_config_matches out_var config_list)
	if(NOT CMAKE_BUILD_TYPE)
		message(FATAL_ERROR
			"Cannot resolve a configuration-dependent target link without "
			"CMAKE_BUILD_TYPE in the configured build tree.")
	endif()

	string(TOUPPER "${CMAKE_BUILD_TYPE}" _durin_active_config)
	string(REPLACE "," ";" _durin_candidate_configs "${config_list}")
	set(_durin_matches FALSE)
	foreach(_durin_candidate_config IN LISTS _durin_candidate_configs)
		string(TOUPPER "${_durin_candidate_config}" _durin_candidate_config)
		if(_durin_candidate_config STREQUAL _durin_active_config)
			set(_durin_matches TRUE)
			break()
		endif()
	endforeach()

	set(${out_var} "${_durin_matches}" PARENT_SCOPE)
endfunction()

function(_durin_link_expression_contains_target out_var link_item)
	string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.:+-]*"
		_durin_expression_tokens "${link_item}")
	foreach(_durin_expression_token IN LISTS _durin_expression_tokens)
		if(TARGET "${_durin_expression_token}")
			set(${out_var} TRUE PARENT_SCOPE)
			return()
		endif()
	endforeach()
	string(REGEX MATCHALL "[:,][A-Za-z_][A-Za-z0-9_.:+-]*"
		_durin_expression_arguments "${link_item}")
	foreach(_durin_expression_argument IN LISTS _durin_expression_arguments)
		string(SUBSTRING "${_durin_expression_argument}" 1 -1
			_durin_expression_argument)
		if(TARGET "${_durin_expression_argument}")
			set(${out_var} TRUE PARENT_SCOPE)
			return()
		endif()
	endforeach()

	if(link_item MATCHES
		"\\$<(TARGET_NAME|TARGET_PROPERTY|TARGET_FILE|LINK_ONLY|LINK_LIBRARY|LINK_GROUP):")
		set(${out_var} TRUE PARENT_SCOPE)
	else()
		set(${out_var} FALSE PARENT_SCOPE)
	endif()
endfunction()

# Resolves the target-bearing forms which CMake stores in LINK_LIBRARIES and
# INTERFACE_LINK_LIBRARIES. Non-target paths, flags, and system libraries
# intentionally resolve to an empty list.
function(durin_resolve_target_link_item out_var owning_target link_item)
	set(_durin_resolved_targets)
	if(link_item MATCHES "^\\$<")
		set_property(GLOBAL APPEND PROPERTY
			DURIN_TARGET_DEPENDENCY_EXPRESSIONS
			"${owning_target}|${link_item}")
	endif()

	if(NOT link_item OR link_item MATCHES "-NOTFOUND$")
		set(${out_var} "" PARENT_SCOPE)
		return()
	endif()

	if(TARGET "${link_item}")
		durin_normalize_target_alias(_durin_resolved_target "${link_item}")
		set(${out_var} "${_durin_resolved_target}" PARENT_SCOPE)
		return()
	endif()

	if(link_item MATCHES "^::@")
		# Directory-scope wrappers are stored as separate list entries around the
		# concrete target item.
		set(${out_var} "" PARENT_SCOPE)
		return()
	elseif(link_item MATCHES "^\\$<LINK_ONLY:(.*)>$")
		durin_resolve_target_link_item(
			_durin_resolved_targets
			"${owning_target}"
			"${CMAKE_MATCH_1}"
		)
	elseif(link_item MATCHES "^\\$<TARGET_NAME:(.*)>$")
		set(_durin_named_target "${CMAKE_MATCH_1}")
		if(NOT TARGET "${_durin_named_target}")
			message(FATAL_ERROR
				"Target ${owning_target} references missing target "
				"${_durin_named_target} in expression '${link_item}'.")
		endif()
		durin_normalize_target_alias(
			_durin_named_target
			"${_durin_named_target}"
		)
		list(APPEND _durin_resolved_targets "${_durin_named_target}")
	elseif(link_item MATCHES "^\\$<TARGET_NAME_IF_EXISTS:(.*)>$")
		set(_durin_optional_target "${CMAKE_MATCH_1}")
		if(TARGET "${_durin_optional_target}")
			durin_normalize_target_alias(
				_durin_optional_target
				"${_durin_optional_target}"
			)
			list(APPEND _durin_resolved_targets "${_durin_optional_target}")
		endif()
	elseif(link_item MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
		durin_resolve_target_link_item(
			_durin_resolved_targets
			"${owning_target}"
			"${CMAKE_MATCH_1}"
		)
	elseif(link_item MATCHES "^\\$<INSTALL_INTERFACE:.*>$")
		# Install-only edges are not part of the configured build-tree closure.
		set(_durin_resolved_targets)
	elseif(link_item MATCHES "^\\$<\\$<CONFIG:([^>]+)>:(.*)>$")
		set(_durin_configs "${CMAKE_MATCH_1}")
		set(_durin_config_value "${CMAKE_MATCH_2}")
		_durin_active_config_matches(_durin_config_matches "${_durin_configs}")
		if(_durin_config_matches)
			durin_resolve_target_link_item(
				_durin_resolved_targets
				"${owning_target}"
				"${_durin_config_value}"
			)
		endif()
	elseif(link_item MATCHES
		"^\\$<\\$<NOT:\\$<CONFIG:([^>]+)>>:(.*)>$")
		set(_durin_configs "${CMAKE_MATCH_1}")
		set(_durin_config_value "${CMAKE_MATCH_2}")
		_durin_active_config_matches(_durin_config_matches "${_durin_configs}")
		if(NOT _durin_config_matches)
			durin_resolve_target_link_item(
				_durin_resolved_targets
				"${owning_target}"
				"${_durin_config_value}"
			)
		endif()
	elseif(link_item MATCHES
		"^\\$<IF:\\$<CONFIG:([^>]+)>,([^,]*),([^>]*)>$")
		set(_durin_configs "${CMAKE_MATCH_1}")
		set(_durin_true_value "${CMAKE_MATCH_2}")
		set(_durin_false_value "${CMAKE_MATCH_3}")
		_durin_active_config_matches(_durin_config_matches "${_durin_configs}")
		if(_durin_config_matches)
			set(_durin_config_value "${_durin_true_value}")
		else()
			set(_durin_config_value "${_durin_false_value}")
		endif()
		durin_resolve_target_link_item(
			_durin_resolved_targets
			"${owning_target}"
			"${_durin_config_value}"
		)
	elseif(link_item MATCHES "^\\$<LINK_(LIBRARY|GROUP):[^,>]+,(.*)>$")
		set(_durin_link_values "${CMAKE_MATCH_2}")
		string(REPLACE "," ";" _durin_link_values "${_durin_link_values}")
		foreach(_durin_link_value IN LISTS _durin_link_values)
			durin_resolve_target_link_item(
				_durin_link_targets
				"${owning_target}"
				"${_durin_link_value}"
			)
			list(APPEND _durin_resolved_targets ${_durin_link_targets})
		endforeach()
	elseif(link_item MATCHES "^\\$<")
		_durin_link_expression_contains_target(
			_durin_expression_contains_target
			"${link_item}"
		)
		if(_durin_expression_contains_target)
			message(FATAL_ERROR
				"Target ${owning_target} has unsupported target-bearing link "
				"expression '${link_item}'.")
		endif()
	endif()

	list(REMOVE_DUPLICATES _durin_resolved_targets)
	list(SORT _durin_resolved_targets)
	set(${out_var} "${_durin_resolved_targets}" PARENT_SCOPE)
endfunction()

function(durin_report_target_dependency_expression_audit)
	get_property(_durin_dependency_expressions GLOBAL PROPERTY
		DURIN_TARGET_DEPENDENCY_EXPRESSIONS)
	list(REMOVE_DUPLICATES _durin_dependency_expressions)
	list(SORT _durin_dependency_expressions)
	message(STATUS "[TargetDependencyExpressions]")
	foreach(_durin_dependency_expression IN LISTS _durin_dependency_expressions)
		message(STATUS "  ${_durin_dependency_expression}")
	endforeach()
endfunction()

function(durin_collect_target_dependency_closure out_var)
	if(NOT ARGN)
		message(FATAL_ERROR "Target dependency closure requires at least one root.")
	endif()

	set(_durin_dependency_queue)
	foreach(_durin_root IN LISTS ARGN)
		if(NOT TARGET "${_durin_root}")
			message(FATAL_ERROR
				"Cannot collect dependency closure for missing target ${_durin_root}.")
		endif()
		durin_normalize_target_alias(_durin_root "${_durin_root}")
		list(APPEND _durin_dependency_queue "${_durin_root}")
	endforeach()
	list(REMOVE_DUPLICATES _durin_dependency_queue)
	list(SORT _durin_dependency_queue)

	set(_durin_visited_dependencies)
	while(_durin_dependency_queue)
		list(POP_FRONT _durin_dependency_queue _durin_dependency)
		durin_normalize_target_alias(_durin_dependency "${_durin_dependency}")
		if(_durin_dependency IN_LIST _durin_visited_dependencies)
			continue()
		endif()
		list(APPEND _durin_visited_dependencies "${_durin_dependency}")

		get_target_property(_durin_private_links
			"${_durin_dependency}" LINK_LIBRARIES)
		get_target_property(_durin_public_links
			"${_durin_dependency}" INTERFACE_LINK_LIBRARIES)
		set(_durin_direct_dependencies)
		foreach(_durin_link IN LISTS _durin_private_links _durin_public_links)
			durin_resolve_target_link_item(
				_durin_resolved_links
				"${_durin_dependency}"
				"${_durin_link}"
			)
			list(APPEND _durin_direct_dependencies ${_durin_resolved_links})
		endforeach()
		list(REMOVE_DUPLICATES _durin_direct_dependencies)
		list(SORT _durin_direct_dependencies)
		list(APPEND _durin_dependency_queue ${_durin_direct_dependencies})
	endwhile()

	list(REMOVE_DUPLICATES _durin_visited_dependencies)
	list(SORT _durin_visited_dependencies)
	set(${out_var} "${_durin_visited_dependencies}" PARENT_SCOPE)
endfunction()
