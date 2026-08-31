#include "Misc/MountPathTestSupport.h"
#include "TextureTestSupport.h"
#include "EditorReimportHandler.h"
#include "Misc/FileHelper.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureRenderResource.h"

static_assert(std::is_base_of_v<
	Durin::FTextureAssetResource, Durin::FTexture2DResource>);
static_assert(std::is_base_of_v<
	Durin::FTextureAssetResource, Durin::FTextureCubeResource>);

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
		Mip.Pixels.resize(static_cast<size_t>(Layout.DataSize));
		return Result;
	}
}

TEST(FTextureResourceCompletionTests, RejectsStaleBuild)
{
	Durin::FTextureResourceCompletion Completion;
	Completion.BeginRequest(1);
	EXPECT_TRUE(Completion.MarkBuilding(1));

	Completion.BeginRequest(2);
	EXPECT_FALSE(Completion.MarkBuilding(1));
	EXPECT_EQ(
		Completion.GetResourceState(),
		Durin::ERenderResourceState::Pending);
	EXPECT_EQ(Completion.GetRequestedRevision(), 2u);
}

TEST(FTextureResourceCompletionTests, RejectsStaleSuccess)
{
	Durin::FTextureResourceCompletion Completion;
	Completion.BeginRequest(1);
	ASSERT_TRUE(Completion.MarkBuilding(1));
	Completion.BeginRequest(2);
	Completion.MarkReady(1);

	EXPECT_EQ(Completion.GetAppliedRevision(), 0u);
	EXPECT_EQ(
		Completion.GetResourceState(),
		Durin::ERenderResourceState::Pending);
}

TEST(FTextureResourceCompletionTests, RejectsStaleFailure)
{
	Durin::FTextureResourceCompletion Completion;
	Completion.BeginRequest(1);
	ASSERT_TRUE(Completion.MarkBuilding(1));
	Completion.BeginRequest(2);
	Completion.MarkFailed(
		1, Durin::ETextureRenderFailure::CreateOrUpload);

	EXPECT_EQ(
		Completion.GetResourceState(),
		Durin::ERenderResourceState::Pending);
	EXPECT_EQ(Completion.GetFailedRevision(), 0u);
	EXPECT_EQ(
		Completion.GetFailureReason(),
		Durin::ETextureRenderFailure::None);
}

TEST(FTextureResourceCompletionTests, ReportsDistinctCurrentFailureReasons)
{
	Durin::FTextureResourceCompletion Completion;
	Completion.BeginRequest(1);
	ASSERT_TRUE(Completion.MarkBuilding(1));
	Completion.MarkFailed(
		1, Durin::ETextureRenderFailure::UnsupportedFormat);
	EXPECT_EQ(
		Completion.GetResourceState(),
		Durin::ERenderResourceState::Failed);
	EXPECT_EQ(Completion.GetFailedRevision(), 1u);
	EXPECT_EQ(
		Completion.GetFailureReason(),
		Durin::ETextureRenderFailure::UnsupportedFormat);

	Completion.BeginRequest(2);
	ASSERT_TRUE(Completion.MarkBuilding(2));
	Completion.MarkFailed(
		2, Durin::ETextureRenderFailure::CreateOrUpload);
	EXPECT_EQ(Completion.GetFailedRevision(), 2u);
	EXPECT_EQ(
		Completion.GetFailureReason(),
		Durin::ETextureRenderFailure::CreateOrUpload);
}

TEST(FTextureResourceCompletionTests, ReportsRelease)
{
	Durin::FTextureResourceCompletion Completion;
	Completion.BeginRequest(4);
	ASSERT_TRUE(Completion.MarkBuilding(4));
	Completion.MarkReleased(4);

	EXPECT_EQ(
		Completion.GetResourceState(),
		Durin::ERenderResourceState::Released);
	EXPECT_EQ(Completion.GetAppliedRevision(), 4u);
	EXPECT_EQ(Completion.GetFailedRevision(), 0u);
	EXPECT_EQ(
		Completion.GetFailureReason(),
		Durin::ETextureRenderFailure::None);
}

