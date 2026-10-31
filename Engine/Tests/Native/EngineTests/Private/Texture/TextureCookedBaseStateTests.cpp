#include "TextureTestSupport.h"
#include "Threading/TaskRuntime.h"
#include "DObject/StrongObjectPtr.h"

#include "Asset/PackageInspection.h"
#include "Asset/Load.h"
#include "NativeAssetTestSupport.h"
#include "NativeAssetRuntimeTestSupport.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"

namespace
{
	constexpr std::string_view FixtureDirectory = "TextureCookedBaseState";

	auto CopyFixtureTree(std::string_view Name) -> std::filesystem::path
	{
		const std::filesystem::path Source =
			std::filesystem::path(DURIN_TEST_DATA_DIR) / FixtureDirectory / Name;
		const std::filesystem::path Destination =
			Durin::Testing::GetTestWorkDirectory() / std::string(Name);
		Durin::Testing::RemoveTestWorkDirectory(Destination);
		std::filesystem::copy(Source, Destination,
			std::filesystem::copy_options::recursive
				| std::filesystem::copy_options::overwrite_existing);
		return Destination;
	}

	auto ExpectNoAuthoredState(const std::filesystem::path& Path,
		std::string_view PackageIdentity) -> void
	{
		Durin::FPackagePath PackagePath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(PackageIdentity, PackagePath)
			|| Durin::FPackagePath::TryCreateProjectContent(PackageIdentity, PackagePath));
		Durin::FAssetPackageInspection Inspection;
		ASSERT_TRUE(Durin::InspectAssetPackage(
			Path.generic_string(), PackagePath, Inspection));
		EXPECT_EQ(Inspection.Header.FormatVersion, 10u);
		const Durin::FAssetPackageField* Source = Inspection.FindField("Source");
		EXPECT_EQ(Source, nullptr);
		const Durin::FAssetPackageField* ImportData = Inspection.FindField("AssetImportData");
		EXPECT_EQ(ImportData, nullptr);
	}

	template<class T>
	auto ExpectLazyCookedInstall(T& Texture) -> void
	{
		const Durin::DTexture& BaseTexture = Texture;
		// PostLoad may read bulk data in the background. Observe getter purity
		// after that read settles, without pumping CPU-product publication.
		const auto ReadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (BaseTexture.GetCookedPlatformData().GetState() == Durin::EBulkDataState::Loading
			&& std::chrono::steady_clock::now() < ReadDeadline) std::this_thread::yield();
		ASSERT_NE(BaseTexture.GetCookedPlatformData().GetState(), Durin::EBulkDataState::Loading);
		const Durin::EBulkDataState BulkState =
			BaseTexture.GetCookedPlatformData().GetState();
		const auto* Resource = BaseTexture.GetResource();
		const bool bCacheComplete = BaseTexture.IsAsyncCacheComplete();
		EXPECT_EQ(Texture.GetPlatformData(), nullptr);
		EXPECT_FALSE(BaseTexture.HasPlatformData());
		EXPECT_EQ(BaseTexture.GetCookedPlatformData().GetState(), BulkState);
		EXPECT_EQ(BaseTexture.GetResource(), Resource);
		EXPECT_EQ(BaseTexture.IsAsyncCacheComplete(), bCacheComplete);
		ASSERT_TRUE(Texture.FinishCachePlatformData());
		ASSERT_NE(Texture.GetPlatformData(), nullptr);
		const auto* Installed = Texture.GetPlatformData();
		const auto InstalledPlatformDataIdentity = Texture.GetPlatformDataShared();
		ASSERT_TRUE(Texture.FinishCachePlatformData());
		EXPECT_EQ(Texture.GetPlatformData(), Installed);
		EXPECT_EQ(Texture.GetPlatformDataShared(), InstalledPlatformDataIdentity);
		Durin::TStrongObjectPtr<T> Offline(Durin::NewObject<T>(nullptr, Durin::FName{}));
		const_cast<Durin::FBulkData&>(Offline->GetCookedPlatformData()) = Texture.GetCookedPlatformData();
		Offline->PostLoad();
		EXPECT_FALSE(Offline->HasPlatformData());
		// Finish is completion-only; simple tools explicitly request synchronous caching.
		EXPECT_FALSE(Offline->FinishCachePlatformData(1e-12));
		EXPECT_FALSE(Offline->HasPlatformData());
		Offline->CachePlatformData();
		EXPECT_TRUE(Offline->HasPlatformData());
	}
}

