#include "AssetForge/Builtins/Texture2DFactory.h"
#include "Threading/Task.h"
#include "NativeAssetTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "TextureTestSupport.h"
#include "Import/EditorReimportHandler.h"
#include "Misc/FileHelper.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Class.h"
#include "DObject/PropertyChange.h"
#include "DObject/ObjectLifecycle.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/Texture2DCompilationTestSupport.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"
#include "RenderResource.h"
#include "Texture/VolumeTexture.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include <latch>

static_assert(std::is_base_of_v<
	Durin::FTextureResource, Durin::FTexture2DResource>);
static_assert(std::is_base_of_v<
	Durin::FTextureResource, Durin::FTextureCubeResource>);

namespace
{
	auto MakeSingleMipPlatformData() -> Durin::FTexturePlatformData
	{
		Durin::FTexturePlatformData Result;
		Result.PixelFormat = Durin::EPixelFormat::BC1_UNORM;
		const Durin::FPixelFormatLayout Layout =
			Durin::GetPixelFormatLayout(Result.PixelFormat, 1, 1);
		Durin::FTexture2DMipData& Mip = Result.Mips.emplace_back();
		Mip.Width = 1;
		Mip.Height = 1;
		Mip.RowPitch = static_cast<uint32>(Layout.RowPitch);
		Mip.Pixels = Durin::FSharedByteBuffer::Take(Durin::FByteBuffer(static_cast<size_t>(Layout.DataSize)));
		return Result;
	}
}

TEST(FTexture2DTests, RejectsUnsupportedSourceWithoutCreatingAsset)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "UnsupportedTexture.gif";
	std::ofstream(Source, std::ios::binary | std::ios::trunc) << "not an image";
	Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(Source.generic_string(), "/TextureImportTests/Unsupported");
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Message.empty());

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/Unsupported", AssetPath));
	EXPECT_EQ(Durin::FindAssetExact(AssetPath), nullptr);
}

TEST(FTexture2DTests, FailureStateRecordsMissingCanonicalDataOnPostLoad)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/FailureTestMissing", AssetPath));
	Durin::DTexture2D* Texture = nullptr;
	const auto CreateResult = Durin::CreatePackageLeafAssetForTesting(AssetPath, Texture);
	ASSERT_TRUE(CreateResult) << CreateResult.Message;
	ASSERT_NE(Texture, nullptr);
	// At creation time, the build has not run.
	EXPECT_FALSE(Texture->HasPlatformData());
	// PostLoad with no canonical imported pixels.
	Texture->PostLoad();
	EXPECT_FALSE(Texture->HasPlatformData());
	ASSERT_TRUE(Durin::UnloadPackage(Texture->GetPackage(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FTexture2DTests, FailureState_ReadyAfterSuccessfulPostLoad)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureFailureMount";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests(
		"/TextureFailureTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "FailureReadySource.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(Source.generic_string(), "/TextureFailureTests/Ready");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_TRUE(Result.Asset->HasPlatformData());

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureFailureTests/Ready", AssetPath));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, MissingSourceAndCorruptDdcRebuildFromAuthoredPixels)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureInvalidateDerivedDataCache");
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureInvalidateMount";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::Testing::RegisterMountPointForTests(
		"/TextureInvalidateTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "InvalidateSource.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Result = Durin::AssetForge::Builtins::ImportTexture2DForTest(Source.generic_string(), "/TextureInvalidateTests/Invalid");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	EXPECT_TRUE(Texture->HasPlatformData());
	ASSERT_TRUE(Texture->GetSource().IsValid());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureInvalidateTests/Invalid", AssetPath));
	const std::filesystem::path CopiedSource = Source;
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	Texture->PostLoad();
	EXPECT_TRUE(Texture->HasPlatformData());
	EXPECT_TRUE(Texture->GetSource().IsValid());
	EXPECT_NE(Texture->GetPlatformData(), nullptr);

	const Durin::FTexturePlatformData RetainedPlatformData = *Texture->GetPlatformData();
	const auto RetainedPlatformDataIdentity = Texture->GetPlatformDataShared();
	{
		const std::array<uint8, 4> CorruptBytes = {1, 2, 3, 4};
		std::ofstream Stream(GetTextureCachePath(*Texture), std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	Texture->PostLoad();
	EXPECT_TRUE(Texture->HasPlatformData());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), RetainedPlatformData);
	// Repeated PostLoad/BeginCache calls preserve already-installed platform data.
	EXPECT_EQ(Texture->GetPlatformDataShared(), RetainedPlatformDataIdentity);
	EXPECT_TRUE(Texture->GetSource().IsValid());

	WriteTextureFixture(CopiedSource);
	Texture->PostLoad();
	EXPECT_TRUE(Texture->HasPlatformData());
	EXPECT_TRUE(Texture->GetSource().IsValid());
	EXPECT_NE(Texture->GetPlatformData(), nullptr);

	ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}


