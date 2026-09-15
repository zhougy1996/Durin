set(_durin_asset_package_modules engine)
set(_durin_asset_package_libraries)
if(DURIN_WITH_EDITOR)
	list(APPEND _durin_asset_package_modules asset-maintenance)
	list(APPEND _durin_asset_package_libraries AssetMaintenance)
endif()

durin_add_native_test(AssetPackageTests
	KIND feature
	DOMAINS asset-package
	MODULES ${_durin_asset_package_modules}
	LIBRARIES Core CoreDObject Engine ${_durin_asset_package_libraries}
	SOURCES Private/BulkDataTests.cpp Private/PackageTests.cpp
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
)
durin_add_native_test(AssetCookTests
	KIND feature
	DOMAINS asset-cook
	MODULES engine
	TIMEOUT 600
	SOURCES Private/CookedAssetTests.cpp
	LIBRARIES Core CoreDObject Engine
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
)

durin_add_native_test(AssetBulkContainerTests
	KIND contract
	DOMAINS asset-cook asset-package
	MODULES engine
	SOURCES Private/BulkContainerInfrastructureTests.cpp
	LIBRARIES Core CoreDObject Engine
	INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Source/Runtime/Engine/Private
)

if(DURIN_WITH_EDITOR)
	durin_add_native_test(CookFunctionalTests
		KIND feature
		DOMAINS asset-cook
		MODULES engine texture-build static-mesh-build shader-build
		LIBRARIES Core CoreDObject Engine TextureBuild StaticMeshBuild ShaderBuild
		TIMEOUT 600
		SOURCES Private/CookFunctionalTests.cpp
	)
else()
	durin_exclude_native_test_sources(
		RATIONALE "Cook family fixtures require authored editor asset preparation."
		SOURCES Private/CookFunctionalTests.cpp)
endif()

if(DURIN_WITH_EDITOR)
	# Separate process ownership: the aggregate cannot restart after terminal shutdown.
	durin_add_native_test(AsyncTaskPilotLifecycleTests
		KIND integration
		DOMAINS async-task-pilot
		MODULES engine texture-build
		LIBRARIES Core CoreDObject Engine TextureBuild
		SOURCES Private/AsyncTaskPilotLifecycleTests.cpp
	)

	# The allocator replacement is qualified against Mach-O symbol interposition.
	if(APPLE)
		durin_add_native_test(AsyncTaskPilotQualificationTests
			KIND qualification
			DOMAINS async-task-pilot
			MODULES engine texture-build
			LIBRARIES Core CoreDObject Engine TextureBuild
			SOURCES Private/AsyncTaskPilotQualificationTests.cpp
		)
	else()
		durin_exclude_native_test_sources(
			RATIONALE "The pilot allocation probe currently qualifies Mach-O C++ allocation interposition only."
			SOURCES Private/AsyncTaskPilotQualificationTests.cpp)
	endif()
	durin_add_native_test(AssetImportDataTests
		KIND contract
		DOMAINS asset-import
		MODULES engine
		LIBRARIES Core CoreDObject Engine Engine
		SOURCES Private/AssetImportDataTests.cpp
	)

	durin_add_native_test(AssetImportTests
		KIND integration
		DOMAINS asset-import
		MODULES engine asset-forge asset-forge-builtins
		TIMEOUT 600
		SOURCES Private/AssetImportTests.cpp
		LIBRARIES Core CoreDObject Engine AssetForgeBuiltins
		DATA_DIRECTORIES "${DURIN_PROJECT_ROOT_DIR}/Tests/Data/AssetImport"
	)

else()
	durin_exclude_native_test_sources(
		RATIONALE "Asset import services and their test fixtures are editor-only."
		SOURCES Private/AsyncTaskPilotLifecycleTests.cpp
			Private/AsyncTaskPilotQualificationTests.cpp
			Private/AssetImportDataTests.cpp
			Private/AssetImportTests.cpp
	)
endif()
