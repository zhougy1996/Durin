durin_add_native_test(PackageRegistryContractTests
	KIND contract
	DOMAINS asset-package asset-registry
	MODULES asset-registry core-dobject
	SOURCES Private/PackageRegistryTests.cpp
	LIBRARIES Core CoreDObject AssetRegistry
)