TEST(FTexture2DTests, ScheduledReimportPublishesOnce)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureAsyncUnloadCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureAsyncUnload.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/AsyncUnload");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	const Durin::FTexturePlatformData LastGood = *Texture->GetPlatformData();
	const auto LastGoodPlatformDataIdentity = Texture->GetPlatformDataShared();

	WriteNpotTextureFixture(Source);
	std::string Error;
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/AsyncUnload", AssetPath));
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTexture2D(
		*Texture));
	ASSERT_TRUE(Texture->FinishCachePlatformData(10.0));
	EXPECT_TRUE(Texture->HasPlatformData());
	EXPECT_NE(Texture->GetPlatformDataShared(), LastGoodPlatformDataIdentity);
	EXPECT_FALSE(std::ranges::equal(Texture->GetPlatformData()->Mips.front().Pixels, LastGood.Mips.front().Pixels));

	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, PostEditChangeUsesObjectQueriesAndManagerNotification)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(Testing::GetTestWorkDirectory() / "PostEditTextureCache");
	const auto Input = Testing::GetTestWorkDirectory() / "PostEdit.png";
	WriteTextureFixture(Input);
	const auto Imported = AssetForge::Builtins::ImportTexture2DForTest(Input.generic_string(), "/TextureImportTests/PostEdit");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto* Texture = Imported.Asset;
	const auto SourceIdentity = Texture->GetSource().GetIdentity();
	const auto Metadata = Texture->GetAssetImportData()->GetState();
	uint32 Notifications = 0;
	struct FNotificationScope
	{
		FDelegateHandle Handle;
		~FNotificationScope()
		{
			FAssetCompilingManager::Get().OnAssetPostCompile().Remove(Handle);
			AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
			FAssetCompilingManager::Get().FinishAllCompilation();
		}
	} Scope{FAssetCompilingManager::Get().OnAssetPostCompile().AddLambda(
		[&](const FAssetPostCompileData& Data) {
			if (Data.CompilerName != FName("Durin.Texture")) return;
			for (const auto& Asset : Data.Assets)
			{
				if (Asset.Get() != Texture) continue;
				++Notifications;
				EXPECT_TRUE(Texture->IsAsyncCacheComplete());
				EXPECT_EQ(Durin::AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).Phase, ETexture2DCompilationPhase::Ready);
				EXPECT_TRUE(Texture->HasPlatformData());
			}
		})};
	Texture->PostEditChange();
	EXPECT_FALSE(Texture->IsAsyncCacheComplete());
	EXPECT_EQ(Notifications, 0u);
	ASSERT_TRUE(Texture->FinishCachePlatformData(10.0));
	EXPECT_EQ(Notifications, 1u);
	const auto RequestId = AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).RequestId;
	const auto* UsageProperty = Texture->GetClass()->FindPropertyByName("Usage");
	ASSERT_NE(UsageProperty, nullptr);
	Texture->PostEditChangeProperty({.MemberProperty = UsageProperty});
	EXPECT_TRUE(Texture->IsAsyncCacheComplete());
	EXPECT_EQ(AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).RequestId, RequestId);
	AssetPrivate::SetTexture2DCompilationPhaseHookForTests([](uint64, ETexture2DCompilationPhase Phase) {
		if (Phase == ETexture2DCompilationPhase::Building) throw std::runtime_error("Injected PostEdit build failure");
	});
	Texture->PostEditChange();
	EXPECT_FALSE(Texture->FinishCachePlatformData(10.0));
	EXPECT_TRUE(Texture->IsAsyncCacheComplete());
	EXPECT_EQ(Durin::AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).Phase, ETexture2DCompilationPhase::Failed);
	EXPECT_EQ(Notifications, 1u);
	AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
	Texture->PostEditChange();
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
	FAssetCompilingManager::Get().FinishCompilationForObject(*Texture);
	EXPECT_TRUE(Texture->IsAsyncCacheComplete());
	EXPECT_EQ(Durin::AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).Phase, ETexture2DCompilationPhase::Canceled);
	EXPECT_EQ(Notifications, 1u);
	EXPECT_EQ(Texture->GetSource().GetIdentity(), SourceIdentity);
	EXPECT_EQ(Texture->GetAssetImportData()->GetState(), Metadata);
	EXPECT_TRUE(UnloadPackage(Texture->GetPackage()));
}

