#include "Import/TextureFileImport.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/PackageInspection.h"
#include "Asset/EditorBulkDataStorage.h"
#include "AssetRegistry/Publication.h"
#include "AssetTools/IAssetTools.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Texture/TexturePlatformDataTestFixtures.h"

#include <gtest/gtest.h>
#include <future>

namespace
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;

	auto MakeNormalPixels(bool bVaried) -> FByteBuffer
	{
		FByteBuffer Bytes(16 * 4);
		for (size_t Index = 0; Index < 16; ++Index)
		{
			Bytes[Index * 4] = std::byte(bVaried ? (Index % 2 ? 153 : 102) : 128);
			Bytes[Index * 4 + 1] = std::byte(128);
			Bytes[Index * 4 + 2] = std::byte(bVaried ? 252 : 255);
			Bytes[Index * 4 + 3] = std::byte(255);
		}
		return Bytes;
	}
}

TEST(FTextureFileImportTests, InfersSemanticFilenameTokensWithoutSubstringMatches)
{
	EXPECT_EQ(InferTexture2DImportSettings("/art/Brick_NORMAL.PNG").Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_n.tga").Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_ORM.png").Usage, ETextureUsage::DataMask);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_roughness.png").Usage, ETextureUsage::DataMask);
	EXPECT_EQ(InferTexture2DImportSettings("/normal/abnormal.png").Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("Brick_basecolor.png").Usage, ETextureUsage::Color);
}

TEST(FTextureFileImportTests, RecognizesNormalVectorsButRespectsExplicitColorAndMaskNames)
{
	Image::FImage Image;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakeNormalPixels(true), Image));
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 3));
	EXPECT_EQ(InferTexture2DImportSettings("unnamed.png", &Source).Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("wall_albedo.png", &Source).Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("wall_orm.png", &Source).Usage, ETextureUsage::DataMask);
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakeNormalPixels(false), Image));
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 3));
	EXPECT_EQ(InferTexture2DImportSettings("purple.png", &Source).Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("flat_normal.png", &Source).Usage, ETextureUsage::Normal);
}

TEST(FTextureFileImportTests, ImportsNormalSavesAndChoosesAvailableNames)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	FModuleManager::Get().LoadModuleChecked("TextureBuild");
	const auto Root = Testing::GetTestWorkDirectory() / "TextureFileImport";
	std::filesystem::create_directories(Root / "Content");
	Testing::RegisterMountPointForTests("/TextureFileImport/", (Root / "Content").generic_string() + "/");
	FScopedDerivedDataCacheRoot Cache(Root / "Ddc");
	const auto Source = Root / "unnamed.tga";
	FByteBuffer Tga(18);
	Tga[2] = std::byte(2); Tga[12] = std::byte(4); Tga[14] = std::byte(4);
	Tga[16] = std::byte(32); Tga[17] = std::byte(0x28);
	auto Pixels = MakeNormalPixels(true);
	for (size_t Index = 0; Index < Pixels.size(); Index += 4) std::swap(Pixels[Index], Pixels[Index + 2]);
	Tga.insert(Tga.end(), Pixels.begin(), Pixels.end());
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Tga, Source));
	// Occupied but uncataloged files must also reserve their names.
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Tga, Root / "Content" / "unnamed.dasset"));
	std::vector<std::string> Published;
	std::string Error;
	Editor::Texture::FTextureFileImport Importer({
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
		.AssetCreated = [&](std::string Path) { Published.push_back(std::move(Path)); }});
	for (int Index = 1; Index <= 2; ++Index)
	{
		const auto Result = Importer.ImportFile(Source.generic_string(), "/TextureFileImport/");
		ASSERT_TRUE(Result) << Error;
		ASSERT_NE(Result.Asset, nullptr);
		const auto* Texture = Cast<DTexture2D>(Result.Asset);
		ASSERT_NE(Texture, nullptr);
		EXPECT_EQ(Texture->GetUsage(), ETextureUsage::Normal);
		EXPECT_FALSE(Texture->IsSRGB());
		EXPECT_EQ(Result.Persistence, EAssetOperationPersistenceState::Persisted);
		EXPECT_EQ(Published.back(), std::format("/TextureFileImport/unnamed_{}", Index));
		EXPECT_TRUE(UnloadPackage(Result.Package));
	}
	EXPECT_FALSE(Importer.HasPendingSaves());
	EXPECT_EQ(Published.size(), 2u);
	EXPECT_EQ(std::filesystem::file_size(Source), Tga.size());
	const auto Invalid = Importer.ImportFile((Root / "missing.png").generic_string(), "/TextureFileImport/");
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Published.size(), 2u);

	int SaveAttempts = 0;
	Editor::Texture::FTextureFileImport RetryImporter({
		.AssetCreated = [&](std::string Path) { Published.push_back(std::move(Path)); }},
		[&](const FAssetSaveRequest& Request) -> FAssetOperationResult {
			if (++SaveAttempts == 1) return {.Kind = EAssetOperationKind::Save,
				.State = EAssetOperationTerminalState::Rejected, .Message = "Injected save failure"};
			return IAssetTools::Get().SaveAssets(Request);
		});
	const auto Unsaved = RetryImporter.ImportFile(Source.generic_string(), "/TextureFileImport/");
	EXPECT_FALSE(Unsaved);
	ASSERT_NE(Unsaved.Asset, nullptr);
	EXPECT_EQ(Unsaved.Persistence, EAssetOperationPersistenceState::Dirty);
	EXPECT_TRUE(RetryImporter.HasPendingSaves());
	EXPECT_EQ(Published.size(), 2u);
	ASSERT_TRUE(std::filesystem::remove(Source));
	RetryImporter.RetryPendingSaves();
	EXPECT_FALSE(RetryImporter.HasPendingSaves());
	EXPECT_EQ(SaveAttempts, 2);
	ASSERT_EQ(Published.size(), 3u);
	EXPECT_EQ(Published.back(), "/TextureFileImport/unnamed_3");
}

