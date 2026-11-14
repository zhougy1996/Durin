#include <gtest/gtest.h>

#include "Asset/AssetSaveReadiness.h"
#include "DObject/Object.h"

namespace
{
	class FReadinessFeature final : public Durin::IAssetSaveReadinessFeature
	{
	public:
		bool bHandles = true;
		std::expected<void, Durin::UnifiedError::FError> Result;

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
	ASSERT_FALSE(Result);
	EXPECT_TRUE(Result.error().Is({"AssetSaveReadiness", "InvalidData"}));
}

TEST(AssetSaveReadinessTests, UsesTheSingleHandlingProvider)
{
	Durin::DObject Object;
	FReadinessFeature Ignored;
	Ignored.bHandles = false;
	FReadinessFeature Rejected;
	Rejected.Result = std::unexpected(Durin::UnifiedError::FError{
		{"AssetSaveReadiness", "StaleData"}, "Asset family is not ready."});

	auto IgnoredRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Ignored);
	auto RejectedRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Rejected);

	const auto Result = Durin::ValidateAssetSaveReadiness(&Object);
	ASSERT_FALSE(Result);
	EXPECT_TRUE(Result.error().Is({"AssetSaveReadiness", "StaleData"}));
	EXPECT_EQ("Asset family is not ready.", Result.error().GetMessage());
}

TEST(AssetSaveReadinessTests, RejectsAmbiguousHandlingProviders)
{
	Durin::DObject Object;
	FReadinessFeature First;
	FReadinessFeature Second;

	auto FirstRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(First);
	auto SecondRegistration = Durin::FModularFeatureRegistry::Get().RegisterFeature(Second);

	const auto Result = Durin::ValidateAssetSaveReadiness(&Object);
	ASSERT_FALSE(Result);
	EXPECT_TRUE(Result.error().Is({"AssetSaveReadiness", "StaleData"}));
	EXPECT_NE(std::string::npos, Result.error().GetMessage().find("ambiguous"));
}
