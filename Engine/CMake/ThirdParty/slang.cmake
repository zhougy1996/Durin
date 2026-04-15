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