#include "ThumbnailManagerTestSupport.h"
#include "Editor/AssetEditorTestHost.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "TextureEditorModule.h"
#include "VolumeTexturePreview.h"
#include "Documents/EditorDocumentManager.h"
#include "Toolkits/AssetEditor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetDefinition/AssetDefinitionRegistry.h"
#include "AssetDefinition/AssetDefinition.h"
#include "Editor/EditorEngineTestSupport.h"
#include "AssetTools/IAssetTools.h"
#include "AssetTools/Relocation.h"
#include "Misc/MountPaths.h"

#include "Thumbnail/AssetThumbnailTestFixtures.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "DObject/Class.h"
#include "Modules/ModuleTestSupport.h"
#include "Texture/TextureCube.h"
#include "Texture/Texture2D.h"
#include "Texture/VolumeTexture.h"
#include "ThirdParty/ImGui/imgui.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	auto MakeAssetPath(const Durin::FPackagePath& PackagePath)
		-> Durin::FTopLevelAssetPath
	{
		return Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(PackagePath);
	}

	auto MakeRequest(const Durin::FPackageData& Data)
		-> Durin::FAssetThumbnailRequest
	{
		return {
			.Asset = {
				.AssetPath = MakeAssetPath(Data.PackagePath),
				.PackagePath = Data.PackagePath,
				.AssetClassName = Data.TopLevelAssets.front().AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks},
			.Priority = Durin::EAssetThumbnailPriority::Visible,
			.RequestSerial = 3};
	}
}

class FTextureAssetSessionTests : public testing::Test
{
protected:
	auto SetUp() -> void override
	{
		std::string Error;
		ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
		Harness = std::make_unique<Durin::FModuleTestHarness>("TextureEditor");
		Harness->Start(Module);
		Host = Durin::NewObject<Durin::DEditorEngine>(nullptr, {});
		ASSERT_NO_THROW(Durin::FEditorEngineTestAccess::StartEditor(*Host));
		Service = Host->GetEditorSubsystem<Durin::DAssetEditorSubsystem>();
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath, Path));
		Durin::FSoftObjectPath ObjectPath;
		ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(MakeAssetPath(Path), std::span<const std::string>{}, ObjectPath));
		Args.AssetEditors = Service;
		Args.Assets = {{ObjectPath, Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()}};
	}
	auto TearDown() -> void override
	{
		if (Host)
		{
			Host->PreExit();
			Durin::MarkObjectHierarchyAsGarbage(Host.Get());
			Host.Reset();
		}
		if (Harness) Harness->Shutdown();
		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	}
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	Durin::FTextureEditorModule Module;
	std::unique_ptr<Durin::FModuleTestHarness> Harness;
	Durin::TStrongObjectPtr<Durin::DEditorEngine> Host;
	Durin::DAssetEditorSubsystem* Service = nullptr;
	Durin::FAssetOpenArgs Args;
};

TEST_F(FTextureAssetSessionTests, DefinitionCreatesAndReusesRealTextureSessionsWithSeparateViewIdentity)
{
	const auto* Definition = Durin::FAssetDefinitionRegistry::Get().GetAssetDefinitionForClass(Args.Assets.front().AssetClassName);
	ASSERT_NE(nullptr, Definition);
	EXPECT_TRUE(Definition->GetAssetOpenSupport({Durin::EAssetOpenMethod::View}).IsSupported);
	ASSERT_TRUE(Service->OpenAssets(Args));
	ASSERT_EQ(1u, Service->GetOwnedAssetEditors().size());
	const auto Edit = Service->GetOwnedAssetEditors().front();
	ASSERT_TRUE(Service->OpenAssets(Args));
	EXPECT_EQ(1u, Service->GetOwnedAssetEditors().size());
	EXPECT_EQ(Edit->GetToolkit(), Service->GetActiveAssetToolkit());
	Args.OpenMethod = Durin::EAssetOpenMethod::View;
	ASSERT_TRUE(Service->OpenAssets(Args));
	EXPECT_EQ(2u, Service->GetOwnedAssetEditors().size());
	const auto View = Service->GetActiveAssetToolkit();
	ASSERT_TRUE(View);
	EXPECT_TRUE(View->IsReadOnly());
	EXPECT_FALSE(View->SaveActiveDocument());
	EXPECT_FALSE(View->Undo());
	EXPECT_NE(View->GetPresentationRootKey("TextureEditor"), Edit->GetToolkit()->GetPresentationRootKey("TextureEditor"));
	EXPECT_TRUE(Service->CloseAllAssetEditors());
	EXPECT_TRUE(Service->GetOwnedAssetEditors().empty());
}

