#include "Asset/Testing/PackageSaveTesting.h"
#include "NativeAssetBuildTestSupport.h"
#include "NativePackageUnloadTestSupport.h"
#include "Asset/PackageSerialization.h"
#include "Import/TextureFileImport.h"
#include "AssetForge/Builtins/Texture2DImport.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetImportData.h"
#include "Asset/Load.h"
#include "Asset/PackageInspection.h"
#include "Asset/EditorBulkDataStorage.h"
#include "AssetRegistry/Publication.h"
#include "AssetTools/IAssetTools.h"
#include "EngineTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Threading/RunnableThread.h"
#include "Texture/TexturePlatformDataTestFixtures.h"
#include "Threading/RunnableThread.h"

#include <gtest/gtest.h>

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
	auto ImageResult1 = Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakeNormalPixels(true));
	ASSERT_TRUE(ImageResult1);
	Image = std::move(*ImageResult1);
	FTextureSource Source;
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 3));
	EXPECT_EQ(InferTexture2DImportSettings("unnamed.png", &Source).Usage, ETextureUsage::Normal);
	EXPECT_EQ(InferTexture2DImportSettings("wall_albedo.png", &Source).Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("wall_orm.png", &Source).Usage, ETextureUsage::DataMask);
	auto ImageResult2 = Image::FImage::TryCreate({.Width = 4, .Height = 4,
		.Format = Image::ERawImageFormat::RGBA8}, MakeNormalPixels(false));
	ASSERT_TRUE(ImageResult2);
	Image = std::move(*ImageResult2);
	ASSERT_TRUE(Source.Init2D(Image.GetView(), 3));
	EXPECT_EQ(InferTexture2DImportSettings("purple.png", &Source).Usage, ETextureUsage::Color);
	EXPECT_EQ(InferTexture2DImportSettings("flat_normal.png", &Source).Usage, ETextureUsage::Normal);
}

