include(GoogleTest)

function(durin_add_asset_functional_test target_name)
	set(options IMPORT_RUNTIME)
	set(one_value_args KIND TIMEOUT PRIVATE_SOURCE_RATIONALE)
	set(multi_value_args DOMAINS MODULES SOURCES LIBRARIES PRIVATE_SOURCES
		PRIVATE_INCLUDE_DIRECTORIES)
	cmake_parse_arguments(
		DURIN_ASSET_TEST
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)

	add_durin_test(${target_name}
		${DURIN_ASSET_TEST_SOURCES}
		${DURIN_ASSET_TEST_PRIVATE_SOURCES}
	)
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
		Engine
		${DURIN_ASSET_TEST_LIBRARIES}
	)
	target_include_directories(${target_name} PRIVATE
		${DURIN_ASSET_TEST_PRIVATE_INCLUDE_DIRECTORIES}
	)

	if(DURIN_ASSET_TEST_IMPORT_RUNTIME)
		target_link_libraries(${target_name} PRIVATE AssetForgeBuiltins)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
		)
	endif()

	if(DURIN_ASSET_TEST_TIMEOUT)
		set(_durin_timeout_arg TIMEOUT "${DURIN_ASSET_TEST_TIMEOUT}")
	endif()
	if(DURIN_ASSET_TEST_PRIVATE_SOURCES)
		durin_register_native_test(${target_name}
			KIND ${DURIN_ASSET_TEST_KIND}
			DOMAINS ${DURIN_ASSET_TEST_DOMAINS}
			MODULES ${DURIN_ASSET_TEST_MODULES}
			${_durin_timeout_arg}
			PRIVATE_SOURCE_OWNER Engine
			PRIVATE_SOURCE_RATIONALE
				"${DURIN_ASSET_TEST_PRIVATE_SOURCE_RATIONALE}"
		)
	else()
		durin_register_native_test(${target_name}
			KIND ${DURIN_ASSET_TEST_KIND}
			DOMAINS ${DURIN_ASSET_TEST_DOMAINS}
			MODULES ${DURIN_ASSET_TEST_MODULES}
			${_durin_timeout_arg}
		)
	endif()
endfunction()

set(_durin_asset_package_modules engine)
set(_durin_asset_package_libraries)
if(DURIN_WITH_EDITOR)
	list(APPEND _durin_asset_package_modules asset-maintenance)
	list(APPEND _durin_asset_package_libraries AssetMaintenance)
endif()

durin_add_asset_functional_test(AssetPackageTests
	KIND feature
	DOMAINS asset-package
	MODULES ${_durin_asset_package_modules}
	LIBRARIES ${_durin_asset_package_libraries}
	SOURCES
		Private/BulkDataTests.cpp
		Private/PackageTests.cpp
	PRIVATE_INCLUDE_DIRECTORIES
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
)
durin_add_asset_functional_test(AssetCookTests
	KIND feature
	DOMAINS asset-cook
	MODULES engine
	TIMEOUT 600
	SOURCES Private/CookedAssetTests.cpp
)
target_include_directories(AssetCookTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
)

durin_add_asset_functional_test(AssetBulkContainerTests
	KIND contract
	DOMAINS asset-cook asset-package
	MODULES engine
	SOURCES Private/BulkContainerInfrastructureTests.cpp
)
target_include_directories(AssetBulkContainerTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
)

if(DURIN_WITH_EDITOR)
	durin_add_asset_functional_test(AssetImportDataTests
		KIND contract
		DOMAINS asset-import
		MODULES engine
		LIBRARIES Engine
		SOURCES Private/AssetImportDataTests.cpp
	)

	durin_add_asset_functional_test(AssetImportTests
		KIND integration
		DOMAINS asset-import
		MODULES engine asset-forge asset-forge-builtins
		IMPORT_RUNTIME
		TIMEOUT 600
		SOURCES Private/AssetImportTests.cpp
	)

else()
	durin_exclude_native_test_sources(
		RATIONALE "Asset import services and their test fixtures are editor-only."
		SOURCES
			Private/AssetImportDataTests.cpp
			Private/AssetImportTests.cpp
	)
endif()
