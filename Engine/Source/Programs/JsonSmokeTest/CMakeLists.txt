durin_log_module(${project_name} "JsonSmokeTest")

add_executable(JsonSmokeTest "Private/Main.cpp")
durin_set_module_output(JsonSmokeTest)

set(json_smoke_test_file "${CMAKE_CURRENT_SOURCE_DIR}/TestData/Sample.json")
target_compile_definitions(JsonSmokeTest PRIVATE
    JSON_SMOKE_TEST_FILE="${json_smoke_test_file}"
)

target_link_libraries(JsonSmokeTest PRIVATE
    Core
)

set_target_properties(JsonSmokeTest PROPERTIES
    FOLDER "${project_name}/Source/Programs/JsonSmokeTest"
)