namespace
{
	class FTextureImportQueueTests : public testing::Test
	{
	protected:
		std::filesystem::path Root, Source;
		std::string Destination;
		std::unique_ptr<FScopedDerivedDataCacheRoot> Cache;
		auto MakeSaveTexture(std::string_view Name = "save") -> DTexture2D*
		{
			FPackagePath Path;
			if (!FPackagePath::TryCreate(Destination + std::string(Name), Path)) return nullptr;
			DTexture2D* Texture = nullptr;
			if (!CreatePackageLeafAssetForTesting(Path, Texture)) return nullptr;
			Image::FImage Image;
			if (!Image::FImage::TryCreate({.Width = 512, .Height = 512,
				.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(512 * 512 * 4, std::byte{17}), Image)) return nullptr;
			FTextureSource Source;
			if (!Source.Init2D(Image.GetView(), 4, 0, ETextureSourceCompression::Raw)) return nullptr;
			Texture->SetSource(Source);
			auto Platform = std::make_unique<FTexturePlatformData>();
			Platform->PixelFormat = EPixelFormat::RGBA8_UNORM;
			Platform->Mips.push_back({.Pixels = FByteBuffer(4, std::byte{17}), .Width = 1, .Height = 1, .RowPitch = 4});
			Texture->SetPlatformData(std::move(Platform));
			return Texture;
		}
		template<typename T> auto WaitForSave(T& Save) -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!Save.IsReady() && std::chrono::steady_clock::now() < Deadline)
			{ PumpGameThreadDeferredWork(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
			return Save.IsReady();
		}
		auto WaitForSave(Tasks::TTask<FAssetResult>&) -> bool
		{ return DPackage::WaitForAsyncFileWrites().WaitStatus == ETaskWaitStatus::Completed; }
		auto FinishSave(Tasks::TTask<FAssetResult>& Save) -> FAssetResult
		{
			EXPECT_TRUE(DPackage::DrainAsyncSaves());
			if (Save.GetState() != ETaskState::Succeeded) return {EAssetError::IoError, "Task execution failed."};
			return Save.GetResult();
		}
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			if (!GetGameThreadDeferredWorkQueueDiagnostics().bInstalled) ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			PackageSavePrivate::SetAsyncSaveAdmission(true);
			if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
			FModuleManager::Get().LoadModuleChecked("TextureBuild");
			const std::string TestName = testing::UnitTest::GetInstance()->current_test_info()->name();
			Root = Testing::CreateTestFixtureDirectory(TestName);
			Destination = "/" + TestName + "/";
			std::filesystem::create_directories(Root / "Content");
			Testing::RegisterMountPointForTests(Destination, (Root / "Content").generic_string() + "/");
			Cache = std::make_unique<FScopedDerivedDataCacheRoot>(Root / "Ddc");
			Source = Root / "wall_normal.tga";
			FByteBuffer Tga(18);
			Tga[2] = std::byte(2); Tga[12] = std::byte(4); Tga[14] = std::byte(4);
			Tga[16] = std::byte(32); Tga[17] = std::byte(0x28);
			auto Pixels = MakeNormalPixels(true);
			for (size_t Index = 0; Index < Pixels.size(); Index += 4) std::swap(Pixels[Index], Pixels[Index + 2]);
			Tga.insert(Tga.end(), Pixels.begin(), Pixels.end());
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(Tga, Source));
		}
		auto Drain(Editor::Texture::FTextureFileImport& Importer) -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (Importer.IsRunning() && std::chrono::steady_clock::now() < Deadline)
			{
				FAssetCompilingManager::Get().ProcessAsyncTasks();
				PumpGameThreadDeferredWork();
				Importer.Tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return !Importer.IsRunning();
		}
		auto TearDown() -> void override
		{
			EXPECT_TRUE(DPackage::DrainAsyncSaves());
			SetAsyncPackageSaveSink({});
			Private::SetDirectPackageWriteFailureForTests({});
			AssetPrivate::SetAsyncSavePublicationFailureForTests(false);
			PackageSavePrivate::SetAsyncSaveAdmission(true);
		}
	};
}