TEST(FTexture2DTests, ImportedSourceAndMetadataSurviveDerivedDataFailure)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "ImportedSourceDdcFailure");
	const auto Original = Durin::Testing::GetTestWorkDirectory() / "Original.png";
	const auto Replacement = Durin::Testing::GetTestWorkDirectory() / "Replacement.png";
	WriteTextureFixture(Original);
	WriteNpotTextureFixture(Replacement);
	const auto Imported = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Original.generic_string(), "/TextureImportTests/DdcFailure");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto* Texture = Imported.Asset;
	const auto Prepared = Durin::AssetForge::Builtins::PrepareTexture2DImport(Replacement.generic_string());
	ASSERT_TRUE(Prepared);
	struct FRestorePhaseHook
	{
		~FRestorePhaseHook()
		{
			Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests({});
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
		}
	} Restore;
	Durin::AssetPrivate::SetTexture2DCompilationPhaseHookForTests([](uint64, Durin::ETexture2DCompilationPhase Phase) {
		if (Phase == Durin::ETexture2DCompilationPhase::Building)
			throw std::runtime_error("Injected derived-data failure");
	});
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTexture2DFromFile(*Texture, Replacement.generic_string()));
	// The authored commit succeeds before the independent compilation is delivered.
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Prepared->Source.GetIdentity());
	ASSERT_NE(Texture->GetAssetImportData(), nullptr);
	const auto CommittedMetadata = Texture->GetAssetImportData()->GetState();
	const auto* Source = CommittedMetadata.SourceData.FindByRole("source");
	ASSERT_NE(Source, nullptr);
	EXPECT_EQ(Source->GetContentHash(), Prepared->ContentHash);
	EXPECT_EQ(Source->ByteCount, Prepared->ByteCount);
	EXPECT_TRUE(Source->Hint.ends_with("Replacement.png"));
	EXPECT_EQ(Source->DisplayLabel, "Replacement.png");
	EXPECT_FALSE(Texture->FinishCachePlatformData(10.0));
	EXPECT_EQ(Durin::AssetPrivate::GetTexture2DCompilationDiagnosticForTests(*Texture).Phase, Durin::ETexture2DCompilationPhase::Failed);
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Prepared->Source.GetIdentity());
	EXPECT_EQ(Texture->GetAssetImportData()->GetState(), CommittedMetadata);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	EXPECT_FALSE(Texture->HasPlatformData());
	// Saving authored data does not require successful DDC compilation.
	ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
	EXPECT_TRUE(Durin::UnloadPackage(Texture->GetPackage()));
}

