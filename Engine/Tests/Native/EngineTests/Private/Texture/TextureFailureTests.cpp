#include "TextureTestSupport.h"
#include "Texture/Texture2DRenderResource.h"
#include "Texture/Texture2DAuthoringCoordinator.h"
#include "Texture/TextureCubeRenderResource.h"
#include "Texture/TextureRenderResource.h"

static_assert(std::is_base_of_v<
	Durin::FTextureAssetResource, Durin::FTexture2DResource>);
static_assert(std::is_base_of_v<
	Durin::FTextureAssetResource, Durin::FTextureCubeResource>);

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

TEST(FTexture2DTests, RejectsUnsupportedSourceWithoutCreatingAsset)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	const std::filesystem::path Source = Durin::Testing::GetTestWorkDirectory() / "UnsupportedTexture.gif";
	std::ofstream(Source, std::ios::binary | std::ios::trunc) << "not an image";
	Durin::FTexture2DImportResult Result = Durin::StandardAssetImport::ImportTexture2DAsset(Source.generic_string(), "/TextureImportTests/Unsupported");
	EXPECT_FALSE(Result);
	EXPECT_EQ(Result.Asset, nullptr);
	EXPECT_FALSE(Result.Message.empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureImportTests/Unsupported", AssetPath));
	EXPECT_EQ(Durin::Asset::GetAssetRegistry().FindAssetExact(AssetPath), nullptr);
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
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
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
	const Durin::FTexture2DImportResult Result = Durin::StandardAssetImport::ImportTexture2DAsset(Source.generic_string(), "/TextureFailureTests/Ready");
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Result.Asset->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_TRUE(Result.Asset->GetLastBuildError().empty());

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate("/TextureFailureTests/Ready", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
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
	const Durin::FTexture2DImportResult Result = Durin::StandardAssetImport::ImportTexture2DAsset(Source.generic_string(), "/TextureInvalidateTests/Invalid");
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
	const Durin::uint64 RetainedRevision = Texture->GetBuildRevision();
	{
		const std::array<Durin::uint8, 4> CorruptBytes = {1, 2, 3, 4};
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
	ASSERT_TRUE(Durin::AssetBuild::WaitForTexture2DBuild(*Texture))
		<< Texture->GetLastBuildError();
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_NE(Texture->GetSourceData(), nullptr);
	EXPECT_NE(Texture->GetPlatformData(), nullptr);
	EXPECT_TRUE(Texture->GetLastBuildError().empty());

	ASSERT_TRUE(Durin::Asset::SavePackage(Texture->GetPackage()));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
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
		static_cast<Durin::uint64>(Durin::ETextureBuildStatus::Unbuilt));
	const Durin::FEnumValue* MissingSource = BuildStatusEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ETextureBuildStatus::MissingSource));
	const Durin::FEnumValue* Building = ResourceStateEnum->FindValueRecordByValue(
		static_cast<Durin::uint64>(Durin::ERenderResourceState::Building));
	ASSERT_NE(Unbuilt, nullptr);
	ASSERT_NE(MissingSource, nullptr);
	ASSERT_NE(Building, nullptr);
	EXPECT_EQ(Unbuilt->DisplayName, "Not Built");
	EXPECT_EQ(MissingSource->DisplayName, "Missing Source");
	EXPECT_EQ(Building->DisplayName, "Building");
	EXPECT_EQ(BuildStatusEnum->FindValueRecordByValue(255), nullptr);
}

TEST(FTexture2DTests, PendingReimportPreservesLastGoodAndCannotCommitAfterUnload)
{
	InitializeDObjectSystem();
	FScopedDerivedDataCacheRoot CacheRoot(
		Durin::Testing::GetTestWorkDirectory() / "TextureAsyncUnloadCache");
	const std::filesystem::path Source =
		Durin::Testing::GetTestWorkDirectory() / "TextureAsyncUnload.png";
	WriteTextureFixture(Source);
	const Durin::FTexture2DImportResult Imported = Durin::StandardAssetImport::ImportTexture2DAsset(
		Source.generic_string(), "/TextureImportTests/AsyncUnload");
	ASSERT_TRUE(Imported) << Imported.Message;
	Durin::DTexture2D* Texture = Imported.Asset;
	ASSERT_NE(Texture, nullptr);
	const Durin::FTexturePlatformData LastGood = *Texture->GetPlatformData();
	const Durin::uint64 LastGoodRevision = Texture->GetBuildRevision();

	ASSERT_TRUE(Durin::AssetBuild::InitializeTexture2DBuildCoordinator());
	Durin::AssetBuild::FTexture2DBuildCoordinator* Coordinator =
		Durin::AssetBuild::GetTexture2DBuildCoordinator();
	ASSERT_NE(Coordinator, nullptr);
	std::mutex Mutex;
	std::condition_variable Condition;
	bool bEntered = false;
	bool bRelease = false;
	Coordinator->SetPhaseHookForTests(
		[&](Durin::uint64, Durin::AssetBuild::ETexture2DBuildPhase Phase) {
			if (Phase != Durin::AssetBuild::ETexture2DBuildPhase::Preparing) return;
			std::unique_lock Lock(Mutex);
			if (bEntered) return;
			bEntered = true;
			Condition.notify_all();
			Condition.wait(Lock, [&] { return bRelease; });
		});
	const Durin::FTextureSourceDiagnostic SourceDiagnostic = Texture->InspectSource();
	ASSERT_EQ(SourceDiagnostic.Status, Durin::ETextureSourceStatus::Available);
	WriteNpotTextureFixture(SourceDiagnostic.PhysicalPath);
	std::string Error;
	ASSERT_TRUE(Durin::StandardAssetImport::ReimportTexture2DSource(
		*Texture, {}, Error)) << Error;
	{
		std::unique_lock Lock(Mutex);
		ASSERT_TRUE(Condition.wait_for(Lock, std::chrono::seconds(10), [&] {
			return bEntered;
		}));
	}
	const Durin::uint64 RequestId =
		Durin::AssetBuild::GetTexture2DBuildDiagnostic(*Texture).RequestId;
	ASSERT_NE(RequestId, 0u);
	EXPECT_EQ(Texture->GetBuildStatus(), Durin::ETextureBuildStatus::Ready);
	EXPECT_EQ(Texture->GetBuildRevision(), LastGoodRevision);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), LastGood);

	Durin::FAssetPath AssetPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/TextureImportTests/AsyncUnload", AssetPath));
	ASSERT_TRUE(Durin::Asset::UnloadPackage(AssetPath));
	{
		std::lock_guard Lock(Mutex);
		bRelease = true;
		Condition.notify_all();
	}
	Coordinator->SetPhaseHookForTests({});
	ASSERT_TRUE(Coordinator->WaitForRequest(RequestId, 10.0));
	Coordinator->PumpCompletions();
	EXPECT_EQ(
		Coordinator->GetDiagnostic(RequestId).Phase,
		Durin::AssetBuild::ETexture2DBuildPhase::UploadPending);
	ASSERT_TRUE(Durin::Asset::DeleteAsset(AssetPath));
}
