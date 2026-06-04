# Optional configure-time timing helpers for nested CMake work.

include_guard(GLOBAL)

if(ENABLE_DURIN_TIMER)
	message(STATUS "[Timer] Profiling is ENABLED")
else()
	function(durin_start block_name)
	endfunction()

	function(durin_end)
	endfunction()

	return()
endif()

set_property(GLOBAL PROPERTY TIMER_START_S_STACK "")
set_property(GLOBAL PROPERTY TIMER_START_F_STACK "")
set_property(GLOBAL PROPERTY TIMER_NAME_STACK "")
set_property(GLOBAL PROPERTY TIMER_INDENT_LEVEL 0)

function(durin_start block_name)
	string(TIMESTAMP CUR_S "%s")
	string(TIMESTAMP CUR_F "%f")

	get_property(s_stack GLOBAL PROPERTY TIMER_START_S_STACK)
	get_property(f_stack GLOBAL PROPERTY TIMER_START_F_STACK)
	get_property(n_stack GLOBAL PROPERTY TIMER_NAME_STACK)
	get_property(indent GLOBAL PROPERTY TIMER_INDENT_LEVEL)

	set(spaces "")
	if(indent GREATER 0)
		math(EXPR range_max "${indent} - 1")
		foreach(i RANGE ${range_max})
			string(APPEND spaces "  ")
		endforeach()
	endif()

	message(STATUS "${spaces}[START] ${block_name}")

	list(APPEND s_stack "${CUR_S}")
	list(APPEND f_stack "${CUR_F}")
	list(APPEND n_stack "${block_name}")

	math(EXPR next_indent "${indent} + 1")
	set_property(GLOBAL PROPERTY TIMER_START_S_STACK "${s_stack}")
	set_property(GLOBAL PROPERTY TIMER_START_F_STACK "${f_stack}")
	set_property(GLOBAL PROPERTY TIMER_NAME_STACK "${n_stack}")
	set_property(GLOBAL PROPERTY TIMER_INDENT_LEVEL ${next_indent})
endfunction()

function(durin_end)
	string(TIMESTAMP END_S "%s")
	string(TIMESTAMP END_F "%f")

	get_property(s_stack GLOBAL PROPERTY TIMER_START_S_STACK)
	get_property(f_stack GLOBAL PROPERTY TIMER_START_F_STACK)
	get_property(n_stack GLOBAL PROPERTY TIMER_NAME_STACK)
	get_property(indent GLOBAL PROPERTY TIMER_INDENT_LEVEL)

	list(LENGTH s_stack stack_len)
	if(stack_len EQUAL 0)
		return()
	endif()

	math(EXPR last_idx "${stack_len} - 1")
	list(GET s_stack ${last_idx} start_s)
	list(GET f_stack ${last_idx} start_f)
	list(GET n_stack ${last_idx} block_name)

	list(REMOVE_AT s_stack ${last_idx})
	list(REMOVE_AT f_stack ${last_idx})
	list(REMOVE_AT n_stack ${last_idx})

	math(EXPR prev_indent "${indent} - 1")
	math(EXPR diff_s "${END_S} - ${start_s}")
	math(EXPR diff_f "${END_F} - ${start_f}")

	if(diff_f LESS 0)
		math(EXPR diff_s "${diff_s} - 1")
		math(EXPR diff_f "${diff_f} + 1000000")
	endif()

	math(EXPR diff_ms "${diff_f} / 1000")

	string(LENGTH "${diff_ms}" ms_len)
	if(ms_len EQUAL 1)
		set(diff_ms "00${diff_ms}")
	elseif(ms_len EQUAL 2)
		set(diff_ms "0${diff_ms}")
	endif()

	set(spaces "")
	if(prev_indent GREATER 0)
		math(EXPR range_max "${prev_indent} - 1")
		foreach(i RANGE ${range_max})
			string(APPEND spaces "  ")
		endforeach()
	endif()

	message(STATUS "${spaces}[END] ${block_name} | Time: ${diff_s}s ${diff_ms}ms")

	set_property(GLOBAL PROPERTY TIMER_START_S_STACK "${s_stack}")
	set_property(GLOBAL PROPERTY TIMER_START_F_STACK "${f_stack}")
	set_property(GLOBAL PROPERTY TIMER_NAME_STACK "${n_stack}")
	set_property(GLOBAL PROPERTY TIMER_INDENT_LEVEL ${prev_indent})
endfunction()
