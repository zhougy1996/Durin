#include "NativeAssetBuildTestSupport.h"
#include "RenderResource.h"
#include "Materials/MaterialShaderMap.h"

#include "AssetForge/Builtins/PBRMaterialParameters.h"
#include "../Materials/ExplicitMaterialProgramTestFixture.h"
#include "../Materials/MaterialCookedProgramTestSupport.h"
#include "Threading/Task.h"
#include "Threading/ThreadEvent.h"
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
#include "DObject/StrongObjectPtr.h"
#include "DObject/Package.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Texture/TextureFactoryTestSupport.h"
#include "Hash/XxHash.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
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
#include "RenderResourcePreparation.h"
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
#include <vulkan/vulkan.hpp>
#include <latch>
#include "VulkanRHIPrivate.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/VolumeTextureRenderResource.h"

#include "NativeDObjectTestSupport.h"

namespace Durin
{
	struct FTextureCookTestAccess
	{
		static auto GetCookedLoad(const DTexture& Texture) { return Texture.CookedLoad; }
		static auto CopyCookedLoad(const DTexture& Texture) -> FPackageResourceRequest
		{
			check(Texture.CookedLoad);
			return *Texture.CookedLoad;
		}
	};
}

namespace
{
	auto WriteNpotTextureFixture(const std::filesystem::path& Path) -> void
	{
		// Keep the compressed mip chain above the external-bulk threshold so
		// cooked loading can validate missing and corrupt sidecars as well as NPOT edges.
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

	auto WriteU32(Durin::FByteBuffer& Bytes, size_t Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteU64(Durin::FByteBuffer& Bytes, size_t Offset, uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto ReadU64(const Durin::FByteBuffer& Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto RefreshEnvelopeHeaderHash(Durin::FByteBuffer& Bytes) -> void
	{
		const uint64 HeaderBytes = ReadU64(Bytes, 32);
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Bytes).first(static_cast<size_t>(HeaderBytes)));
		WriteU64(Bytes, 48, Hash.HashLow);
		WriteU64(Bytes, 56, Hash.HashHigh);
	}

	auto ContainsText(Durin::FByteView Bytes, std::string_view Text) -> bool
	{
		const Durin::FByteView TextBytes =
			std::as_bytes(std::span{Text.data(), Text.size()});
		return std::search(Bytes.begin(), Bytes.end(),
			TextBytes.begin(), TextBytes.end()) != Bytes.end();
	}

	class FTextureCookTests : public testing::Test
	{
	protected:
		static auto SetUpTestSuite() -> void
		{
			InitializeDObjectSystem();
			if (!Durin::IsTaskSchedulerRunning())
				ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
			Durin::FModuleManager::Get().LoadModuleChecked("TextureCompressor");
			ASSERT_TRUE(Durin::InitializeAssetCompilingManager());
		}

		static auto TearDownTestSuite() -> void
		{
			// Retire compilers before the resource-update suite closes task admission.
			Durin::ShutdownAssetCompilingManager();
		}
	};

}

TEST_F(FTextureCookTests, ColdCookRebuildsFromAuthoredPixelsWithoutSourceOrDdc)
{
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
	Durin::Testing::RemoveTestWorkDirectory(CacheRoot / "records" / "v5" / "Texture2D");

	Durin::DTexture2D* Loaded = nullptr;
	const auto Load =
		Durin::LoadObject<Durin::DTexture2D>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath));
		Loaded = Load.value_or(nullptr);
	ASSERT_TRUE(Load) << (Load ? std::string{} : Load.error().Message);
	ASSERT_NE(Loaded, nullptr);
	ASSERT_TRUE(Loaded->FinishCachePlatformData());
	EXPECT_TRUE(Loaded->HasPlatformData());
	Durin::FCookContext Cook(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::SaveCookPackage((*Loaded).GetPackage(), "/Game/ColdTexture", Cook)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Cook, CookRoot)) << Error;
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Game/ColdTexture.dasset"));
	EXPECT_TRUE(std::filesystem::is_regular_file(
		CookRoot / "Game/ColdTexture.dbulk"));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST_F(FTextureCookTests, CookedPackageIsDeterministicAndLoadsWithoutSourceOrDdc)
{
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
		Import.Asset->GetSource().GetIdentity();
	ASSERT_NE(Import.Asset->GetAssetImportData(), nullptr);
	const Durin::FSourceFile* ImportedSource =
		Import.Asset->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	const Durin::FSourceFile SourceBeforeCook =
		*ImportedSource;
	const bool bPackageDirtyBeforeCook = Import.Asset->GetPackage()->IsDirty();

	// Runtime sampling uses a cooked material; only its dynamic texture value is bound later.
	Durin::FPackagePath SourceMaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureCookTests/SampleMaterial", SourceMaterialPath));
	auto* SourceMaterialPackage = Durin::CreatePackage(SourceMaterialPath);
	ASSERT_NE(SourceMaterialPackage, nullptr);
	auto* SourceMaterial = Durin::NewObject<Durin::DMaterial>(SourceMaterialPackage, "SampleMaterial");
	ASSERT_TRUE(Durin::Testing::MakePBRMaterialExpressionsForTest().Apply(*SourceMaterial));
	ASSERT_TRUE(SourceMaterial->SetVectorParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorName(), Durin::FVector3(1.0)));
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*SourceMaterial);
	ASSERT_TRUE(SourceMaterial->GetMaterialCompileStatus().IsCurrent());
	const auto ExpectedMaterialIdentity = Durin::Testing::ExpectedGameMaterialIdentity(*SourceMaterial);

	Durin::FCookContext First(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::SaveCookPackage((*Import.Asset).GetPackage(), "/Game/CookedTexture", First)) << Error;
	ASSERT_TRUE(Durin::SaveCookPackage((*SourceMaterial).GetPackage(), "/Game/SampleMaterial", First)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(First, CookRoot)) << Error;
	EXPECT_EQ(Import.Asset->GetSource().GetIdentity(), SourceIdentityBeforeCook);

	Durin::FCookContext Second(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::SaveCookPackage((*Import.Asset).GetPackage(), "/Game/CookedTexture", Second)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Second, SecondCookRoot)) << Error;

	Durin::FCookContext Diagnostic(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game,
		true);
	ASSERT_TRUE(Durin::SaveCookPackage((*Import.Asset).GetPackage(), "/Game/CookedTexture", Diagnostic)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Diagnostic, DiagnosticCookRoot)) << Error;
	ASSERT_NE(Import.Asset->GetAssetImportData(), nullptr);
	ImportedSource = Import.Asset->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(*ImportedSource, SourceBeforeCook);
	EXPECT_EQ(Import.Asset->GetPackage()->IsDirty(), bPackageDirtyBeforeCook);

	Durin::FByteBuffer FirstPackage;
	Durin::FByteBuffer SecondPackage;
	Durin::FByteBuffer FirstBulk;
	Durin::FByteBuffer SecondBulk;
	Durin::FByteBuffer DiagnosticPackage;
	auto FirstPackageRead = Durin::FFileHelper::LoadFileToArray((CookRoot / "Game/CookedTexture.dasset"));
	ASSERT_TRUE(FirstPackageRead) << FirstPackageRead.error().ToString();
	FirstPackage = std::move(*FirstPackageRead);
	auto SecondPackageRead = Durin::FFileHelper::LoadFileToArray((SecondCookRoot / "Game/CookedTexture.dasset"));
	ASSERT_TRUE(SecondPackageRead) << SecondPackageRead.error().ToString();
	SecondPackage = std::move(*SecondPackageRead);
	auto FirstBulkRead = Durin::FFileHelper::LoadFileToArray((CookRoot / "Game/CookedTexture.dbulk"));
	ASSERT_TRUE(FirstBulkRead) << FirstBulkRead.error().ToString();
	FirstBulk = std::move(*FirstBulkRead);
	auto SecondBulkRead = Durin::FFileHelper::LoadFileToArray((SecondCookRoot / "Game/CookedTexture.dbulk"));
	ASSERT_TRUE(SecondBulkRead) << SecondBulkRead.error().ToString();
	SecondBulk = std::move(*SecondBulkRead);
	auto DiagnosticPackageRead = Durin::FFileHelper::LoadFileToArray((DiagnosticCookRoot / "Game/CookedTexture.dasset"));
	ASSERT_TRUE(DiagnosticPackageRead) << DiagnosticPackageRead.error().ToString();
	DiagnosticPackage = std::move(*DiagnosticPackageRead);
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
		FirstBulk, Durin::EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
	DecodedPlatformData.Serialize(PayloadAr);
	ASSERT_FALSE(PayloadAr.IsError()) << PayloadAr.GetError();
	ASSERT_TRUE(Durin::RequireArchiveEnd(PayloadAr));
	ExpectPlatformDataEqual(DecodedPlatformData, ExpectedPlatformData);
	ASSERT_EQ(DecodedPlatformData.Mips.back().Width, 1u);
	ASSERT_EQ(DecodedPlatformData.Mips.back().Height, 1u);

	const std::filesystem::path CorruptRoot =
		std::filesystem::absolute(Root / "CookCorrupt");
	std::filesystem::copy(CookRoot, CorruptRoot,
		std::filesystem::copy_options::recursive);
	Durin::FByteBuffer CorruptBulk = FirstBulk;
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
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
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
	const auto LoadResult =
		Durin::LoadObject<Durin::DTexture2D>(CookedAssetPath);
		CookedTexture = LoadResult.value_or(nullptr);
	ASSERT_TRUE(LoadResult) << (LoadResult ? std::string{} : LoadResult.error().Message);
	ASSERT_NE(CookedTexture, nullptr);
	const auto BulkStateBeforeGet = CookedTexture->GetCookedPlatformData().GetState();
	const auto PlatformBeforeGetIdentity = CookedTexture->GetPlatformDataShared();
	const Durin::DTexture2D& ConstTexture = *CookedTexture;
	EXPECT_EQ(ConstTexture.GetPlatformData(), nullptr);
	EXPECT_FALSE(CookedTexture->HasPlatformData());
	EXPECT_EQ(CookedTexture->GetCookedPlatformData().GetState(), BulkStateBeforeGet);
	EXPECT_EQ(CookedTexture->GetPlatformDataShared(), PlatformBeforeGetIdentity);
	const auto CacheDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!CookedTexture->IsAsyncCacheComplete() && std::chrono::steady_clock::now() < CacheDeadline)
	{
		Durin::PumpGameThreadDeferredWork();
		std::this_thread::yield();
	}
	ASSERT_TRUE(CookedTexture->HasPlatformData()); // PostLoad finishes without MapChange or a blocking getter.
	ASSERT_TRUE(static_cast<Durin::DTexture&>(*CookedTexture).FinishCachePlatformData());
	ASSERT_NE(CookedTexture->GetPlatformData(), nullptr);
	const auto* InstalledPlatform = CookedTexture->GetPlatformData();
	const auto InstalledPlatformDataIdentity = CookedTexture->GetPlatformDataShared();
	ASSERT_TRUE(static_cast<Durin::DTexture&>(*CookedTexture).FinishCachePlatformData());
	EXPECT_EQ(CookedTexture->GetPlatformData(), InstalledPlatform);
	EXPECT_EQ(CookedTexture->GetPlatformDataShared(), InstalledPlatformDataIdentity);
	auto* MissingPlatform = Durin::NewObject<Durin::DTexture2D>(
		nullptr, "MissingCookedPlatformData");
	MissingPlatform->CachePlatformData();
	EXPECT_FALSE(MissingPlatform->FinishCachePlatformData());
	EXPECT_EQ(MissingPlatform->GetResource(), nullptr);
	EXPECT_EQ(MissingPlatform->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*CookedTexture->GetPlatformData(), ExpectedPlatformData);
	EXPECT_EQ(CookedTexture->GetAssetImportData(), nullptr);
	EXPECT_NE(CookedTexture->GetCookedPlatformData().GetMetadata().LogicalSize, 0u);
	// Complete decode without pumping publication, then invalidate its owner. A
	// finished old request must not install CPU data or resurrect a GPU update.
	Durin::TStrongObjectPtr<Durin::DTexture2D> Invalidated(
		Durin::NewObject<Durin::DTexture2D>(nullptr, "InvalidatedCookedPublication"));
	const_cast<Durin::FBulkData&>(Invalidated->GetCookedPlatformData()) = CookedTexture->GetCookedPlatformData();
	Invalidated->PostLoad();
	const auto PendingIdentity = Durin::FTextureCookTestAccess::GetCookedLoad(*Invalidated);
	ASSERT_TRUE(PendingIdentity);
	EXPECT_FALSE(Invalidated->FinishCachePlatformData(0.0));
	EXPECT_FALSE(Invalidated->FinishCachePlatformData(1e-12));
	EXPECT_FALSE(Invalidated->FinishCachePlatformData(-1.0));
	EXPECT_FALSE(Invalidated->FinishCachePlatformData(std::numeric_limits<double>::quiet_NaN()));
	EXPECT_FALSE(Invalidated->FinishCachePlatformData(std::numeric_limits<double>::infinity()));
	EXPECT_EQ(Durin::FTextureCookTestAccess::GetCookedLoad(*Invalidated), PendingIdentity);
	Invalidated->ReleaseResource();
	EXPECT_EQ(Durin::FTextureCookTestAccess::GetCookedLoad(*Invalidated), PendingIdentity);
	EXPECT_FALSE(Invalidated->IsAsyncCacheComplete());
	EXPECT_FALSE(Invalidated->HasPlatformData());
	EXPECT_FALSE(Invalidated->HasPendingRenderResourceInitialization());
	const auto OldRequest = Durin::FTextureCookTestAccess::CopyCookedLoad(*Invalidated);
	const auto DecodeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!OldRequest.IsReady() && std::chrono::steady_clock::now() < DecodeDeadline) std::this_thread::yield();
	ASSERT_TRUE(OldRequest.IsReady());
	ASSERT_FALSE(Invalidated->HasPlatformData());
	Invalidated->SetPlatformData(nullptr); // CPU-product mutation retires the old preparation.
	for (uint32 Frame = 0; Frame < 4; ++Frame) Durin::PumpGameThreadDeferredWork();
	EXPECT_FALSE(Invalidated->HasPlatformData());
	EXPECT_FALSE(Invalidated->HasPendingRenderResourceInitialization());
	EXPECT_EQ(Invalidated->GetResource(), nullptr);
	Invalidated->BeginCachePlatformData();
	const auto RetriedRequest = Durin::FTextureCookTestAccess::GetCookedLoad(*Invalidated);
	ASSERT_TRUE(RetriedRequest);
	EXPECT_FALSE(Invalidated->FinishCachePlatformData(0.0));
	// Explicit completion installs only this request, without pumping queued work.
	Durin::Tasks::FTaskEvent QueuedWork("TestTextureWaitIsolation");
	bool UnrelatedCallbackRan = false;
	auto Unrelated = Durin::Tasks::LaunchTask(Durin::Tasks::ETaskExecutor::GameThreadDeferred,
		{.DebugName = "TestTextureWaitIsolation"}, [&] { UnrelatedCallbackRan = true; }, QueuedWork);
	QueuedWork.Trigger();
	ASSERT_TRUE(Invalidated->FinishCachePlatformData(10.0));
	EXPECT_FALSE(UnrelatedCallbackRan);
	EXPECT_TRUE(RetriedRequest->IsReady());
	ASSERT_TRUE(Invalidated->HasPlatformData());
	const auto InstalledRetry = Invalidated->GetPlatformDataShared();
	EXPECT_TRUE(Invalidated->FinishCachePlatformData(0.0));
	EXPECT_EQ(Invalidated->GetPlatformDataShared(), InstalledRetry);
	Durin::PumpGameThreadDeferredWork();
	EXPECT_TRUE(UnrelatedCallbackRan);
	EXPECT_EQ(Invalidated->GetPlatformDataShared(), InstalledRetry); // Late wakeup does not install twice.
	auto* Retired = Invalidated.Get(); Invalidated.Reset(); Durin::MarkAsGarbage(Retired); Durin::CollectGarbage();

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
	Durin::FPackagePath SampleMaterialPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/SampleMaterial", SampleMaterialPath));
	Durin::DMaterial* CookedMaterial = nullptr;
	const auto MaterialLoad = Durin::LoadObject<Durin::DMaterial>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(SampleMaterialPath));
	CookedMaterial = MaterialLoad.value_or(nullptr);
	ASSERT_TRUE(MaterialLoad) << (MaterialLoad ? std::string{} : MaterialLoad.error().Message);
	ASSERT_NE(CookedMaterial, nullptr);
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*CookedMaterial);
	ASSERT_TRUE(CookedMaterial->GetAcceptedCompiledProgram());
	EXPECT_EQ(CookedMaterial->GetAcceptedCompiledProgram()->GetShaderMap().ShaderMapId, ExpectedMaterialIdentity);
	ASSERT_TRUE(CookedMaterial->GetMaterialCompileStatus().IsCurrent());
	auto* SampleMaterial = Durin::NewObject<Durin::DMaterialInstance>(nullptr, "CookedTextureSampleInstance");
	ASSERT_TRUE(SampleMaterial->SetParent(CookedMaterial));
	EXPECT_EQ(SampleMaterial->GetAcceptedCompiledProgram(), CookedMaterial->GetAcceptedCompiledProgram());
	const auto MaterialGeneration = SampleMaterial->GetMaterialCompileStatus().RequestGeneration;
	ASSERT_TRUE(SampleMaterial->SetTextureParameterValue(
		Durin::AssetForge::Builtins::MaterialParameters::BaseColorTextureName(), CookedTexture));
	EXPECT_EQ(SampleMaterial->GetMaterialCompileStatus().RequestGeneration, MaterialGeneration);
	EXPECT_FALSE(Durin::RequestMaterialRecompile(*SampleMaterial));
	EXPECT_FALSE(CookedMaterial->GetPackage()->IsDirty());

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
		Durin::FByteBuffer SamplePixels;
	};
	auto UploadResult = std::make_shared<FTextureCookUploadResult>();
	struct FValidateCookedTextureUpload
	{
		static constexpr auto GetName() -> const char* { return "ValidateCookedTextureUpload"; }
	};
	const auto Preparation = Durin::FRenderResourcePreparation::Create();
	bool Pending = false;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	do
	{
	Durin::EnqueueRenderCommand<FValidateCookedTextureUpload>(
		[&Renderer, &Scene, TextureReference, ExpectedPlatformData, UploadResult, SampleProxy, Preparation, &Pending](
			Durin::FRHICommandListImmediate& CommandList) {
			++Durin::GRenderFrameCounterRenderThread;
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

			if (*SampleProxy)
			Durin::FSceneInterfaceTestAccess::ReplacePrimitiveProxy(Scene,
				Durin::FPrimitiveComponentId(1), std::move(*SampleProxy), Durin::FMatrix(1.0));
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
			Renderer.BeginFrame_RenderThread(CommandList);
			Renderer.PrepareViewResources(CommandList, {&Scene, View, Color, false, {}}, Preparation);
			Pending = Preparation.GetProgress().State == Durin::ERenderResourcePreparationState::Pending;
			if (Pending) return;
			if (Renderer.RenderView(CommandList, {&Scene, View, Color, false, {}})
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
		if (!Pending) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	} while (std::chrono::steady_clock::now() < Deadline);
	if (Pending) { UploadResult->bSucceeded=false; UploadResult->Error="Cooked texture view preparation timed out."; }
	EXPECT_TRUE(UploadResult->bSucceeded) << UploadResult->Error;
	if (UploadResult->bSucceeded)
	{
		ASSERT_EQ(UploadResult->SamplePixels.size(), 17u * 17u * 4u);
		const size_t Center = (8u * 17u + 8u) * 4u;
		EXPECT_GT(std::to_integer<uint8>(UploadResult->SamplePixels[Center]), 180u);
		EXPECT_GT(std::to_integer<uint8>(UploadResult->SamplePixels[Center + 1]), 180u);
		EXPECT_GT(std::to_integer<uint8>(UploadResult->SamplePixels[Center + 2]), 180u);
	}

	// A CPU decode failure must not invalidate an already published allocation.
	{
	ASSERT_TRUE(CookedTexture->FinishCachePlatformData());
	ASSERT_FALSE(CookedTexture->HasPendingRenderResourceInitialization());
	const auto PublishedBeforeFailure = CookedTexture->GetResource()->GetTextureRHI_GameThread();
	ASSERT_TRUE(PublishedBeforeFailure);
	const std::array InvalidBytes{std::byte{0}};
	auto InvalidCookedData = Durin::FBulkData::TryCreateDetached(InvalidBytes);
	ASSERT_TRUE(InvalidCookedData);
	const_cast<Durin::FBulkData&>(CookedTexture->GetCookedPlatformData()) = std::move(*InvalidCookedData);
	CookedTexture->PostLoad();
	EXPECT_FALSE(CookedTexture->FinishCachePlatformData(10.0));
	EXPECT_TRUE(CookedTexture->IsAsyncCacheComplete());

	EXPECT_FALSE(CookedTexture->HasPlatformData());
	EXPECT_EQ(CookedTexture->GetResource()->GetTextureRHI_GameThread(), PublishedBeforeFailure);
	Durin::FlushRenderingCommands();
	EXPECT_EQ(CookedTexture->GetResource()->GetTextureRHI_GameThread(), PublishedBeforeFailure);
	}

	Durin::FSceneInterfaceTestAccess::ReleaseScene(SceneOwner);
	Durin::FlushRenderingCommands();
	Durin::MarkAsGarbage(SampleComponent);
	Durin::MarkAsGarbage(SampleMesh);
	Durin::MarkAsGarbage(SampleMaterial);
	Durin::FlushRenderingCommands();
	Durin::CollectGarbage();
	ASSERT_TRUE(Durin::UnloadPackage(SampleMaterialPath));
	CookedMaterial = nullptr;
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
			return Result.Context.ReaderCause
				&& Result.Context.ReaderCause->Reason == Durin::ObjectPackage::EPackageReaderReason::RegistryBulk;
		}));

	ASSERT_TRUE(AssetRuntime.RestartCooked(CorruptRoot));
	Durin::Testing::RegisterMountPointForTests(
		"/Game/", (CorruptRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));
	const auto CorruptBulkLoad =
		Durin::LoadObject<Durin::DTexture2D>(CookedAssetPath);
		CookedTexture = CorruptBulkLoad.value_or(nullptr);
	EXPECT_FALSE(CorruptBulkLoad);
	EXPECT_EQ(CookedTexture, nullptr);
	if (!CorruptBulkLoad)
	{
		EXPECT_EQ(CorruptBulkLoad.error().Code, Durin::EAssetReadError::CorruptFile);
		EXPECT_FALSE(CorruptBulkLoad.error().Message.empty());
	}
	ASSERT_TRUE(AssetRuntime.Restore());
}