TEST_F(FTextureImportQueueTests, AsyncSavePublishesVerifiedBulkOnlyOnCompletion)
{
	auto* Texture = MakeSaveTexture();
	ASSERT_NE(Texture, nullptr);
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(Texture->GetPackage()->GetPackagePath(), Path));
	int Published = 0;
	FAssetOperationResult Admission;
	std::unique_ptr<FAssetSaveOperation> Save;
	Save = FAssetSaveOperation::Begin({.AssetPaths = {Path},
		.Publish = [&](const auto&) { ++Published; EXPECT_TRUE(Save->Complete()); }}, Admission);
	ASSERT_TRUE(Save) << Admission.Message;
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dbulk"));
	EXPECT_EQ(Published, 0);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	ASSERT_TRUE(WaitForSave(*Save));
	const auto Result = Save->Complete();
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_TRUE(Save->Complete());
	EXPECT_EQ(Published, 1);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	FByteBuffer PackageBytes, BulkBytes;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(PackageBytes, Root / "Content/save.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(BulkBytes, Root / "Content/save.dbulk"));
	EXPECT_TRUE(ValidateAssetPackageBytes(PackageBytes, Path, BulkBytes));
}

TEST_F(FTextureImportQueueTests, SavePathsRejectUnsupportedVersionBeforeStaging)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	auto* Package = Texture->GetPackage();
	ASSERT_TRUE(SavePackage(Package));
	const auto Expected = CaptureAssetRegistryPublication();
	auto Unsupported = Expected.Assets.at(Package->GetPackagePathIdentity());
	Unsupported.FormatVersion = 0;
	ASSERT_TRUE(AssetsSaved({Unsupported}, Expected));
	Package->MarkDirty();
	// Rejected saves must leave existing files untouched, including asynchronous Begin().
	const auto Bulk = Root / "Content/save.dbulk";
	const auto Backup = std::filesystem::path(Bulk.string() + std::string(EditorBulkDataCompanionBackupSuffix));
	std::filesystem::rename(Bulk, Backup);
	EXPECT_EQ(SavePackage(Package).Error, EAssetError::UnsupportedVersion);
	DPackage* Packages[] = {Package};
	EXPECT_EQ(SavePackagesAtomically(Packages, {}).Error, EAssetError::UnsupportedVersion);
	FAssetResult Admission;
	auto Save = (Package)->SaveAsync(Admission, FAssetPackageSaveContext{});
	EXPECT_FALSE(Save.IsValid());
	EXPECT_EQ(Admission.Error, EAssetError::UnsupportedVersion);
	Save = {};
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Bulk));
	EXPECT_TRUE(std::filesystem::exists(Backup));
	for (const auto& Entry : std::filesystem::directory_iterator(Root / "Content"))
		EXPECT_EQ(Entry.path().filename().generic_string().find(".save-"), std::string::npos);
	std::filesystem::rename(Backup, Bulk);
	ASSERT_TRUE(AssetsSaved({Expected.Assets.at(Package->GetPackagePathIdentity())},
		CaptureAssetRegistryPublication()));
}

