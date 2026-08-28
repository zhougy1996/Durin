#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "TextureEditorModule.h"
#include "VolumeTexturePreview.h"
#include "Editor/WorkspaceManager.h"

#include "Thumbnail/AssetThumbnailTestFixtures.h"

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "AssetCook.h"
#include "Asset/CanonicalResave.h"
#include "Asset/Compatibility.h"
#include "DObject/Class.h"
#include "Texture/TextureCube.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"
#include "ThirdParty/ImGui/imgui.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeRequest(const Durin::Asset::FAssetData& Data)
		-> Durin::Editor::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
			.RequestSerial = 3};
	}
}

class FCapturingTextureCubeThumbnailPreviewScene final
	: public Durin::Editor::IThumbnailPreviewScene
{
public:
	auto GetWorld() -> Durin::DWorld* override
	{
		++WorldRequests;
		return nullptr;
	}

	auto SetView(
		const Durin::Editor::FThumbnailPreviewView& View,
		std::string& OutError) -> bool override
	{
		LastView = View;
		OutError.clear();
		return true;
	}

	auto SetViewEnvironment(
		const Durin::FViewEnvironmentOverride& Environment,
		std::string& OutError) -> bool override
	{
		LastEnvironment = Environment;
		OutError.clear();
		return true;
	}

	uint32 WorldRequests = 0;
	Durin::Editor::FThumbnailPreviewView LastView;
	std::optional<Durin::FViewEnvironmentOverride> LastEnvironment;
};

TEST(FTextureAssetThumbnailTests, Texture2DRendererNeverCapturesReimportSource)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath TexturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath,
		TexturePath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(TexturePath);
	ASSERT_NE(Data, nullptr);
	Durin::Editor::Texture::DTextureThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailSourceImage Source;
	EXPECT_FALSE(Renderer.UsesSourceImage());
	EXPECT_FALSE(Renderer.CaptureSourceImage(*Data, Source, Error));
	EXPECT_TRUE(Source.PhysicalPath.empty());
	EXPECT_FALSE(Error.empty());
}

TEST(FTextureAssetThumbnailTests, Texture2DRendererGeneratesCanonicalSquarePixels)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath TexturePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath,
		TexturePath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(TexturePath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::Texture::DTextureThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	ASSERT_TRUE(Renderer.CaptureGenerationRequest(
		MakeRequest(*Data), 7, Captured, Error)) << Error;
	ASSERT_NE(Captured.GeneratedPixels, nullptr);
	EXPECT_EQ(Captured.GeneratedPixels->Width, 256u);
	EXPECT_EQ(Captured.GeneratedPixels->Height, 256u);
	EXPECT_EQ(Captured.GeneratedPixels->Pixels.size(), 256u * 256u * 4u);
	EXPECT_NE(Captured.GeneratedPixels->AssetRevision, 0u);
	EXPECT_EQ(Captured.AssetRevision, Captured.GeneratedPixels->AssetRevision);
	EXPECT_EQ(Captured.RendererGeneration, 7u);
	EXPECT_EQ(Captured.KeyInput.Output.Width, 256u);
	EXPECT_EQ(Captured.KeyInput.Output.Height, 256u);
	EXPECT_EQ(Captured.KeyInput.RendererName, "Texture2DSourceThumbnail");
	EXPECT_FALSE(Captured.KeyInput.PreviewFixtureIdentity.empty());
}

TEST(FTextureAssetThumbnailTests, ModuleOwnsBothExactRenderersAndWorkspaceLifecycle)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FTextureEditorModule Module;
	const std::string Texture2DClass =
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString();
	const std::string TextureCubeClass =
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString();
	ASSERT_TRUE(Module.RegisterTextureEditor(Manager, ThumbnailManager));
	EXPECT_TRUE(ThumbnailManager.Find(Texture2DClass));
	EXPECT_FALSE(ThumbnailManager.UsesSourceImage(Texture2DClass));
	EXPECT_TRUE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_NE(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("TextureEditor")), nullptr);
	EXPECT_NE(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("VolumeTextureEditor")), nullptr);
	EXPECT_FALSE(Module.RegisterTextureEditor(Manager, ThumbnailManager));
	Module.UnregisterTextureEditor();
	EXPECT_FALSE(ThumbnailManager.Find(Texture2DClass));
	EXPECT_FALSE(ThumbnailManager.UsesSourceImage(Texture2DClass));
	EXPECT_FALSE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("TextureEditor")), nullptr);
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("VolumeTextureEditor")), nullptr);
}