TEST(FTextureCookedBaseStateTests, CookedFixturesKeepNativePlatformDataIdentities)
{
	InitializeDObjectSystem();
	const std::filesystem::path Root = CopyFixtureTree("CookedPackages");
	ASSERT_NO_FATAL_FAILURE(ExpectNoAuthoredState(
		Root / "Game/Texture2D.dasset", "/Game/Texture2D"));
	ASSERT_NO_FATAL_FAILURE(ExpectNoAuthoredState(
		Root / "Game/TextureCube.dasset", "/Game/TextureCube"));
	ASSERT_NO_FATAL_FAILURE(ExpectNoAuthoredState(
		Root / "Game/VolumeTexture.dasset", "/Game/VolumeTexture"));
	Durin::FAssetPackageInspection Inspection;
	Durin::FPackagePath PackagePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent("/Game/Texture2D", PackagePath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Game/Texture2D.dasset").generic_string(), PackagePath, Inspection));
	ASSERT_NE(Inspection.FindField("PlatformData"), nullptr);
	EXPECT_EQ(Inspection.FindField("PlatformData")->DeclaringClass, "Durin::DTexture2D");
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent("/Game/TextureCube", PackagePath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Game/TextureCube.dasset").generic_string(), PackagePath, Inspection));
	ASSERT_NE(Inspection.FindField("PlatformData"), nullptr);
	EXPECT_EQ(Inspection.FindField("PlatformData")->DeclaringClass, "Durin::DTextureCube");
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent("/Game/VolumeTexture", PackagePath));
	ASSERT_TRUE(Durin::InspectAssetPackage(
		(Root / "Game/VolumeTexture.dasset").generic_string(), PackagePath, Inspection));
	ASSERT_NE(Inspection.FindField("PlatformData"), nullptr);
	EXPECT_EQ(Inspection.FindField("PlatformData")->DeclaringClass, "Durin::DVolumeTexture");

	Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
	ASSERT_TRUE(AssetRuntime.RestartCooked(Root));
	struct FRestoreTaskRuntime
	{
		bool bDeferred = Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting;
		~FRestoreTaskRuntime()
		{
			EXPECT_TRUE(Durin::InitializeTaskScheduler(2));
			if (bDeferred) EXPECT_TRUE(Durin::InitializeGameThreadDeferredExecutor());
		}
	} RestoreTasks;
	Durin::FAssetCompilingManager::Get().FinishAllCompilation();
	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2)); // Package-object loading uses the existing asset IO runtime.
	ASSERT_FALSE(Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting);
	ASSERT_TRUE(Durin::RefreshAssetRegistry(
		Durin::EAssetRegistryScanMode::FullValidation));

	Durin::FPackagePath Texture2DPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/Texture2D", Texture2DPath));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(Texture2DPath));
	Durin::DTexture2D* Texture2D = nullptr;
	const auto Texture2DLoad = Durin::LoadObject<Durin::DTexture2D>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Texture2DPath));
	Texture2D = Texture2DLoad.value_or(nullptr);
	ASSERT_TRUE(Texture2DLoad) << (Texture2DLoad ? std::string{} : Texture2DLoad.error().Message);
	ASSERT_NE(Texture2D, nullptr);
	EXPECT_FALSE(Texture2D->GetSource().IsValid());
	EXPECT_EQ(Texture2D->GetAssetImportData(), nullptr);

	Durin::FPackagePath TextureCubePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/TextureCube", TextureCubePath));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(TextureCubePath));
	Durin::DTextureCube* TextureCube = nullptr;
	const auto TextureCubeLoad = Durin::LoadObject<Durin::DTextureCube>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(TextureCubePath));
	TextureCube = TextureCubeLoad.value_or(nullptr);
	ASSERT_TRUE(TextureCubeLoad) << (TextureCubeLoad ? std::string{} : TextureCubeLoad.error().Message);
	ASSERT_NE(TextureCube, nullptr);
	EXPECT_FALSE(TextureCube->GetSource().IsValid());
	EXPECT_EQ(TextureCube->GetAssetImportData(), nullptr);

	Durin::FPackagePath VolumePath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
		"/Game/VolumeTexture", VolumePath));
	ASSERT_TRUE(Durin::AdmitAssetPackageToCatalog(VolumePath));
	Durin::DVolumeTexture* Volume = nullptr;
	const auto VolumeLoad = Durin::LoadObject<Durin::DVolumeTexture>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(VolumePath));
	Volume = VolumeLoad.value_or(nullptr);
	ASSERT_TRUE(VolumeLoad) << (VolumeLoad ? std::string{} : VolumeLoad.error().Message);
	ASSERT_NE(Volume, nullptr);
	EXPECT_FALSE(Volume->GetSource().IsValid());
	EXPECT_EQ(Volume->GetAssetImportData(), nullptr);
	Durin::ShutdownTaskSystem(Durin::ETaskShutdownMode::Drain);
	ASSERT_FALSE(Durin::IsTaskSchedulerRunning());
	ASSERT_NO_FATAL_FAILURE(ExpectLazyCookedInstall(*Texture2D));
	ASSERT_TRUE(Texture2D->GetPlatformData()->IsValid());
	EXPECT_EQ(Texture2D->GetPlatformData()->Mips.front().Width, 2u);
	EXPECT_EQ(Texture2D->GetPlatformData()->Mips.front().Height, 1u);
	ASSERT_NO_FATAL_FAILURE(ExpectLazyCookedInstall(*TextureCube));
	ASSERT_TRUE(TextureCube->GetPlatformData()->IsValid());
	EXPECT_EQ(TextureCube->GetSizeX(), 1u);
	ASSERT_NO_FATAL_FAILURE(ExpectLazyCookedInstall(*Volume));
	ASSERT_TRUE(Volume->GetPlatformData()->IsValid());
	EXPECT_EQ(Volume->GetPlatformData()->Mips.front().Width, 65u);
	EXPECT_EQ(Volume->GetPlatformData()->Mips.front().Height, 65u);
	EXPECT_EQ(Volume->GetPlatformData()->Mips.front().Depth, 65u);
	EXPECT_EQ(Volume->GetPlatformData()->PixelFormat, Durin::EPixelFormat::R8_UNORM);
	ASSERT_TRUE(Durin::UnloadPackage(Texture2DPath));
	ASSERT_TRUE(Durin::UnloadPackage(TextureCubePath));
	ASSERT_TRUE(Durin::UnloadPackage(VolumePath));
	EXPECT_FALSE(Durin::IsTaskSchedulerRunning());
	EXPECT_FALSE(Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting);
	// Background work can also finish without installing a deferred executor.
	ASSERT_TRUE(Durin::InitializeTaskScheduler(2));
	const auto AsyncReload = Durin::LoadObject<Durin::DTexture2D>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Texture2DPath));
	ASSERT_TRUE(AsyncReload);
	EXPECT_FALSE((*AsyncReload)->HasPlatformData());
	EXPECT_FALSE((*AsyncReload)->IsAsyncCacheComplete());
	EXPECT_FALSE((*AsyncReload)->HasPendingRenderResourceInitialization());
	ASSERT_TRUE((*AsyncReload)->FinishCachePlatformData(10.0));
	EXPECT_TRUE((*AsyncReload)->HasPlatformData());
	EXPECT_FALSE(Durin::GetGameThreadDeferredWorkQueueDiagnostics().bAccepting);
	ASSERT_TRUE(Durin::UnloadPackage(Texture2DPath));
}
