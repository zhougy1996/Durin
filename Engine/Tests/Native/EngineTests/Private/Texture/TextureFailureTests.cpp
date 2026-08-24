#include "TextureTestSupport.h"
#include "AssetBuild/BuildHost.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/Texture2DAuthoringCoordinator.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureRenderResource.h"
#include "ImportService.h"

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
		"Texture2DExchangeTest", true, Error)) << Error;

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
	Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(Source.generic_string(), "/TextureImportTests/Unsupported");
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Message.empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Unsupported", AssetPath));
	EXPECT_EQ(Durin::Asset::FindAssetExact(AssetPath), nullptr);
}

TEST(FTexture2DTests, FailureState_RecordsMissingSourceOnPostLoad)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/FailureTestMissing", AssetPath));
	Durin::DTexture2D* Texture = nullptr;
	Durin::Asset::FAssetResult CreateResult = Durin::Asset::CreateAsset(AssetPath, Texture);
	ASSERT_TRUE(CreateResult) << CreateResult.Message;
	ASSERT_NE(Texture, nullptr);
	// At creation time, the build has not run.
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Unbuilt);
	// PostLoad with an empty source file.
	std::string Error;
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	EXPECT_FALSE(Texture->GetLastBuildError().empty());
	ASSERT_TRUE(Durin::Asset::UnloadPackage(Texture->GetPackage(), Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FTexture2DTests, FailureState_ReadyAfterSuccessfulPostLoad)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureFailureMount";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/TextureFailureTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "FailureReadySource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(Source.generic_string(), "/TextureFailureTests/Ready");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Asset->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_TRUE(Result.Asset->GetLastBuildError().empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureFailureTests/Ready", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, MissingSourceUsesPersistedIdentityAndCanRecover)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureInvalidateDerivedDataCache");
	const std::filesystem::path Root =
		Durin::Testing::GetTestWorkDirectory() / "TextureInvalidateMount";
	Durin::Testing::RemoveTestWorkDirectory(Root);
	Durin::PathUtilities::RegisterMountPointForTests(
		"/TextureInvalidateTests/", Root.generic_string() + "/");

	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "InvalidateSource.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Result = Durin::Asset::Forge::ImportTexture2DAsset(Source.generic_string(), "/TextureInvalidateTests/Invalid");
	ASSERT_TRUE(Result) << Result.Message;
	Durin::DTexture2D* Texture = Result.Asset;
	ASSERT_NE(Texture, nullptr);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	ASSERT_NE(Texture->GetSourceData(), nullptr);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureInvalidateTests/Invalid", AssetPath));
	const std::filesystem::path CopiedSource =
		Durin::Testing::GetTestWorkDirectory() / "TextureInvalidateMount"
		/ "Textures" / "Invalid.png";
	ASSERT_TRUE(std::filesystem::remove(CopiedSource));

	std::string Error;
	EXPECT_TRUE(Texture->PostLoad(Error)) << Error;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->WasLoadedFromDerivedDataCache());
	EXPECT_EQ(Texture->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::SourceUnavailableCached);
	EXPECT_TRUE(Texture->GetLastBuildError().empty());

	const Durin::FTexturePlatformData RetainedPlatformData = *Texture->GetPlatformData();
	const uint64 RetainedRevision = Texture->GetBuildRevision();
	{
		const std::array<uint8, 4> CorruptBytes = {1, 2, 3, 4};
		std::ofstream Stream(GetTextureCachePath(*Texture), std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	EXPECT_FALSE(Texture->PostLoad(Error));
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::MissingSource);
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), RetainedPlatformData);
	EXPECT_EQ(Texture->GetBuildRevision(), RetainedRevision);
	EXPECT_EQ(Texture->GetDerivedDataDiagnostic().Status,
		Durin::ETextureDerivedDataStatus::SourceUnavailable);

	WriteTextureFixture(CopiedSource);
	ASSERT_TRUE(Texture->PostLoad(Error)) << Error;
	ASSERT_TRUE(Durin::Asset::Build::WaitForTexture2DBuild(*Texture))
		<< Texture->GetLastBuildError();
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_NE(Texture->GetSourceData(), nullptr);
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
	const Durin::FEnumValue* MissingSource = BuildStatusEnum->FindValueRecordByValue(
		static_cast<uint64>(Durin::ETextureBuildStatus::MissingSource));
	const Durin::FEnumValue* Building = ResourceStateEnum->FindValueRecordByValue(
		static_cast<uint64>(Durin::ERenderResourceState::Building));
	ASSERT_NE(Unbuilt, nullptr);
	ASSERT_NE(MissingSource, nullptr);
	ASSERT_NE(Building, nullptr);
	EXPECT_EQ(Unbuilt->DisplayName, "Not Built");
	EXPECT_EQ(MissingSource->DisplayName, "Missing Source");
	EXPECT_EQ(Building->DisplayName, "Building");
	EXPECT_EQ(BuildStatusEnum->FindValueRecordByValue(255), nullptr);
}

