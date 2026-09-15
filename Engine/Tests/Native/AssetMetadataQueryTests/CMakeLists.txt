durin_add_native_test(AssetMetadataQueryTests
	KIND contract
	DOMAINS asset-registry
	MODULES asset-registry
	SOURCES Private/AssetMetadataQueryTests.cpp
	LIBRARIES Core CoreDObject AssetRegistry
)