TEST(FTextureFileImportTests, ImportsNormalSavesAndChoosesAvailableNames)
{
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	FModuleManager::Get().LoadModuleChecked("TextureCompressor");
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
	TextureEditor::FTextureFileImport Importer({
		.ReportError = [&](std::string Message) { Error = std::move(Message); },
		.AssetCreated = [&](std::string Path) { Published.push_back(std::move(Path)); }});
	for (int Index = 1; Index <= 2; ++Index)
	{
		const auto Result = Importer.ImportFile(Source.generic_string(), "/TextureFileImport/");
		ASSERT_TRUE(Result) << Error;
		ASSERT_NE(Result, nullptr);
		const auto* Texture = Cast<DTexture2D>(Result);
		ASSERT_NE(Texture, nullptr);
		EXPECT_EQ(Texture->GetUsage(), ETextureUsage::Normal);
		EXPECT_FALSE(Texture->IsSRGB());
		EXPECT_FALSE(Result->GetPackage()->IsDirty());
		EXPECT_EQ(Published.back(), std::format("/TextureFileImport/unnamed_{}", Index));
		EXPECT_TRUE(UnloadPackage(Result->GetPackage()));
	}
	EXPECT_FALSE(Importer.HasPendingSaves());
	EXPECT_EQ(Published.size(), 2u);
	EXPECT_EQ(std::filesystem::file_size(Source), Tga.size());
	const auto Invalid = Importer.ImportFile((Root / "missing.png").generic_string(), "/TextureFileImport/");
	EXPECT_FALSE(Invalid);
	EXPECT_EQ(Published.size(), 2u);

	int SaveAttempts = 0;
	TextureEditor::FTextureFileImport RetryImporter({
		.AssetCreated = [&](std::string Path) { Published.push_back(std::move(Path)); }});
	Testing::FScopedPackageSaveFault Fault([&](Testing::EPackageSaveFaultPoint Point, size_t) {
		return Point == Testing::EPackageSaveFaultPoint::PublishPackage && ++SaveAttempts == 1;
	});
	const auto Unsaved = RetryImporter.ImportFile(Source.generic_string(), "/TextureFileImport/");
	EXPECT_TRUE(Unsaved);
	ASSERT_NE(Unsaved, nullptr);
	EXPECT_TRUE(Unsaved->GetPackage()->IsDirty());
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
			auto ImageResult3 = Image::FImage::TryCreate({.Width = 512, .Height = 512,
				.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(512 * 512 * 4, std::byte{17}));
			if (!ImageResult3) return nullptr;
			Image = std::move(*ImageResult3);
			FTextureSource Source;
			if (!Source.Init2D(Image.GetView(), 4, 0, ETextureSourceCompression::Raw)) return nullptr;
			Texture->SetSource(Source);
			auto Platform = std::make_unique<FTexturePlatformData>();
			Platform->PixelFormat = EPixelFormat::RGBA8_UNORM;
			Platform->Mips.push_back({.Pixels = FSharedByteBuffer::Take(FByteBuffer(4, std::byte{17})), .Width = 1, .Height = 1, .RowPitch = 4});
			Texture->SetPlatformData(std::move(Platform));
			return Texture;
		}
		auto WaitForSave(Tasks::TTask<FAssetWriteResult>&) -> bool
		{ return DPackage::WaitForAsyncFileWrites().WaitStatus == ETaskWaitStatus::Completed; }
		auto FinishSave(Tasks::TTask<FAssetWriteResult>& Save) -> FAssetWriteResult
		{
			EXPECT_TRUE(DPackage::DrainAsyncSaves());
			if (Save.GetState() != ETaskState::Succeeded) return {EAssetWriteError::IoError, "Task execution failed."};
			return Save.GetResult();
		}
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			if (!GetGameThreadDeferredWorkQueueDiagnostics().bInstalled) ASSERT_TRUE(InitializeGameThreadDeferredExecutor());
			PackageSavePrivate::SetAsyncSaveAdmission(true);
			if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
			FModuleManager::Get().LoadModuleChecked("TextureCompressor");
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
		auto Drain(TextureEditor::FTextureFileImport& Importer) -> bool
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
	const auto Before = GetAssetCatalogRevision();
	FAssetWriteResult Admission;
	auto Save = Texture->GetPackage()->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()) << Admission.Message;
	ASSERT_TRUE(WaitForSave(Save));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dasset"));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dbulk"));
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
	const auto Result = FinishSave(Save);
	ASSERT_TRUE(Result) << Result.Message;
	const auto PublishedRevision = GetAssetCatalogRevision();
	EXPECT_GT(PublishedRevision, Before);
	EXPECT_TRUE(Save.GetResult());
	EXPECT_EQ(GetAssetCatalogRevision(), PublishedRevision);
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	FByteBuffer PackageBytes, BulkBytes;
	auto PackageBytesRead = FFileHelper::LoadFileToArray(Root / "Content/save.dasset");
	ASSERT_TRUE(PackageBytesRead) << PackageBytesRead.error().ToString();
	PackageBytes = std::move(*PackageBytesRead);
	auto BulkBytesRead = FFileHelper::LoadFileToArray(Root / "Content/save.dbulk");
	ASSERT_TRUE(BulkBytesRead) << BulkBytesRead.error().ToString();
	BulkBytes = std::move(*BulkBytesRead);
	EXPECT_TRUE(ValidateAssetPackageBytes(PackageBytes, Path, BulkBytes));
}

TEST_F(FTextureImportQueueTests, CleanPackageAsyncSaveSharesContentArgsWithSynchronousSave)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	auto* Package = Texture->GetPackage();
	const FAssetPackageSaveArgs Args{.Mode = EAssetPackageSaveMode::Complete};
	ASSERT_TRUE(SavePackage(Package, Args));
	ASSERT_FALSE(Package->IsDirty());
	const auto Before = FFileHelper::LoadFileToArray(Root / "Content/save.dasset");
	ASSERT_TRUE(Before);
	FAssetWriteResult Admission;
	auto Save = Package->SaveAsync(Admission, FAssetPackageSaveContext{.Args = Args});
	ASSERT_TRUE(Save.IsValid()) << Admission.Message;
	ASSERT_TRUE(WaitForSave(Save));
	ASSERT_TRUE(FinishSave(Save));
	EXPECT_FALSE(Package->IsDirty());
	const auto After = FFileHelper::LoadFileToArray(Root / "Content/save.dasset");
	ASSERT_TRUE(After);
	EXPECT_EQ(*Before, *After);
}

