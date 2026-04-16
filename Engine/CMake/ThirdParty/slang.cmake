include_guard(GLOBAL)
message(STATUS "  ThirdParty: SlangInterface (Alias: Slang::slang)")

add_library(SlangInterface INTERFACE)
add_library(Slang::slang ALIAS SlangInterface)

set(SLANG_ROOT "${DOGE_PROJECT_SOURCE_DIR}/ThirdParty/slang")

target_include_directories(SlangInterface INTERFACE
        "${SLANG_ROOT}/include"
)

if (WIN32)
    target_link_directories(SlangInterface INTERFACE
            "${SLANG_ROOT}/lib"
    )

    target_link_libraries(SlangInterface INTERFACE
            slang
    )
endif()

function(add_copy_slang_runtime_command target)
    if (WIN32)
        set(SLANG_STAGE_DIR "${DOGE_PROJECT_BINARY_DIR}/ThirdParty/${DOGE_ARCH}/$<CONFIG>")

        add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SLANG_STAGE_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLANG_ROOT}/bin/slang.dll" "${SLANG_STAGE_DIR}/slang.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLANG_ROOT}/bin/slang-compiler.dll" "${SLANG_STAGE_DIR}/slang-compiler.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLANG_ROOT}/bin/slang-glslang.dll" "${SLANG_STAGE_DIR}/slang-glslang.dll"
                VERBATIM
        )
    endif()
endfunction()