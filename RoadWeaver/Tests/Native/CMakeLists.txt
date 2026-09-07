include(GoogleTest)

# Project-owned sources use the workspace test deployment root expected by DevTool.
get_target_property(_road_test_bin CoreUtilityTests DURIN_TEST_BIN_DIR)
get_filename_component(DURIN_PROJECT_TEST_OUTPUT_ROOT "${_road_test_bin}" DIRECTORY)

add_durin_test(RoadGraphContractTests Private/RoadGraphContractTests.cpp)
target_link_libraries(RoadGraphContractTests PRIVATE RoadWeaver Engine)
durin_register_native_test(RoadGraphContractTests
    KIND contract
    DOMAINS road-graph road-surface
    MODULES roadweaver
)

add_durin_test(RoadSceneIntegrationTests Private/RoadSceneIntegrationTests.cpp)
target_link_libraries(RoadSceneIntegrationTests PRIVATE RoadWeaver Engine)
durin_register_native_test(RoadSceneIntegrationTests
    KIND integration
    DOMAINS road-scene
    MODULES roadweaver
)