namespace
{
	auto MakeUpdatePlatform2D(std::byte Value) -> Durin::FTexturePlatformData
	{
		Durin::FTexturePlatformData Data;
		Data.PixelFormat = Durin::EPixelFormat::RGBA8_UNORM;
		auto& Mip = Data.Mips.emplace_back();
		Mip.Width = Mip.Height = 1;
		Mip.RowPitch = 4;
		Mip.Pixels = Durin::FSharedByteBuffer::Take(Durin::FByteBuffer(4, Value));
		return Data;
	}

	template<class TTexture>
	auto InstallUpdateInput(TTexture& Texture, std::byte Value) -> void
	{
		if constexpr (std::is_same_v<TTexture, Durin::DTexture2D>)
			Texture.SetPlatformData(std::make_unique<Durin::FTexturePlatformData>(MakeUpdatePlatform2D(Value)));
		else if constexpr (std::is_same_v<TTexture, Durin::DTextureCube>)
		{
			auto Data = std::make_unique<Durin::FTextureCubePlatformData>();
			Data->PixelFormat = Durin::EPixelFormat::RGBA8_UNORM;
			for (auto& Face : Data->Faces) Face = MakeUpdatePlatform2D(Value);
			Texture.SetPlatformData(std::move(Data));
		}
		else
		{
			auto Data = std::make_unique<Durin::FVolumeTexturePlatformData>();
			Data->PixelFormat = Durin::EPixelFormat::RGBA8_UNORM;
			auto& Mip = Data->Mips.emplace_back();
			Mip.Width = Mip.Height = Mip.Depth = 1;
			Mip.RowPitch = Mip.DepthPitch = 4;
			Mip.Voxels = Durin::FSharedByteBuffer::Take(Durin::FByteBuffer(4, Value));
			Texture.SetPlatformData(std::move(Data));
		}
	}