TEST_F(FTextureAssetSessionTests, DiscardMigratesSessionRootsAndRebindsAssetAssociations)
{
	ASSERT_TRUE(Service->OpenAssets(Args));
	ASSERT_EQ(1u, Service->GetOwnedAssetEditors().size());
	const auto Session = Service->GetOwnedAssetEditors().front();
	const auto Objects = Session->GetObjectsToEdit();
	ASSERT_EQ(1u, Objects.size());
	const Durin::FObjectKey Old(Objects.front());
	const auto Document = Session->GetDocumentManager()->GetDocuments().front();
	ASSERT_TRUE(Session->GetToolkit()->DiscardDocument(Document));
	const auto Replacements = Session->GetObjectsToEdit();
	ASSERT_EQ(1u, Replacements.size());
	EXPECT_NE(Old, Durin::FObjectKey(Replacements.front()));
	EXPECT_EQ(Session.Get(), Service->FindEditorForAsset(Replacements.front(), false));
	EXPECT_EQ(1u, Service->GetAllOpenEditors().size());
	EXPECT_TRUE(Session->CloseWindow(Durin::EAssetEditorCloseReason::AssetEditorHostClosed));
}

TEST_F(FTextureAssetSessionTests, ExistingViewDoesNotInterceptADefaultEditOpen)
{
	Args.OpenMethod = Durin::EAssetOpenMethod::View;
	ASSERT_TRUE(Service->OpenAssets(Args));
	ASSERT_TRUE(Service->GetActiveAssetToolkit()->IsReadOnly());
	auto* Object = Args.LoadAsset(Args.Assets.front());
	ASSERT_NE(nullptr, Object);
	EXPECT_TRUE(Service->OpenEditorForAsset(Object));
	ASSERT_EQ(2u, Service->GetOwnedAssetEditors().size());
	EXPECT_FALSE(Service->GetActiveAssetToolkit()->IsReadOnly());
}

TEST_F(FTextureAssetSessionTests, RelocationPreservesOpenTextureRoutingAndSave)
{
	const auto SourcePackage = Args.Assets.front().AssetPath.GetPackagePath().ToString();
	std::string DuplicateError;
	Durin::FFeedbackContext DuplicateFeedback({}, [&](Durin::ELogLevel Level, std::string_view Message) {
		if (Level >= Durin::ELogLevel::Error) { if (!DuplicateError.empty()) DuplicateError += "\n"; DuplicateError += Message; }
	});
	const auto Duplicate = Durin::IAssetTools::Get().DuplicateAsset({
		.SourcePath = Args.Assets.front().AssetPath.GetAssetPath(),
		.DestinationDirectory = SourcePackage.substr(0, SourcePackage.find_last_of('/')) + "/SessionRelocation"}, &DuplicateFeedback);
	ASSERT_TRUE(Duplicate) << DuplicateError;
	ASSERT_TRUE(Durin::SavePackage(Duplicate->GetPackage()));
	ASSERT_NE(nullptr, Duplicate);
	(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Duplicate);
	ASSERT_TRUE(Durin::Cast<Durin::DTexture2D>(Duplicate)->IsAsyncCacheComplete());
	Durin::FSoftObjectPath DuplicatedPath;
	ASSERT_TRUE(Durin::FSoftObjectPath::TryCreate(Duplicate->GetObjectPath(), DuplicatedPath));
	Args.Assets.front().AssetPath = DuplicatedPath;
	ASSERT_TRUE(Service->OpenAssets(Args));
	const auto Session = Service->GetOwnedAssetEditors().front();
	Durin::FPackagePath Destination;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(DuplicatedPath.GetPackagePath().ToString() + "Moved", Destination));
	const Durin::FAssetRelocationMapping Mapping{DuplicatedPath.GetPackagePath(), Destination};
	ASSERT_TRUE(Durin::RelocateAssets(std::span{&Mapping, 1}).Result);
	(void)Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Duplicate);
	const auto Document = Session->GetDocumentManager()->GetDocuments().front();
	EXPECT_NE(DuplicatedPath.ToString(), Document.ResourceId);
	EXPECT_EQ(Duplicate->GetObjectPath(), Document.ResourceId);
	EXPECT_EQ(Session.Get(), Service->FindEditorForAsset(Duplicate, false));
	EXPECT_TRUE(Session->GetToolkit()->SaveDocument(Document));
	EXPECT_TRUE(Session->CloseWindow(Durin::EAssetEditorCloseReason::AssetEditorHostClosed));
}

