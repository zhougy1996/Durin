include(GoogleTest)

add_durin_test(RoadGraphContractTests Private/RoadGraphContractTests.cpp)
target_link_libraries(RoadGraphContractTests PRIVATE RoadWeaver Engine)
durin_register_native_test(RoadGraphContractTests
    KIND contract
    DOMAINS road-graph road-surface
    MODULES roadweaver
)

add_durin_test(RoadSceneIntegrationTests Private/RoadSceneIntegrationTests.cpp)
target_link_libraries(RoadSceneIntegrationTests PRIVATE RoadWeaver Engine StaticMeshBuild)
if(DURIN_WITH_EDITOR)
    target_link_libraries(RoadSceneIntegrationTests PRIVATE AssetForgeBuiltins)
    set_target_properties(RoadSceneIntegrationTests PROPERTIES
        DURIN_TEST_HEAVY_RUNTIME_RATIONALE "Loads the checked-in RoadWeaver level and its editor import metadata.")
endif()
durin_register_native_test(RoadSceneIntegrationTests
    KIND integration
    DOMAINS road-scene
    MODULES roadweaver
)