TEST(FTexture2DTests, ReimportSaveFailureIsSeparateAndRetainsCommittedData)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "ReimportSaveFailure");
	const auto Input = Durin::Testing::GetTestWorkDirectory() / "Input.png";
	WriteTextureFixture(Input);
	const std::string AssetPath = "/TextureImportTests/SaveFailure";
	const auto Imported = Durin::AssetForge::Builtins::ImportTexture2DForTest(Input.generic_string(), AssetPath);
	ASSERT_TRUE(Imported) << Imported.Message;
	auto* Texture = Imported.Asset;
	const auto Resolved = Durin::FMountPaths::ResolveAssetPath(AssetPath, Durin::EMountPathExistence::AllowMissing);
	ASSERT_TRUE(Resolved);
	auto Destination = Resolved->PhysicalPath;
	Destination += ".dasset";
	ASSERT_TRUE(std::filesystem::remove(Destination));
	ASSERT_TRUE(std::filesystem::create_directory(Destination));
	WriteNpotTextureFixture(Input);
	const auto Prepared = Durin::AssetForge::Builtins::PrepareTexture2DImport(Input.generic_string());
	ASSERT_TRUE(Prepared);
	std::optional<Durin::FReimportResult> Completed;
	Durin::FReimportManager::Reimport(*Texture, {},
		[&](Durin::FReimportResult Result) { Completed = std::move(Result); });
	ASSERT_TRUE(Completed);
	EXPECT_EQ(Completed->Status, Durin::EReimportResult::Failed);
	EXPECT_NE(Completed->SaveCause, nullptr);
	EXPECT_EQ(Completed->FactoryCause, nullptr);
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Prepared->Source.GetIdentity());
	EXPECT_EQ(Texture->GetAssetImportData()->GetSourceData().FindByRole("source")->GetContentHash(), Prepared->ContentHash);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(std::filesystem::remove(Destination));
	ASSERT_TRUE(std::filesystem::remove(Input));
	// Persistence retry uses the committed data, without repeating import.
	ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
	EXPECT_TRUE(Durin::UnloadPackage(Texture->GetPackage()));
}

