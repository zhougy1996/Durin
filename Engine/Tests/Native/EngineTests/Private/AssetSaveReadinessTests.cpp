#include <gtest/gtest.h>

#include "Asset/AssetSaveReadiness.h"
#include "DObject/Object.h"

namespace
{
	class FReadinessFeature final : public Durin::IAssetSaveReadinessFeature
	{
	public:
		bool bHandles = true;
		Durin::FAssetWriteResult Result;

		auto Validate(const Durin::DObject&) const
			-> Durin::FAssetSaveReadinessFeatureResult override
		{
			return {.bHandled = bHandles, .Result = Result};
		}
	};
}

TEST(AssetSaveReadinessTests, RejectsMissingAsset)
{
	const auto Result = Durin::ValidateAssetSaveReadiness(nullptr);
	EXPECT_EQ(Durin::EAssetWriteError::InvalidData, Result.Error);
}

TEST(AssetSaveReadinessTests, UsesTheSingleHandlingProvider)
{
	Durin::DObject Object;
	FReadinessFeature Ignored;
	Ignored.bHandles = false;
	FReadinessFeature Rejected;
	Rejected.Result = {
		Durin::EAssetWriteError::StaleData, "Asset family is not ready."};

	auto IgnoredRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Ignored);
	auto RejectedRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Rejected);

	const auto Result = Durin::ValidateAssetSaveReadiness(&Object);
	EXPECT_EQ(Durin::EAssetWriteError::StaleData, Result.Error);
	EXPECT_EQ("Asset family is not ready.", Result.Message);
}

TEST(AssetSaveReadinessTests, RejectsAmbiguousHandlingProviders)
{
	Durin::DObject Object;
	FReadinessFeature First;
	FReadinessFeature Second;

	auto FirstRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(First);
	auto SecondRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Second);

	const auto Result = Durin::ValidateAssetSaveReadiness(&Object);
	EXPECT_EQ(Durin::EAssetWriteError::StaleData, Result.Error);
	EXPECT_NE(std::string::npos, Result.Message.find("ambiguous"));
}
