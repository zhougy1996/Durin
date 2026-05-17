durin_add_module(RenderCore)

target_link_options(RenderCore PRIVATE
    "/DELAYLOAD:slang.dll"
)

target_link_libraries(RenderCore PRIVATE
    Slang_Imported
    delayimp
)

set(destination_dir "${DURIN_PROJECT_DIR}/Binaries/ThirdParty/${DURIN_ARCH}/$<CONFIG>")
set(SLANG_ROOT "${DURIN_PROJECT_SOURCE_DIR}/ThirdParty/slang")
add_custom_command(TARGET RenderCore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${destination_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLANG_ROOT}/bin/slang.dll" "${destination_dir}/slang.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLANG_ROOT}/bin/slang-compiler.dll" "${destination_dir}/slang-compiler.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SLANG_ROOT}/bin/slang-glslang.dll" "${destination_dir}/slang-glslang.dll"
)