class FCapturingTextureCubeThumbnailPreviewScene final
	: public Durin::IThumbnailRenderContext
{
public:
	auto SetScene(Durin::FThumbnailPreviewScene&) -> std::expected<void, std::string> override
	{
		++SceneBindings;
		return {};
	}

	auto SetView(
		const Durin::FThumbnailPreviewView& View) -> std::expected<void, std::string> override
	{
		LastView = View;
		return {};
	}

	auto SetViewEnvironment(
		const Durin::FViewEnvironmentOverride& Environment) -> std::expected<void, std::string> override
	{
		LastEnvironment = Environment;
		return {};
	}

	uint32 SceneBindings = 0;
	Durin::FThumbnailPreviewView LastView;
	std::optional<Durin::FViewEnvironmentOverride> LastEnvironment;
};

TEST(FTextureAssetThumbnailTests, Texture2DRendererCapturesBuiltSessionWithoutSourcePixels)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FPackagePath TexturePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath,
		TexturePath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(TexturePath);
	ASSERT_NE(Data, nullptr);

	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::TextureEditor::DTextureThumbnailRenderer>();
	auto& Renderer = *RendererOwner;
	Durin::FAssetThumbnailGenerationRequest Captured;
	auto CapturedResult = Renderer.CaptureGenerationRequest(
		MakeRequest(*Data), 7);
	ASSERT_TRUE(CapturedResult) << CapturedResult.error();
	Captured = std::move(*CapturedResult);
	ASSERT_NE(Captured.Input, nullptr);
	EXPECT_EQ(Captured.KeyInput.GeneratorSchemaVersion, 2u);
	EXPECT_EQ(Captured.KeyInput.ShaderContractVersion, 2u);
	auto Session = Renderer.CreateGenerationSession(Captured, *Captured.Input);
	ASSERT_TRUE(Session) << Session.error();
	ASSERT_NE(*Session, nullptr);
	EXPECT_EQ(Captured.RendererGeneration, 7u);
	EXPECT_EQ(Captured.KeyInput.Output.Width, 256u);
	EXPECT_EQ(Captured.KeyInput.Output.Height, 256u);
	EXPECT_EQ(Captured.KeyInput.RendererName, "Texture2DBuiltThumbnail");
	EXPECT_FALSE(Captured.KeyInput.PreviewFixtureIdentity.empty());
}