TEST(FTexture2DTests, ScheduledInterchangeReimportPublishesOnceAndCancellationPreservesLastGood)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureAsyncUnloadCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureAsyncUnload.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Imported = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/AsyncUnload");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	const Durin::FTexturePlatformData LastGood = *Texture->GetPlatformData();
	const uint64 LastGoodRevision = Texture->GetBuildRevision();

	const Durin::FTextureSourceDiagnostic SourceDiagnostic = Texture->InspectSource();
	ASSERT_EQ(SourceDiagnostic.Status, Durin::ETextureSourceStatus::Available);
	WriteNpotTextureFixture(SourceDiagnostic.PhysicalPath);
	std::string Error;
	Durin::Asset::FInterchangeProvenance Existing;
	ASSERT_TRUE(Durin::Asset::Forge::InspectTexture2DInterchangeProvenance(
		*Texture, Existing, Error)) << Error;
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/AsyncUnload", AssetPath));
	Durin::FTexture2DImportSettings Settings{
		.Usage = Texture->GetUsage(),
		.CompressionQuality = Texture->GetCompressionQuality(),
		.AlphaMipMode = Texture->GetAlphaMipMode(),
		.AlphaCoverageThreshold = Texture->GetAlphaCoverageThreshold(),
		.MaxResolution = Texture->GetMaxResolution(),
		.bSRGB = Texture->IsSRGB()};
	Durin::Asset::FInterchangeImportRequest Request;
	ASSERT_TRUE(Durin::Asset::Forge::MakeTexture2DInterchangeRequest(
		Texture->GetSourceImportData().Source.SourcePath, AssetPath, Settings,
		Durin::Asset::EInterchangeImportMode::Reimport,
		{.OwnerId = "Tests.Texture2D.ScheduledReimport",
			.ConflictIdentities = {AssetPath.ToString()}},
		Existing, Request, Error)) << Error;
	auto Handle = Durin::Asset::GetImportService().SubmitInterchangeImport(
		Request, "Scheduled Texture2D reimport");
	ASSERT_TRUE(Handle);
	Durin::Asset::FInterchangeImportResult Reimported;
	bool bReimportReady = false;
	const auto ReimportDeadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(5);
	while (!(bReimportReady = Handle.TryGetResult(Reimported))
		&& std::chrono::steady_clock::now() < ReimportDeadline)
	{
		(void)Durin::Asset::GetImportService().PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(bReimportReady);
	ASSERT_EQ(Reimported.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Reimported.Outcome.Diagnostic;
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetBuildRevision(), LastGoodRevision + 1);
	EXPECT_NE(Texture->GetPlatformData()->Mips.front().Pixels,
		LastGood.Mips.front().Pixels);

	const Durin::FTexturePlatformData Published = *Texture->GetPlatformData();
	const uint64 PublishedRevision = Texture->GetBuildRevision();
	Request.Owner.OwnerId = "Tests.Texture2D.CanceledReimport";
	auto CanceledHandle = Durin::Asset::GetImportService().SubmitInterchangeImport(
		std::move(Request), "Canceled Texture2D reimport");
	ASSERT_TRUE(CanceledHandle);
	ASSERT_TRUE(CanceledHandle.GetOperationHandle().RequestCancel());
	Durin::Asset::GetImportService().CancelAndDrainImportOperation(
		CanceledHandle.GetOperationHandle());
	Durin::Asset::FInterchangeImportResult Canceled;
	ASSERT_TRUE(CanceledHandle.TryGetResult(Canceled));
	EXPECT_EQ(Canceled.Outcome.State, Durin::Asset::EImportOperationState::Canceled);
	EXPECT_EQ(Texture->GetBuildRevision(), PublishedRevision);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), Published);

	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}