TEST(FTexture2DTests, ImportedCommitRejectsInvalidMetadataAndRetiresPriorCompilation)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "ImportedSourceCommit");
	const auto Original = Durin::Testing::GetTestWorkDirectory() / "Original.png";
	const auto Replacement = Durin::Testing::GetTestWorkDirectory() / "Replacement.png";
	WriteTextureFixture(Original);
	WriteNpotTextureFixture(Replacement);
	const auto Imported = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Original.generic_string(), "/TextureImportTests/AuthoredCommit");
	ASSERT_TRUE(Imported) << Imported.Message;
	auto* Texture = Imported.Asset;
	const auto OldIdentity = Texture->GetSource().GetIdentity();
	const auto OldMetadata = Texture->GetAssetImportData()->GetState();
	const auto OldProduct = Texture->GetPlatformDataShared();
	const auto Prepared = Durin::AssetForge::Builtins::PrepareTexture2DImport(Replacement.generic_string());
	ASSERT_TRUE(Prepared);
	auto InvalidMetadata = OldMetadata;
	InvalidMetadata.SourceData.Sources.front().Hint = "Invalid/./Outside.png";
	EXPECT_FALSE(Texture->SetImportedSource(Prepared->Source, {}, std::move(InvalidMetadata)));
	EXPECT_EQ(Texture->GetSource().GetIdentity(), OldIdentity);
	EXPECT_EQ(Texture->GetAssetImportData()->GetState(), OldMetadata);
	EXPECT_EQ(Texture->GetPlatformDataShared(), OldProduct);
	auto Retired = std::make_shared<uint32>(0);
	ASSERT_TRUE(Texture->BeginCachePlatformData({.Build = Texture->CreateBuildRequest({}).value()}, [Texture, OldIdentity, OldMetadata, Retired](Durin::FTextureOperationResult Result) {
			EXPECT_EQ(Result.Status, Durin::ETextureCompilationStatus::Succeeded);
			EXPECT_EQ(Texture->GetSource().GetIdentity(), OldIdentity);
			EXPECT_EQ(Texture->GetAssetImportData()->GetState(), OldMetadata);
			++*Retired;
		}));
	auto Metadata = OldMetadata;
	Metadata.SourceData.Sources.front().Hint = Replacement.generic_string();
	Metadata.SourceData.Sources.front().HintBase = Durin::ESourceHintBase::Absolute;
	Metadata.SourceData.Sources.front().ContentHashLow = Prepared->ContentHash.HashLow;
	Metadata.SourceData.Sources.front().ContentHashHigh = Prepared->ContentHash.HashHigh;
	Metadata.SourceData.Sources.front().ByteCount = Prepared->ByteCount;
	const Durin::FTexture2DBuildSettings Settings{.Usage = Durin::ETextureUsage::DataMask, .bSRGB = false};
	ASSERT_TRUE(Texture->SetImportedSource(Prepared->Source, Settings, Metadata));
	EXPECT_EQ(*Retired, 1u);
	EXPECT_FALSE(!Texture->IsAsyncCacheComplete());
	EXPECT_EQ(Texture->GetSource().GetIdentity(), Prepared->Source.GetIdentity());
	EXPECT_EQ(Texture->GetAssetImportData()->GetState(), Metadata);
	EXPECT_EQ(Texture->GetUsage(), Settings.Usage);
	EXPECT_FALSE(Texture->IsSRGB());
	EXPECT_FALSE(Texture->HasPlatformData());
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	EXPECT_TRUE(Durin::UnloadPackage(Texture->GetPackage(), Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FTexture2DTests, ReimportManagerSavesOnlyWhenRequested)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureImportRollbackDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureImportRollback.png";
	WriteTextureFixture(Source);
	const Durin::Testing::TFactoryImportResult<Durin::DTexture2D> Imported = Durin::AssetForge::Builtins::ImportTexture2DForTest(
		Source.generic_string(), "/TextureImportTests/ImportRollback");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	ASSERT_NE(Texture->GetAssetImportData(), nullptr);
	const Durin::FSourceFile* ImportedSource =
		Texture->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	const Durin::FReimportCapabilities Capabilities =
		Durin::FReimportManager::GetCapabilities(*Texture);
	EXPECT_TRUE(Capabilities.bCanReimport) << Capabilities.Diagnostic;
	EXPECT_TRUE(Capabilities.bCanReimportFromFile) << Capabilities.Diagnostic;
	const std::string PriorSource = ImportedSource->Hint;
	const Durin::FTexturePlatformData PriorPlatform = *Texture->GetPlatformData();
	const std::string PriorKey = GetTextureDerivedDataKey(*Texture);
	const auto PriorPlatformDataIdentity = Texture->GetPlatformDataShared();
	ASSERT_FALSE(Texture->GetPackage()->IsDirty());

	WriteNpotTextureFixture(Source);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/ImportRollback", AssetPath));
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Texture, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	ASSERT_TRUE(Texture->FinishCachePlatformData(10.0));
	ASSERT_TRUE(Reimported) << Reimported.Message;
	ASSERT_NE(Texture->GetAssetImportData(), nullptr);
	ImportedSource = Texture->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->Hint, PriorSource);
	EXPECT_FALSE(std::ranges::equal(Texture->GetPlatformData()->Mips.front().Pixels, PriorPlatform.Mips.front().Pixels));
	EXPECT_NE(GetTextureDerivedDataKey(*Texture), PriorKey);
	EXPECT_NE(Texture->GetPlatformDataShared(), PriorPlatformDataIdentity);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	const Durin::FTexturePlatformData LastGood = *Texture->GetPlatformData();
	const auto LastGoodPlatformDataIdentity = Texture->GetPlatformDataShared();
	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "TextureManagerCorrupt.png";
	const std::array CorruptBytes{std::byte{0x01}, std::byte{0x02}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(CorruptBytes, Corrupt));
	const std::array Files{Corrupt.generic_string()};
	Durin::FReimportManager::ReimportFromFiles(*Texture, Files, {.bSave = false},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	EXPECT_EQ(Reimported.Status, Durin::EReimportResult::Failed);
	const auto* Detail = dynamic_cast<const Durin::AssetForge::Builtins::FTexture2DFactoryError*>(Reimported.FactoryCause.get());
	ASSERT_NE(Detail, nullptr);
	EXPECT_FALSE(Detail->Diagnostic.empty());
	EXPECT_EQ(Texture->GetPlatformDataShared(), LastGoodPlatformDataIdentity);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), LastGood);
	std::optional<Durin::FReimportResult> Previous, Current;
	Durin::FReimportManager::Reimport(*Texture, {.bSave = false},
		[&](Durin::FReimportResult Result) { Previous = std::move(Result); });
	Durin::FReimportManager::Reimport(*Texture, {.bSave = false},
		[&](Durin::FReimportResult Result) { Current = std::move(Result); });
	const auto CommittedIdentity = Texture->GetSource().GetIdentity();
	const auto CommittedMetadata = Texture->GetAssetImportData()->GetState();
	Durin::FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Previous);
	ASSERT_TRUE(Current);
	EXPECT_EQ(Previous->Status, Durin::EReimportResult::Succeeded);
	EXPECT_EQ(Current->Status, Durin::EReimportResult::Succeeded);
	EXPECT_EQ(Texture->GetSource().GetIdentity(), CommittedIdentity);
	EXPECT_EQ(Texture->GetAssetImportData()->GetState(), CommittedMetadata);
	EXPECT_FALSE(!Texture->IsAsyncCacheComplete());
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