TEST_F(FTextureImportQueueTests, SaveTransactionsIgnoreUnownedStagingAndBackups)
{
	for (int Mode = 0; Mode < 3; ++Mode)
	{
		SCOPED_TRACE(Mode);
		const auto Name = std::format("save{}", Mode);
		auto* Texture = MakeSaveTexture(Name); ASSERT_NE(Texture, nullptr);
		auto* Package = Texture->GetPackage();
		ASSERT_TRUE(SavePackage(Package));
		std::vector<std::filesystem::path> Leftovers;
		for (const auto& Extension : {".dasset", ".dbulk"})
			for (const auto& Suffix : {".bundle-stage", ".bundle-backup", ".durin-backup",
				".save-abandoned.stage", ".save-abandoned.backup"})
				Leftovers.push_back(Root / "Content" / (Name + Extension + Suffix));
		const FByteBuffer UnownedBytes(3, std::byte{0x7f});
		for (const auto& File : Leftovers) ASSERT_TRUE(FFileHelper::SaveArrayToFile(UnownedBytes, File));
		Package->MarkDirty();
		FAssetResult Result;
		if (Mode == 0) Result = SavePackage(Package);
		else if (Mode == 1)
		{
			DPackage* Packages[] = {Package};
			Result = SavePackagesAtomically(Packages);
		}
		else
		{
			auto Save = (Package)->SaveAsync(Result, FAssetPackageSaveContext{});
			ASSERT_TRUE(Save.IsValid()) << Result.Message;
			ASSERT_TRUE(WaitForSave(Save));
			Result = FinishSave(Save);
		}
		ASSERT_TRUE(Result) << Result.Message;
		EXPECT_FALSE(Package->IsDirty());
		for (const auto& File : Leftovers)
		{
			FByteBuffer Actual;
			ASSERT_TRUE(FFileHelper::LoadFileToArray(Actual, File));
			EXPECT_EQ(Actual, UnownedBytes);
			ASSERT_TRUE(std::filesystem::remove(File));
		}
		for (const auto& Entry : std::filesystem::directory_iterator(Root / "Content"))
			if (Entry.path().filename().generic_string().starts_with(Name + "."))
				EXPECT_TRUE(Entry.path().extension() == ".dasset" || Entry.path().extension() == ".dbulk");
	}
}

TEST_F(FTextureImportQueueTests, CompetingAsyncSavesOwnSeparateTemporaryFiles)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	Texture->GetPackage()->MarkDirty();
	FAssetResult Admission;
	auto First = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(First.IsValid()) << Admission.Message;
	auto Second = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Second.IsValid()) << Admission.Message;
	ASSERT_TRUE(WaitForSave(First)); ASSERT_TRUE(WaitForSave(Second));
	EXPECT_TRUE(FinishSave(First));
	EXPECT_EQ(FinishSave(Second).Error, EAssetError::StaleData);
	First = {}; Second = {};
	for (const auto& Entry : std::filesystem::directory_iterator(Root / "Content"))
		EXPECT_TRUE(Entry.path().extension() == ".dasset" || Entry.path().extension() == ".dbulk");
}

TEST_F(FTextureImportQueueTests, AsyncSaveRejectsNewEditsAndRetriesCurrentVersion)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetResult Admission;
	auto Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()) << Admission.Message;
	ASSERT_TRUE(WaitForSave(Save));
	Texture->GetPackage()->MarkDirty();
	EXPECT_FALSE(FinishSave(Save));
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dasset"));
	Save = {};
	Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
	EXPECT_TRUE(FinishSave(Save));
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
}

TEST_F(FTextureImportQueueTests, AsyncSaveRejectsCompetingSaveAndChangedStaging)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetResult Admission;
	auto Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	EXPECT_FALSE(FinishSave(Save));
	Save = {};
	Texture->GetPackage()->MarkDirty();
	Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
	for (const auto& Entry : std::filesystem::directory_iterator(Root / "Content"))
		if (Entry.path().filename().generic_string().starts_with("save.dasset.save-"))
			ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer(1, std::byte{0}), Entry.path()));
	EXPECT_FALSE(FinishSave(Save));
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
}

