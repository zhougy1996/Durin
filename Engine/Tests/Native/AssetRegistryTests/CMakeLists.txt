include(GoogleTest)

add_durin_test(PackageLinkerAdapterTests
	Private/PackageLinkerAdapterTests.cpp
)
target_link_libraries(PackageLinkerAdapterTests PRIVATE
	Core
	CoreDObject
	AssetRegistry
	Engine
)
target_include_directories(PackageLinkerAdapterTests PRIVATE
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/AssetRegistry/Private
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/Engine/Private
)
durin_register_native_test(PackageLinkerAdapterTests
	KIND contract
	DOMAINS asset-package reflection
	MODULES asset-registry core-dobject
)

add_durin_test(PackageRegistryContractTests
	Private/PackageRegistryTests.cpp
)
target_link_libraries(PackageRegistryContractTests PRIVATE
	Core
	CoreDObject
	AssetRegistry
)
durin_register_native_test(PackageRegistryContractTests
	KIND contract
	DOMAINS asset-package asset-registry
	MODULES asset-registry core-dobject
)

add_durin_test(PackageMigrationTests
	Private/PackageMigrationTests.cpp
)
target_link_libraries(PackageMigrationTests PRIVATE
	Core
	CoreDObject
	AssetRegistry
	AssetMaintenance
	Engine
)
target_include_directories(PackageMigrationTests PRIVATE
	${DURIN_PROJECT_SOURCE_DIR}/Runtime/Engine/Private
)
durin_register_native_test(PackageMigrationTests
	KIND contract
	DOMAINS asset-package asset-maintenance
	MODULES asset-registry asset-maintenance core-dobject
)