TEST(FTextureAssetThumbnailTests, ModuleStartupPublishesIndependentRenderersWithoutSharedToolkits)
{
	InitializeDObjectSystem();
	Durin::FEditorDocumentManager Manager;
	auto& ThumbnailManager = Durin::GetDefaultThumbnailManager();
	Durin::FTextureEditorModule Module;
	Durin::FModuleTestHarness Harness("TextureEditor");
	Harness.Start(Module);
	EXPECT_TRUE(ThumbnailManager.Find(Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(!Manager.GetToolkit());
	Harness.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_FALSE(ThumbnailManager.Find(Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString()));
}


TEST(FTextureAssetThumbnailTests, VolumePreviewExtractsFrozenR8AxisOrientation)
{
	Durin::FVolumeTextureMipData Mip;
	Mip.Width = 3;
	Mip.Height = 2;
	Mip.Depth = 2;
	Mip.RowPitch = 3;
	Mip.DepthPitch = 6;
	Durin::FByteBuffer Voxels(12);
	for (uint32 Z = 0; Z < Mip.Depth; ++Z)
		for (uint32 Y = 0; Y < Mip.Height; ++Y)
			for (uint32 X = 0; X < Mip.Width; ++X)
				Voxels[Z * Mip.DepthPitch + Y * Mip.RowPitch + X] =
					static_cast<std::byte>(X + Y * 10 + Z * 100);

	Mip.Voxels = Durin::FSharedByteBuffer::Take(std::move(Voxels));

	using namespace Durin::TextureEditor;
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
	Mip.Voxels = Durin::FSharedByteBuffer::Take(Durin::FByteBuffer{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
		std::byte{10}, std::byte{20}, std::byte{30}, std::byte{40}});
	const auto Slice = Durin::TextureEditor::ExtractVolumeTexturePreviewSlice(
		Mip, Durin::EPixelFormat::RGBA8_UNORM,
		Durin::TextureEditor::EVolumeTexturePreviewAxis::XY, 99);
	ASSERT_TRUE(Slice.IsValid());
	EXPECT_TRUE(std::ranges::equal(Slice.Pixels, Mip.Voxels));
}

TEST(FTextureAssetThumbnailTests, VolumePreviewReadsCanonicalSourceAndRejectsOtherShapes)
{
	auto Image = Durin::Image::FImage::TryCreate({.Width = 2, .Height = 1, .Depth = 2,
		.Format = Durin::Image::ERawImageFormat::G8},
		Durin::FByteBuffer{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}});
	ASSERT_TRUE(Image);
	Durin::FTextureSource Source;
	ASSERT_TRUE(Source.InitVolume(Image->GetView()));
	const auto Identity = Source.GetIdentity();
	const auto Payload = Source.GetBulkData().GetInstanceId();
	const auto Slice = Durin::TextureEditor::ExtractVolumeTexturePreviewSlice(
		Source, Durin::TextureEditor::EVolumeTexturePreviewAxis::XY, 99);
	ASSERT_TRUE(Slice.IsValid());
	EXPECT_EQ(Slice.Width, 2u);
	EXPECT_EQ(Slice.Height, 1u);
	EXPECT_EQ(Slice.Pixels[0], std::byte{3});
	EXPECT_EQ(Slice.Pixels[4], std::byte{4});
	EXPECT_EQ(Slice.Pixels[3], std::byte{255});
	EXPECT_EQ(Source.GetIdentity(), Identity);
	EXPECT_EQ(Source.GetBulkData().GetInstanceId(), Payload);
	EXPECT_FALSE(Durin::TextureEditor::ExtractVolumeTexturePreviewSlice(
		Durin::FTextureSource{}, Durin::TextureEditor::EVolumeTexturePreviewAxis::XY, 0).IsValid());
	auto Flat = Durin::Image::FImage::TryCreate({.Width = 2, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::G8}, Durin::FByteBuffer(2));
	ASSERT_TRUE(Flat);
	Durin::FTextureSource Source2D;
	ASSERT_TRUE(Source2D.Init2D(Flat->GetView(), 1));
	EXPECT_FALSE(Durin::TextureEditor::ExtractVolumeTexturePreviewSlice(
		Source2D, Durin::TextureEditor::EVolumeTexturePreviewAxis::XY, 0).IsValid());
}

TEST(FTextureAssetThumbnailTests, RendererConflictDoesNotAffectIndependentModuleContributions)
{
	InitializeDObjectSystem();
	Durin::FEditorDocumentManager Manager;
	Durin::DThumbnailManager ThumbnailManager;
	std::string Error;
	auto Existing = ThumbnailManager.RegisterCustomRenderer(Durin::DTextureCube::StaticClass(), Durin::TextureEditor::DTextureCubeThumbnailRenderer::StaticClass());
	ASSERT_TRUE(Existing) << Existing.error();
	Durin::FTextureEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("TextureEditor");
	ModuleHarness.Start(Module);
	EXPECT_FALSE(ThumbnailManager.RegisterCustomRenderer(Durin::DTextureCube::StaticClass(), Durin::TextureEditor::DTextureCubeThumbnailRenderer::StaticClass()));
	EXPECT_TRUE(Durin::GetDefaultThumbnailManager().Find(Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_FALSE(ThumbnailManager.Find(
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_TRUE(ThumbnailManager.Find(
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString()));
	EXPECT_EQ(Manager.FindToolkit(
		Durin::FAssetEditorToolkitId("TextureEditor")), nullptr);
	ModuleHarness.Shutdown();
}

TEST(FTextureAssetThumbnailTests,
	Texture2DWorkspaceDrawsWideFirstFrameWithoutImportHandler)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FTextureEditorModule Module;
	Durin::FModuleTestHarness ModuleHarness("TextureEditor");
	ModuleHarness.Start(Module);
	Durin::Testing::FScopedAssetEditorTestHost TestHost;
	ASSERT_NE(nullptr, TestHost.GetService());
	Durin::FPackagePath ParentTexturePackagePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath,
		ParentTexturePackagePath));
	ASSERT_TRUE(TestHost.Open(
		MakeAssetPath(ParentTexturePackagePath).ToString(),
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString()));

	const auto Session = TestHost.GetService()->GetOwnedAssetEditors().front();
	auto& Manager = *Session->GetDocumentManager();
	ImGuiContext* Context = ImGui::CreateContext();
	ASSERT_NE(Context, nullptr);
	ImGui::GetIO().IniFilename = nullptr;
	ImGui::GetIO().DisplaySize = ImVec2(1280.0f, 720.0f);
	ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
	ImGui::GetIO().Fonts->Build();
	ImGui::NewFrame();
	auto Workspace = Session->GetToolkit();
	ASSERT_NE(Workspace, nullptr);
	ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f), ImGuiCond_Always);
	EXPECT_NO_FATAL_FAILURE(Session->GetToolkit()->DrawToolkit(true));
	ImGui::EndFrame();
	ImGui::DestroyContext(Context);
	ASSERT_EQ(Manager.RequestCloseDocument(Manager.GetDocuments().front().Id),
		Durin::EDocumentCloseResult::Closed);
	Workspace.reset();
	TestHost.Shutdown();
	ModuleHarness.Shutdown();
}

