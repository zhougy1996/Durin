#include "TextureResourceUpdateTestSupport.h"
#include "NativeAssetTestSupport.h"
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
		Mip.Pixels.resize(static_cast<size_t>(Layout.DataSize));
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

TEST(FTexture2DTests, LoadPublishesTextureWhenPostLoadBuildProviderIsUnavailable)
{
	InitializeDObjectSystem();
	InitializeTextureImportMount();
	ASSERT_TRUE(EnsureTextureCompilingManager());
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate("/TextureImportTests/UnavailableProvider", AssetPath));
	Durin::DTexture2D* Texture = nullptr;
	ASSERT_TRUE(Durin::CreatePackageLeafAssetForTesting(AssetPath, Texture));
	Durin::Image::FImage SourceImage;
	EXPECT_TRUE(Durin::Image::FImage::TryCreate({.Width = 1, .Height = 1,
		.Format = Durin::Image::ERawImageFormat::RGBA8}, Durin::FByteBuffer(4), SourceImage));
	Durin::FTextureSource Source;
	EXPECT_TRUE(Source.Init2D(SourceImage.GetView(), 4));
	std::string Error;
	ASSERT_TRUE(Texture->SetSource(std::move(Source), Error)) << Error;
	const auto Saved = Durin::SavePackage(Texture->GetPackage());
	ASSERT_TRUE(Saved) << Saved.Message;
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	auto& Modules = Durin::FModuleManager::Get();
	ASSERT_TRUE(Modules.UnloadModule("TextureBuild").Succeeded());
	struct FRestoreProvider
	{
		~FRestoreProvider() { Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild"); }
	} RestoreProvider;

	Durin::DTexture2D* Loaded = nullptr;
	const auto Result = Durin::LoadObject(
		Durin::Testing::MakePackageLeafAssetObjectPathForTests(AssetPath), Loaded);
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_NE(Loaded, nullptr);
	EXPECT_FALSE(Loaded->HasPlatformData());
	EXPECT_FALSE(Loaded->EnsurePlatformDataLoadedBlocking());
	EXPECT_EQ(Durin::FindResidentPackage(AssetPath), Loaded->GetPackage());

	Modules.LoadModuleChecked("TextureBuild");
	Loaded->PostLoad();
	EXPECT_TRUE(Loaded->HasPlatformData());
	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
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
	Durin::Testing::FTextureUpdateRequestRecorder ResourceRequests;
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
	const uint64 RetainedRequestCount = ResourceRequests.Count(*Texture);
	{
		const std::array<uint8, 4> CorruptBytes = {1, 2, 3, 4};
		std::ofstream Stream(GetTextureCachePath(*Texture), std::ios::binary | std::ios::trunc);
		Stream.write(reinterpret_cast<const char*>(CorruptBytes.data()), CorruptBytes.size());
	}
	Texture->PostLoad();
	EXPECT_TRUE(Texture->HasPlatformData());
	ASSERT_NE(Texture->GetPlatformData(), nullptr);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), RetainedPlatformData);
	EXPECT_GT(ResourceRequests.Count(*Texture), RetainedRequestCount);
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

TEST(FTexture2DTests, RenderStatusEnumExposesSharedDisplayMetadata)
{
	InitializeDObjectSystem();
	Durin::DEnum* ResourceStateEnum = Durin::FindEnumByQualifiedName("Durin::ETextureResourceUpdateState");
	ASSERT_NE(ResourceStateEnum, nullptr);
	EXPECT_EQ(ResourceStateEnum->GetDisplayName(), "Texture Resource Update State");

	const Durin::FEnumValue* Building = ResourceStateEnum->FindValueRecordByValue(
		static_cast<uint64>(Durin::ETextureResourceUpdateState::Building));
	ASSERT_NE(Building, nullptr);
	EXPECT_EQ(Building->DisplayName, "Building");
	EXPECT_EQ(ResourceStateEnum->FindValueRecordByValue(255), nullptr);
}

TEST(FTexture2DTests, ScheduledReimportPublishesOnce)
{
	Durin::Testing::FTextureUpdateRequestRecorder ResourceRequests;
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
	const uint64 LastGoodRequestCount = ResourceRequests.Count(*Texture);

	WriteNpotTextureFixture(Source);
	std::string Error;
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/AsyncUnload", AssetPath));
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportTexture2D(
		*Texture, Error)) << Error;
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	EXPECT_TRUE(Texture->HasPlatformData());
	EXPECT_EQ(ResourceRequests.Count(*Texture), LastGoodRequestCount + 1);
	EXPECT_NE(Texture->GetPlatformData()->Mips.front().Pixels,
		LastGood.Mips.front().Pixels);

	ASSERT_TRUE(Durin::UnloadPackage(AssetPath));
	ASSERT_TRUE(Durin::Testing::RemoveAssetPackageForTests(AssetPath));
}

