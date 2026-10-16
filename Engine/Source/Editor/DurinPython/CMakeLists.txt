add_durin_module(DurinPython)
set_source_files_properties(Private/PythonRuntime.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
option(DURIN_ENABLE_PYTHON "Enable the optional packaged editor Python runtime" ON)

# Only the qualified SDK enables embedding. Other hosts retain an unavailable path.
set(_python_root "${DURIN_PROJECT_ROOT_DIR}/External/Packages/python/3.13.15/Win64/tools")
if(DURIN_ENABLE_PYTHON AND WIN32 AND EXISTS "${_python_root}/include/Python.h")
    set(_python_zip "${CMAKE_CURRENT_BINARY_DIR}/python313.zip")
    file(GLOB_RECURSE _python_stdlib CONFIGURE_DEPENDS "${_python_root}/Lib/*.py")
    add_custom_command(OUTPUT "${_python_zip}"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${_python_zip}" --format=zip -- .
        WORKING_DIRECTORY "${_python_root}/Lib"
        DEPENDS ${_python_stdlib}
        VERBATIM)
    add_custom_target(DurinPythonStdlib DEPENDS "${_python_zip}")
    set(_durin_python_zip "${CMAKE_CURRENT_BINARY_DIR}/durin-python.zip")
    file(GLOB_RECURSE _durin_python_sources CONFIGURE_DEPENDS
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/Python" "${CMAKE_CURRENT_SOURCE_DIR}/Python/durin/*.py")
    list(TRANSFORM _durin_python_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/Python/"
        OUTPUT_VARIABLE _durin_python_dependencies)
    add_custom_command(OUTPUT "${_durin_python_zip}"
        COMMAND ${CMAKE_COMMAND} -E tar cf "${_durin_python_zip}" --format=zip -- ${_durin_python_sources}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Python"
        DEPENDS ${_durin_python_dependencies}
        VERBATIM)
    # Deploy Python-only edits even when the native module does not relink.
    add_custom_target(DurinPythonPackage
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_durin_python_zip}"
            "${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}/durin-python.zip"
        DEPENDS "${_durin_python_zip}"
        VERBATIM)
    add_library(DurinCPython SHARED IMPORTED GLOBAL)
    set_target_properties(DurinCPython PROPERTIES
        IMPORTED_LOCATION "${_python_root}/python313.dll"
        IMPORTED_IMPLIB "${_python_root}/libs/python313.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_python_root}/include"
        DURIN_RUNTIME_DEPLOY_FILES "${_python_root}/python313.dll;${_python_zip};${_python_root}/LICENSE.txt;${_durin_python_zip}")
    add_dependencies(DurinCPython DurinPythonStdlib DurinPythonPackage)
    target_include_directories(DurinPython PRIVATE "${_python_root}/include")
    add_dependencies(DurinPython DurinPythonStdlib DurinPythonPackage)
    # Keep deployment in the runtime closure without importing CPython symbols.
    set_property(TARGET DurinPython PROPERTY DURIN_RUNTIME_DEPLOY_FILES
        "${_python_root}/python313.dll;${_python_zip};${_python_root}/LICENSE.txt;${_durin_python_zip}")
    target_compile_definitions(DurinPython PRIVATE DURIN_WITH_PYTHON=1)
    target_link_options(DurinPython PRIVATE /NODEFAULTLIB:python313.lib)
    durin_target_deploy_runtime_files(DurinPython DurinCPython)
else()
    target_compile_definitions(DurinPython PRIVATE DURIN_WITH_PYTHON=0)
endif()
