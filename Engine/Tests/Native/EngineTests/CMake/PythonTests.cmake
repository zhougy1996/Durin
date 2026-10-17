if(NOT TARGET DurinCPython OR NOT TARGET DurinPython)
    durin_exclude_native_test_sources(
        RATIONALE "Requires the optional qualified CPython SDK and editor module."
        SOURCES Private/PythonToolIntegrationTests.cpp Private/MCPPythonJobTests.cpp)
    return()
endif()
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