TEST(FTexture2DTests, DirectReimportPublishesAndSaves)
{
	Durin::Testing::FTextureUpdateRequestRecorder ResourceRequests;
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
	const uint64 PriorRequestCount = ResourceRequests.Count(*Texture);
	ASSERT_FALSE(Texture->GetPackage()->IsDirty());

	WriteNpotTextureFixture(Source);
	Durin::FPackagePath AssetPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/TextureImportTests/ImportRollback", AssetPath));
	Durin::FReimportResult Reimported;
	Durin::FReimportManager::Reimport(*Texture, {},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	ASSERT_TRUE(Durin::WaitForTexture2DCompilation(*Texture, 10.0));
	ASSERT_TRUE(Reimported) << Reimported.Message;
	ASSERT_NE(Texture->GetAssetImportData(), nullptr);
	ImportedSource = Texture->GetAssetImportData()->GetSourceData().FindByRole("source");
	ASSERT_NE(ImportedSource, nullptr);
	EXPECT_EQ(ImportedSource->Hint, PriorSource);
	EXPECT_NE(Texture->GetPlatformData()->Mips.front().Pixels,
		PriorPlatform.Mips.front().Pixels);
	EXPECT_NE(GetTextureDerivedDataKey(*Texture), PriorKey);
	EXPECT_EQ(ResourceRequests.Count(*Texture), PriorRequestCount + 1);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());

	const Durin::FTexturePlatformData LastGood = *Texture->GetPlatformData();
	const uint64 LastGoodRequestCount = ResourceRequests.Count(*Texture);
	const std::filesystem::path Corrupt =
		Durin::Testing::GetTestWorkDirectory() / "TextureManagerCorrupt.png";
	const std::array CorruptBytes{std::byte{0x01}, std::byte{0x02}};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(CorruptBytes, Corrupt));
	const std::array Files{Corrupt.generic_string()};
	Durin::FReimportManager::ReimportFromFiles(*Texture, Files, {.bSave = false},
		[&](Durin::FReimportResult Result) { Reimported = std::move(Result); });
	EXPECT_EQ(Reimported.Status, Durin::EReimportStatus::SourceOrBuildFailure);
	EXPECT_EQ(ResourceRequests.Count(*Texture), LastGoodRequestCount);
	ExpectPlatformDataEqual(*Texture->GetPlatformData(), LastGood);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
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

	class FTextureResourceUpdateTests : public testing::Test
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
			Durin::TryEnqueueRenderCommand("DrainOwnedTextureTestResources", [](Durin::FRHICommandListImmediate&) {
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
		auto Start(const std::shared_ptr<Durin::FTextureResourceUpdate>& Update,
			Durin::FTextureReference& Reference, bool bInitialize = false) -> void
		{
			Durin::TryEnqueueRenderCommand("OwnedTextureTestUpdate", [Update, &Reference, bInitialize](Durin::FRHICommandListImmediate& Commands) {
				Update->Execute_RenderThread(Commands, Reference, bInitialize);
			});
		}
		auto Retire(const std::shared_ptr<Durin::FTextureResourceUpdate>& Update) -> void
		{
			Update->Wait();
			auto Candidate = Update->TakeCandidate();
			if (Candidate->IsInitialized())
			{
				Candidate->BeginRelease_GameThread();
				Durin::BeginCleanupRenderResource(Durin::FDeferredRenderResourceCleanup(std::move(Candidate)));
			}
			Durin::FlushRenderingCommands();
		}
		auto Resolve(Durin::FTextureReference& Reference) -> Durin::FRHITexture*
		{
			Durin::FRHITexture* Result = nullptr;
			Durin::TryEnqueueRenderCommand("ResolveOwnedTextureTest", [&Reference, &Result](Durin::FRHICommandListImmediate&) {
				Result = Reference.GetReferencedTexture_RenderThread();
			});
			Durin::FlushRenderingCommands();
			return Result;
		}
	};
}

