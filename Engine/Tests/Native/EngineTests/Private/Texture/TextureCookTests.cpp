#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "CookedAsset.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "RendererModule.h"
#include "RHICommandList.h"
#include "RHIGlobals.h"
#include "RenderingThread.h"
#include "Scene.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshTestAccess.h"
#include "Texture/TextureDerivedData.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DRenderResource.h"

#include <gtest/gtest.h>

namespace
{
	auto WriteNpotTextureFixture(const std::filesystem::path& Path) -> void
	{
		constexpr Durin::uint16 Width = 5;
		constexpr Durin::uint16 Height = 3;
		std::array<Durin::uint8, 18> Header{};
		Header[2] = 2;
		Header[12] = static_cast<Durin::uint8>(Width);
		Header[14] = static_cast<Durin::uint8>(Height);
		Header[16] = 32;
		Header[17] = 0x28;
		std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(Header.data()), Header.size());
		for (Durin::uint16 Y = 0; Y < Height; ++Y)
		{
			for (Durin::uint16 X = 0; X < Width; ++X)
			{
				const Durin::uint8 Value = X == Width - 1 ? 255 : 0;
				const std::array<Durin::uint8, 4> Pixel = {Value, Value, Value, 255};
				Stream.write(reinterpret_cast<const char*>(Pixel.data()), Pixel.size());
			}
		}
	}

	auto ExpectPlatformDataEqual(
		const Durin::FTexturePlatformData& Actual,
		const Durin::FTexturePlatformData& Expected) -> void
	{
		EXPECT_EQ(Actual.PixelFormat, Expected.PixelFormat);
		ASSERT_EQ(Actual.Mips.size(), Expected.Mips.size());
		for (size_t MipIndex = 0; MipIndex < Actual.Mips.size(); ++MipIndex)
		{
			EXPECT_EQ(Actual.Mips[MipIndex].Width, Expected.Mips[MipIndex].Width);
			EXPECT_EQ(Actual.Mips[MipIndex].Height, Expected.Mips[MipIndex].Height);
			EXPECT_EQ(Actual.Mips[MipIndex].RowPitch, Expected.Mips[MipIndex].RowPitch);
			EXPECT_EQ(Actual.Mips[MipIndex].Pixels, Expected.Mips[MipIndex].Pixels);
		}
	}

	struct FScopedDerivedDataCacheRoot
	{
		explicit FScopedDerivedDataCacheRoot(const std::filesystem::path& Root)
			: PreviousRoot(Durin::FPaths::DerivedDataCacheDir())
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedDerivedDataCacheRoot()
		{
			Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousRoot);
		}

		std::string PreviousRoot;
	};

	auto WriteU32(std::vector<Durin::uint8>& Bytes, size_t Offset, Durin::uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (Durin::uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}

	auto WriteU64(std::vector<Durin::uint8>& Bytes, size_t Offset, Durin::uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (Durin::uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<Durin::uint8>(Value >> (Byte * 8));
	}

	auto ContainsText(std::span<const Durin::uint8> Bytes, std::string_view Text) -> bool
	{
		return std::search(
			Bytes.begin(), Bytes.end(),
			reinterpret_cast<const Durin::uint8*>(Text.data()),
			reinterpret_cast<const Durin::uint8*>(Text.data() + Text.size())) != Bytes.end();
	}

	auto SaveVariantPackage(
		Durin::DTexture2D& Texture,
		const std::filesystem::path& PackagePath,
		const Durin::Asset::FCookedPayloadDescriptor& Descriptor) -> void
	{
		auto* DescriptorProperty = Texture.GetClass()->FindPropertyByName("CookedPayload");
		ASSERT_NE(DescriptorProperty, nullptr);
		auto* StoredDescriptor = static_cast<Durin::Asset::FCookedPayloadDescriptor*>(
			DescriptorProperty->GetValuePtr(&Texture));
		const Durin::Asset::FCookedPayloadDescriptor SavedDescriptor = *StoredDescriptor;
		*StoredDescriptor = Descriptor;
		std::vector<Durin::uint8> PackageBytes;
		const Durin::Asset::FAssetResult Result =
			Durin::Asset::SerializeAssetPackageBytes(Texture.GetPackage(), PackageBytes);
		*StoredDescriptor = SavedDescriptor;
		ASSERT_TRUE(Result) << Result.Message;
		ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
			std::as_bytes(std::span(PackageBytes)), PackagePath));
	}

	auto RestartAssetManager() -> void
	{
		Durin::Asset::ShutdownAssetManager();
		Durin::CollectGarbage();
		Durin::Asset::FAssetManager::Get().Initialize();
	}
}

