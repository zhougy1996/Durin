#include "AssetPackageVersionPolicy.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin::Asset;

	static_assert(LatestAssetPackageWriterVersion == AssetPackageV4FormatVersion);
	static_assert(OrdinaryAssetPackageWriterVersion == AssetPackageV4FormatVersion);
	static_assert(AssetPackageMigrationWriterVersion == LatestAssetPackageWriterVersion);
	static_assert(IsSupportedAssetPackageReaderVersion(AssetPackageV4FormatVersion));

	TEST(FAssetPackageVersionPolicyTests, SeparatesReaderAndWriterPolicyWithoutIo)
	{
		EXPECT_EQ(SupportedAssetPackageReaderVersions,
			(decltype(SupportedAssetPackageReaderVersions){AssetPackageV4FormatVersion}));
		EXPECT_EQ(SelectAssetPackageReader(AssetPackageV4FormatVersion), EAssetPackageReaderKind::DastV4);
		EXPECT_EQ(SelectAssetPackageReader(AssetPackageV4FormatVersion - 1), EAssetPackageReaderKind::Unsupported);
		EXPECT_EQ(SelectAssetPackageReader(AssetPackageV4FormatVersion + 1), EAssetPackageReaderKind::Unsupported);
		EXPECT_EQ(OrdinaryAssetPackageWriterVersion, LatestAssetPackageWriterVersion);
	}
}
