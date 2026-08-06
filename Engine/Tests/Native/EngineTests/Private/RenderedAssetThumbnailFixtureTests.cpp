#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"

#include <gtest/gtest.h>

namespace
{
	auto HasDependency(
		const Durin::Asset::FAssetData& Asset,
		std::string_view Dependency
	) -> bool
	{
		Durin::FAssetPath Path;
		if (!Durin::Tests::MakeRenderedThumbnailFixturePath(Dependency, Path)) return false;
		return std::ranges::find(Asset.Dependencies, Path) != Asset.Dependencies.end();
	}
}

TEST(FRenderedAssetThumbnailFixtureTests, CreatesVersionedMaterialAndCubeFixtures)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error)) << Error;

	ASSERT_NE(Fixtures.Material, nullptr);
	ASSERT_NE(Fixtures.MaterialInstance, nullptr);
	ASSERT_NE(Fixtures.InvalidMaterialInstance, nullptr);
	ASSERT_NE(Fixtures.ParentTexture, nullptr);
	ASSERT_NE(Fixtures.OverrideTexture, nullptr);
	ASSERT_NE(Fixtures.DirectionalCube, nullptr);
	EXPECT_EQ(Durin::Tests::FRenderedAssetThumbnailFixtureSet::Version, 1u);

	Durin::FVector3 BaseColor;
	ASSERT_TRUE(Fixtures.Material->GetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, Durin::FVector3(0.35, 0.55, 0.75));
	ASSERT_TRUE(Fixtures.MaterialInstance->GetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), BaseColor));
	EXPECT_EQ(BaseColor, Durin::FVector3(0.8, 0.28, 0.12));
	EXPECT_EQ(Fixtures.MaterialInstance->GetParent(), Fixtures.Material);
	EXPECT_EQ(Fixtures.InvalidMaterialInstance->GetParent(), nullptr);

	ASSERT_NE(Fixtures.DirectionalCube->GetSourceData(), nullptr);
	ASSERT_NE(Fixtures.DirectionalCube->GetPlatformData(), nullptr);
	EXPECT_EQ(Fixtures.DirectionalCube->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Fixtures.DirectionalCube->GetBuildRevision(), 1u);
}

TEST(FRenderedAssetThumbnailFixtureTests, RecordsDirectAndTransitiveDependencyInputs)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error)) << Error;

	Durin::FAssetPath MaterialPath;
	Durin::FAssetPath InstancePath;
	ASSERT_TRUE(Durin::Tests::MakeRenderedThumbnailFixturePath(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath, MaterialPath));
	ASSERT_TRUE(Durin::Tests::MakeRenderedThumbnailFixturePath(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialInstancePath, InstancePath));

	const Durin::Asset::FAssetData* MaterialData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(MaterialPath);
	const Durin::Asset::FAssetData* InstanceData =
		Durin::Asset::GetAssetRegistry().FindAssetExact(InstancePath);
	ASSERT_NE(MaterialData, nullptr);
	ASSERT_NE(InstanceData, nullptr);
	EXPECT_TRUE(HasDependency(
		*MaterialData,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(HasDependency(
		*InstanceData,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(HasDependency(
		*InstanceData,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::OverrideTexturePath));
	EXPECT_FALSE(HasDependency(
		*InstanceData,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath));
}