TEST(FTextureCubeThumbnailRendererTests, RendererCapturesPackageAndCubeVisualContract)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FPackagePath CubePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::DirectionalCubePath,
		CubePath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(CubePath);
	ASSERT_NE(Data, nullptr);

	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::TextureEditor::DTextureCubeThumbnailRenderer>();
	auto& Renderer = *RendererOwner;
	const Durin::FThumbnailRenderingInfo Registration =
		Renderer.GetRegistration();
	Durin::FAssetThumbnailGenerationRequest Captured;
	auto CapturedResult = Renderer.CaptureGenerationRequest(
		MakeRequest(*Data), 9);
	ASSERT_TRUE(CapturedResult) << CapturedResult.error();
	Captured = std::move(*CapturedResult);
	EXPECT_EQ(Captured.RendererGeneration, 9u);
	EXPECT_EQ(Captured.RequestSerial, 3u);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureIdentity,
		Durin::FThumbnailVisualContract::
			TextureCubeEnvironmentViewIdentity);
	EXPECT_EQ(
		Captured.KeyInput.PreviewFixtureVersion,
		Durin::FThumbnailVisualContract::
			TextureCubeEnvironmentViewVersion);
	EXPECT_EQ(Captured.KeyInput.ShaderContractVersion, 2u);
	EXPECT_TRUE(Captured.KeyInput.Dependencies.empty());
	EXPECT_NE(
		std::dynamic_pointer_cast<
			const Durin::TextureEditor::FTextureCubeThumbnailGenerationInput>(Captured.Input),
		nullptr);

	Captured.KeyInput.Asset = MakeRequest(*Data).Asset;
	Captured.KeyInput.RendererName = Registration.RendererName;
	Captured.KeyInput.GeneratorSchemaVersion =
		Registration.GeneratorSchemaVersion;
	const std::string OriginalKey =
		Durin::BuildAssetThumbnailCacheKey(Captured.KeyInput);
	Durin::FAssetThumbnailKeyInput Rebuilt = Captured.KeyInput;
	++Rebuilt.Asset.LastWriteTimeTicks;
	EXPECT_NE(OriginalKey, Durin::BuildAssetThumbnailCacheKey(Rebuilt));
}

