if(NOT TARGET DurinCPython OR NOT TARGET DurinPython)
    durin_exclude_native_test_sources(
        RATIONALE "Requires the optional qualified CPython SDK and editor module."
        SOURCES Private/PythonToolIntegrationTests.cpp)
    return()
endif()
durin_add_native_test(PythonToolIntegrationTests
    KIND integration
    DOMAINS python editor-tools
    MODULES durin-python durin-ed
    STACKS editor
    SOURCES Private/PythonToolIntegrationTests.cpp
    LIBRARIES Core CoreDObject Engine DurinEd DurinPython
    REQUIRES editor
    REQUIREMENT_RATIONALE "Runs embedded CPython against real CPU editor tool dispatch."
    HEAVY_RUNTIME_RATIONALE "Qualifies interpreter lifetime and detached endpoint integration without GPU startup."
)
