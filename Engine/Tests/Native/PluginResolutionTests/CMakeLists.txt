durin_add_native_test(PluginResolutionTests
    KIND contract
    DOMAINS plugins
    MODULES projects core
    SOURCES Private/PluginResolutionTests.cpp
    LIBRARIES Core Projects
    DATA_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/Data"
)
set_source_files_properties(Private/PluginResolutionTests.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