TEST(FTextureCubeThumbnailRendererTests,
	GenerationSessionRejectsAnUnavailableResourceSnapshot)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FPackagePath CubePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::DirectionalCubePath,
		CubePath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(CubePath);
	ASSERT_NE(Data, nullptr);

	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::TextureEditor::DTextureCubeThumbnailRenderer>();
	auto& Renderer = *RendererOwner;
	Durin::FAssetThumbnailGenerationRequest Request;
	auto RequestResult = Renderer.CaptureGenerationRequest(
		MakeRequest(*Data), 1);
	ASSERT_TRUE(RequestResult) << RequestResult.error();
	Request = std::move(*RequestResult);
	ASSERT_NE(Request.Input, nullptr);
	auto Session = Renderer.CreateGenerationSession(Request, *Request.Input);
	ASSERT_TRUE(Session) << Session.error();
	ASSERT_NE(*Session, nullptr);
	EXPECT_EQ(
		(*Session)->Load().State,
		Durin::EThumbnailRendererSessionState::WaitingForResources);

	FCapturingTextureCubeThumbnailPreviewScene PreviewScene;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	auto Update = (*Session)->PollResources();
	while (Update.State == Durin::EThumbnailRendererSessionState::WaitingForResources
		&& std::chrono::steady_clock::now() < Deadline)
	{
		Durin::ProcessAsyncLoading(100.0, 16);
		Durin::FAssetCompilingManager::Get().ProcessAsyncTasks(false);
		Update = (*Session)->PollResources();
		if (Update.State == Durin::EThumbnailRendererSessionState::WaitingForResources)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	EXPECT_EQ(Update.State, Durin::EThumbnailRendererSessionState::Failed);
	const auto Prepared = (*Session)->PreparePreview(PreviewScene);
	ASSERT_FALSE(Prepared);
	EXPECT_FALSE(Prepared.error().empty());
	EXPECT_EQ(PreviewScene.SceneBindings, 0u);
	EXPECT_FALSE(PreviewScene.LastEnvironment);
	(*Session)->ResetPreview();
	Session->reset();
	EXPECT_EQ(PreviewScene.SceneBindings, 0u);
}

TEST(FTextureCubeThumbnailRendererTests, RendererRejectsMissingRegistryData)
{
	Durin::Tests::RegisterAssetThumbnailFixtureMount();
	Durin::FPackagePath MissingPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/ThumbnailFixtures/Textures/TC_Missing", MissingPath));
	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::TextureEditor::DTextureCubeThumbnailRenderer>();
	auto& Renderer = *RendererOwner;
	Durin::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	auto CapturedResult = Renderer.CaptureGenerationRequest({
		.Asset = {
			.AssetPath = MakeAssetPath(MissingPath),
			.PackagePath = MissingPath,
			.AssetClassName =
				Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString(),
			.PackageFormatVersion = 1,
			.FileSize = 1,
			.LastWriteTimeTicks = 1},
		.Priority = Durin::EAssetThumbnailPriority::Visible,
		.RequestSerial = 1}, 1);
	ASSERT_FALSE(CapturedResult);
	EXPECT_NE(CapturedResult.error().find(MissingPath.ToString()), std::string::npos);
}