	template<class TTexture>
	auto CheckTextureFamilyUpdateLifecycle() -> void
	{
		auto* Texture = Durin::NewObject<TTexture>(nullptr, "OwnedTextureVulkanLifecycle");
		Durin::AddToRoot(Texture);
		InstallUpdateInput(*Texture, std::byte{0x11});
		ASSERT_TRUE(Texture->HasPlatformData());
		// All families release their private upload copy while retaining the initialized resource.
		using FPlatformData = std::remove_cvref_t<decltype(*Texture->GetPlatformData())>;
		auto UploadInput = std::make_shared<const FPlatformData>(*Texture->GetPlatformData());
		std::weak_ptr<const FPlatformData> RetainedInput = UploadInput;
		Durin::FTextureReference UploadReference;
		std::unique_ptr<Durin::FTextureResource> UploadedResource;
		if constexpr (std::is_same_v<TTexture, Durin::DTexture2D>)
			UploadedResource = std::make_unique<Durin::FTexture2DResource>(&UploadReference, UploadInput);
		else if constexpr (std::is_same_v<TTexture, Durin::DTextureCube>)
			UploadedResource = std::make_unique<Durin::FTextureCubeResource>(&UploadReference, UploadInput);
		else
			UploadedResource = std::make_unique<Durin::FVolumeTextureResource>(&UploadReference, UploadInput);
		UploadInput.reset();
		EXPECT_FALSE(RetainedInput.expired());
		UploadedResource->BeginInit_GameThread();
		Durin::FlushRenderingCommands();
		EXPECT_TRUE(RetainedInput.expired());
		EXPECT_TRUE(UploadedResource->IsInitialized());
		EXPECT_NE(UploadedResource->GetTextureRHI_GameThread(), nullptr);
		UploadedResource->BeginRelease_GameThread();
		Durin::BeginCleanupRenderResource(Durin::FDeferredRenderResourceCleanup(std::move(UploadedResource)));
		Durin::FlushRenderingCommands();
		const auto StableReference = Texture->GetTextureReferenceRHI();
		Texture->UpdateResource();
		Durin::FlushRenderingCommands();
		ASSERT_NE(Texture->GetResource(), nullptr);
		ASSERT_TRUE(Texture->GetResource()->GetTextureRHI_GameThread());
		const auto CachedPlatformData = Texture->GetPlatformDataShared();
		Texture->ReleaseResource();
		EXPECT_EQ(Texture->GetResource(), nullptr);
		EXPECT_EQ(Texture->GetPlatformDataShared(), CachedPlatformData);
		EXPECT_EQ(Texture->GetTextureReferenceRHI(), StableReference);
		Durin::FlushRenderingCommands();
		EXPECT_EQ(Texture->GetResource(), nullptr);
		Texture->UpdateResource();
		Durin::FlushRenderingCommands();
		ASSERT_NE(Texture->GetResource(), nullptr);
		ASSERT_TRUE(Texture->GetResource()->GetTextureRHI_GameThread());
		EXPECT_EQ(Texture->GetPlatformDataShared(), CachedPlatformData);
		const auto OldSnapshot = Texture->GetResource()->GetTextureRHI_GameThread();
		ASSERT_TRUE(OldSnapshot);
		std::latch Blocked(1), Resume(1);
		Durin::EnqueueRenderCommand("HoldTextureAdmission", [&](Durin::FRHICommandListImmediate&) {
			Blocked.count_down(); Resume.wait();
		});
		Blocked.wait();
		Texture->UpdateResource(); // Accepted uploads execute and retire in queue order.
		InstallUpdateInput(*Texture, std::byte{0x22});
		Texture->UpdateResource();
		InstallUpdateInput(*Texture, std::byte{0x33});
		Texture->UpdateResource(); // C is the final allocation.
		EXPECT_TRUE(Texture->HasPendingRenderResourceInitialization());
		EXPECT_NE(Texture->GetResource(), nullptr); // RHI is not readable before initialization completes.
		Resume.count_down();
		Durin::FlushRenderingCommands();
		EXPECT_FALSE(Texture->HasPendingRenderResourceInitialization());
		ASSERT_NE(Texture->GetResource(), nullptr);
		ASSERT_TRUE(Texture->GetResource()->GetTextureRHI_GameThread());
		const auto CompletedAllocation = Texture->GetResource()->GetTextureRHI_GameThread();
		Durin::PumpGameThreadDeferredWork({.MaxCallbacks = 1});
		EXPECT_EQ(Texture->GetResource()->GetTextureRHI_GameThread(), CompletedAllocation);
		EXPECT_EQ(Texture->GetTextureReferenceRHI(), StableReference);
		EXPECT_NE(Texture->GetResource(), nullptr);
		if constexpr (std::is_same_v<TTexture, Durin::DTextureCube>)
		{
			Durin::FByteBuffer Earlier, Latest;
			const auto Current = Texture->GetResource()->GetTextureRHI_GameThread();
			Durin::EnqueueRenderCommand("ReadOwnedTextureSnapshots", [OldSnapshot, Current, &Earlier, &Latest](Durin::FRHICommandListImmediate& Commands) {
				EXPECT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(Commands, OldSnapshot, 0, 0, Earlier));
				EXPECT_TRUE(Durin::GDynamicRHI->RHIReadTexture2D(Commands, Current, 0, 0, Latest));
			});
			Durin::FlushRenderingCommands();
			EXPECT_EQ(Earlier, Durin::FByteBuffer(4, std::byte{0x11}));
			EXPECT_EQ(Latest, Durin::FByteBuffer(4, std::byte{0x33}));
		}
		// Generic render-fence completion waits for queued updates
		// without executing unrelated GameThread callbacks.
		Durin::Tasks::FTaskEvent Gate("TextureResourceJoinIsolation");
		auto CallbackRan = std::make_shared<std::atomic_bool>(false);
		auto Unrelated = Durin::Tasks::LaunchTask(Durin::Tasks::ETaskExecutor::GameThreadDeferred,
			{.DebugName = "TextureResourceJoinIsolation"}, [CallbackRan] { *CallbackRan = true; }, Gate);
		Gate.Trigger();
		Durin::FRenderCommandFence ResourceFence;
		Texture->UpdateResource();
		InstallUpdateInput(*Texture, std::byte{0x55});
		Texture->UpdateResource();
		ASSERT_TRUE(Texture->FinishCachePlatformData());
		ResourceFence.BeginFence();
		ResourceFence.Wait();
		EXPECT_FALSE(*CallbackRan);
		EXPECT_FALSE(Texture->HasPendingRenderResourceInitialization());
		Texture->UpdateResource();
		ASSERT_TRUE(Texture->FinishCachePlatformData());
		ResourceFence.BeginFence();
		ResourceFence.Wait();
		const auto JoinedAllocation = Texture->GetResource()->GetTextureRHI_GameThread();
		ASSERT_TRUE(JoinedAllocation);
		Durin::PumpGameThreadDeferredWork();
		EXPECT_TRUE(*CallbackRan);
		EXPECT_EQ(Texture->GetResource()->GetTextureRHI_GameThread(), JoinedAllocation); // Deferred callbacks cannot alter the completed allocation.
		EXPECT_FALSE(Texture->HasPendingRenderResourceInitialization());
		// Direct source mutation invalidates both CPU and GPU data across every family.
		Texture->SetSource({});
		EXPECT_FALSE(Texture->HasPlatformData());
		EXPECT_EQ(Texture->GetResource(), nullptr);
		Texture->UpdateResource();
		EXPECT_EQ(Texture->GetResource(), nullptr);
		Durin::FlushRenderingCommands();
		Durin::EnqueueRenderCommand("CheckInvalidSourceFallback", [StableReference](Durin::FRHICommandListImmediate&) {
			EXPECT_EQ(StableReference->GetReferencedTexture_RenderThread(), nullptr);
		});
		Durin::FlushRenderingCommands();
		InstallUpdateInput(*Texture, std::byte{0x44});
		// Destruction retires the owned resource and its stable reference in order.
		Texture->UpdateResource();
		Durin::FlushRenderingCommands();
		Durin::RemoveFromRoot(Texture);
		Durin::MarkAsGarbage(Texture);
		Durin::CollectGarbage();
		Durin::FlushRenderingCommands();
		Durin::PumpGameThreadDeferredWork();
	}
}

