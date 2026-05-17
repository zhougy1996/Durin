include_guard(GLOBAL)

include(${CMAKE_CURRENT_LIST_DIR}/BuildConfigs.cmake)

if(NOT DEFINED ENABLE_DURIN_TIMER)
    set(ENABLE_DURIN_TIMER OFF)
endif()

if(ENABLE_DURIN_TIMER)
    message(STATUS "[Timer] Profiling is ENABLED")
else()
    function(durin_start BLOCK_NAME)
    endfunction()
    function(durin_end)
    endfunction()
    return()
endif()

# Initialize global stacks
set_property(GLOBAL PROPERTY TIMER_START_S_STACK "")
set_property(GLOBAL PROPERTY TIMER_START_F_STACK "")
set_property(GLOBAL PROPERTY TIMER_NAME_STACK "")
set_property(GLOBAL PROPERTY TIMER_INDENT_LEVEL 0)

function(durin_start BLOCK_NAME)
    # Get current time: %s is seconds, %f is microseconds (6 digits)
    string(TIMESTAMP CUR_S "%s")
    string(TIMESTAMP CUR_F "%f")

    get_property(S_STACK GLOBAL PROPERTY TIMER_START_S_STACK)
    get_property(F_STACK GLOBAL PROPERTY TIMER_START_F_STACK)
    get_property(N_STACK GLOBAL PROPERTY TIMER_NAME_STACK)
    get_property(INDENT GLOBAL PROPERTY TIMER_INDENT_LEVEL)

    # Indentation for visual hierarchy
    set(SPACES "")
    if(INDENT GREATER 0)
        math(EXPR RANGE_MAX "${INDENT} - 1")
        foreach(I RANGE ${RANGE_MAX})
            string(APPEND SPACES "  ")
        endforeach()
    endif()

    message(STATUS "${SPACES}[START] ${BLOCK_NAME}")

    # Push values to global stacks
    list(APPEND S_STACK "${CUR_S}")
    list(APPEND F_STACK "${CUR_F}")
    list(APPEND N_STACK "${BLOCK_NAME}")

    math(EXPR NEXT_INDENT "${INDENT} + 1")
    set_property(GLOBAL PROPERTY TIMER_START_S_STACK "${S_STACK}")
    set_property(GLOBAL PROPERTY TIMER_START_F_STACK "${F_STACK}")
    set_property(GLOBAL PROPERTY TIMER_NAME_STACK "${N_STACK}")
    set_property(GLOBAL PROPERTY TIMER_INDENT_LEVEL ${NEXT_INDENT})
endfunction()

function(durin_end)
    string(TIMESTAMP END_S "%s")
    string(TIMESTAMP END_F "%f")

    get_property(S_STACK GLOBAL PROPERTY TIMER_START_S_STACK)
    get_property(F_STACK GLOBAL PROPERTY TIMER_START_F_STACK)
    get_property(N_STACK GLOBAL PROPERTY TIMER_NAME_STACK)
    get_property(INDENT GLOBAL PROPERTY TIMER_INDENT_LEVEL)

    list(LENGTH S_STACK STACK_LEN)
    if(STACK_LEN EQUAL 0)
        return()
    endif()

    # Pop the last entry (LIFO)
    math(EXPR LAST_IDX "${STACK_LEN} - 1")
    list(GET S_STACK ${LAST_IDX} START_S)
    list(GET F_STACK ${LAST_IDX} START_F)
    list(GET N_STACK ${LAST_IDX} BLOCK_NAME)

    list(REMOVE_AT S_STACK ${LAST_IDX})
    list(REMOVE_AT F_STACK ${LAST_IDX})
    list(REMOVE_AT N_STACK ${LAST_IDX})

    math(EXPR PREV_INDENT "${INDENT} - 1")

    # --- Duration Calculation ---
    # Calculate difference in seconds and microseconds
    math(EXPR DIFF_S "${END_S} - ${START_S}")
    math(EXPR DIFF_F "${END_F} - ${START_F}")

    # Adjust if microseconds difference is negative
    if(DIFF_F LESS 0)
        math(EXPR DIFF_S "${DIFF_S} - 1")
        math(EXPR DIFF_F "${DIFF_F} + 1000000")
    endif()

    # Convert microseconds to milliseconds (3 digits)
    math(EXPR DIFF_MS "${DIFF_F} / 1000")

    # Visualization: Formatting the MS to always be 3 digits (e.g., 005ms)
    string(LENGTH "${DIFF_MS}" MS_LEN)
    if(MS_LEN EQUAL 1)
        set(DIFF_MS "00${DIFF_MS}")
    elseif(MS_LEN EQUAL 2)
        set(DIFF_MS "0${DIFF_MS}")
    endif()

    set(SPACES "")
    if(PREV_INDENT GREATER 0)
        math(EXPR RANGE_MAX "${PREV_INDENT} - 1")
        foreach(I RANGE ${RANGE_MAX})
            string(APPEND SPACES "  ")
        endforeach()
    endif()

    # Final Output: seconds and milliseconds
    message(STATUS "${SPACES}[END] ${BLOCK_NAME} | Time: ${DIFF_S}s ${DIFF_MS}ms")

    # Sync back to global properties
    set_property(GLOBAL PROPERTY TIMER_START_S_STACK "${S_STACK}")
    set_property(GLOBAL PROPERTY TIMER_START_F_STACK "${F_STACK}")
    set_property(GLOBAL PROPERTY TIMER_NAME_STACK "${N_STACK}")
    set_property(GLOBAL PROPERTY TIMER_INDENT_LEVEL ${PREV_INDENT})
endfunction()