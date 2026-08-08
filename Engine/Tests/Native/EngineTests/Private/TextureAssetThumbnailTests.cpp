#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Thumbnail/Texture2DAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "TextureEditorModule.h"
#include "Editor/EditorWorkspace.h"

#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"

#include "AssetSystem.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "Preview/TextureCubePreviewComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"
#include "Texture/Texture2D.h"

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

TEST(FTextureAssetThumbnailTests, Texture2DProviderCapturesAuthoredSourceImage)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath TexturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath,
		TexturePath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(TexturePath);
	ASSERT_NE(Data, nullptr);
	Durin::FTexture2DAssetThumbnailProvider Provider;
	Durin::FAssetThumbnailSourceImage Source;
	ASSERT_TRUE(Provider.CaptureSourceImage(*Data, Source, Error)) << Error;
	EXPECT_FALSE(Source.PhysicalPath.empty());
	EXPECT_GT(Source.FileSize, 0u);
	EXPECT_TRUE(std::filesystem::exists(Source.PhysicalPath));
}

TEST(FTextureAssetThumbnailTests, ModuleOwnsBothExactProvidersAndWorkspaceLifecycle)
{
	InitializeDObjectSystem();
	Durin::FEditorWorkspaceManager Manager;
	Durin::FRenderedAssetThumbnailService Service;
	Durin::FTextureEditorModule Module;
	const std::string Texture2DClass =
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString();
	const std::string TextureCubeClass =
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString();
	ASSERT_TRUE(Module.RegisterTextureEditor(Manager, Service));
	EXPECT_TRUE(Service.Find(Texture2DClass));
	EXPECT_TRUE(Service.UsesSourceImage(Texture2DClass));
	EXPECT_TRUE(Service.Find(TextureCubeClass));
	EXPECT_NE(Manager.FindWorkspace(
		Durin::FEditorWorkspaceTypeId("TextureEditor")), nullptr);
	EXPECT_FALSE(Module.RegisterTextureEditor(Manager, Service));
	Module.UnregisterTextureEditor();
	EXPECT_FALSE(Service.Find(Texture2DClass));
	EXPECT_FALSE(Service.UsesSourceImage(Texture2DClass));
	EXPECT_FALSE(Service.Find(TextureCubeClass));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::FEditorWorkspaceTypeId("TextureEditor")), nullptr);
}

TEST(FTextureAssetThumbnailTests, ProviderConflictRollsBackWholeIntegration)
{
	InitializeDObjectSystem();
	Durin::FEditorWorkspaceManager Manager;
	Durin::FRenderedAssetThumbnailService Service;
	std::string Error;
	auto Existing = Service.RegisterScoped(
		std::make_unique<Durin::FTextureCubeAssetThumbnailProvider>(), Error);
	ASSERT_TRUE(Existing) << Error;
	Durin::FTextureEditorModule Module;
	EXPECT_FALSE(Module.RegisterTextureEditor(Manager, Service));
	EXPECT_FALSE(Service.Find(
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(Service.Find(
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::FEditorWorkspaceTypeId("TextureEditor")), nullptr);
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
		Durin::Asset::GetAssetRegistry().FindAssetExact(CubePath);
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
		Durin::FRenderedAssetThumbnailVisualContract::
			TextureCubeEnvironmentViewIdentity);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureVersion,
		Durin::FRenderedAssetThumbnailVisualContract::
			TextureCubeEnvironmentViewVersion);
	EXPECT_EQ(Captured.KeyInput.ShaderContractVersion, 2u);
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

TEST(FTextureCubeAssetThumbnailTests, PreviewComponentCreatesStableCubeReference)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::DStaticMesh* Mesh = Durin::DStaticMesh::CreateDebugTriangle();
	ASSERT_NE(Mesh, nullptr);

	auto* Component = Durin::NewObject<Durin::DTextureCubePreviewComponent>(
		nullptr, "TextureCubeThumbnailPreviewComponent");
	Component->SetStaticMesh(Mesh);
	Component->SetTextureCube(Fixtures.DirectionalCube);
	std::unique_ptr<Durin::PrimitiveSceneProxy> Primitive =
		Component->CreateSceneProxy();
	ASSERT_NE(Primitive, nullptr) << Error;
	auto* CubeProxy =
		dynamic_cast<Durin::FTextureCubePreviewSceneProxy*>(Primitive.get());
	ASSERT_NE(CubeProxy, nullptr);
	EXPECT_EQ(CubeProxy->GetRenderData(), Mesh->GetRenderData());
	EXPECT_EQ(
		CubeProxy->GetTextureReference(),
		Fixtures.DirectionalCube->GetTextureReferenceRHI());

	Primitive.reset();
	Durin::MarkAsGarbage(Component);
	Durin::MarkAsGarbage(Mesh);
	Durin::CollectGarbage();
}

TEST(FTextureCubeAssetThumbnailTests, ProviderRejectsMissingRegistryData)
{
	Durin::Tests::RegisterRenderedAssetThumbnailFixtureMount();
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
