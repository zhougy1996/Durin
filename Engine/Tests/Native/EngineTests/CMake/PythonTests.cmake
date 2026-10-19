if(TARGET DurinPython AND NOT TARGET DurinCPython)
    durin_add_native_test(PythonRuntimeUnavailableTests
        KIND contract
        DOMAINS python
        MODULES durin-python
        STACKS editor
        SOURCES Private/PythonRuntimeUnavailableTests.cpp
        LIBRARIES Core CoreDObject Engine DurinEd DurinPython
        REQUIRES editor
        REQUIREMENT_RATIONALE "Verifies the optional interpreter-unavailable contract without a Python loader dependency."
        HEAVY_RUNTIME_RATIONALE "Starts a CPU editor Host and checks native tool availability while Python is disabled."
    )
else()
    durin_exclude_native_test_sources(
        RATIONALE "The unavailable-runtime contract requires the editor module without an enabled CPython SDK."
        SOURCES Private/PythonRuntimeUnavailableTests.cpp)
endif()
if(NOT TARGET DurinCPython OR NOT TARGET DurinPython)
    durin_exclude_native_test_sources(
        RATIONALE "Requires the optional qualified CPython SDK and editor module."
        SOURCES Private/PythonToolIntegrationTests.cpp Private/MCPPythonJobTests.cpp Private/PythonReflectionIntegrationTests.cpp Private/PythonScriptPanelTests.cpp)
    return()
endif()
durin_add_native_test(PythonScriptPanelTests
    KIND integration
    DOMAINS python editor-shell
    MODULES main-frame durin-python content-browser
    STACKS editor
    PRIVATE_SOURCE_OWNER MainFrame
    PRIVATE_SOURCE_RATIONALE "Exercises the production Python presenter and scoped browser action without exporting host UI state."
    SOURCES Private/PythonScriptPanelTests.cpp
    PRIVATE_SOURCES ${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MainFrame/Private/Panels/PythonScriptPanel.cpp
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Editor/MainFrame/Private
    LIBRARIES Core CoreDObject Engine DurinEd DurinPython ContentBrowser MonaImGui
    REQUIRES editor
    REQUIREMENT_RATIONALE "Runs the browser command through the real interpreter on a CPU editor Host."
    HEAVY_RUNTIME_RATIONALE "Loads native editor modules without an application window or GPU renderer."
)
target_compile_definitions(PythonScriptPanelTests PRIVATE
    DURIN_PYTHON_EXAMPLE_PATH="${CMAKE_SOURCE_DIR}/Engine/Content/Scripts/Examples/CreateTestScene.py")

durin_add_native_test(PythonToolIntegrationTests
    KIND integration
    DOMAINS python editor-tools
    MODULES durin-python durin-ed level-editor
    STACKS editor
    SOURCES Private/PythonToolIntegrationTests.cpp
    LIBRARIES Core CoreDObject Engine DurinEd DurinPython
    RUNTIME_ONLY_TARGETS LevelEditor
    RUNTIME_ONLY_RATIONALE "Loads the LevelEditor PIE provider before initializing a CPU editor host."
    REQUIRES editor
    REQUIREMENT_RATIONALE "Runs embedded CPython against real CPU editor tool dispatch."
    HEAVY_RUNTIME_RATIONALE "Qualifies interpreter lifetime and detached endpoint integration without GPU startup."
)
durin_add_native_test(MCPPythonJobTests
    KIND integration
    DOMAINS python mcp
    MODULES durin-python durin-mcp durin-ed level-editor
    STACKS editor
    PRIVATE_SOURCE_OWNER DurinMCP
    PRIVATE_SOURCE_RATIONALE "MCP script-job HTTP integration uses the private wire codec for structured requests."
    SOURCES Private/MCPPythonJobTests.cpp
    PRIVATE_SOURCES ${CMAKE_SOURCE_DIR}/Engine/Source/Editor/DurinMCP/Private/MCPCodec.cpp
    LIBRARIES Core CoreDObject Engine DurinEd DurinPython DurinMCP httplib::httplib
    RUNTIME_ONLY_TARGETS LevelEditor
    RUNTIME_ONLY_RATIONALE "Loads the LevelEditor PIE provider before initializing a CPU editor host."
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Editor/DurinMCP/Private
    REQUIRES editor
    REQUIREMENT_RATIONALE "Exercises real MCP HTTP sessions and embedded Python against CPU editor hosts."
    HEAVY_RUNTIME_RATIONALE "Validates cross-adapter job lifecycle without GPU startup."
)
set_source_files_properties(Private/MCPPythonJobTests.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set(_python_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/PythonFixture")
file(GLOB_RECURSE _python_fixture_dht_sources CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/Engine/Source/Programs/DurinHeaderTool/durin_header_tool/*.py")
add_custom_command(
    OUTPUT "${_python_fixture_dir}/PythonReflectionTestTypes.gen.h" "${_python_fixture_dir}/PythonReflectionTestTypes.gen.cpp"
    COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/GeneratePythonReflectionFixture.py" "${_python_fixture_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/GeneratePythonReflectionFixture.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Private/PythonReflectionTestTypes.h" ${_python_fixture_dht_sources}
    VERBATIM)
durin_add_native_test(PythonReflectionIntegrationTests
    KIND integration
    DOMAINS python reflection
    MODULES durin-python durin-ed core-dobject
    STACKS editor
    SOURCES Private/PythonReflectionIntegrationTests.cpp
    LIBRARIES Core CoreDObject Engine DurinEd DurinPython
    REQUIRES editor
    REQUIREMENT_RATIONALE "Qualifies native reflection wrapping against the real optional CPython runtime."
    HEAVY_RUNTIME_RATIONALE "Owns one interpreter and CPU-only editor hosts, without application or GPU startup."
)
target_sources(PythonReflectionIntegrationTests PRIVATE "${_python_fixture_dir}/PythonReflectionTestTypes.gen.cpp")
target_include_directories(PythonReflectionIntegrationTests PRIVATE "${_python_fixture_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/Private")
target_compile_definitions(PythonReflectionIntegrationTests PRIVATE PYTHONFIXTURE_API=)
