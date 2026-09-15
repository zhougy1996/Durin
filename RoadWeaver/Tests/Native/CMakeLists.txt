durin_add_native_test(RoadGraphContractTests
	KIND contract
	DOMAINS road-graph road-surface
	MODULES roadweaver
	SOURCES Private/RoadGraphContractTests.cpp
	LIBRARIES RoadWeaver Engine
)

set(_road_scene_editor_libraries)
set(_road_scene_rationale)
if(DURIN_WITH_EDITOR)
	set(_road_scene_editor_libraries AssetForgeBuiltins)
	set(_road_scene_rationale HEAVY_RUNTIME_RATIONALE "Loads the checked-in RoadWeaver level and its editor import metadata.")
endif()

durin_add_native_test(RoadSceneIntegrationTests
	KIND integration
	DOMAINS road-scene
	MODULES roadweaver
	SOURCES Private/RoadSceneIntegrationTests.cpp
	LIBRARIES RoadWeaver Engine StaticMeshBuild ${_road_scene_editor_libraries}
	${_road_scene_rationale}
)