TEST_F(FTextureImportQueueTests, IndependentAsyncSavesCanBothCommit)
{
	auto* First = MakeSaveTexture("first"); auto* Second = MakeSaveTexture("second");
	ASSERT_NE(First, nullptr); ASSERT_NE(Second, nullptr);
	FAssetResult Admission;
	auto FirstSave = (First->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	auto SecondSave = (Second->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(FirstSave.IsValid()); ASSERT_TRUE(SecondSave.IsValid());
	ASSERT_TRUE(WaitForSave(FirstSave)); ASSERT_TRUE(WaitForSave(SecondSave));
	EXPECT_TRUE(FinishSave(FirstSave)); EXPECT_TRUE(FinishSave(SecondSave));
}

TEST_F(FTextureImportQueueTests, AsyncSaveRejectsNewDestinationOccupant)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetResult Admission;
	auto Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
	const FByteBuffer OtherBytes(3, std::byte{99});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(OtherBytes, Root / "Content/save.dasset"));
	EXPECT_FALSE(FinishSave(Save));
	FByteBuffer Actual;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(Actual, Root / "Content/save.dasset"));
	EXPECT_EQ(Actual, OtherBytes);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dbulk"));
}

TEST_F(FTextureImportQueueTests, AsyncSaveRollsBackBulkWhenCommitFails)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	FByteBuffer BeforePackage, BeforeBulk;
	ASSERT_TRUE(FFileHelper::LoadFileToArray(BeforePackage, Root / "Content/save.dasset"));
	ASSERT_TRUE(FFileHelper::LoadFileToArray(BeforeBulk, Root / "Content/save.dbulk"));
	Image::FImage ChangedImage;
	ASSERT_TRUE(Image::FImage::TryCreate({.Width = 512, .Height = 512,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(512 * 512 * 4, std::byte{33}), ChangedImage));
	FTextureSource ChangedSource;
	ASSERT_TRUE(ChangedSource.Init2D(ChangedImage.GetView(), 4, 0, ETextureSourceCompression::Raw));
	Texture->SetSource(ChangedSource);
	Texture->GetPackage()->MarkDirty();
	for (auto Failure : {EAssetBundleSavePhase::PublishPackage, EAssetBundleSavePhase::PublishRegistry})
	{
		FAssetResult Admission;
		auto Save = Texture->GetPackage()->SaveAsync(Admission, FAssetPackageSaveContext{
			.Options = {.ShouldFail = [Failure](auto Phase, size_t) { return Phase == Failure; }, .bRollbackOnRegistryFailure = true}});
		ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
		EXPECT_FALSE(FinishSave(Save));
		EXPECT_TRUE(Texture->GetPackage()->IsDirty());
		FByteBuffer AfterPackage, AfterBulk;
		ASSERT_TRUE(FFileHelper::LoadFileToArray(AfterPackage, Root / "Content/save.dasset"));
		ASSERT_TRUE(FFileHelper::LoadFileToArray(AfterBulk, Root / "Content/save.dbulk"));
		EXPECT_EQ(BeforePackage, AfterPackage); EXPECT_EQ(BeforeBulk, AfterBulk);
	}
}

TEST_F(FTextureImportQueueTests, DroppedAsyncSaveHandleStillPublishes)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetResult Admission;
	auto Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); Save = {};
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/save.dasset"));
}