namespace
{
	class FUpdateTestTexture final : public Durin::FRHITexture {};

	struct FUpdateResourceObservations
	{
		int Initialized = 0;
		int Released = 0;
		int Destroyed = 0;
	};

	class FUpdateTestResource final : public Durin::FTextureResource
	{
	public:
		FUpdateTestResource(Durin::FTextureReference& Reference, FUpdateResourceObservations& InEvents,
			bool bInFail = false,
			std::function<void()> InInitialize = {})
			: FTextureResource(&Reference), Events(InEvents), bFail(bInFail), Initialize(std::move(InInitialize)) {}
		~FUpdateTestResource() override { ++Events.Destroyed; }
		auto InitRHI(Durin::FRHICommandListBase&) -> void override
		{
			++Events.Initialized;
			if (Initialize) Initialize();
			if (!bFail) SetTextureRHI_RenderThread(new FUpdateTestTexture());
		}
		auto ReleaseRHI() -> void override
		{
			++Events.Released;
			FTextureResource::ReleaseRHI();
		}
	private:
		FUpdateResourceObservations& Events;
		bool bFail;
		std::function<void()> Initialize;
	};

	class FTextureResourceLifetimeTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			Durin::InitRenderingThread();
		}
		auto TearDown() -> void override
		{
			Durin::FlushRenderingCommands();
			Durin::EnqueueRenderCommand("DrainOwnedTextureTestResources", [](Durin::FRHICommandListImmediate&) {
				for (;;)
				{
					std::vector<Durin::FRHIResource*> Resources;
					Durin::FRHIResource::GatherResourcesToDelete(Resources);
					if (Resources.empty()) break;
					Durin::FRHIResource::DeleteResources(Resources);
				}
			});
			Durin::FlushRenderingCommands();
			EXPECT_EQ(Durin::GetNumInitializedRenderResources(), 0u);
			Durin::ShutdownRenderingThread();
		}
		auto Start(Durin::FTextureResource& Resource,
			Durin::FTextureReference& Reference, bool bInitialize = false) -> void
		{
			Durin::EnqueueRenderCommand("OwnedTextureTestInit", [&Resource, &Reference, bInitialize](Durin::FRHICommandListImmediate& Commands) {
				if (bInitialize) Reference.InitResource(Commands);
				Reference.ResetToFallback_RenderThread();
				Resource.InitResource(Commands);
				if (Resource.GetTextureRHI_RenderThread()) Resource.PublishTexture_RenderThread();
			});
			Durin::FRenderCommandFence Fence;
			Fence.BeginFence();
			Fence.Wait();
		}
		auto Retire(std::unique_ptr<Durin::FTextureResource> Resource) -> void
		{
			Resource->BeginRelease_GameThread();
			Durin::BeginCleanupRenderResource(Durin::FDeferredRenderResourceCleanup(std::move(Resource)));
			Durin::FlushRenderingCommands();
		}

		auto Resolve(Durin::FTextureReference& Reference) -> Durin::FRHITexture*
		{
			Durin::FRHITexture* Result = nullptr;
			Durin::EnqueueRenderCommand("ResolveOwnedTextureTest", [&Reference, &Result](Durin::FRHICommandListImmediate&) {
				Result = Reference.GetReferencedTexture_RenderThread();
			});
			Durin::FlushRenderingCommands();
			return Result;
		}
	};
}

