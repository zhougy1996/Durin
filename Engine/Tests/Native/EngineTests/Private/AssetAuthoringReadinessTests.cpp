#include <gtest/gtest.h>

#include "AssetAuthoringReadiness.h"
#include "DObject/Object.h"
#include "Modules/ModuleTestSupport.h"

namespace
{
	class FReadinessFeature final : public Durin::IAssetAuthoringReadinessFeature
	{
	public:
		bool bHandles = true;
		Durin::Asset::FAssetResult Result;

		auto Validate(const Durin::DObject&) const
			-> Durin::FAssetAuthoringReadinessFeatureResult override
		{
			return {.bHandled = bHandles, .Result = Result};
		}
	};
}

TEST(AssetAuthoringReadinessTests, RejectsMissingAsset)
{
	const auto Result = Durin::ValidateAssetAuthoringReadiness(nullptr);
	EXPECT_EQ(Durin::Asset::EAssetError::InvalidObjectGraph, Result.Error);
}

TEST(AssetAuthoringReadinessTests, UsesTheSingleHandlingProvider)
{
	Durin::DObject Object;
	FReadinessFeature Ignored;
	Ignored.bHandles = false;
	FReadinessFeature Rejected;
	Rejected.Result = {
		Durin::Asset::EAssetError::StaleData, "Asset family is not ready."};
	Durin::FModuleTestOwner IgnoredOwner("ReadinessIgnored");
	Durin::FModuleTestOwner RejectedOwner("ReadinessRejected");
	auto IgnoredRegistration = IgnoredOwner.RegisterFeature(Ignored);
	auto RejectedRegistration = RejectedOwner.RegisterFeature(Rejected);

	const auto Result = Durin::ValidateAssetAuthoringReadiness(&Object);
	EXPECT_EQ(Durin::Asset::EAssetError::StaleData, Result.Error);
	EXPECT_EQ("Asset family is not ready.", Result.Message);
}

TEST(AssetAuthoringReadinessTests, RejectsAmbiguousHandlingProviders)
{
	Durin::DObject Object;
	FReadinessFeature First;
	FReadinessFeature Second;
	Durin::FModuleTestOwner FirstOwner("ReadinessFirst");
	Durin::FModuleTestOwner SecondOwner("ReadinessSecond");
	auto FirstRegistration = FirstOwner.RegisterFeature(First);
	auto SecondRegistration = SecondOwner.RegisterFeature(Second);

	const auto Result = Durin::ValidateAssetAuthoringReadiness(&Object);
	EXPECT_EQ(Durin::Asset::EAssetError::StaleData, Result.Error);
	EXPECT_NE(std::string::npos, Result.Message.find("ambiguous"));
}