TEST(FTextureResourceLifetimeIntegrationTests, OrderedUpdatesPublishAndRetireAcrossAllFamiliesWithoutHandoffs)
{
	InitializeDObjectSystem();
	ASSERT_TRUE(Durin::InitializeTaskScheduler());
	Durin::FModuleManager::Get().LoadModule("RenderCore");
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	Durin::RHIInit(Durin::Tests::GetVulkanEngineTestInitializationContext());
	ASSERT_NE(Durin::GDynamicRHI, nullptr);
	Durin::InitRenderingThread();
	Durin::EnqueueRenderCommand("BeginOwnedTextureFrame", [](Durin::FRHICommandListImmediate& Commands) {
		Commands.SwitchPipeline(Durin::ERHIPipeline::Graphics);
		Durin::GDynamicRHI->RHIBeginFrame_RenderThread(Commands);
	});
	CheckTextureFamilyUpdateLifecycle<Durin::DTexture2D>();
	CheckTextureFamilyUpdateLifecycle<Durin::DTextureCube>();
	CheckTextureFamilyUpdateLifecycle<Durin::DVolumeTexture>();
	// A queued upload retains everything it needs after the asset is destroyed.
	auto* Closing = Durin::NewObject<Durin::DTexture2D>(nullptr, "ClosingQueuedTexture");
	InstallUpdateInput(*Closing, std::byte{0x77});
	std::latch Entered(1), Resume(1);
	Durin::FThreadEvent AssetClosed;
	Durin::EnqueueRenderCommand("BlockTextureDestruction", [&](Durin::FRHICommandListImmediate&) {
		Entered.count_down(); Resume.wait();
	});
	Entered.wait();
	bool bClosedWithoutRenderProgress = false;
	std::thread Unblock([&] {
		// Fail safely instead of hanging if destruction regresses to joining render work.
		bClosedWithoutRenderProgress = AssetClosed.WaitFor(5.0);
		Resume.count_down();
	});
	Closing->UpdateResource();
	Durin::MarkAsGarbage(Closing);
	Durin::CollectGarbage();
	AssetClosed.Trigger();
	Unblock.join();
	EXPECT_TRUE(bClosedWithoutRenderProgress);
	Durin::FlushRenderingCommands();
	Durin::PumpGameThreadDeferredWork();
	// GPU updates and retirement remain operational after task admission closes.
	auto* Draining = Durin::NewObject<Durin::DTexture2D>(nullptr, "DrainingTexture");
	Durin::AddToRoot(Draining);
	InstallUpdateInput(*Draining, std::byte{0x55});
	Draining->UpdateResource();
	Draining->UpdateResource();
	Durin::FlushRenderingCommands();
	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
	EXPECT_FALSE(Draining->HasPendingRenderResourceInitialization());
	ASSERT_NE(Draining->GetResource(), nullptr);
	ASSERT_TRUE(Draining->GetResource()->GetTextureRHI_GameThread());
	{
		const auto BeforeOfflineUpdate = Draining->GetResource()->GetTextureRHI_GameThread();
		InstallUpdateInput(*Draining, std::byte{0x56});
		Draining->UpdateResource();
		Durin::FlushRenderingCommands();
		EXPECT_NE(Draining->GetResource(), nullptr);
		EXPECT_NE(Draining->GetResource()->GetTextureRHI_GameThread(), BeforeOfflineUpdate);
	}
	Durin::RemoveFromRoot(Draining);
	Durin::MarkAsGarbage(Draining);
	Durin::CollectGarbage();
	Durin::FlushRenderingCommands();

	ASSERT_TRUE(Durin::InitializeTaskScheduler());
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	auto* Canceled = Durin::NewObject<Durin::DTexture2D>(nullptr, "TextureAfterTaskCancellation");
	Durin::AddToRoot(Canceled);
	InstallUpdateInput(*Canceled, std::byte{0x66});
	Canceled->UpdateResource();
	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Cancel);
	Durin::FlushRenderingCommands();
	ASSERT_NE(Canceled->GetResource(), nullptr);
	ASSERT_TRUE(Canceled->GetResource()->GetTextureRHI_GameThread());
	Canceled->UpdateResource();
	Durin::FlushRenderingCommands();
	EXPECT_NE(Canceled->GetResource(), nullptr);
	Durin::RemoveFromRoot(Canceled);
	Durin::MarkAsGarbage(Canceled);
	Durin::CollectGarbage();
	Durin::FlushRenderingCommands();
	ASSERT_TRUE(Durin::InitializeTaskScheduler());
	ASSERT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
	Durin::EnqueueRenderCommand("EndOwnedTextureFrame", [](Durin::FRHICommandListImmediate& Commands) {
		Durin::GDynamicRHI->RHIEndFrame_RenderThread(Commands);
	});
	Durin::FlushRenderingCommands();
	Durin::ShutdownRenderingThread();
	auto* Rejected = Durin::NewObject<Durin::DTexture2D>(nullptr, "StoppedTextureAdmission");
	InstallUpdateInput(*Rejected, std::byte{0x44});
	Rejected->UpdateResource();
	Durin::PumpGameThreadDeferredWork();
	EXPECT_EQ(Rejected->GetResource(), nullptr);
	EXPECT_FALSE(Rejected->HasPendingRenderResourceInitialization());
	Durin::MarkAsGarbage(Rejected);
	Durin::CollectGarbage();
	Durin::FRHICommandListImmediate::Get().SwitchPipeline(Durin::ERHIPipeline::None);
	Durin::RHIExit();
	Durin::ShutdownTaskSystem();
}