TEST_F(FTextureImportQueueTests, AsyncSaveCapturesFaultOwnershipBeforeScopeEnds)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	auto Owner = std::make_shared<std::string>("owned fault");
	const std::weak_ptr<std::string> WeakOwner = Owner;
	FAssetWriteResult Admission;
	Tasks::TTask<FAssetWriteResult> Save;
	{
		Durin::Testing::FScopedPackageSaveFault Fault([Owner](auto Phase, size_t) {
			return *Owner == "owned fault" && Phase == Durin::Testing::EPackageSaveFaultPoint::PublishPackage;
		});
		Save = Texture->GetPackage()->SaveAsync(Admission, FAssetPackageSaveContext{});
	}
	Owner.reset();
	ASSERT_TRUE(Save.IsValid()) << Admission.Message;
	EXPECT_FALSE(WeakOwner.expired());
	ASSERT_TRUE(WaitForSave(Save));
	const auto Failed = FinishSave(Save);
	EXPECT_EQ(Failed.Error, EAssetWriteError::IoError);
	EXPECT_EQ(Failed.Effect, EAssetWriteEffect::None);
	EXPECT_TRUE(SavePackage(Texture->GetPackage()));
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
	const auto Backup = std::filesystem::path(Bulk.string() + std::string(".durin-backup"));
	std::filesystem::rename(Bulk, Backup);
	EXPECT_EQ(SavePackage(Package).Error, EAssetWriteError::UnsupportedVersion);
	DPackage* Packages[] = {Package};
	EXPECT_EQ(SavePackages(Packages, {}).Result.Error, EAssetWriteError::UnsupportedVersion);
	FAssetWriteResult Admission;
	auto Save = (Package)->SaveAsync(Admission, FAssetPackageSaveContext{});
	EXPECT_FALSE(Save.IsValid());
	EXPECT_EQ(Admission.Error, EAssetWriteError::UnsupportedVersion);
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
				".save-abandoned.stage", ".save-abandoned.backup",
				".save-abandoned.stage.tmp", ".save-abandoned.backup.tmp"})
				Leftovers.push_back(Root / "Content" / (Name + Extension + Suffix));
		const FByteBuffer UnownedBytes(3, std::byte{0x7f});
		for (const auto& File : Leftovers) ASSERT_TRUE(FFileHelper::SaveArrayToFile(UnownedBytes, File));
		Package->MarkDirty();
		FAssetWriteResult Result;
		if (Mode == 0) Result = SavePackage(Package);
		else if (Mode == 1)
		{
			DPackage* Packages[] = {Package};
			Result = SavePackages(Packages).Result;
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
			auto ActualRead = FFileHelper::LoadFileToArray(File);
			ASSERT_TRUE(ActualRead) << ActualRead.error().ToString();
			Actual = std::move(*ActualRead);
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
	FAssetWriteResult Admission;
	auto First = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(First.IsValid()) << Admission.Message;
	auto Second = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Second.IsValid()) << Admission.Message;
	ASSERT_TRUE(WaitForSave(First)); ASSERT_TRUE(WaitForSave(Second));
	EXPECT_TRUE(FinishSave(First));
	EXPECT_EQ(FinishSave(Second).Error, EAssetWriteError::StaleData);
	First = {}; Second = {};
	for (const auto& Entry : std::filesystem::directory_iterator(Root / "Content"))
		EXPECT_TRUE(Entry.path().extension() == ".dasset" || Entry.path().extension() == ".dbulk");
}

TEST_F(FTextureImportQueueTests, AsyncSaveRejectsNewEditsAndRetriesCurrentVersion)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetWriteResult Admission;
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
	FAssetWriteResult Admission;
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
	FAssetWriteResult Admission;
	auto FirstSave = (First->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	auto SecondSave = (Second->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(FirstSave.IsValid()); ASSERT_TRUE(SecondSave.IsValid());
	ASSERT_TRUE(WaitForSave(FirstSave)); ASSERT_TRUE(WaitForSave(SecondSave));
	EXPECT_TRUE(FinishSave(FirstSave)); EXPECT_TRUE(FinishSave(SecondSave));
}

TEST_F(FTextureImportQueueTests, AsyncSaveRejectsNewDestinationOccupant)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetWriteResult Admission;
	auto Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
	const FByteBuffer OtherBytes(3, std::byte{99});
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(OtherBytes, Root / "Content/save.dasset"));
	EXPECT_FALSE(FinishSave(Save));
	FByteBuffer Actual;
	auto ActualRead = FFileHelper::LoadFileToArray(Root / "Content/save.dasset");
	ASSERT_TRUE(ActualRead) << ActualRead.error().ToString();
	Actual = std::move(*ActualRead);
	EXPECT_EQ(Actual, OtherBytes);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content/save.dbulk"));
}

