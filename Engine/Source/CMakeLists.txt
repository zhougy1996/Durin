function(doge_print_project_build_info)
    message("-- Build ${PROJECT_NAME}")
endfunction()

function(doge_organize_source_files)
    foreach(source ${ARGV})
        file(RELATIVE_PATH relative_path ${CMAKE_CURRENT_SOURCE_DIR} ${source})
        get_filename_component(group_dir ${relative_path} DIRECTORY)

        if(NOT group_dir)
            set(group_name "Source Files")
        else()
            string(REPLACE "/" "\\" group_name ${group_dir})
        endif()

        source_group("${group_name}" FILES ${source})
    endforeach()
endfunction()

function(doge_collect_and_organize_source_files OUT_SRCS)
    set(patterns ${ARGN})
        if(NOT patterns)
            set(patterns 
                "*.h" "*.hpp" "*.hxx"
                "*.c" "*.cpp" "*.cxx" "*.cc"
            )
    endif()
    file(GLOB_RECURSE all_sources CONFIGURE_DEPENDS ${patterns})
    doge_organize_source_files(${all_sources})
    set(${OUT_SRCS} ${all_sources} PARENT_SCOPE)
endfunction()

function(doge_setup_shared_library target)
    doge_set_module_output(${target})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "DogeEditor-${target}")
    string(TOUPPER "${target}" uppercase_target)

    # target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-W4>)

    target_compile_definitions(${target} PRIVATE ${uppercase_target}_EXPORTS)
    target_compile_definitions(${target} PRIVATE MODULE_NAME="${target}")

    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Private
    )
    target_include_directories(${target} PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Public
    )
    target_precompile_headers(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/Private/PCH.${target}.h>"
    )
    install(FILES $<TARGET_FILE:${target}> DESTINATION bin)
endfunction()

add_subdirectory(Runtime)
add_subdirectory(Editor)
add_subdirectory(ThirdParty)