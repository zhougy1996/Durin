#include "Thumbnail/TextureCubeAssetThumbnail.h"

#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"

#include "AssetSystem.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeRequest(const Durin::Asset::FAssetData& Data)
		-> Durin::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<Durin::uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::EAssetThumbnailPriority::Visible,
			.RequestSerial = 3};
	}
}

TEST(FTextureCubeAssetThumbnailTests, ProviderCapturesPackageAndCubeVisualContract)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath CubePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::DirectionalCubePath,
		CubePath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAsset(CubePath);
	ASSERT_NE(Data, nullptr);

	Durin::FTextureCubeAssetThumbnailProvider Provider;
	const Durin::FAssetThumbnailProviderRegistration Registration =
		Provider.GetRegistration();
	Durin::FAssetThumbnailGenerationRequest Captured;
	ASSERT_TRUE(Provider.CaptureGenerationRequest(
		MakeRequest(*Data), 9, Captured, Error)) << Error;
	EXPECT_EQ(Captured.ProviderGeneration, 9u);
	EXPECT_EQ(Captured.RequestSerial, 3u);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureIdentity,
		Durin::FRenderedAssetThumbnailVisualContract::SphereVirtualPath);
	EXPECT_EQ(Captured.KeyInput.ShaderContractVersion, 1u);
	EXPECT_TRUE(Captured.KeyInput.Dependencies.empty());
	EXPECT_NE(
		std::dynamic_pointer_cast<
			const Durin::FTextureCubeThumbnailGenerationInput>(Captured.Input),
		nullptr);

	Captured.KeyInput.Asset = MakeRequest(*Data).Asset;
	Captured.KeyInput.ProviderName = Registration.ProviderName;
	Captured.KeyInput.GeneratorSchemaVersion =
		Registration.GeneratorSchemaVersion;
	const std::string OriginalKey =
		Durin::BuildAssetThumbnailCacheKey(Captured.KeyInput);
	Durin::FAssetThumbnailKeyInput Rebuilt = Captured.KeyInput;
	++Rebuilt.Asset.LastWriteTimeTicks;
	EXPECT_NE(OriginalKey, Durin::BuildAssetThumbnailCacheKey(Rebuilt));
}

TEST(FTextureCubeAssetThumbnailTests, PreviewPrimitiveRetainsExactCubeResource)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);

	std::unique_ptr<Durin::PrimitiveSceneProxy> Primitive =
		Durin::CreateTextureCubePreviewPrimitive(
			Mesh, Fixtures.DirectionalCube, Error);
	ASSERT_NE(Primitive, nullptr) << Error;
	auto* CubeProxy =
		dynamic_cast<Durin::FTextureCubePreviewSceneProxy*>(Primitive.get());
	ASSERT_NE(CubeProxy, nullptr);
	EXPECT_EQ(CubeProxy->GetRenderData(), Mesh->GetRenderData());
	EXPECT_EQ(
		CubeProxy->GetTextureResource(),
		Fixtures.DirectionalCube->GetRenderResource());

	Primitive.reset();
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FTextureCubeAssetThumbnailTests, ProviderRejectsMissingRegistryData)
{
	InitializeDObjectSystem();
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Textures/TC_Missing", MissingPath));
	Durin::FTextureCubeAssetThumbnailProvider Provider;
	Durin::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Provider.CaptureGenerationRequest({
		.Asset = {
			.VirtualPath = MissingPath,
			.AssetClassName =
				Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.PackageFormatVersion = 1,
			.FileSize = 1,
			.LastWriteTimeTicks = 1},
		.Priority = Durin::EAssetThumbnailPriority::Visible,
		.RequestSerial = 1}, 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);
}