TEST_F(FTextureImportQueueTests, DirectSaveAdmissionAndDiskDrainDoNotPublish)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	auto* Package = Texture->GetPackage();
	auto Results = std::make_shared<std::vector<FAssetResult>>();
	SetAsyncPackageSaveSink([Results](const auto&, const auto& Result) {
		EXPECT_TRUE(IsInGameThread());
		EXPECT_EQ(DPackage::DrainAsyncSaves().Error, EPackageSaveError::Busy);
		Results->push_back(Result);
	});
	std::promise<void> Release;
	auto Gate = Release.get_future().share();
	Private::SetDirectPackageWriteFailureForTests([Gate](size_t) { Gate.wait(); return false; });
	ASSERT_TRUE(SavePackage(Package, SAVE_Async));
	EXPECT_TRUE(DPackage::HasAsyncFileWrites());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_TRUE(Package->IsNewlyCreated());
	EXPECT_TRUE(Results->empty());
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dasset"));
	EXPECT_FALSE(SavePackage(Package));
	EXPECT_FALSE(SavePackage(Package, SAVE_Async));
	FAssetResult Admission;
	EXPECT_FALSE(Package->SaveAsync(Admission, FAssetPackageSaveContext{}).IsValid());
	FAssetPackageInspection Inspection;
	EXPECT_FALSE(InspectAssetPackage((Root / "Content/save.dasset").generic_string(), Inspection));
	Release.set_value();
	ASSERT_EQ(DPackage::WaitForAsyncFileWrites().WaitStatus, ETaskWaitStatus::Completed);
	EXPECT_FALSE(DPackage::HasAsyncFileWrites());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_TRUE(Results->empty());
	Package->MarkDirty(); // Captured bytes can publish, but cannot clear a later edit.
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	ASSERT_EQ(Results->size(), 1u);
	EXPECT_TRUE(Results->front()) << Results->front().Message;
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_FALSE(Package->IsNewlyCreated());
	for (const auto& Entry : std::filesystem::directory_iterator(Root / "Content"))
		EXPECT_TRUE(Entry.path().extension() == ".dasset" || Entry.path().extension() == ".dbulk");
}

TEST_F(FTextureImportQueueTests, DirectSaveFailureFencesOnlyDestructiveOutputAndCanRetry)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	auto* Package = Texture->GetPackage();
	const auto Path = Package->GetPackagePathIdentity();
	ASSERT_TRUE(SavePackage(Package));
	for (size_t FailIndex : {size_t{0}, size_t{1}})
	{
		SCOPED_TRACE(FailIndex);
		Package->MarkDirty();
		auto Results = std::make_shared<std::vector<FAssetResult>>();
		SetAsyncPackageSaveSink([Results](const auto&, const auto& Result) { Results->push_back(Result); });
		Private::SetDirectPackageWriteFailureForTests([FailIndex](size_t Index) { return Index == FailIndex; });
		ASSERT_TRUE(SavePackage(Package, SAVE_Async));
		ASSERT_TRUE(DPackage::DrainAsyncSaves());
		ASSERT_EQ(Results->size(), 1u);
		EXPECT_FALSE(Results->front());
		EXPECT_EQ(Results->front().WriteOutcome.Disposition, FailIndex == 0 ? EAssetResultDisposition::Default : EAssetResultDisposition::PartiallyWritten);
		EXPECT_EQ(Results->front().WriteOutcome.AffectedFiles.size(), FailIndex);
		EXPECT_EQ(IsAssetRegistryProjectionFenced(Path), FailIndex != 0);
		EXPECT_TRUE(Package->IsDirty());
		Private::SetDirectPackageWriteFailureForTests({});
		ASSERT_TRUE(SavePackage(Package, SAVE_Async));
		ASSERT_TRUE(DPackage::DrainAsyncSaves());
		ASSERT_EQ(Results->size(), 2u);
		EXPECT_TRUE(Results->back()) << Results->back().Message;
		EXPECT_FALSE(IsAssetRegistryProjectionFenced(Path));
		EXPECT_FALSE(Package->IsDirty());
	}
}

TEST_F(FTextureImportQueueTests, DirectSaveProjectionFailureCanReconcileAndShutdownDrains)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	auto* Package = Texture->GetPackage();
	const auto Path = Package->GetPackagePathIdentity();
	auto Results = std::make_shared<std::vector<FAssetResult>>();
	SetAsyncPackageSaveSink([Results](const auto&, const auto& Result) { Results->push_back(Result); });
	AssetPrivate::SetAsyncSavePublicationFailureForTests(true);
	ASSERT_TRUE(SavePackage(Package, SAVE_Async));
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	ASSERT_EQ(Results->size(), 1u);
	EXPECT_EQ(Results->front().WriteOutcome.Disposition, EAssetResultDisposition::ContentCommittedProjectionPending);
	EXPECT_TRUE(IsAssetRegistryProjectionFenced(Path));
	EXPECT_TRUE(Package->IsDirty());
	AssetPrivate::SetAsyncSavePublicationFailureForTests(false);
	ASSERT_TRUE(SavePackage(Package, SAVE_Async));
	PackageSavePrivate::SetAsyncSaveAdmission(false);
	EXPECT_FALSE(SavePackage(Package, SAVE_Async));
	ShutdownAssetManager();
	ASSERT_EQ(Results->size(), 2u);
	EXPECT_TRUE(Results->back()) << Results->back().Message;
	EXPECT_FALSE(IsAssetRegistryProjectionFenced(Path));
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(DPackage::HasAsyncFileWrites());
	EXPECT_TRUE(InitializeAssetManager());
}