TEST(FTextureResourceCompletionTests, AppliedRevisionIsMonotonic)
{
	Durin::FTextureResourceCompletion Completion;
	Completion.BeginRequest(2);
	ASSERT_TRUE(Completion.MarkBuilding(2));
	Completion.MarkReady(2);
	EXPECT_EQ(Completion.GetAppliedRevision(), 2u);

	Completion.BeginRequest(3);
	Completion.MarkFailed(
		3, Durin::ETextureRenderFailure::UnsupportedFormat);
	EXPECT_EQ(Completion.GetAppliedRevision(), 2u);
	EXPECT_EQ(Completion.GetFailedRevision(), 3u);

	Completion.BeginRequest(4);
	Completion.MarkReleased(4);
	EXPECT_EQ(Completion.GetAppliedRevision(), 4u);

	Completion.BeginRequest(1);
	ASSERT_TRUE(Completion.MarkBuilding(1));
	Completion.MarkReady(1);
	EXPECT_EQ(Completion.GetAppliedRevision(), 4u);
}

TEST(FTexture2DTests, ExchangeInvalidatesTheSideWithoutPlatformData)
{
	InitializeDObjectSystem();
	auto* Populated = Durin::NewObject<Durin::DTexture2D>(nullptr, "PopulatedTexture2D");
	auto* Empty = Durin::NewObject<Durin::DTexture2D>(nullptr, "EmptyTexture2D");
	std::string Error;
	ASSERT_TRUE(Populated->PublishDerivedDataLoad(
		std::make_unique<Durin::FTexturePlatformData>(MakeSingleMipPlatformData()),
		"Texture2DExchangeTest", Error)) << Error;

	Populated->ExchangeImportedState(*Empty);

	EXPECT_EQ(Populated->GetPlatformData(), nullptr);
	EXPECT_EQ(Populated->GetRenderResourceState(), Durin::ERenderResourceState::Released);
	ASSERT_NE(Empty->GetPlatformData(), nullptr);
	EXPECT_TRUE(Empty->GetPlatformData()->IsValid());
	Durin::MarkAsGarbage(Populated);
	Durin::MarkAsGarbage(Empty);
}

TEST(FTextureCubeTests, ExchangeInvalidatesTheSideWithoutPlatformData)
{
	InitializeDObjectSystem();
	auto* Populated = Durin::NewObject<Durin::DTextureCube>(nullptr, "PopulatedTextureCube");
	auto* Empty = Durin::NewObject<Durin::DTextureCube>(nullptr, "EmptyTextureCube");
	auto PlatformData = std::make_unique<Durin::FTextureCubePlatformData>();
	PlatformData->PixelFormat = Durin::EPixelFormat::BC1_UNORM;
	for (Durin::FTexturePlatformData& Face : PlatformData->Faces)
		Face = MakeSingleMipPlatformData();
	std::string Error;
	ASSERT_TRUE(Populated->PublishDerivedDataLoad(
		std::move(PlatformData), "TextureCubeExchangeTest", Error)) << Error;

	Populated->ExchangeImportedState(*Empty);

	EXPECT_EQ(Populated->GetPlatformData(), nullptr);
	EXPECT_EQ(Populated->GetRenderResourceState(), Durin::ERenderResourceState::Released);
	ASSERT_NE(Empty->GetPlatformData(), nullptr);
	EXPECT_TRUE(Empty->GetPlatformData()->IsValid());
	Durin::MarkAsGarbage(Populated);
	Durin::MarkAsGarbage(Empty);
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
	EXPECT_EQ(Durin::Asset::FindAssetExact(AssetPath), nullptr);
}