TEST(FTextureAssetThumbnailTests, VolumePreviewExtractsFrozenR8AxisOrientation)
{
	Durin::FVolumeTextureMipData Mip;
	Mip.Width = 3;
	Mip.Height = 2;
	Mip.Depth = 2;
	Mip.RowPitch = 3;
	Mip.DepthPitch = 6;
	Mip.Voxels.resize(12);
	for (uint32 Z = 0; Z < Mip.Depth; ++Z)
		for (uint32 Y = 0; Y < Mip.Height; ++Y)
			for (uint32 X = 0; X < Mip.Width; ++X)
				Mip.Voxels[Z * Mip.DepthPitch + Y * Mip.RowPitch + X] =
					static_cast<std::byte>(X + Y * 10 + Z * 100);

	using namespace Durin::Editor::Texture;
	const auto XY = ExtractVolumeTexturePreviewSlice(
		Mip, Durin::EPixelFormat::R8_UNORM, EVolumeTexturePreviewAxis::XY, 1);
	ASSERT_TRUE(XY.IsValid());
	EXPECT_EQ(XY.Width, 3u);
	EXPECT_EQ(XY.Height, 2u);
	EXPECT_EQ(XY.Pixels[0], std::byte{100});
	EXPECT_EQ(XY.Pixels[(1 * XY.Width + 2) * 4], std::byte{112});
	const auto XZ = ExtractVolumeTexturePreviewSlice(
		Mip, Durin::EPixelFormat::R8_UNORM, EVolumeTexturePreviewAxis::XZ, 1);
	ASSERT_TRUE(XZ.IsValid());
	EXPECT_EQ(XZ.Width, 3u);
	EXPECT_EQ(XZ.Height, 2u);
	EXPECT_EQ(XZ.Pixels[(1 * XZ.Width + 2) * 4], std::byte{112});
	const auto YZ = ExtractVolumeTexturePreviewSlice(
		Mip, Durin::EPixelFormat::R8_UNORM, EVolumeTexturePreviewAxis::YZ, 2);
	ASSERT_TRUE(YZ.IsValid());
	EXPECT_EQ(YZ.Width, 2u);
	EXPECT_EQ(YZ.Height, 2u);
	EXPECT_EQ(YZ.Pixels[(1 * YZ.Width + 1) * 4], std::byte{112});
}

TEST(FTextureAssetThumbnailTests, VolumePreviewPreservesRGBAAndClampsSlice)
{
	Durin::FVolumeTextureMipData Mip;
	Mip.Width = 2;
	Mip.Height = 1;
	Mip.Depth = 1;
	Mip.RowPitch = 8;
	Mip.DepthPitch = 8;
	Mip.Voxels = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
		std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40}};
	const auto Slice = Durin::Editor::Texture::ExtractVolumeTexturePreviewSlice(
		Mip, Durin::EPixelFormat::RGBA8_UNORM,
		Durin::Editor::Texture::EVolumeTexturePreviewAxis::XY, 99);
	ASSERT_TRUE(Slice.IsValid());
	EXPECT_EQ(Slice.Pixels, Mip.Voxels);
}

TEST(FTextureAssetThumbnailTests, RendererConflictRollsBackWholeIntegration)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterScoped(
		std::make_unique<Durin::Editor::Texture::DTextureCubeThumbnailRenderer>(), Error);
	ASSERT_TRUE(Existing) << Error;
	Durin::FTextureEditorModule Module;
	EXPECT_FALSE(Module.RegisterTextureEditor(Manager, ThumbnailManager));
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("TextureEditor")), nullptr);
}

TEST(FTextureAssetThumbnailTests,
	Texture2DWorkspaceDrawsWideFirstFrameWithoutImportHandler)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FTextureEditorModule Module;
	ASSERT_TRUE(Module.RegisterTextureEditor(Manager, ThumbnailManager));
	ASSERT_TRUE(Manager.OpenAsset(
		std::string(Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath),
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));

	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::GetIO().Fonts->Build();
	ImGui::NewFrame();
	auto Workspace = Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("TextureEditor"));
	ASSERT_NE(Workspace, nullptr);
	ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f), ImGuiCond_Always);
	EXPECT_NO_FATAL_FAILURE(Workspace->DrawWorkspace(true));
	ImGui::EndFrame();
	ImGui::DestroyContext(Context);
	Module.UnregisterTextureEditor();
}

