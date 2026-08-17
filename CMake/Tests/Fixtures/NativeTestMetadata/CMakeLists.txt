cmake_minimum_required(VERSION 3.24)
project(DurinNativeTestMetadataProbe LANGUAGES NONE)

if(NOT DEFINED DURIN_WORKSPACE_DIR OR NOT DEFINED DURIN_METADATA_PROBE)
	message(FATAL_ERROR "DURIN_WORKSPACE_DIR and DURIN_METADATA_PROBE are required.")
endif()
include("${DURIN_WORKSPACE_DIR}/CMake/Project/ProjectTargets.cmake")

if(DURIN_METADATA_PROBE STREQUAL "unavailable")
	durin_generate_native_test_registry(
		"${CMAKE_BINARY_DIR}/DurinNativeTestRegistry.json")
	return()
endif()

add_custom_target(ProbeTests SOURCES Empty.cpp)
set_target_properties(ProbeTests PROPERTIES
	DURIN_TEST_DIRECT_LIFECYCLE TRUE
	DURIN_TEST_TIMEOUT 30
)
set(_durin_metadata KIND feature DOMAINS world MODULES engine STACKS process)
if(DURIN_METADATA_PROBE STREQUAL "missing-kind")
	set(_durin_metadata DOMAINS world)
elseif(DURIN_METADATA_PROBE STREQUAL "missing-domain")
	set(_durin_metadata KIND feature)
elseif(DURIN_METADATA_PROBE STREQUAL "invalid-kind")
	set(_durin_metadata KIND smoke DOMAINS world)
elseif(DURIN_METADATA_PROBE STREQUAL "invalid-value")
	set(_durin_metadata KIND feature DOMAINS World_Rendering)
elseif(DURIN_METADATA_PROBE STREQUAL "duplicate")
	set(_durin_metadata KIND feature DOMAINS world WORLD)
elseif(DURIN_METADATA_PROBE STREQUAL "reserved-label")
	set_target_properties(ProbeTests PROPERTIES DURIN_TEST_LABELS domain-world)
elseif(DURIN_METADATA_PROBE STREQUAL "characterization-direct")
	set(_durin_metadata KIND characterization DOMAINS process)
elseif(DURIN_METADATA_PROBE STREQUAL "qualification")
	set(_durin_metadata KIND qualification DOMAINS renderer)
elseif(DURIN_METADATA_PROBE STREQUAL "private-source")
	target_sources(ProbeTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/FakeModule/Private/Foreign.cpp")
elseif(DURIN_METADATA_PROBE STREQUAL "application-host")
	set_target_properties(ProbeTests PROPERTIES DURIN_TEST_EXECUTION_HOST application)
elseif(DURIN_METADATA_PROBE STREQUAL "invalid-execution-host")
	set_target_properties(ProbeTests PROPERTIES DURIN_TEST_EXECUTION_HOST gui)
elseif(DURIN_METADATA_PROBE STREQUAL "empty-execution-host")
	set_target_properties(ProbeTests PROPERTIES DURIN_TEST_EXECUTION_HOST "")
endif()

durin_finalize_native_test(ProbeTests ${_durin_metadata})
if(DURIN_METADATA_PROBE STREQUAL "late-execution-host")
	set_target_properties(ProbeTests PROPERTIES DURIN_TEST_EXECUTION_HOST application)
	durin_validate_native_test_finalization(ProbeTests)
endif()
set_target_properties(ProbeTests PROPERTIES
	DURIN_TEST_DISCOVERED TRUE
	DURIN_TEST_DISCOVERY_RESOURCE_LOCKS "durin-gpu"
	DURIN_TEST_HEAVY_RUNTIME_RATIONALE "Probe"
)
set_property(GLOBAL APPEND PROPERTY DURIN_NATIVE_TEST_TARGETS ProbeTests)
durin_generate_native_test_registry("${CMAKE_BINARY_DIR}/DurinNativeTestRegistry.json")