TEST_F(FTextureResourceLifetimeTests, FailedReplacementUsesFallbackAndLateReleasePreservesNewTarget)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations OldEvents, FailedEvents, NewEvents;
	auto Old = std::make_unique<FUpdateTestResource>(Reference, OldEvents);
	Start(*Old, Reference, true);
	auto OldSnapshot = Old->GetTextureRHI_GameThread();
	EXPECT_NE(OldSnapshot, nullptr);
	auto Failed = std::make_unique<FUpdateTestResource>(Reference, FailedEvents, true);
	Start(*Failed, Reference);
	EXPECT_EQ(Failed->GetTextureRHI_GameThread(), nullptr);
	EXPECT_EQ(Resolve(Reference), nullptr);
	Retire(std::move(Failed));
	EXPECT_EQ(FailedEvents.Released, 1);
	EXPECT_EQ(FailedEvents.Destroyed, 1);
	auto New = std::make_unique<FUpdateTestResource>(Reference, NewEvents);
	Start(*New, Reference);
	auto NewSnapshot = New->GetTextureRHI_GameThread();
	EXPECT_NE(NewSnapshot, OldSnapshot);
	Retire(std::move(Old));
	EXPECT_EQ(Resolve(Reference), NewSnapshot.GetReference());
	EXPECT_EQ(OldEvents.Released, 1);
	EXPECT_EQ(OldEvents.Destroyed, 1);
	Retire(std::move(New));
	EXPECT_EQ(Resolve(Reference), nullptr);
	EXPECT_EQ(NewEvents.Destroyed, 1);
	EXPECT_EQ(Durin::GetNumInitializedRenderResources(), 1u);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST_F(FTextureResourceLifetimeTests, QueuedReleaseRetainsResourceUntilInitializationAndRetirementComplete)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations Events;
	auto Resource = std::make_unique<FUpdateTestResource>(Reference, Events);
	std::latch Entered(1), Resume(1);
	Durin::EnqueueRenderCommand("HoldTextureLifetime", [&](Durin::FRHICommandListImmediate&) {
		Entered.count_down(); Resume.wait();
	});
	Entered.wait();
	Durin::EnqueueRenderCommand("InitBeforeTextureRetirement", [Pointer = Resource.get(), &Reference](Durin::FRHICommandListImmediate& Commands) {
		Reference.InitResource(Commands);
		Pointer->InitResource(Commands);
		Pointer->PublishTexture_RenderThread();
	});
	Resource->BeginRelease_GameThread();
	Durin::BeginCleanupRenderResource(Durin::FDeferredRenderResourceCleanup(std::move(Resource)));
	Durin::FRenderCommandFence Fence;
	Fence.BeginFence();
	EXPECT_FALSE(Fence.IsFenceComplete());
	EXPECT_EQ(Events.Destroyed, 0);
	Resume.count_down();
	Fence.Wait();
	EXPECT_EQ(Events.Initialized, 1);
	EXPECT_EQ(Events.Released, 1);
	EXPECT_EQ(Events.Destroyed, 1);
	EXPECT_EQ(Resolve(Reference), nullptr);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST(FTextureResourceAdmissionTests, MissingPlatformDataDoesNotAdmitWork)
{
	InitializeDObjectSystem();

	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "MissingPlatformData");
	Texture->UpdateResource();
	EXPECT_FALSE(Texture->HasPendingRenderResourceInitialization());
	EXPECT_EQ(Texture->GetResource(), nullptr);
}

TEST(FTextureResourceAdmissionTests, MissingRHIRejectsSynchronouslyWithoutTaskExecutor)
{
	InitializeDObjectSystem();
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "RejectedResourceInputs");
	Texture->SetPlatformData(std::make_unique<Durin::FTexturePlatformData>(MakeSingleMipPlatformData()));
	for (int Attempt = 0; Attempt < 3; ++Attempt)
	{
		Texture->UpdateResource();
		EXPECT_FALSE(Texture->HasPendingRenderResourceInitialization());
		EXPECT_EQ(Texture->GetResource(), nullptr);
	}
}
