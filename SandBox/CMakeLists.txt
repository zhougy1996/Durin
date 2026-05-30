set(DURIN_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DURIN_PROJECT_BINARY_DIR "${DURIN_PROJECT_DIR}/Binaries")

if(NOT DEFINED DURIN_ARCH)
    if(MSVC)
        set(DURIN_ARCH "Win64")
    elseif(APPLE)
        set(DURIN_ARCH "MacOS")
    endif()
endif()

durin_add_project(SandBox)
