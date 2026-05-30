durin_add_module(RenderCore)

target_link_options(RenderCore PRIVATE
    "/DELAYLOAD:slang.dll"
)

target_link_libraries(RenderCore PRIVATE
    Slang_Imported
    delayimp
)

set(SLANG_ROOT "${DURIN_PROJECT_DIR}/External/Packages/slang")
set(SLANG_RUNTIME_DLLS
    "${SLANG_ROOT}/bin/slang.dll"
    "${SLANG_ROOT}/bin/slang-compiler.dll"
    "${SLANG_ROOT}/bin/slang-glslang.dll"
)

durin_copy_external_binaries(RenderCore "${SLANG_RUNTIME_DLLS}")
