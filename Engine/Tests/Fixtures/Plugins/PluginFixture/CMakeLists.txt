add_durin_project(PluginFixture)
if(TARGET PluginFixtureConsumer)
 add_executable(PluginFixtureProbe Probe.cpp)
 durin_target_apply_common_definitions(PluginFixtureProbe PluginFixtureProbe)
 target_link_libraries(PluginFixtureProbe PRIVATE PluginFixtureConsumer)
 set_target_properties(PluginFixtureProbe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PluginFixtureProbe/$<CONFIG>")
endif()

add_executable(PluginFixtureStartupProbe StartupProbe.cpp)
durin_target_apply_common_definitions(PluginFixtureStartupProbe PluginFixtureStartupProbe)
target_link_libraries(PluginFixtureStartupProbe PRIVATE Core Projects CoreDObject Engine)
set_target_properties(PluginFixtureStartupProbe PROPERTIES
 RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/PluginFixtureStartupProbe/$<CONFIG>")
