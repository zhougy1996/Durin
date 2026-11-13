add_durin_project(WaterSurfaceFixture)

add_executable(WaterSurfaceStartupProbe StartupProbe.cpp)
durin_target_apply_common_definitions(WaterSurfaceStartupProbe WaterSurfaceStartupProbe)
target_link_libraries(WaterSurfaceStartupProbe PRIVATE Core Projects CoreDObject)
set_target_properties(WaterSurfaceStartupProbe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/WaterSurfaceStartupProbe/$<CONFIG>")

if(TARGET Water)
    add_executable(WaterSurfaceFeatureProbe FeatureProbe.cpp WaterMaterialRecipe.cpp)
    durin_target_apply_common_definitions(WaterSurfaceFeatureProbe WaterSurfaceFeatureProbe)
    target_link_libraries(WaterSurfaceFeatureProbe PRIVATE Water)
    set_target_properties(WaterSurfaceFeatureProbe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/WaterSurfaceFeatureProbe/$<CONFIG>")
endif()