TEST(FTextureCookTests, CookedPackageIsDeterministicAndLoadsWithoutSourceOrDdc)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureCookedConsumer";
	const std::filesystem::path CacheRoot = Root / "DerivedDataCache";
	const std::filesystem::path ContentRoot = Root / "Content";
	const std::filesystem::path CookRoot = std::filesystem::absolute(Root / "Cook");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Root / "CookSecond");
	Durin::Testing::RemoveTestWorkDirectory(Root);
	std::filesystem::create_directories(ContentRoot);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/TextureCookTests/", ContentRoot.generic_string() + "/");
	FScopedDerivedDataCacheRoot ScopedCache(CacheRoot);

	const std::filesystem::path Source = Root / "NpotTexture.tga";
	WriteNpotTextureFixture(Source);
	const Durin::FTexture2DImportResult Import = Durin::DTexture2D::ImportAsset(
		Source.generic_string(), "/TextureCookTests/Texture");
	ASSERT_TRUE(Import) << Import.Message;
	ASSERT_NE(Import.Asset, nullptr);
	ASSERT_NE(Import.Asset->GetPlatformData(), nullptr);
	const Durin::FTexturePlatformData ExpectedPlatformData = *Import.Asset->GetPlatformData();
	const Durin::FTextureDerivedDataDiagnostic DiagnosticBeforeCook =
		Import.Asset->GetDerivedDataDiagnostic();

	std::string Error;
	Durin::Asset::FCookContext First(
		CookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Import.Asset->AddToCook(First, "/Game/CookedTexture", Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;
	EXPECT_EQ(
		Import.Asset->GetDerivedDataDiagnostic().bSourceDecoderInvoked,
		DiagnosticBeforeCook.bSourceDecoderInvoked);

	Durin::Asset::FCookContext Second(
		SecondCookRoot,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game);
	ASSERT_TRUE(Import.Asset->AddToCook(Second, "/Game/CookedTexture", Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;

	std::vector<Durin::uint8> FirstPackage;
	std::vector<Durin::uint8> SecondPackage;
	std::vector<Durin::uint8> FirstBulk;
	std::vector<Durin::uint8> SecondBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (CookRoot / "Game/CookedTexture.dasset").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondCookRoot / "Game/CookedTexture.dasset").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstBulk, (CookRoot / "Game/CookedTexture.dbulk").generic_string()));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondBulk, (SecondCookRoot / "Game/CookedTexture.dbulk").generic_string()));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstBulk, SecondBulk);
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceFile"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceImportData"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceContentHash"));
	EXPECT_FALSE(ContainsText(FirstPackage, "SourceWidth"));

	Durin::Asset::FCookedBulkContainer DecodedBulk;
	ASSERT_TRUE(Durin::Asset::DecodeCookedBulk(
		FirstBulk,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game,
		DecodedBulk,
		&Error)) << Error;
	ASSERT_EQ(DecodedBulk.Entries.size(), 1u);
	ASSERT_EQ(DecodedBulk.Payloads.size(), 1u);
	EXPECT_EQ(DecodedBulk.Entries.front().PayloadId, Durin::Texture2DPrimaryCookedPayloadId);
	std::unique_ptr<Durin::FTexturePlatformData> DecodedPlatformData;
	const Durin::FPayloadDecodeResult DecodeResult = Durin::DecodeTexture2DPayload(
		DecodedBulk.Payloads.front(),
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game,
		DecodedPlatformData);
	ASSERT_TRUE(DecodeResult) << DecodeResult.Message;
	ASSERT_NE(DecodedPlatformData, nullptr);
	ExpectPlatformDataEqual(*DecodedPlatformData, ExpectedPlatformData);
	ASSERT_EQ(DecodedPlatformData->Mips.back().Width, 1u);
	ASSERT_EQ(DecodedPlatformData->Mips.back().Height, 1u);

	const std::filesystem::path WrongProfileRoot =
		std::filesystem::absolute(Root / "CookWrongProfile");
	std::vector<Durin::uint8> WrongProfileBulk = FirstBulk;
	WriteU32(
		WrongProfileBulk,
		12,
		static_cast<Durin::uint32>(Durin::Asset::ECookTargetProfile::EditorValidation));
	std::filesystem::create_directories(WrongProfileRoot / "Game");
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(FirstPackage)),
		WrongProfileRoot / "Game/CookedTexture.dasset"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(WrongProfileBulk)),
		WrongProfileRoot / "Game/CookedTexture.dbulk"));

	const std::filesystem::path UnsupportedFormatRoot =
		std::filesystem::absolute(Root / "CookUnsupportedFormat");
	std::vector<Durin::uint8> UnsupportedFormatPayload = DecodedBulk.Payloads.front();
	WriteU32(UnsupportedFormatPayload, 24, std::numeric_limits<Durin::uint32>::max());
	WriteU64(
		UnsupportedFormatPayload,
		64,
		Durin::FXxHash64::HashBuffer(
			std::span<const Durin::uint8>(UnsupportedFormatPayload)
				.subspan(Durin::TexturePayloadHeaderSize)).HashValue);
	std::vector<Durin::Asset::FCookedBulkPayload> UnsupportedPayloads;
	UnsupportedPayloads.push_back({
		.PayloadId = Durin::Texture2DPrimaryCookedPayloadId,
		.Flags = 1,
		.PayloadSchemaVersion = Durin::TexturePayloadSchemaVersion,
		.Compression = Durin::Asset::ECookedPayloadCompression::None,
		.Alignment = Durin::TexturePayloadAlignment,
		.Bytes = std::move(UnsupportedFormatPayload)});
	std::vector<Durin::uint8> UnsupportedFormatBulk;
	std::vector<Durin::Asset::FCookedPayloadDescriptor> UnsupportedDescriptors;
	ASSERT_TRUE(Durin::Asset::EncodeCookedBulk(
		UnsupportedPayloads,
		Durin::Asset::ECookTargetPlatform::Win64,
		Durin::Asset::ECookTargetProfile::Game,
		UnsupportedFormatBulk,
		&UnsupportedDescriptors,
		&Error)) << Error;
	std::filesystem::create_directories(UnsupportedFormatRoot / "Game");
	SaveVariantPackage(
		*Import.Asset,
		UnsupportedFormatRoot / "Game/CookedTexture.dasset",
		UnsupportedDescriptors.front());
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(UnsupportedFormatBulk)),
		UnsupportedFormatRoot / "Game/CookedTexture.dbulk"));

	const std::filesystem::path CorruptRoot =
		std::filesystem::absolute(Root / "CookCorrupt");
	std::vector<Durin::uint8> CorruptBulk = FirstBulk;
	CorruptBulk.back() ^= 0x80u;
	std::filesystem::create_directories(CorruptRoot / "Game");
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(FirstPackage)),
		CorruptRoot / "Game/CookedTexture.dasset"));
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(
		std::as_bytes(std::span(CorruptBulk)),
		CorruptRoot / "Game/CookedTexture.dbulk"));

	Durin::Testing::RemoveTestWorkDirectory(CacheRoot);
	Durin::Testing::RemoveTestWorkDirectory(Root / "Content" / "Textures");
	RestartAssetManager();
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime, CookRoot}));
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	Durin::RHIInit();
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
	Durin::FAssetPath CookedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedTexture", CookedPath));
	Durin::DTexture2D* CookedTexture = nullptr;
	const Durin::Asset::FAssetResult LoadResult =
		Durin::Asset::LoadAsset(CookedPath, CookedTexture);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(CookedTexture, nullptr);
	ASSERT_NE(CookedTexture->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*CookedTexture->GetPlatformData(), ExpectedPlatformData);
	EXPECT_EQ(
		CookedTexture->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::CookedLoaded);
	EXPECT_FALSE(CookedTexture->GetSourceImportData().HasSource());
	EXPECT_TRUE(CookedTexture->GetSourceContentHash().empty());
	EXPECT_TRUE(CookedTexture->GetDerivedDataKey().empty());
	EXPECT_EQ(
		CookedTexture->GetCookedPayloadDescriptor().PayloadId,
		Durin::Texture2DPrimaryCookedPayloadId);

	Durin::FRHITextureReferenceRef TextureReference =
		CookedTexture->GetTextureReferenceRHI();
	ASSERT_NE(TextureReference, nullptr);
	Durin::FRendererModule Renderer;
	Durin::FScene Scene;
	Renderer.StartupModule();
	auto* SampleMesh = Durin::DStaticMesh::CreateDebugTriangle();
	for (Durin::FVector2f& TexCoord :
		Durin::FStaticMeshTestAccess::GetMutableRenderData(SampleMesh)
			->LODResources.front()
			.VertexBuffers.StaticMeshVertexBuffer
				.TexCoordVertexBuffer.GetMutableTexCoords().front())
	{
		TexCoord = {0.99f, 0.0f};
	}
	auto* SampleMaterial =
		Durin::NewObject<Durin::DMaterial>(nullptr, "CookedTextureSampleMaterial");
	SampleMaterial->SetVectorParameterValue(
		Durin::MaterialParameters::BaseColorName(), Durin::FVector3(1.0));
	SampleMaterial->SetTextureParameterValue(
		Durin::MaterialParameters::BaseColorTextureName(), CookedTexture);
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
		std::vector<Durin::uint8> SamplePixels;
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

			Scene.AddOrReplacePrimitive(
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
			View.Settings.RenderMode = Durin::ERenderMode::Unlit;
			View.ViewProjectionMatrix = Durin::FMatrix(1.0);
			View.ViewLocation = {0.0, 0.0, -1.0};
			View.ViewportWidth = 17;
			View.ViewportHeight = 17;
			(void)Renderer.RenderView(
				CommandList, &Scene, View, Color, false, {});
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
		EXPECT_GT(UploadResult->SamplePixels[Center], 180u);
		EXPECT_GT(UploadResult->SamplePixels[Center + 1], 180u);
		EXPECT_GT(UploadResult->SamplePixels[Center + 2], 180u);
	}

	Scene.Release();
	Durin::MarkAsGarbage(SampleComponent);
	Durin::MarkAsGarbage(SampleMesh);
	Durin::MarkAsGarbage(SampleMaterial);
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(CookedPath));
	CookedTexture = nullptr;
	RestartAssetManager();
	struct FRetireCookedTextureResource
	{
		static constexpr auto GetName() -> const char* { return "RetireCookedTextureResource"; }
	};
	Durin::EnqueueRenderCommand<FRetireCookedTextureResource>(
		[Reference = std::move(TextureReference)](
			Durin::FRHICommandListImmediate&) {});
	Durin::FlushRenderingCommands();
	Renderer.ShutdownModule();
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
	ASSERT_TRUE(std::filesystem::remove(CookRoot / "Game/CookedTexture.dbulk"));
	ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
		Durin::Asset::EPackageLoadMode::CookedRuntime, CookRoot}));
	Durin::PathUtilities::RegisterMountPointForTests(
		"/Game/", (CookRoot / "Game").generic_string() + "/");
	const Durin::Asset::FAssetResult MissingBulk =
		Durin::Asset::LoadAsset(CookedPath, CookedTexture);
	EXPECT_FALSE(MissingBulk);
	EXPECT_EQ(CookedTexture, nullptr);
	EXPECT_NE(MissingBulk.Message.find("Cooked Texture2D"), std::string::npos);

	auto ExpectCookedFailure = [](const std::filesystem::path& FailureRoot,
								   std::string_view ExpectedText) {
		RestartAssetManager();
		ASSERT_TRUE(Durin::Asset::ConfigurePackageLoadContext({
			Durin::Asset::EPackageLoadMode::CookedRuntime, FailureRoot}));
		Durin::PathUtilities::RegisterMountPointForTests(
			"/Game/", (FailureRoot / "Game").generic_string() + "/");
		Durin::FAssetPath Path;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate("/Game/CookedTexture", Path));
		Durin::DTexture2D* Texture = nullptr;
		const Durin::Asset::FAssetResult Result = Durin::Asset::LoadAsset(Path, Texture);
		EXPECT_FALSE(Result);
		EXPECT_EQ(Texture, nullptr);
		EXPECT_NE(Result.Message.find(ExpectedText), std::string::npos) << Result.Message;
	};
	ExpectCookedFailure(WrongProfileRoot, "target");
	ExpectCookedFailure(UnsupportedFormatRoot, "pixel format");
	ExpectCookedFailure(CorruptRoot, "checksum");
	Durin::Asset::ShutdownAssetManager();
	Durin::CollectGarbage();
}