TEST(FTexture2DTests, FailureStateRecordsMissingCanonicalDataOnPostLoad)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/FailureTestMissing", AssetPath));
	Durin::DTexture2D* Texture = nullptr;
	Durin::Asset::FAssetResult CreateResult = Durin::Asset::CreateAsset(AssetPath, Texture);
	ASSERT_TRUE(CreateResult) << CreateResult.Message;
	ASSERT_NE(Texture, nullptr);
	// At creation time, the build has not run.
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Unbuilt);
	// PostLoad with no canonical imported pixels.
	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::BuildFailure);
	EXPECT_FALSE(Texture->GetLastBuildError().empty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Texture->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
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
	EXPECT_EQ(Result.Asset->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_TRUE(Result.Asset->GetLastBuildError().empty());

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureFailureTests/Ready", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
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
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	ASSERT_NE(Texture->GetSourceData(), nullptr);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);

	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureInvalidateTests/Invalid", AssetPath));
	const std::filesystem::path CopiedSource = Source;
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Texture->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::Hit);
	EXPECT_TRUE(Texture->GetLastBuildError().empty());

	const Durin::FTexturePlatformData RetainedPlatformData = *Texture->GetPlatformData();
	const uint64 RetainedRevision = Texture->GetBuildRevision();
	{
		const std::array<uint8, 4> CorruptBytes = {1, 2, 3, 4};
		std::ofstream Stream(GetTextureCachePath(*Texture), std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), RetainedPlatformData);
	EXPECT_GT(Texture->GetBuildRevision(), RetainedRevision);
	EXPECT_EQ(Texture->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::Rebuilt);
	EXPECT_FALSE(Texture->GetDerivedDataDiagnostic().bSourceDecoderInvoked);
	EXPECT_NE(Texture->GetSourceData(), nullptr);

	WriteTextureFixture(CopiedSource);
	ASSERT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->GetLastBuildError().empty());

	ASSERT_TRUE(Durin::Asset::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, StatusEnumsExposeSharedDisplayMetadata)
{
	InitializeDObjectSystem();
	Durin::DEnum* BuildStatusEnum = Durin::FindEnumByQualifiedName("Durin::ETextureBuildStatus");
	Durin::DEnum* ResourceStateEnum = Durin::FindEnumByQualifiedName("Durin::ERenderResourceState");
	ASSERT_NE(BuildStatusEnum, nullptr);
	ASSERT_NE(ResourceStateEnum, nullptr);
	EXPECT_EQ(BuildStatusEnum->GetDisplayName(), "Texture Build Status");
	EXPECT_EQ(ResourceStateEnum->GetDisplayName(), "Render Resource State");

	const Durin::FEnumValue* Unbuilt = BuildStatusEnum->FindValueRecordByValue(
		static_cast<uint64>(Durin::ETextureBuildStatus::Unbuilt));
	const Durin::FEnumValue* BuildFailure = BuildStatusEnum->FindValueRecordByValue(
		static_cast<uint64>(Durin::ETextureBuildStatus::BuildFailure));
	const Durin::FEnumValue* Building = ResourceStateEnum->FindValueRecordByValue(
		static_cast<uint64>(Durin::ERenderResourceState::Building));
	ASSERT_NE(Unbuilt, nullptr);
	ASSERT_NE(BuildFailure, nullptr);
	ASSERT_NE(Building, nullptr);
	EXPECT_EQ(Unbuilt->DisplayName, "Not Built");
	EXPECT_EQ(BuildFailure->DisplayName, "Build Failure");
	EXPECT_EQ(Building->DisplayName, "Building");
	EXPECT_EQ(BuildStatusEnum->FindValueRecordByValue(255), nullptr);
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
	const uint64 LastGoodRevision = Texture->GetBuildRevision();

	WriteNpotTextureFixture(Source);
	std::string Error;
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/AsyncUnload", AssetPath));
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTexture2D(
		*Texture, Error)) << Error;
	ASSERT_TRUE(Durin::Asset::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetBuildRevision(), LastGoodRevision + 1);
	EXPECT_NE(Texture->GetPlatformData()->Mips.front().Pixels,
		LastGood.Mips.front().Pixels);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, DirectReimportPublishesAndSaves)
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
	const std::string PriorKey = Texture->GetDerivedDataKey();
	const uint64 PriorRevision = Texture->GetBuildRevision();
	ASSERT_FALSE(Texture->GetPackage()->IsDirty());

	WriteNpotTextureFixture(Source);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/ImportRollback", AssetPath));
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Texture, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	ASSERT_TRUE(Durin::Asset::WaitForTexture2DCompilation(*Texture, 10.0));
	ASSERT_TRUE(Reimported) << Reimported.Message;
	ASSERT_NE(Texture->GetAssetImportData(), nullptr);
	ImportedSource = Texture->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->Hint, PriorSource);
	EXPECT_NE(Texture->GetPlatformData()->Mips.front().Pixels,
		PriorPlatform.Mips.front().Pixels);
	EXPECT_NE(Texture->GetDerivedDataKey(), PriorKey);
	EXPECT_EQ(Texture->GetBuildRevision(), PriorRevision + 1);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	const Durin::FTexturePlatformData LastGood = *Texture->GetPlatformData();
	const uint64 LastGoodRevision = Texture->GetBuildRevision();
	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "TextureManagerCorrupt.png";
	const std::array CorruptBytes{std::byte{0x01}, std::byte{0x02}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(CorruptBytes, Corrupt));
	const std::array Files{Corrupt.generic_string()};
	Durin::FReimportManager::ReimportFromFiles(*Texture, Files, {.bSave = false},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	EXPECT_EQ(Reimported.Status, Durin::EReimportStatus::SourceOrBuildFailure);
	EXPECT_EQ(Texture->GetBuildRevision(), LastGoodRevision);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), LastGood);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}