TEST_F(FTextureResourceUpdateTests, FailedReplacementRetainsAllocationAndLateReleasePreservesNewTarget)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations OldEvents, FailedEvents, NewEvents;
	auto Old = std::make_shared<Durin::FTextureResourceUpdate>(std::make_unique<FUpdateTestResource>(Reference, OldEvents));
	Start(Old, Reference, true);
	Old->Wait();
	auto OldSnapshot = Old->GetPublishedTexture();
	EXPECT_NE(OldSnapshot, nullptr);
	auto Failed = std::make_shared<Durin::FTextureResourceUpdate>(std::make_unique<FUpdateTestResource>(Reference, FailedEvents,
		true));
	Start(Failed, Reference);
	Failed->Wait();
	EXPECT_EQ(Failed->GetState(), Durin::ETextureResourceUpdateState::Failed);
	EXPECT_EQ(Resolve(Reference), OldSnapshot.GetReference());
	Retire(Failed);
	EXPECT_EQ(FailedEvents.Released, 1);
	EXPECT_EQ(FailedEvents.Destroyed, 1);
	auto New = std::make_shared<Durin::FTextureResourceUpdate>(std::make_unique<FUpdateTestResource>(Reference, NewEvents));
	Start(New, Reference);
	New->Wait();
	auto NewSnapshot = New->GetPublishedTexture();
	EXPECT_NE(NewSnapshot, OldSnapshot);
	Retire(Old);
	EXPECT_EQ(Resolve(Reference), NewSnapshot.GetReference());
	EXPECT_EQ(Failed->GetState(), Durin::ETextureResourceUpdateState::Failed);
	EXPECT_EQ(OldEvents.Released, 1);
	EXPECT_EQ(OldEvents.Destroyed, 1);
	Retire(New);
	EXPECT_EQ(Resolve(Reference), nullptr);
	EXPECT_EQ(NewEvents.Destroyed, 1);
	EXPECT_EQ(Durin::GetNumInitializedRenderResources(), 1u);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST_F(FTextureResourceUpdateTests, SupersededAndClosedSuccessorsNeverInitialize)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations ActiveEvents, SupersededEvents, LatestEvents;
	auto Update = std::make_shared<Durin::FTextureResourceUpdate>(
		std::make_unique<FUpdateTestResource>(Reference, ActiveEvents));
	Update->SetSuccessor(std::make_unique<FUpdateTestResource>(Reference, SupersededEvents));
	Update->SetSuccessor(std::make_unique<FUpdateTestResource>(Reference, LatestEvents));
	EXPECT_EQ(SupersededEvents.Destroyed, 1);
	EXPECT_EQ(SupersededEvents.Initialized, 0);
	Update->Close();
	EXPECT_EQ(LatestEvents.Destroyed, 1);
	EXPECT_EQ(LatestEvents.Initialized, 0);
	Start(Update, Reference, true);
	Update->Wait();
	EXPECT_EQ(Update->TakeSuccessor(), nullptr);
	Retire(Update);
	EXPECT_EQ(ActiveEvents.Initialized, 0);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST_F(FTextureResourceUpdateTests, CloseDuringInitializationPreventsPublication)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations Events;
	std::latch Started(1), Resume(1);
	auto Update = std::make_shared<Durin::FTextureResourceUpdate>(std::make_unique<FUpdateTestResource>(Reference, Events,
		false, [&]() { Started.count_down(); Resume.wait(); }));
	Start(Update, Reference, true);
	Started.wait();
	Update->Close();
	Resume.count_down();
	Update->Wait();
	EXPECT_EQ(Update->GetState(), Durin::ETextureResourceUpdateState::Closed);
	EXPECT_EQ(Update->GetPublishedTexture(), nullptr);
	EXPECT_EQ(Resolve(Reference), nullptr);
	Retire(Update);
	EXPECT_EQ(Events.Initialized, 1);
	EXPECT_EQ(Events.Released, 1);
	EXPECT_EQ(Events.Destroyed, 1);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST_F(FTextureResourceUpdateTests, CloseBeforeExecutionSkipsCandidateInitialization)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations Events;
	auto Update = std::make_shared<Durin::FTextureResourceUpdate>(std::make_unique<FUpdateTestResource>(Reference, Events));
	Update->Close();
	Start(Update, Reference, true);
	Update->Wait();
	EXPECT_EQ(Resolve(Reference), nullptr);
	Retire(Update);
	EXPECT_EQ(Events.Initialized, 0);
	EXPECT_EQ(Events.Released, 0);
	EXPECT_EQ(Events.Destroyed, 1);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST(FTextureResourceAdmissionTests, MissingPlatformDataDoesNotAdmitWork)
{
	InitializeDObjectSystem();
	Durin::Testing::FTextureUpdateRequestRecorder Requests;
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "MissingPlatformData");
	Texture->UpdateResource();
	EXPECT_EQ(Requests.Count(*Texture), 0u);
	EXPECT_FALSE(Texture->IsResourceUpdatePending());
	EXPECT_FALSE(Texture->HasUsableResource());
	EXPECT_EQ(Texture->GetResourceUpdateState(), Durin::ETextureResourceUpdateState::Idle);
}

