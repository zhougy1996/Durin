#include "Thumbnail/TextureCubeAssetThumbnail.h"
#include "Thumbnail/Texture2DAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "TextureEditorModule.h"
#include "Editor/WorkspaceManager.h"

#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"

#include "AssetTools.h"
#include "Texture/TextureCube.h"
#include "Texture/Texture2D.h"
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
				.FileSize = static_cast<Durin::uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
			.RequestSerial = 3};
	}
}

class FCapturingTextureCubeThumbnailPreviewScene final
	: public Durin::Editor::IRenderedAssetThumbnailPreviewScene
{
public:
	auto GetWorld() -> Durin::DWorld* override
	{
		++WorldRequests;
		return nullptr;
	}

	auto SetView(
		const Durin::Editor::FRenderedAssetThumbnailPreviewView& View,
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

	Durin::uint32 WorldRequests = 0;
	Durin::Editor::FRenderedAssetThumbnailPreviewView LastView;
	std::optional<Durin::FViewEnvironmentOverride> LastEnvironment;
};

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
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(TexturePath);
	ASSERT_NE(Data, nullptr);
	Durin::Editor::Texture::FTexture2DAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailSourceImage Source;
	ASSERT_TRUE(Provider.CaptureSourceImage(*Data, Source, Error)) << Error;
	EXPECT_FALSE(Source.PhysicalPath.empty());
	EXPECT_GT(Source.FileSize, 0u);
	EXPECT_TRUE(std::filesystem::exists(Source.PhysicalPath));
}

TEST(FTextureAssetThumbnailTests, ModuleOwnsBothExactProvidersAndWorkspaceLifecycle)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::FRenderedAssetThumbnailService Service;
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
		Durin::Editor::FWorkspaceTypeId("TextureEditor")), nullptr);
	EXPECT_FALSE(Module.RegisterTextureEditor(Manager, Service));
	Module.UnregisterTextureEditor();
	EXPECT_FALSE(Service.Find(Texture2DClass));
	EXPECT_FALSE(Service.UsesSourceImage(Texture2DClass));
	EXPECT_FALSE(Service.Find(TextureCubeClass));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("TextureEditor")), nullptr);
}

TEST(FTextureAssetThumbnailTests, ProviderConflictRollsBackWholeIntegration)
{
	InitializeDObjectSystem();
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::FRenderedAssetThumbnailService Service;
	std::string Error;
	auto Existing = Service.RegisterScoped(
		std::make_unique<Durin::Editor::Texture::FTextureCubeAssetThumbnailProvider>(), Error);
	ASSERT_TRUE(Existing) << Error;
	Durin::FTextureEditorModule Module;
	EXPECT_FALSE(Module.RegisterTextureEditor(Manager, Service));
	EXPECT_FALSE(Service.Find(
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(Service.Find(
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindWorkspace(
		Durin::Editor::FWorkspaceTypeId("TextureEditor")), nullptr);
}

TEST(FTextureAssetThumbnailTests,
	Texture2DWorkspaceDrawsWideFirstFrameWithoutImportHandler)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::FRenderedAssetThumbnailService Service;
	Durin::FTextureEditorModule Module;
	ASSERT_TRUE(Module.RegisterTextureEditor(Manager, Service));
	ASSERT_TRUE(Manager.OpenAsset(
		std::string(Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath),
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
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(CubePath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::Texture::FTextureCubeAssetThumbnailProvider Provider;
	const Durin::Editor::FAssetThumbnailProviderRegistration Registration =
		Provider.GetRegistration();
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	ASSERT_TRUE(Provider.CaptureGenerationRequest(
		MakeRequest(*Data), 9, Captured, Error)) << Error;
	EXPECT_EQ(Captured.ProviderGeneration, 9u);
	EXPECT_EQ(Captured.RequestSerial, 3u);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureIdentity,
		Durin::Editor::FRenderedAssetThumbnailVisualContract::
			TextureCubeEnvironmentViewIdentity);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureVersion,
		Durin::Editor::FRenderedAssetThumbnailVisualContract::
			TextureCubeEnvironmentViewVersion);
	EXPECT_EQ(Captured.KeyInput.ShaderContractVersion, 2u);
	EXPECT_TRUE(Captured.KeyInput.Dependencies.empty());
	EXPECT_NE(
		std::dynamic_pointer_cast<
			const Durin::Editor::Texture::FTextureCubeThumbnailGenerationInput>(Captured.Input),
		nullptr);

	Captured.KeyInput.Asset = MakeRequest(*Data).Asset;
	Captured.KeyInput.ProviderName = Registration.ProviderName;
	Captured.KeyInput.GeneratorSchemaVersion =
		Registration.GeneratorSchemaVersion;
	const std::string OriginalKey =
		Durin::Editor::BuildAssetThumbnailCacheKey(Captured.KeyInput);
	Durin::Editor::FAssetThumbnailKeyInput Rebuilt = Captured.KeyInput;
	++Rebuilt.Asset.LastWriteTimeTicks;
	EXPECT_NE(OriginalKey, Durin::Editor::BuildAssetThumbnailCacheKey(Rebuilt));
}

TEST(FTextureCubeAssetThumbnailTests,
	GenerationSessionConfiguresAValueOnlyStableEnvironment)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath CubePath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::DirectionalCubePath,
		CubePath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(CubePath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::Texture::FTextureCubeAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	ASSERT_TRUE(Provider.CaptureGenerationRequest(
		MakeRequest(*Data), 1, Request, Error)) << Error;
	ASSERT_NE(Request.Input, nullptr);
	std::unique_ptr<Durin::Editor::IRenderedAssetThumbnailGenerationSession> Session =
		Provider.CreateGenerationSession(Request, *Request.Input, Error);
	ASSERT_NE(Session, nullptr) << Error;
	EXPECT_EQ(
		Session->Load().State,
		Durin::Editor::ERenderedAssetThumbnailSessionState::WaitingForResources);

	FCapturingTextureCubeThumbnailPreviewScene PreviewScene;
	ASSERT_TRUE(Session->PreparePreview(PreviewScene, Error)) << Error;
	EXPECT_EQ(PreviewScene.WorldRequests, 0u);
	EXPECT_NEAR(
		PreviewScene.LastView.VerticalFieldOfViewDegrees,
		Durin::Editor::Texture::FTextureCubeAssetThumbnailVisualContract::
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

TEST(FTextureCubeAssetThumbnailTests, ProviderRejectsMissingRegistryData)
{
	Durin::Tests::RegisterRenderedAssetThumbnailFixtureMount();
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Textures/TC_Missing", MissingPath));
	Durin::Editor::Texture::FTextureCubeAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Provider.CaptureGenerationRequest({
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