TEST(FTexture2DTests, InterchangePreviewIsDeterministicAndMatchesScheduledInspection)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureInterchangePreviewDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureInterchangePreview.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Imported = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/PreviewSeed");
	ASSERT_TRUE(Imported) << Imported.Message;

	Durin::FAssetPath PreviewPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/PreviewOnly", PreviewPath));
	Durin::Asset::FInterchangeImportRequest Request;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::MakeTexture2DInterchangeRequest(
		Imported.Asset->GetSourceImportData().Source.SourcePath, PreviewPath,
		{}, Durin::Asset::EInterchangeImportMode::Preview,
		{.OwnerId = "Tests.Texture2D.Preview",
			.ConflictIdentities = {PreviewPath.ToString()}},
		std::nullopt, Request, Error)) << Error;
	Request.Lifetime = Durin::Asset::EImportOperationLifetime::EphemeralPreview;
	auto& Service = Durin::Asset::GetImportService();
	const Durin::Asset::FInterchangeImportResult First =
		Service.RunInterchangeImportInline(Request, "Texture2D preview");
	const Durin::Asset::FInterchangeImportResult Second =
		Service.RunInterchangeImportInline(Request, "Repeated Texture2D preview");
	ASSERT_EQ(First.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< First.Outcome.Diagnostic;
	ASSERT_EQ(Second.Outcome.State, Durin::Asset::EImportOperationState::Succeeded)
		<< Second.Outcome.Diagnostic;
	EXPECT_EQ(First.Provenance, Second.Provenance);
	EXPECT_EQ(First.Inspection.TranslatedGraphFingerprint,
		Second.Inspection.TranslatedGraphFingerprint);
	EXPECT_EQ(First.Inspection.FactoryGraphFingerprint,
		Second.Inspection.FactoryGraphFingerprint);
	ASSERT_EQ(First.Inspection.Outputs.size(), 1u);
	EXPECT_EQ(First.Inspection.Outputs.front().StableIdentity, "texture2d");
	EXPECT_EQ(First.Inspection.Outputs.front().AssetPath, PreviewPath);
	EXPECT_EQ(Durin::Asset::FindAssetExact(PreviewPath), nullptr);

	auto Scheduled = Service.SubmitInterchangeImport(
		Request, "Scheduled Texture2D preview");
	ASSERT_TRUE(Scheduled);
	Durin::Asset::FInterchangeImportResult ScheduledResult;
	bool bPreviewReady = false;
	const auto PreviewDeadline = std::chrono::steady_clock::now()
		+ std::chrono::seconds(5);
	while (!(bPreviewReady = Scheduled.TryGetResult(ScheduledResult))
		&& std::chrono::steady_clock::now() < PreviewDeadline)
	{
		(void)Service.PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(bPreviewReady);
	ASSERT_EQ(ScheduledResult.Outcome.State,
		Durin::Asset::EImportOperationState::Succeeded)
		<< ScheduledResult.Outcome.Diagnostic;
	EXPECT_EQ(ScheduledResult.Provenance, First.Provenance);
	EXPECT_EQ(ScheduledResult.Inspection.Outputs, First.Inspection.Outputs);
	EXPECT_EQ(Durin::Asset::FindAssetExact(PreviewPath), nullptr);

	Durin::FAssetPath SeedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/PreviewSeed", SeedPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(SeedPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(SeedPath));
}

TEST(FTexture2DTests, InterchangeSaveFailureRestoresAuthoredRuntimeAndProvenanceState)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureInterchangeRollbackDdc");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureInterchangeRollback.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Imported = Durin::Asset::Forge::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/InterchangeRollback");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	const Durin::FTexture2DSourceImportData PriorSource = Texture->GetSourceImportData();
	const Durin::FTexturePlatformData PriorPlatform = *Texture->GetPlatformData();
	const std::string PriorKey = Texture->GetDerivedDataKey();
	const std::string PriorProvenance(Texture->GetInterchangeProvenance());
	const uint64 PriorRevision = Texture->GetBuildRevision();
	ASSERT_FALSE(Texture->GetPackage()->IsDirty());

	const Durin::FTextureSourceDiagnostic SourceDiagnostic = Texture->InspectSource();
	ASSERT_EQ(SourceDiagnostic.Status, Durin::ETextureSourceStatus::Available);
	WriteNpotTextureFixture(SourceDiagnostic.PhysicalPath);
	Durin::Asset::FInterchangeProvenance Existing;
	std::string Error;
	ASSERT_TRUE(Durin::Asset::Forge::InspectTexture2DInterchangeProvenance(
		*Texture, Existing, Error)) << Error;
	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/InterchangeRollback", AssetPath));
	const Durin::FTexture2DImportSettings Settings{
		.Usage = Texture->GetUsage(),
		.CompressionQuality = Texture->GetCompressionQuality(),
		.AlphaMipMode = Texture->GetAlphaMipMode(),
		.AlphaCoverageThreshold = Texture->GetAlphaCoverageThreshold(),
		.MaxResolution = Texture->GetMaxResolution(),
		.bSRGB = Texture->IsSRGB()};
	Durin::Asset::FInterchangeImportRequest Request;
	ASSERT_TRUE(Durin::Asset::Forge::MakeTexture2DInterchangeRequest(
		PriorSource.Source.SourcePath, AssetPath, Settings,
		Durin::Asset::EInterchangeImportMode::Reimport,
		{.OwnerId = "Tests.Texture2D.Rollback",
			.ConflictIdentities = {AssetPath.ToString()}},
		Existing, Request, Error)) << Error;
	Request.SaveOptions.ShouldFail = [](
		Durin::Asset::EAssetBundleSavePhase Phase, size_t) {
		return Phase == Durin::Asset::EAssetBundleSavePhase::StagePackage;
	};
	const Durin::Asset::FInterchangeImportResult Failed =
		Durin::Asset::GetImportService().RunInterchangeImportInline(
			std::move(Request), "Texture2D rollback injection");
	EXPECT_EQ(Failed.Outcome.State, Durin::Asset::EImportOperationState::Failed);
	EXPECT_EQ(Texture->GetSourceImportData(), PriorSource);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), PriorPlatform);
	EXPECT_EQ(Texture->GetDerivedDataKey(), PriorKey);
	// Resource revisions are monotonic scheduling tokens. Reversal schedules the
	// restored last-good resource instead of rewinding an already-issued token.
	EXPECT_GT(Texture->GetBuildRevision(), PriorRevision);
	EXPECT_EQ(Texture->GetInterchangeProvenance(), PriorProvenance);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAssetForTesting(AssetPath));
}
