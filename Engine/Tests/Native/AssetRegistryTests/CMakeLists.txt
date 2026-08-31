include(GoogleTest)

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