TEST_F(FTextureImportQueueTests, AsyncSaveDistinguishesPrecommitFailureFromCommittedProjectionFailure)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	ASSERT_TRUE(SavePackage(Texture->GetPackage()));
	FByteBuffer BeforePackage, BeforeBulk;
	auto BeforePackageRead = FFileHelper::LoadFileToArray(Root / "Content/save.dasset");
	ASSERT_TRUE(BeforePackageRead) << BeforePackageRead.error().ToString();
	BeforePackage = std::move(*BeforePackageRead);
	auto BeforeBulkRead = FFileHelper::LoadFileToArray(Root / "Content/save.dbulk");
	ASSERT_TRUE(BeforeBulkRead) << BeforeBulkRead.error().ToString();
	BeforeBulk = std::move(*BeforeBulkRead);
	auto ImageResult4 = Image::FImage::TryCreate({.Width = 512, .Height = 512,
		.Format = Image::ERawImageFormat::RGBA8}, FByteBuffer(512 * 512 * 4, std::byte{33}));
	ASSERT_TRUE(ImageResult4);
	auto ChangedImage = std::move(*ImageResult4);
	FTextureSource ChangedSource;
	ASSERT_TRUE(ChangedSource.Init2D(ChangedImage.GetView(), 4, 0, ETextureSourceCompression::Raw));
	Texture->SetSource(ChangedSource);
	Texture->GetPackage()->MarkDirty();
	for (auto Failure : {Durin::Testing::EPackageSaveFaultPoint::PublishPackage, Durin::Testing::EPackageSaveFaultPoint::PublishRegistry})
	{
		FAssetWriteResult Admission;
		auto Save = ([&] { Durin::Testing::FScopedPackageSaveFault Fault([Failure](auto Phase, size_t) { return Phase == Failure; }); return Texture->GetPackage()->SaveAsync(Admission, Durin::FAssetPackageSaveContext{}); }());
		ASSERT_TRUE(Save.IsValid()); ASSERT_TRUE(WaitForSave(Save));
		EXPECT_FALSE(FinishSave(Save));
		EXPECT_EQ(Texture->GetPackage()->IsDirty(), Failure != Durin::Testing::EPackageSaveFaultPoint::PublishRegistry);
		FByteBuffer AfterPackage, AfterBulk;
		auto AfterPackageRead = FFileHelper::LoadFileToArray(Root / "Content/save.dasset");
		ASSERT_TRUE(AfterPackageRead) << AfterPackageRead.error().ToString();
		AfterPackage = std::move(*AfterPackageRead);
		auto AfterBulkRead = FFileHelper::LoadFileToArray(Root / "Content/save.dbulk");
		ASSERT_TRUE(AfterBulkRead) << AfterBulkRead.error().ToString();
		AfterBulk = std::move(*AfterBulkRead);
		if (Failure == Durin::Testing::EPackageSaveFaultPoint::PublishRegistry)
		{
			EXPECT_NE(BeforePackage, AfterPackage);
			EXPECT_EQ(Save.GetResult().Effect, EAssetWriteEffect::ContentCommittedProjectionPending);
			ASSERT_TRUE(SavePackage(Texture->GetPackage()));
			EXPECT_FALSE(IsAssetRegistryProjectionFenced(Texture->GetPackage()->GetPackagePathIdentity()));
		}
		else { EXPECT_EQ(BeforePackage, AfterPackage); EXPECT_EQ(BeforeBulk, AfterBulk); }
	}
}

TEST_F(FTextureImportQueueTests, DroppedAsyncSaveHandleStillPublishes)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetWriteResult Admission;
	auto Save = (Texture->GetPackage())->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Save.IsValid()); Save = {};
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_FALSE(Texture->GetPackage()->IsDirty());
	EXPECT_TRUE(std::filesystem::exists(Root / "Content/save.dasset"));
}

