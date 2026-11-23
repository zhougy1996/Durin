add_durin_module(DurinPython)
set_source_files_properties(Private/PythonRuntime.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
option(DURIN_ENABLE_PYTHON "Enable the optional packaged editor Python runtime" ON)

# Only the qualified SDK enables embedding. Other hosts retain an unavailable path.
set(_python_root "${DURIN_PROJECT_ROOT_DIR}/External/Packages/python/3.13.15/Win64/tools")
set(DURIN_PYTHON_SDK_ROOT "" CACHE PATH "Explicit CPython 3.13.15 GIL SDK for embedding verification")
if(DURIN_PYTHON_SDK_ROOT)
    set(_python_root "${DURIN_PYTHON_SDK_ROOT}")
endif()
set(_python_library "${_python_root}/python313.dll")
if(APPLE)
    set(_python_library "${_python_root}/libpython3.13.dylib")
endif()
if(DURIN_ENABLE_PYTHON AND (WIN32 OR (APPLE AND DURIN_PYTHON_SDK_ROOT)) AND EXISTS "${_python_root}/include/Python.h")
    file(READ "${_python_root}/include/patchlevel.h" _python_version)
    if(NOT _python_version MATCHES "PY_VERSION[ \t]+\"3.13.15\"")
        message(FATAL_ERROR "Durin embedding requires CPython 3.13.15 release GIL ABI")
    endif()
    set(_python_zip "${CMAKE_CURRENT_BINARY_DIR}/python313.zip")
    file(GLOB_RECURSE _python_stdlib CONFIGURE_DEPENDS "${_python_root}/Lib/*.py")
    add_custom_command(OUTPUT "${_python_zip}"
        COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PackagePython.py" "${_python_root}/Lib" "${_python_zip}"
        DEPENDS ${_python_stdlib} "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PackagePython.py"
        VERBATIM)
    add_custom_target(DurinPythonStdlib DEPENDS "${_python_zip}")
    set(_durin_python_zip "${CMAKE_CURRENT_BINARY_DIR}/durin-python.zip")
    set(_durin_python_root "${CMAKE_CURRENT_SOURCE_DIR}/Python")
    file(GLOB_RECURSE _durin_python_sources CONFIGURE_DEPENDS "${_durin_python_root}/*.py")
    # A removed source changes the inventory even if all remaining files predate the zip.
    set(_durin_python_inventory "${CMAKE_CURRENT_BINARY_DIR}/durin-python.inputs")
    file(GENERATE OUTPUT "${_durin_python_inventory}" CONTENT "${_durin_python_sources}\n")
    add_custom_command(OUTPUT "${_durin_python_zip}"
        COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PackagePython.py" "${_durin_python_root}" "${_durin_python_zip}"
        DEPENDS ${_durin_python_sources} "${_durin_python_inventory}" "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PackagePython.py"
        COMMENT "Packaging durin-python.zip"
        VERBATIM)
    # Deploy Python-only edits even when the native module does not relink.
    set(_durin_python_runtime_zip "${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}/durin-python.zip")
    add_custom_command(OUTPUT "${_durin_python_runtime_zip}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${DURIN_PROJECT_EXTERNAL_RUNTIME_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_durin_python_zip}" "${_durin_python_runtime_zip}"
        DEPENDS "${_durin_python_zip}"
        COMMENT "Deploying durin-python.zip"
        VERBATIM)
    add_custom_target(DurinPythonPackage ALL DEPENDS "${_durin_python_runtime_zip}")
    set(_python_runtime_files "${_python_library};${_python_zip};${_python_root}/LICENSE.txt")
    set(_python_deploy_files "${_python_runtime_files};${_durin_python_zip}")
    add_library(DurinCPython SHARED IMPORTED GLOBAL)
    set_target_properties(DurinCPython PROPERTIES
        IMPORTED_LOCATION "${_python_library}"
        IMPORTED_IMPLIB "${_python_root}/libs/python313.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_python_root}/include"
        DURIN_RUNTIME_DEPLOY_FILES "${_python_deploy_files}")
    add_dependencies(DurinCPython DurinPythonStdlib DurinPythonPackage)
    target_include_directories(DurinPython PRIVATE "${_python_root}/include")
    add_dependencies(DurinPython DurinPythonStdlib DurinPythonPackage)
    # Keep deployment in the runtime closure without importing CPython symbols.
    set_property(TARGET DurinPython PROPERTY DURIN_RUNTIME_DEPLOY_FILES "${_python_deploy_files}")
    target_compile_definitions(DurinPython PRIVATE DURIN_WITH_PYTHON=1)
    get_filename_component(_python_library_name "${_python_library}" NAME)
    target_compile_definitions(DurinPython PRIVATE DURIN_PYTHON_LIBRARY="${_python_library_name}")
    if(WIN32)
        target_link_options(DurinPython PRIVATE /NODEFAULTLIB:python313.lib)
    endif()
    durin_target_deploy_files(DurinPython "${_python_runtime_files}")
else()
    target_compile_definitions(DurinPython PRIVATE DURIN_WITH_PYTHON=0)
endif()
