#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Asset/PackageSerialization.h"
#include "AssetRegistry/Catalog.h"
#include "VulkanEngineTestSupport.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/AssetCompilingManager.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "NativeTestSupport.h"
#include "RendererModule.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "SceneTestAccess.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshTestAccess.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/Texture2D.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Texture/Texture2DRenderResource.h"
#include "TexturePlatformDataTestFixtures.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	auto WriteNpotTextureFixture(const std::filesystem::path& Path) -> void
	{
		constexpr uint16 Width = 1025;
		constexpr uint16 Height = 513;
		std::array<uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<uint8>(Width);
		Header[13] = static_cast<uint8>(Width >> 8);
		Header[14] = static_cast<uint8>(Height);
		Header[15] = static_cast<uint8>(Height >> 8);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		for (uint16 Y = 0; Y < Height; ++Y)
		{
			for (uint16 X = 0; X < Width; ++X)
			{
				const uint8 Value = X == Width - 1 ? 255 : 0;
				const std::array<uint8, 4> Pixel = {Value, Value, Value, 255};
				Stream.write(reinterpret_cast<const char*>(Pixel.data()), Pixel.size());
			}
		}
	}

	auto WriteU32(Durin::FByteArray& Bytes, size_t Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteU64(Durin::FByteArray& Bytes, size_t Offset, uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto ReadU64(const Durin::FByteArray& Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto RefreshEnvelopeHeaderHash(Durin::FByteArray& Bytes) -> void
	{
		const uint64 HeaderBytes = ReadU64(Bytes, 32);
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Bytes).first(static_cast<size_t>(HeaderBytes)));
		WriteU64(Bytes, 48, Hash.HashLow);
		WriteU64(Bytes, 56, Hash.HashHigh);
	}

	auto ContainsText(std::span<const std::byte> Bytes, std::string_view Text) -> bool
	{
		const std::span<const std::byte> TextBytes =
			std::as_bytes(std::span{Text.data(), Text.size()});
		return std::search(Bytes.begin(), Bytes.end(),
			TextBytes.begin(), TextBytes.end()) != Bytes.end();
	}

}

