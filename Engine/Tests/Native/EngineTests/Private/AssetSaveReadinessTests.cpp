#include <gtest/gtest.h>

#include "Asset/AssetSaveReadiness.h"
#include "DObject/Object.h"
#include "Modules/ModuleTestSupport.h"

namespace
{
	class FReadinessFeature final : public Durin::IAssetSaveReadinessFeature
	{
	public:
		bool bHandles = true;
		Durin::FAssetResult Result;

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
	EXPECT_EQ(Durin::EAssetError::InvalidObjectGraph, Result.Error);
}

TEST(AssetSaveReadinessTests, UsesTheSingleHandlingProvider)
{
	Durin::DObject Object;
	FReadinessFeature Ignored;
	Ignored.bHandles = false;
	FReadinessFeature Rejected;
	Rejected.Result = {
		Durin::EAssetError::StaleData, "Asset family is not ready."};
	Durin::FModuleTestOwner IgnoredOwner("ReadinessIgnored");
	Durin::FModuleTestOwner RejectedOwner("ReadinessRejected");
	auto IgnoredRegistration = IgnoredOwner.RegisterFeature(Ignored);
	auto RejectedRegistration = RejectedOwner.RegisterFeature(Rejected);

	const auto Result = Durin::ValidateAssetSaveReadiness(&Object);
	EXPECT_EQ(Durin::EAssetError::StaleData, Result.Error);
	EXPECT_EQ("Asset family is not ready.", Result.Message);
}

TEST(AssetSaveReadinessTests, RejectsAmbiguousHandlingProviders)
{
	Durin::DObject Object;
	FReadinessFeature First;
	FReadinessFeature Second;
	Durin::FModuleTestOwner FirstOwner("ReadinessFirst");
	Durin::FModuleTestOwner SecondOwner("ReadinessSecond");
	auto FirstRegistration = FirstOwner.RegisterFeature(First);
	auto SecondRegistration = SecondOwner.RegisterFeature(Second);

	const auto Result = Durin::ValidateAssetSaveReadiness(&Object);
	EXPECT_EQ(Durin::EAssetError::StaleData, Result.Error);
	EXPECT_NE(std::string::npos, Result.Message.find("ambiguous"));
}
