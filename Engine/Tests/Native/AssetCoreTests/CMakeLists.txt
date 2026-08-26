include(GoogleTest)

function(durin_add_asset_functional_test target_name)
	set(options IMPORT_RUNTIME)
	set(one_value_args KIND TIMEOUT)
	set(multi_value_args DOMAINS MODULES SOURCES LIBRARIES)
	cmake_parse_arguments(
		DURIN_ASSET_TEST
		"${options}"
		"${one_value_args}"
		"${multi_value_args}"
		${ARGN}
	)

	add_durin_test(${target_name} ${DURIN_ASSET_TEST_SOURCES})
	target_link_libraries(${target_name} PRIVATE
		Core
		CoreDObject
		AssetCore
		${DURIN_ASSET_TEST_LIBRARIES}
	)

	if(DURIN_ASSET_TEST_IMPORT_RUNTIME)
		target_link_libraries(${target_name} PRIVATE AssetForge AssetForgeBuiltins)
		durin_test_deploy_directory_to_data(
			${target_name}
			"${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
		)
	endif()

	if(DURIN_ASSET_TEST_TIMEOUT)
		set(_durin_timeout_arg TIMEOUT "${DURIN_ASSET_TEST_TIMEOUT}")
	endif()
	durin_register_native_test(${target_name}
		KIND ${DURIN_ASSET_TEST_KIND}
		DOMAINS ${DURIN_ASSET_TEST_DOMAINS}
		MODULES ${DURIN_ASSET_TEST_MODULES}
		${_durin_timeout_arg}
	)
endfunction()

durin_add_asset_functional_test(AssetPackageTests
	KIND feature
	DOMAINS asset-package
	MODULES asset-core engine
	LIBRARIES Engine
	SOURCES
		Private/BulkDataTests.cpp
		Private/PackageTests.cpp
		Private/PackageV6Tests.cpp
		Private/PackageObjectStreamWireContract.cpp
		Private/PackageObjectStreamWireContractTests.cpp
		Private/PackageObjectStreamReferenceModel.cpp
		Private/PackageObjectStreamReferenceModelTests.cpp
		Private/PackageObjectStreamWriterTests.cpp
		Private/PackageObjectStreamReaderTests.cpp
)
target_include_directories(AssetPackageTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
)
durin_test_deploy_directory_to_data(
	AssetPackageTests
	"${CMAKE_CURRENT_SOURCE_DIR}/Data/Compatibility"
)

durin_add_asset_functional_test(AssetCookTests
	KIND feature
	DOMAINS asset-cook
	MODULES asset-core
	TIMEOUT 600
	SOURCES Private/CookedAssetTests.cpp
)
target_include_directories(AssetCookTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
)

durin_add_asset_functional_test(AssetBulkContainerTests
	KIND contract
	DOMAINS asset-cook asset-package
	MODULES asset-core
	SOURCES Private/BulkContainerInfrastructureTests.cpp
)
target_include_directories(AssetBulkContainerTests PRIVATE
	${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
)

durin_add_asset_functional_test(AssetMountedSourceTests
	KIND contract
	DOMAINS asset-workflow
	MODULES asset-core
	SOURCES Private/MountedSourceTests.cpp
)

if(DURIN_WITH_EDITOR)
	durin_add_asset_functional_test(AssetImportDataTests
		KIND contract
		DOMAINS asset-import asset-package
		MODULES asset-core engine asset-forge
		LIBRARIES Engine AssetForge
		SOURCES Private/AssetImportDataTests.cpp
	)

	durin_add_asset_functional_test(AssetImportTests
		KIND integration
		DOMAINS asset-import
		MODULES asset-core asset-forge asset-forge-builtins
		IMPORT_RUNTIME
		TIMEOUT 600
		SOURCES Private/AssetImportTests.cpp
	)

	durin_add_asset_functional_test(AssetForgeTests
		KIND contract
		DOMAINS asset-import
		MODULES asset-core asset-forge
		LIBRARIES AssetForge
		SOURCES
			Private/AssetForgeContractTests.cpp
	)
	target_include_directories(AssetForgeTests PRIVATE
		${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/AssetCore/Private
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Asset import services and their test fixtures are editor-only."
		SOURCES
			Private/AssetImportDataTests.cpp
			Private/AssetImportTests.cpp
			Private/AssetForgeContractTests.cpp
	)
endif()
