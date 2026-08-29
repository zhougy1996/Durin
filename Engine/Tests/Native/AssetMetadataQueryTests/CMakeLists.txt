include(GoogleTest)

add_durin_test(AssetMetadataQueryTests
	Private/AssetMetadataQueryTests.cpp
)
target_link_libraries(AssetMetadataQueryTests PRIVATE
	Core
	CoreDObject
	AssetRegistry
)
durin_register_native_test(AssetMetadataQueryTests
	KIND contract
	DOMAINS asset-registry
	MODULES asset-registry
)