TEST(FTextureCubeThumbnailRendererTests, RendererCapturesPackageAndCubeVisualContract)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath CubePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::DirectionalCubePath,
		CubePath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(CubePath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::Texture::DTextureCubeThumbnailRenderer Renderer;
	const Durin::Editor::FThumbnailRenderingInfo Registration =
		Renderer.GetRegistration();
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	ASSERT_TRUE(Renderer.CaptureGenerationRequest(
		MakeRequest(*Data), 9, Captured, Error)) << Error;
	EXPECT_EQ(Captured.RendererGeneration, 9u);
	EXPECT_EQ(Captured.RequestSerial, 3u);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureIdentity,
		Durin::Editor::FThumbnailVisualContract::
			TextureCubeEnvironmentViewIdentity);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureVersion,
		Durin::Editor::FThumbnailVisualContract::
			TextureCubeEnvironmentViewVersion);
	EXPECT_EQ(Captured.KeyInput.ShaderContractVersion, 2u);
	EXPECT_TRUE(Captured.KeyInput.Dependencies.empty());
	EXPECT_NE(
		std::dynamic_pointer_cast<
			const Durin::Editor::Texture::FTextureCubeThumbnailGenerationInput>(Captured.Input),
		nullptr);

	Captured.KeyInput.Asset = MakeRequest(*Data).Asset;
	Captured.KeyInput.RendererName = Registration.RendererName;
	Captured.KeyInput.GeneratorSchemaVersion =
		Registration.GeneratorSchemaVersion;
	const std::string OriginalKey =
		Durin::Editor::BuildAssetThumbnailCacheKey(Captured.KeyInput);
	Durin::Editor::FAssetThumbnailKeyInput Rebuilt = Captured.KeyInput;
	++Rebuilt.Asset.LastWriteTimeTicks;
	EXPECT_NE(OriginalKey, Durin::Editor::BuildAssetThumbnailCacheKey(Rebuilt));
}

TEST(FTextureCubeThumbnailRendererTests,
	GenerationSessionConfiguresAValueOnlyStableEnvironment)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath CubePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::DirectionalCubePath,
		CubePath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(CubePath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::Texture::DTextureCubeThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	ASSERT_TRUE(Renderer.CaptureGenerationRequest(
		MakeRequest(*Data), 1, Request, Error)) << Error;
	ASSERT_NE(Request.Input, nullptr);
	std::unique_ptr<Durin::Editor::IThumbnailRendererSession> Session =
		Renderer.CreateGenerationSession(Request, *Request.Input, Error);
	ASSERT_NE(Session, nullptr) << Error;
	EXPECT_EQ(
		Session->Load().State,
		Durin::Editor::EThumbnailRendererSessionState::WaitingForResources);

	FCapturingTextureCubeThumbnailPreviewScene PreviewScene;
	ASSERT_TRUE(Session->PreparePreview(PreviewScene, Error)) << Error;
	EXPECT_EQ(PreviewScene.WorldRequests, 0u);
	EXPECT_NEAR(
		PreviewScene.LastView.VerticalFieldOfViewDegrees,
		Durin::Editor::Texture::FTextureCubeThumbnailRendererVisualContract::
			VerticalFieldOfViewDegrees,
		1.0e-5);
	ASSERT_TRUE(PreviewScene.LastEnvironment);
	EXPECT_EQ(
		PreviewScene.LastEnvironment->TextureReference,
		Fixtures.DirectionalCube->GetTextureReferenceRHI());
	Session->ResetPreview();
	Session.reset();
	EXPECT_EQ(PreviewScene.WorldRequests, 0u);
}

TEST(FTextureCubeThumbnailRendererTests, RendererRejectsMissingRegistryData)
{
	Durin::Tests::RegisterAssetThumbnailFixtureMount();
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/ThumbnailFixtures/Textures/TC_Missing", MissingPath));
	Durin::Editor::Texture::DTextureCubeThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Renderer.CaptureGenerationRequest({
		.Asset = {
			.VirtualPath = MissingPath,
			.AssetClassName =
				Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.PackageFormatVersion = 1,
			.FileSize = 1,
			.LastWriteTimeTicks = 1},
		.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
		.RequestSerial = 1}, 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);
}