TEST_F(FTextureImportQueueTests, BatchContinuesAfterFailureAndPresentsOnce)
{
	const auto Second = Root / "other_normal.tga";
	std::filesystem::copy_file(Source, Second);
	int Assets = 0, Directories = 0;
	Editor::Texture::FTextureFileImport Importer({
		.AssetCreated = [&](std::string) { ++Assets; },
		.ImportedDirectory = [&](std::string Directory) { EXPECT_EQ(Directory, Destination); ++Directories; }});
	ASSERT_TRUE(Importer.Begin({(Root / "missing.png").generic_string(), Source.generic_string(),
		Second.generic_string(), Source.generic_string()}, Destination));
	EXPECT_FALSE(Importer.Begin({Source.generic_string()}, Destination));
	EXPECT_EQ(Importer.GetTotalCount(), 3u);
	PumpGameThreadDeferredWork();
	Importer.Tick(false);
	EXPECT_EQ(Importer.GetCompletedCount(), 0u);
	EXPECT_EQ(Assets + Directories, 0);
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetSavedCount(), 2u);
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Importer.GetCanceledCount(), 0u);
	EXPECT_EQ(Assets, 0);
	EXPECT_EQ(Directories, 1);
	EXPECT_NE(Importer.GetTimingDetails().find("wall_normal.tga: prepare"), std::string::npos);
	EXPECT_NE(Importer.GetTimingDetails().find("cache hit"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content" / "other_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, CancelSkipsQueuedFilesButFinishesActiveCapture)
{
	Editor::Texture::FTextureFileImport Importer;
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetCanceledCount(), 1u);
	EXPECT_EQ(Importer.GetSavedCount(), 0u);
	ASSERT_TRUE(Importer.Begin({Source.generic_string(), (Root / "missing.png").generic_string()}, Destination));
	Importer.Tick(); // Starts the detached capture before cancellation.
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetSavedCount(), 1u);
	EXPECT_EQ(Importer.GetCanceledCount(), 1u);
	EXPECT_EQ(Importer.GetFailedCount(), 0u);
}

TEST_F(FTextureImportQueueTests, FailedBatchSaveRetainsAssetForRetryWithoutSource)
{
	int Attempts = 0, Published = 0;
	Editor::Texture::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }},
		[&](const FAssetSaveRequest& Request) -> FAssetOperationResult {
			if (++Attempts == 1) return {.Kind = EAssetOperationKind::Save,
				.State = EAssetOperationTerminalState::Rejected, .Message = "Injected save failure"};
			return IAssetTools::Get().SaveAssets(Request);
		});
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	ASSERT_TRUE(Drain(Importer));
	EXPECT_TRUE(Importer.HasPendingSaves());
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Published, 0);
	ASSERT_TRUE(std::filesystem::remove(Source));
	PumpGameThreadDeferredWork();
	Importer.Tick(false);
	EXPECT_FALSE(Importer.CanRetrySaves());
	Importer.RetryPendingSaves();
	EXPECT_TRUE(Importer.HasPendingSaves());
	EXPECT_EQ(Attempts, 1);
	Importer.Tick(true);
	Importer.RetryPendingSaves();
	EXPECT_FALSE(Importer.HasPendingSaves());
	EXPECT_EQ(Published, 1);
	EXPECT_EQ(Attempts, 2);
}

TEST_F(FTextureImportQueueTests, TeardownJoinsCaptureWithoutPublishing)
{
	int Published = 0;
	{
		Editor::Texture::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
		ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
		Importer.Tick();
	}
	EXPECT_EQ(Published, 0);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, TeardownDrainsActiveCompilationAndDiscardsCandidate)
{
	int Published = 0;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(Destination + "wall_normal", Path));
	{
		Editor::Texture::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
		ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!FindResidentPackage(Path) && std::chrono::steady_clock::now() < Deadline)
		{
			Importer.Tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		ASSERT_NE(FindResidentPackage(Path), nullptr);
	}
	EXPECT_EQ(Published, 0);
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
}