TEST_F(FTextureImportQueueTests, SaveTaskReportsProjectionFailureAndShutdownDrainsRetry)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	auto* Package = Texture->GetPackage();
	const auto Path = Package->GetPackagePathIdentity();
	FAssetWriteResult Admission;
	Tasks::TTask<FAssetWriteResult> Save;
	{
		Testing::FScopedPackageSaveFault Fault([](auto Phase, size_t) {
			return Phase == Testing::EPackageSaveFaultPoint::PublishRegistry;
		});
		Save = Package->SaveAsync(Admission, FAssetPackageSaveContext{});
	}
	ASSERT_TRUE(Save.IsValid()) << Admission.Message;
	ASSERT_TRUE(WaitForSave(Save));
	EXPECT_EQ(FinishSave(Save).Effect, EAssetWriteEffect::ContentCommittedProjectionPending);
	EXPECT_TRUE(IsAssetRegistryProjectionFenced(Path));

	auto Retry = Package->SaveAsync(Admission, FAssetPackageSaveContext{});
	ASSERT_TRUE(Retry.IsValid()) << Admission.Message;
	PackageSavePrivate::SetAsyncSaveAdmission(false);
	EXPECT_FALSE(Package->SaveAsync(Admission, FAssetPackageSaveContext{}).IsValid());
	ShutdownAssetRuntime();
	ASSERT_TRUE(Retry.IsCompleted());
	ASSERT_EQ(Retry.GetState(), ETaskState::Succeeded);
	EXPECT_TRUE(Retry.GetResult()) << Retry.GetResult().Message;
	EXPECT_FALSE(IsAssetRegistryProjectionFenced(Path));
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(DPackage::HasAsyncFileWrites());
	EXPECT_TRUE(InitializeAssetRuntime());
}

TEST_F(FTextureImportQueueTests, BatchContinuesAfterFailureAndPresentsOnce)
{
	const auto Second = Root / "other_normal.tga";
	std::filesystem::copy_file(Source, Second);
	int Assets = 0, Directories = 0;
	TextureEditor::FTextureFileImport Importer({
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
	EXPECT_NE(Importer.GetTimingDetails().find("compilation elapsed"), std::string::npos);
	EXPECT_NE(Importer.GetTimingDetails().find("save"), std::string::npos);
	EXPECT_TRUE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
	EXPECT_TRUE(std::filesystem::exists(Root / "Content" / "other_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, CancelBeforeCommitDiscardsPreparedSource)
{
	TextureEditor::FTextureFileImport Importer;
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetCanceledCount(), 1u);
	EXPECT_EQ(Importer.GetSavedCount(), 0u);
	ASSERT_TRUE(Importer.Begin({Source.generic_string(), (Root / "missing.png").generic_string()}, Destination));
	Importer.Tick(); // Starts the detached capture before cancellation.
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetSavedCount(), 0u);
	EXPECT_EQ(Importer.GetCanceledCount(), 2u);
	EXPECT_EQ(Importer.GetFailedCount(), 0u);
	EXPECT_EQ(FindPackage(Destination + "wall_normal"), nullptr);
	ASSERT_TRUE(Importer.Begin({(Root / "missing.png").generic_string()}, Destination));
	Importer.Tick();
	Importer.Cancel();
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetCanceledCount(), 1u);
	EXPECT_EQ(Importer.GetFailedCount(), 0u);
}

TEST_F(FTextureImportQueueTests, FailedBatchSaveRetainsAssetForRetryWithoutSource)
{
	int Attempts = 0, Published = 0;
	TextureEditor::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
	Testing::FScopedPackageSaveFault Fault([&](Testing::EPackageSaveFaultPoint Point, size_t) {
		return Point == Testing::EPackageSaveFaultPoint::PublishPackage && ++Attempts == 1;
	});
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	ASSERT_TRUE(Drain(Importer));
	EXPECT_TRUE(Importer.HasPendingSaves());
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Published, 0);
	auto* Package = FindPackage(Destination + "wall_normal");
	ASSERT_NE(Package, nullptr);
	auto* Texture = Cast<DTexture2D>(Package->FindTopLevelAsset("wall_normal"));
	ASSERT_NE(Texture, nullptr);
	EXPECT_TRUE(Texture->IsAsyncCacheComplete());
	EXPECT_TRUE(Texture->HasPlatformData());
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
		TextureEditor::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
		ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
		Importer.Tick();
	}
	EXPECT_EQ(Published, 0);
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, TeardownDrainsCompilationAndRetainsCommittedImport)
{
	int Published = 0;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(Destination + "wall_normal", Path));
	{
		TextureEditor::FTextureFileImport Importer({.AssetCreated = [&](std::string) { ++Published; }});
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
	auto* Retained = FindResidentPackage(Path);
	ASSERT_NE(Retained, nullptr);
	auto* Texture = Cast<DTexture2D>(Retained->FindTopLevelAsset("wall_normal"));
	ASSERT_NE(Texture, nullptr);
	EXPECT_TRUE(Texture->GetSource().IsValid());
	EXPECT_NE(Texture->GetAssetImportData(), nullptr);
	EXPECT_FALSE(!Texture->IsAsyncCacheComplete());
	EXPECT_TRUE(UnloadPackage(Retained, EAssetPackageUnloadPolicy::DiscardUnsaved));
	EXPECT_FALSE(std::filesystem::exists(Root / "Content" / "wall_normal.dasset"));
}

