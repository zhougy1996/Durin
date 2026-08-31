#include "Thumbnail/AssetThumbnailTestFixtures.h"

#include <gtest/gtest.h>

namespace
{
	auto HasDependency(
		const Durin::Asset::FAssetData& Asset,
		std::string_view Dependency
	) -> bool
	{
		Durin::FPackagePath Path;
		if (!Durin::Tests::MakeThumbnailFixturePath(Dependency, Path)) return false;
		return std::ranges::find(Asset.Dependencies, Path) != Asset.Dependencies.end();
	}
}

TEST(FAssetThumbnailFixtureTests, CreatesVersionedRenderedAssetFixtures)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;

	ASSERT_NE(Fixtures.Material, nullptr);
	ASSERT_NE(Fixtures.MaterialInstance, nullptr);
	ASSERT_NE(Fixtures.InvalidMaterialInstance, nullptr);
	ASSERT_NE(Fixtures.ParentTexture, nullptr);
	ASSERT_NE(Fixtures.OverrideTexture, nullptr);
	ASSERT_NE(Fixtures.DirectionalCube, nullptr);
	ASSERT_NE(Fixtures.StaticMesh, nullptr);
	EXPECT_EQ(Durin::Tests::FAssetThumbnailFixtureSet::Version, 2u);

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
	EXPECT_TRUE(Fixtures.StaticMesh->GetLOD0LocalBounds().has_value());
}

TEST(FAssetThumbnailFixtureTests, RecordsDirectAndTransitiveDependencyInputs)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;

	Durin::FPackagePath MaterialPath;
	Durin::FPackagePath InstancePath;
	Durin::FPackagePath StaticMeshPath;
	ASSERT_TRUE(Durin::Tests::MakeThumbnailFixturePath(
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath, MaterialPath));
	ASSERT_TRUE(Durin::Tests::MakeThumbnailFixturePath(
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialInstancePath, InstancePath));
	ASSERT_TRUE(Durin::Tests::MakeThumbnailFixturePath(
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath, StaticMeshPath));

	const Durin::Asset::FAssetCatalogEntry MaterialData =
		Durin::Asset::FindAssetExact(MaterialPath);
	const Durin::Asset::FAssetCatalogEntry InstanceData =
		Durin::Asset::FindAssetExact(InstancePath);
	const Durin::Asset::FAssetCatalogEntry StaticMeshData =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(MaterialData, nullptr);
	ASSERT_NE(InstanceData, nullptr);
	ASSERT_NE(StaticMeshData, nullptr);
	EXPECT_TRUE(HasDependency(
		*MaterialData,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(HasDependency(
		*InstanceData,
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(HasDependency(
		*InstanceData,
		Durin::Tests::FAssetThumbnailFixtureSet::OverrideTexturePath));
	EXPECT_FALSE(HasDependency(
		*InstanceData,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	EXPECT_TRUE(HasDependency(
		*StaticMeshData,
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath));
}