TEST(FTextureCookTests, ColdCookRebuildsFromAuthoredPixelsWithoutSourceOrDdc)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
	Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
	std::string Error;
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureColdAuthoredCook";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	const std::filesystem::path ContentRoot = Root / "Content";
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	const std::filesystem::path CookRoot = std::filesystem::absolute(Root / "Cook");
	std::filesystem::create_directories(ContentRoot);
	Durin::Testing::RegisterMountPointForTests(
		"/TextureColdCookTests/", ContentRoot.generic_string() + "/");
	FScopedDerivedDataCacheRoot ScopedCache(CacheRoot);
	const std::filesystem::path Source = Root / "ColdCook.tga";
	WriteNpotTextureFixture(Source);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureColdCookTests/Texture", AssetPath));
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported =
		Durin::AssetForge::Builtins::ImportTexture2DForTest(
			Source.generic_string(), AssetPath.GetView());
	ASSERT_TRUE(Imported) << Imported.Message;
	ASSERT_TRUE(Imported.Asset->GetSource().IsValid());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(std::filesystem::remove(Source));
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot / "Textures");

	Durin::DTexture2D* Loaded = nullptr;
	const Durin::FAssetResult Load =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded);
	ASSERT_TRUE(Load) << Load.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_TRUE(Loaded->HasPlatformData());
	Durin::FCookContext Cook(
		CookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Loaded, "/Game/ColdTexture", Cook, Error)) << Error;
	ASSERT_TRUE(Cook.Publish(&Error)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Game/ColdTexture.dasset"));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Game/ColdTexture.dbulk"));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTextureCookTests, CookedPackageIsDeterministicAndLoadsWithoutSourceOrDdc)
{
	InitializeDObjectSystem();
	std::string Error;
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureCookedConsumer";
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	const std::filesystem::path ContentRoot = Root / "Content";
	const std::filesystem::path CookRoot = std::filesystem::absolute(Root / "Cook");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Root / "CookSecond");
	const std::filesystem::path DiagnosticCookRoot =
		std::filesystem::absolute(Root / "CookDiagnostic");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(ContentRoot);
	Durin::Testing::RegisterMountPointForTests(
		"/TextureCookTests/", ContentRoot.generic_string() + "/");
	FScopedDerivedDataCacheRoot ScopedCache(CacheRoot);

	const std::filesystem::path Source = Root / "NpotTexture.tga";
	WriteNpotTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Import = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureCookTests/Texture");
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_NE(Import.Asset, nullptr);
	ASSERT_NE(Import.Asset->GetPlatformData(), nullptr);
	const Durin::FTexturePlatformData ExpectedPlatformData = *Import.Asset->GetPlatformData();
	const Durin::FXxHash128 SourceIdentityBeforeCook =
		Import.Asset->GetImportedDataIdentity();
	ASSERT_NE(Import.Asset->GetAssetImportData(), nullptr);
	const Durin::FSourceFile* ImportedSource =
		Import.Asset->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	const Durin::FSourceFile SourceBeforeCook =
		*ImportedSource;
	const bool bPackageDirtyBeforeCook = Import.Asset->GetPackage()->IsDirty();

	Durin::FCookContext First(
		CookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Import.Asset, "/Game/CookedTexture", First, Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;
	EXPECT_EQ(Import.Asset->GetImportedDataIdentity(), SourceIdentityBeforeCook);

	Durin::FCookContext Second(
		SecondCookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Import.Asset, "/Game/CookedTexture", Second, Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;

	Durin::FCookContext Diagnostic(
		DiagnosticCookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		true);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Import.Asset, "/Game/CookedTexture", Diagnostic, Error)) << Error;
	ASSERT_TRUE(Diagnostic.Publish(&Error)) << Error;
	ASSERT_NE(Import.Asset->GetAssetImportData(), nullptr);
	ImportedSource = Import.Asset->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(*ImportedSource, SourceBeforeCook);
	EXPECT_EQ(Import.Asset->GetPackage()->IsDirty(), bPackageDirtyBeforeCook);

	Durin::FByteArray FirstPackage;
	Durin::FByteArray SecondPackage;
	Durin::FByteArray FirstBulk;
	Durin::FByteArray SecondBulk;
	Durin::FByteArray DiagnosticPackage;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (CookRoot / "Game/CookedTexture.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondCookRoot / "Game/CookedTexture.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstBulk, (CookRoot / "Game/CookedTexture.dbulk")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondBulk, (SecondCookRoot / "Game/CookedTexture.dbulk")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		DiagnosticPackage, (DiagnosticCookRoot / "Game/CookedTexture.dasset")));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstBulk, SecondBulk);
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceFile"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceImportData"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceContentHash"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceWidth"));
	EXPECT_TRUE(ContainsText(DiagnosticPackage, "AssetImportData"));
	EXPECT_TRUE(ContainsText(DiagnosticPackage, "Hint"));
	EXPECT_FALSE(ContainsText(DiagnosticPackage, "SourceImportData"));
	EXPECT_FALSE(ContainsText(DiagnosticPackage, "SourceContentHash"));

	Durin::FTexturePlatformData DecodedPlatformData;
	Durin::FCanonicalMemoryReader PayloadAr(
		FirstBulk, Durin::EArchivePurpose::CookedPayload);
	DecodedPlatformData.Serialize(PayloadAr, {
		.TargetPlatform = Durin::ECookTargetPlatform::Win64,
		.TargetProfile = Durin::ECookTargetProfile::Game});
	ASSERT_FALSE(PayloadAr.HasError()) << PayloadAr.GetError();
	ASSERT_TRUE(Durin::RequireArchiveEnd(PayloadAr));
	ExpectPlatformDataEqual(DecodedPlatformData, ExpectedPlatformData);
	ASSERT_EQ(DecodedPlatformData.Mips.back().Width, 1u);
	ASSERT_EQ(DecodedPlatformData.Mips.back().Height, 1u);

	const std::filesystem::path CorruptRoot =
		std::filesystem::absolute(Root / "CookCorrupt");
	std::filesystem::copy(CookRoot, CorruptRoot,
		std::filesystem::copy_options::recursive);
	Durin::FByteArray CorruptBulk = FirstBulk;
	CorruptBulk.back() ^= std::byte{0x80};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CorruptBulk)),
		CorruptRoot / "Game/CookedTexture.dbulk"));

	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::Testing::RemoveTestWorkDirectory(Root / "Content" / "Textures");
	Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
	ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	Durin::FPackagePath CookedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedTexture", CookedPath));
	const Durin::FAssetCatalogEntry CookedAssetData =
		Durin::FindAssetExact(CookedPath);
	ASSERT_NE(CookedAssetData, nullptr);
	ASSERT_EQ(CookedAssetData->TopLevelAssets.size(), 1u);
	const Durin::FTopLevelAssetPath CookedAssetPath =
		CookedAssetData->TopLevelAssets.front().AssetPath;
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	struct FBeginCookedTextureUploadFrame
	{
		static constexpr auto GetName() -> const char* { return "BeginCookedTextureUploadFrame"; }
	};
	Durin::EnqueueRenderCommand<FBeginCookedTextureUploadFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			CommandList.SwitchPipeline(Durin::ERHIPipeline::Graphics);
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
		});
	Durin::DTexture2D* CookedTexture = nullptr;
	const Durin::FAssetResult LoadResult =
		Durin::LoadObject(CookedAssetPath, CookedTexture);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(CookedTexture, nullptr);
	const auto BulkStateBeforeGet = CookedTexture->GetCookedPlatformData().GetState();
	const auto RevisionBeforeGet = CookedTexture->GetBuildRevision();
	const Durin::DTexture2D& ConstTexture = *CookedTexture;
	EXPECT_EQ(ConstTexture.GetPlatformData(), nullptr);
	EXPECT_EQ(ConstTexture.GetPlatformData(), nullptr);
	EXPECT_FALSE(CookedTexture->HasPlatformData());
	EXPECT_EQ(CookedTexture->GetCookedPlatformData().GetState(), BulkStateBeforeGet);
	EXPECT_EQ(CookedTexture->GetBuildRevision(), RevisionBeforeGet);
	ASSERT_TRUE(static_cast<Durin::DTexture&>(*CookedTexture).EnsurePlatformDataLoadedBlocking());
	ASSERT_NE(CookedTexture->GetPlatformData(), nullptr);
	const auto* InstalledPlatform = CookedTexture->GetPlatformData();
	const auto InstalledRevision = CookedTexture->GetBuildRevision();
	ASSERT_TRUE(static_cast<Durin::DTexture&>(*CookedTexture).EnsurePlatformDataLoadedBlocking());
	EXPECT_EQ(CookedTexture->GetPlatformData(), InstalledPlatform);
	EXPECT_EQ(CookedTexture->GetBuildRevision(), InstalledRevision);
	auto* MissingPlatform = Durin::NewObject<Durin::DTexture2D>(
		nullptr, "MissingCookedPlatformData");
	EXPECT_FALSE(MissingPlatform->EnsurePlatformDataLoadedBlocking());
	EXPECT_EQ(MissingPlatform->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*CookedTexture->GetPlatformData(), ExpectedPlatformData);
	EXPECT_EQ(CookedTexture->GetAssetImportData(), nullptr);
	EXPECT_NE(CookedTexture->GetCookedPlatformData().GetMetadata().LogicalSize, 0u);

	Durin::FRHITextureReferenceRef TextureReference =
		CookedTexture->GetTextureReferenceRHI();
	ASSERT_NE(TextureReference, nullptr);
	Durin::FRendererModule Renderer;
	Durin::FModuleTestHarness RendererLifecycle("TextureCookRendererTest");
	RendererLifecycle.Start(Renderer);
	Durin::FScenePtr SceneOwner = Renderer.CreateScene();
	auto& Scene = static_cast<Durin::FScene&>(*SceneOwner);
	auto* SampleMesh = Durin::DStaticMesh::CreateDebugTriangle();
	for (Durin::FVector2f& TexCoord :
		Durin::FStaticMeshTestAccess::GetMutableRenderData(SampleMesh)
			->LODResources.front()
			.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.GetMutableTexCoords().front())
	{
		const float Width = static_cast<float>(
			ExpectedPlatformData.Mips.front().Width);
		TexCoord = {(Width - 0.5f) / Width, 0.0f};
	}
	auto* SampleMaterial =
		Durin::NewObject<Durin::DMaterial>(nullptr, "CookedTextureSampleMaterial");
	Durin::FMaterialProgramValidationResult SampleMaterialValidation;
	ASSERT_TRUE(SampleMaterial->SetMaterialProgram(
		Durin::MakeCanonicalMaterialProgram(), SampleMaterialValidation));
	ASSERT_TRUE(SampleMaterial->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(1.0)));
	ASSERT_TRUE(SampleMaterial->SetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), CookedTexture));
	Durin::DObject* SampleMaterialCompilationObject = SampleMaterial;
	Durin::FAssetCompilingManager::Get().FinishCompilationForObjects(
		std::span<Durin::DObject* const>(&SampleMaterialCompilationObject, 1));
	ASSERT_TRUE(SampleMaterial->GetMaterialCompileStatus().IsCurrent());
	auto* SampleComponent =
		Durin::NewObject<Durin::DStaticMeshComponent>(nullptr, "CookedTextureSampleMesh");
	SampleComponent->SetStaticMesh(SampleMesh);
	SampleComponent->SetMaterial(SampleMaterial);
	auto SampleProxy = std::make_shared<std::unique_ptr<Durin::FPrimitiveSceneProxy>>(
		SampleComponent->CreateSceneProxy());
	ASSERT_NE(*SampleProxy, nullptr);
	struct FEndCookedTextureUploadFrame
	{
		static constexpr auto GetName() -> const char* { return "EndCookedTextureUploadFrame"; }
	};
	Durin::EnqueueRenderCommand<FEndCookedTextureUploadFrame>(
		[](Durin::FRHICommandListImmediate& CommandList) {
			Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
		});
	Durin::FlushRenderingCommands();
	struct FTextureCookUploadResult
	{
		bool bSucceeded = true;
		std::string Error;
		Durin::FByteArray SamplePixels;
	};
	auto UploadResult = std::make_shared<FTextureCookUploadResult>();
	struct FValidateCookedTextureUpload
	{
		static constexpr auto GetName() -> const char* { return "ValidateCookedTextureUpload"; }
	};
	Durin::EnqueueRenderCommand<FValidateCookedTextureUpload>(
		[&Renderer, &Scene, TextureReference, ExpectedPlatformData, UploadResult, SampleProxy](
			Durin::FRHICommandListImmediate& CommandList) {
			Durin::GDynamicRHI->RHIBeginFrame_RenderThread(CommandList);
			struct FEndFrameGuard
			{
				Durin::FRHICommandListImmediate& CommandList;
				~FEndFrameGuard()
				{
					Durin::GDynamicRHI->RHIEndFrame_RenderThread(CommandList);
				}
			} EndFrameGuard{CommandList};
			Durin::FRHITexture* TextureRHI =
				TextureReference->GetReferencedTexture_RenderThread();
			if (!TextureRHI
				|| TextureRHI->GetDimension() != Durin::ETextureDimension::Texture2D
				|| TextureRHI->GetSizeX() != ExpectedPlatformData.Mips.front().Width
				|| TextureRHI->GetSizeY() != ExpectedPlatformData.Mips.front().Height
				|| TextureRHI->GetFormat() != ExpectedPlatformData.PixelFormat
				|| TextureRHI->GetNumMips() != ExpectedPlatformData.Mips.size())
			{
				UploadResult->bSucceeded = false;
				UploadResult->Error =
					"Cooked Texture2D Vulkan resource descriptor did not match TXPL.";
				return;
			}

			Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene,
				Durin::FPrimitiveSceneId(1), std::move(*SampleProxy), Durin::FMatrix(1.0));
			Durin::FRHITextureCreateDesc ColorDesc =
				Durin::FRHITextureCreateDesc::Create2D(
					"CookedTextureSampleColor", 17, 17, Durin::EPixelFormat::SRGBA8_UNORM)
					.SetFlags(Durin::ETextureCreateFlags::RenderTargetable
						| Durin::ETextureCreateFlags::ShaderResource
						| Durin::ETextureCreateFlags::CPUReadback);
			Durin::FTextureRHIRef Color =
				Durin::GDynamicRHI->RHICreateTexture(CommandList, ColorDesc);
			Durin::FSceneView View;
			View.Settings.Mode.RenderMode = Durin::ERenderMode::Unlit;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewLocation = {0.0, 0.0, -1.0};
			View.ViewportWidth = 17;
			View.ViewportHeight = 17;
			if (Renderer.RenderView(
					CommandList, &Scene, View, Color, false, {})
				!= Durin::ERenderViewResult::Success)
			{
				UploadResult->bSucceeded = false;
				UploadResult->Error =
					"Failed to render the cooked Texture2D sample target.";
				return;
			}
			if (!Durin::GDynamicRHI->RHIReadTexture2D(
				CommandList, Color, 0, 0, UploadResult->SamplePixels))
			{
				UploadResult->bSucceeded = false;
				UploadResult->Error = "Failed to read the cooked Texture2D sample target.";
			}
		});
	Durin::FlushRenderingCommands();
	EXPECT_TRUE(UploadResult->bSucceeded) << UploadResult->Error;
	if (UploadResult->bSucceeded)
	{
		ASSERT_EQ(UploadResult->SamplePixels.size(), 17u * 17u * 4u);
		const size_t Center = (8u * 17u + 8u) * 4u;
		EXPECT_GT(std::to_integer<uint8>(UploadResult->SamplePixels[Center]), 180u);
		EXPECT_GT(std::to_integer<uint8>(UploadResult->SamplePixels[Center + 1]), 180u);
		EXPECT_GT(std::to_integer<uint8>(UploadResult->SamplePixels[Center + 2]), 180u);
	}

	Durin::FSceneInterfaceTestAccess::ReleaseScene(SceneOwner);
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(SampleComponent);
	Durin::MarkAsGarbage(SampleMesh);
	Durin::MarkAsGarbage(SampleMaterial);
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::UnloadPackage(CookedPath));
	CookedTexture = nullptr;
	ASSERT_TRUE(AssetRuntime.Restore());
	struct FRetireCookedTextureResource
	{
		static constexpr auto GetName() -> const char* { return "RetireCookedTextureResource"; }
	};
	Durin::EnqueueRenderCommand<FRetireCookedTextureResource>(
		[Reference = std::move(TextureReference)](
			Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	RendererLifecycle.Shutdown();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
	ASSERT_TRUE(std::filesystem::remove(CookRoot / "Game/CookedTexture.dbulk"));
	ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	const Durin::FAssetCatalogRefreshResult MissingBulkRefresh =
		Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation);
	EXPECT_FALSE(MissingBulkRefresh);
	EXPECT_TRUE(std::ranges::any_of(
		MissingBulkRefresh.Errors,
		[](const Durin::FAssetRegistryResult& Result) {
			return Result.Message.find("bulk binding") != std::string::npos;
		}));

	ASSERT_TRUE(AssetRuntime.RestartCooked(CorruptRoot));
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (CorruptRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	const Durin::FAssetResult CorruptBulkLoad =
		Durin::LoadObject(CookedAssetPath, CookedTexture);
	EXPECT_FALSE(CorruptBulkLoad);
	EXPECT_EQ(CookedTexture, nullptr);
	EXPECT_NE(CorruptBulkLoad.Message.find("bulk segment"), std::string::npos)
		<< CorruptBulkLoad.Message;
	ASSERT_TRUE(AssetRuntime.Restore());
}