TEST(FTextureResourceAdmissionTests, CoalescesSuccessorsAndAdvancesWithoutGettersOrTaskExecutor)
{
	InitializeDObjectSystem();
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	auto* Texture = Durin::NewObject<Durin::DTexture2D>(nullptr, "CoalescedResourceInputs");
	Texture->SetPlatformData(std::make_unique<Durin::FTexturePlatformData>(MakeSingleMipPlatformData()));
	int Completed = 0;
	const auto Handle = Durin::OnTextureResourceChanged().AddLambda([&](Durin::DTexture& Changed, Durin::ETextureResourceChange Change) {
		if (&Changed == Texture && Change == Durin::ETextureResourceChange::Completed) ++Completed;
	});
	Texture->UpdateResource();
	Texture->UpdateResource();
	Texture->UpdateResource();
	EXPECT_TRUE(Texture->IsResourceUpdatePending());
	EXPECT_EQ(Completed, 0);
	Durin::PumpTextureResourceUpdates();
	EXPECT_EQ(Completed, 1);
	EXPECT_TRUE(Texture->IsResourceUpdatePending());
	Durin::PumpTextureResourceUpdates();
	EXPECT_EQ(Completed, 2);
	EXPECT_FALSE(Texture->IsResourceUpdatePending());
	EXPECT_EQ(Texture->GetResourceUpdateState(), Durin::ETextureResourceUpdateState::Failed);
	Durin::PumpTextureResourceUpdates();
	EXPECT_EQ(Completed, 2);
	Durin::OnTextureResourceChanged().Remove(Handle);
}

TEST_F(FTextureResourceUpdateTests, InitializationExceptionStillHandsOffCleanup)
{
	Durin::FTextureReference Reference;
	FUpdateResourceObservations Events;
	auto Update = std::make_shared<Durin::FTextureResourceUpdate>(std::make_unique<FUpdateTestResource>(Reference, Events,
		false, []() { throw std::runtime_error("controlled initialization failure"); }));
	Start(Update, Reference, true);
	Update->Wait();
	EXPECT_EQ(Update->GetState(), Durin::ETextureResourceUpdateState::Failed);
	EXPECT_EQ(Resolve(Reference), nullptr);
	Retire(Update);
	EXPECT_EQ(Events.Released, 1);
	EXPECT_EQ(Events.Destroyed, 1);
	Reference.BeginRelease_GameThread();
	Durin::FlushRenderingCommands();
}

TEST(FTextureResourceAdmissionTests, CompletionListenerCanDestroyAnotherPendingOwner)
{
	InitializeDObjectSystem();
	ASSERT_EQ(Durin::GDynamicRHI, nullptr);
	auto* First = Durin::NewObject<Durin::DTexture2D>(nullptr, "FirstPendingTexture");
	auto* Second = Durin::NewObject<Durin::DTexture2D>(nullptr, "SecondPendingTexture");
	Durin::AddToRoot(First);
	Durin::AddToRoot(Second);
	First->SetPlatformData(std::make_unique<Durin::FTexturePlatformData>(MakeSingleMipPlatformData()));
	Second->SetPlatformData(std::make_unique<Durin::FTexturePlatformData>(MakeSingleMipPlatformData()));
	First->UpdateResource();
	Second->UpdateResource();
	int Completions = 0;
	const auto Handle = Durin::OnTextureResourceChanged().AddLambda([&](Durin::DTexture& Texture, Durin::ETextureResourceChange Change) {
		if (Change != Durin::ETextureResourceChange::Completed) return;
		if (&Texture == First)
		{
			++Completions;
			Durin::RemoveFromRoot(Second);
			Durin::MarkAsGarbage(Second);
			Durin::CollectGarbage();
			Durin::PumpTextureResourceUpdates(); // Reentrant pumping is harmless.
		}
		else if (&Texture == Second) ++Completions;
	});
	Durin::PumpTextureResourceUpdates();
	EXPECT_EQ(Completions, 1);
	EXPECT_FALSE(First->IsResourceUpdatePending());
	Durin::OnTextureResourceChanged().Remove(Handle);
	Durin::RemoveFromRoot(First);
	Durin::MarkAsGarbage(First);
	Durin::CollectGarbage();
}