TEST_F(FTextureImportQueueTests, ExternalCompilationCancellationRetainsResidentAsset)
{
	Testing::FScopedPackageSaveFault Fault([](Testing::EPackageSaveFaultPoint Point, size_t) {
		return Point == Testing::EPackageSaveFaultPoint::PublishPackage;
	});
	TextureEditor::FTextureFileImport Importer;
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	DPackage* Package = nullptr;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!Package && std::chrono::steady_clock::now() < Deadline)
	{
		Importer.Tick();
		Package = FindPackage(Destination + "wall_normal");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_NE(Package, nullptr);
	auto* Texture = Cast<DTexture2D>(Package->FindTopLevelAsset("wall_normal"));
	ASSERT_NE(Texture, nullptr);
	const auto ImportedIdentity = Texture->GetSource().GetIdentity();
	const auto Metadata = Texture->GetAssetImportData()->GetState();
	FAssetCompilingManager::Get().MarkCompilationAsCanceled(*Texture);
	ASSERT_TRUE(Drain(Importer));
	EXPECT_EQ(Importer.GetCanceledCount(), 0u);
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Importer.GetSavedCount(), 0u);
	EXPECT_EQ(FindPackage(Destination + "wall_normal"), Package);
	EXPECT_EQ(Texture->GetSource().GetIdentity(), ImportedIdentity);
	EXPECT_EQ(Texture->GetAssetImportData()->GetState(), Metadata);
	EXPECT_FALSE(!Texture->IsAsyncCacheComplete());
	EXPECT_TRUE(Importer.HasPendingSaves());
	EXPECT_NE(Importer.GetErrors().front().find("save failed"), std::string::npos);
	EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST_F(FTextureImportQueueTests, MutationCanceledImportDoesNotUnloadNewCompilation)
{
	Testing::FScopedPackageSaveFault Fault([](Testing::EPackageSaveFaultPoint Point, size_t) {
		return Point == Testing::EPackageSaveFaultPoint::PublishPackage;
	});
	TextureEditor::FTextureFileImport Importer;
	ASSERT_TRUE(Importer.Begin({Source.generic_string()}, Destination));
	DPackage* Package = nullptr;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!Package && std::chrono::steady_clock::now() < Deadline)
	{
		Importer.Tick();
		Package = FindPackage(Destination + "wall_normal");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_NE(Package, nullptr);
	auto* Texture = Cast<DTexture2D>(Package->FindTopLevelAsset("wall_normal"));
	ASSERT_NE(Texture, nullptr);
	auto Prepared = PrepareTexture2DImport(Source.generic_string());
	ASSERT_TRUE(Prepared);
	std::optional<FTextureOperationResult> Replacement;
	ASSERT_TRUE(Texture->BeginCachePlatformData({.Build = MakeTexture2DBuildRequest(Prepared->Source).value(),
		 .ResultApplication = {.SourceReplacement = Prepared->Source}},
		[&](FTextureOperationResult Value) { Replacement = std::move(Value); }));
	ASSERT_TRUE(Drain(Importer));
	EXPECT_FALSE(Importer.IsRunning());
	EXPECT_EQ(Importer.GetCanceledCount(), 0u);
	EXPECT_EQ(Importer.GetFailedCount(), 1u);
	EXPECT_EQ(Importer.GetSavedCount(), 0u);
	EXPECT_EQ(FindPackage(Destination + "wall_normal"), Package);
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Replacement);
	EXPECT_TRUE(Replacement->Succeeded());
	EXPECT_TRUE(Texture->HasPlatformData());
	EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST_F(FTextureImportQueueTests, TeardownAfterMutationCancellationRetainsReplacement)
{
	auto Importer = std::make_unique<TextureEditor::FTextureFileImport>();
	ASSERT_TRUE(Importer->Begin({Source.generic_string()}, Destination));
	DPackage* Package = nullptr;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!Package && std::chrono::steady_clock::now() < Deadline)
	{
		Importer->Tick();
		Package = FindPackage(Destination + "wall_normal");
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_NE(Package, nullptr);
	auto* Texture = Cast<DTexture2D>(Package->FindTopLevelAsset("wall_normal"));
	ASSERT_NE(Texture, nullptr);
	auto Prepared = PrepareTexture2DImport(Source.generic_string());
	ASSERT_TRUE(Prepared);
	std::optional<FTextureOperationResult> Replacement;
	struct FDrainCompilation
	{
		~FDrainCompilation() { FAssetCompilingManager::Get().FinishAllCompilation(); }
	} DrainCompilation;
	ASSERT_TRUE(Texture->BeginCachePlatformData({.Build = MakeTexture2DBuildRequest(Prepared->Source).value(),
		 .ResultApplication = {.SourceReplacement = Prepared->Source}},
		[&](FTextureOperationResult Value) { Replacement = std::move(Value); }));
	// No importer tick occurs between external cancellation and destruction.
	Importer.reset();
	EXPECT_EQ(FindPackage(Destination + "wall_normal"), Package);
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Replacement);
	EXPECT_TRUE(Replacement->Succeeded());
	EXPECT_TRUE(Texture->HasPlatformData());
	EXPECT_TRUE(UnloadPackage(Package, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST_F(FTextureImportQueueTests, AsyncSaveUsesExplicitCancellationForTypedPublication)
{
	auto* Texture = MakeSaveTexture(); ASSERT_NE(Texture, nullptr);
	FAssetWriteResult Admission;
	FAssetPackageSaveContext Context;
	Tasks::FCancellationToken Cancellation;
	Context.IsCancellationRequested = [&Cancellation] { return Cancellation.IsCanceled(); };
	auto Save = Texture->GetPackage()->SaveAsync(Admission, Context);
	ASSERT_TRUE(Save.IsValid()) << Admission.Message;
	EXPECT_EQ(Save.GetDiagnostics().Target, ETaskTarget::GameThreadDeferred);
	EXPECT_EQ(Save.WaitDetailed().WaitStatus, ETaskWaitStatus::UnsupportedThread);
	Cancellation.Cancel();
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Save.GetState(), ETaskState::Succeeded);
	EXPECT_EQ(Save.GetResult().Error, EAssetWriteError::Cancelled);
	EXPECT_TRUE(Texture->GetPackage()->IsDirty());
}

TEST_F(FTextureImportQueueTests, TeardownDropsTaskObservationWithoutLatePresentation)
{
	int Presented = 0;
	auto Importer = std::make_unique<TextureEditor::FTextureFileImport>(
		FImportDialogCallbacks{.AssetCreated = [&](std::string) { ++Presented; }});
	ASSERT_TRUE(Importer->Begin({Source.generic_string()}, Destination));
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (!Importer->GetActivity().starts_with("Saving") && std::chrono::steady_clock::now() < Deadline)
	{
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		PumpGameThreadDeferredWork();
		Importer->Tick();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_TRUE(Importer->GetActivity().starts_with("Saving"));
	EXPECT_EQ(Presented, 0);
	Importer.reset();
	ASSERT_TRUE(DPackage::DrainAsyncSaves());
	EXPECT_EQ(Presented, 0);
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(Destination + "wall_normal", Path));
	EXPECT_TRUE(FindPackageExact(Path));
	EXPECT_TRUE(std::filesystem::is_regular_file(Root / "Content/wall_normal.dasset"));
	ASSERT_TRUE(UnloadPackage(Path));
}
