#include "Thumbnail/AssetThumbnailKey.h"
#include "ThumbnailManagerTestSupport.h"
#include "Misc/UnifiedError.h"
#include "Editor/ContentBrowserItemTestSupport.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Asset/AssetCompilingManager.h"
#include "NativePackageUnloadTestSupport.h"
#include "Thumbnail/StaticMeshThumbnailRenderer.h"

#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Editor/ContentBrowserQueryTestSupport.h"
#include "Panels/ContentBrowserDataTypes.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Thumbnail/ThumbnailStorage.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "DObject/Class.h"
#include "DObject/Package.h"
#include "Documents/EditorDocumentManager.h"
#include "MaterialEditorModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "NativeTestSupport.h"
#include "Image/ImageEncoder.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "StaticMeshEditorModule.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "TextureEditorModule.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

using Durin::ContentBrowser::Private::TestKind;
using Durin::ContentBrowser::Private::TestName;
using Durin::ContentBrowser::Private::TestVirtualPath;
using Durin::ContentBrowser::Private::TestPhysicalPath;
using Durin::ContentBrowser::Private::TestPackagePath;
using Durin::ContentBrowser::Private::TestAssetClassName;
using Durin::ContentBrowser::Private::TestExtension;
using Durin::ContentBrowser::Private::TestRedirectDestination;
using Durin::ContentBrowser::Private::TestFileSize;
using Durin::ContentBrowser::Private::TestPackageFileSize;
using Durin::ContentBrowser::Private::TestBulkFileSize;
using Durin::ContentBrowser::Private::TestLastWriteTime;

namespace
{
	// Cache reads finish on the I/O workers and are consumed on later frames.
	template <typename FCondition>
	auto AdvanceCacheUntil(Durin::FAssetThumbnailPool& Cache, FCondition&& Complete) -> bool
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		do
		{
			Durin::ProcessAsyncLoading();

			Cache.Tick();
			if (Complete()) return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		} while (std::chrono::steady_clock::now() < Deadline);
		return false;
	}

	auto MakeAssetPath(const Durin::FPackagePath& PackagePath)
		-> Durin::FTopLevelAssetPath
	{
		return Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(PackagePath);
	}

	auto RegisterStaticMeshThumbnailRenderer()
		-> Durin::FThumbnailRendererRegistrationHandle
	{
		std::string Error;
		auto Handle = Durin::GetDefaultThumbnailManager().RegisterCustomRenderer(Durin::DStaticMesh::StaticClass(), Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer::StaticClass());
		EXPECT_TRUE(Handle) << Error;
		return Handle ? std::move(Handle) : Durin::FThumbnailRendererRegistrationHandle{};
	}

	auto CaptureKey(Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer& Renderer, const Durin::FAssetData& Asset, std::string& OutError) -> std::string
	{
		auto Input = Durin::CaptureThumbnailKeyInput(Asset, Renderer.GetClass());
		if (!Input) { OutError = Input.error(); return {}; }
		return Durin::BuildAssetThumbnailCacheKey(*Input);
	}

	auto ContainsDependency(
		const Durin::FAssetThumbnailKeyInput& Request,
		std::string_view Path) -> bool
	{
		return std::ranges::any_of(
			Request.Packages,
			[Path](const Durin::FPackageData& Dependency) {
				return TestPackagePath(Dependency).GetView() == Path;
			});
	}

	auto MakeThumbnailPngBytes(uint32 Width, uint32 Height) -> Durin::FByteBuffer
	{
		// Cache-hit coverage owns a valid image matching the requested output,
		// independently of optional materialization of repository branding assets.
		Durin::FByteBuffer Pixels(static_cast<size_t>(Width) * Height * 4, std::byte{128});
		Durin::FByteBuffer Encoded;
		EXPECT_TRUE(Durin::Image::EncodeRgba8Png(Pixels, Width, Height, Encoded));
		return Encoded;
	}

	auto MakeCacheRoot(std::string_view Name) -> std::filesystem::path
	{
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "StaticMeshThumbnailCache" / Name;
		Durin::Testing::RemoveTestWorkDirectory(Root);
		return Root;
	}

	class FScopedThumbnailDerivedDataCache
	{
	public:
		explicit FScopedThumbnailDerivedDataCache(std::string_view Name)
			: Previous(Durin::FPaths::DerivedDataCacheDir())
			, Root(Durin::Testing::GetTestWorkDirectory()
				/ "StaticMeshThumbnailDDC" / Name)
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::FPaths::SetDerivedDataCacheDirForTests(Root.generic_string());
		}

		~FScopedThumbnailDerivedDataCache()
		{
			Durin::FPaths::SetDerivedDataCacheDirForTests(Previous);
			Durin::Testing::RemoveTestWorkDirectory(Root);
		}

	private:
		std::string Previous;
		std::filesystem::path Root;
	};
}

TEST(FStaticMeshThumbnailRendererTests,
	MissingDerivedDataRebuildsBeforeThumbnailReadiness)
{
	std::optional<Durin::UnifiedError::FError> LoadResultError;
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		Durin::InitializeAssetCompilingManager();
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	Durin::FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	std::string Error;

	Durin::FPackagePath SplineBoxPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/Engine/Models/SplineBox", SplineBoxPath));
	(void)Durin::UnloadPackage(
		SplineBoxPath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
	FScopedThumbnailDerivedDataCache DerivedDataCache("SplineBoxRecovery");
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(SplineBoxPath);
	ASSERT_NE(Data, nullptr);
	Durin::DStaticMesh* Mesh = nullptr;
	const auto LoadResult =
		Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(SplineBoxPath), nullptr, &LoadResultError);
		Mesh = LoadResult;
	ASSERT_TRUE(LoadResult) << (LoadResultError ? LoadResultError->ToString() : "");
	ASSERT_NE(Mesh, nullptr);
	ASSERT_TRUE(Mesh->IsCompiling());

	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer>();
	auto& Renderer = *RendererOwner;

	ASSERT_FALSE(CaptureKey(Renderer, Data->TopLevelAssets.front(), Error).empty())
		<< Error;
	{
		Durin::FThumbnailDrawContext Context(Mesh);
		Renderer.Draw(Mesh, Context);
		EXPECT_TRUE(Durin::FThumbnailManagerTestAccess::IsWaiting(Context));
		EXPECT_TRUE(Durin::FThumbnailManagerTestAccess::GetDiagnostic(Context).empty());
		EXPECT_FALSE(Durin::FThumbnailManagerTestAccess::Validate(Context));
		Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
		ASSERT_TRUE(Mesh->GetLOD0LocalBounds().has_value());
		ASSERT_NE(Mesh->GetRenderData(), nullptr);
	}
	ASSERT_TRUE(Durin::UnloadPackage(
		SplineBoxPath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshThumbnailRendererTests, PoolLoadsAsynchronouslyAndCancellationReleasesRoot)
{
	using namespace Durin;
	Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
	const auto Path = Fixtures.StaticMesh->GetPackage()->GetPackagePathIdentity();
	const auto Data = FindPackageExact(Path);
	ASSERT_NE(Data, nullptr);
	const auto Asset = Data->TopLevelAssets.front();
	auto Registration = RegisterStaticMeshThumbnailRenderer();
	ASSERT_TRUE(Registration);
	ASSERT_TRUE(UnloadPackage(Path));
	FAssetThumbnailPool Pool({}, {.CacheRoot = Testing::GetTestWorkDirectory() / "AsyncThumbnailRoot"});
	Pool.Prefetch(Asset);
	ASSERT_TRUE(AdvanceCacheUntil(Pool, [&] { return Pool.GetStats().Generation.Loads == 1; }));
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	Pool.CancelPendingRequests();
	ProcessAsyncLoading();
	EXPECT_EQ(FindResidentPackage(Path), nullptr);
	Pool.Prefetch(Asset);
	ASSERT_TRUE(AdvanceCacheUntil(Pool, [&] { return Pool.GetStats().Generation.Loads == 2; }));
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!FindResidentPackage(Path) && std::chrono::steady_clock::now() < Deadline)
	{
		ProcessAsyncLoading();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_NE(FindResidentPackage(Path), nullptr);
	EXPECT_FALSE(UnloadPackage(Path));
	Pool.CancelPendingRequests();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_TRUE(UnloadPackage(Path));
}

TEST(FStaticMeshThumbnailRendererTests, CacheKeyIncludesTransitiveMeshPackages)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error)) << Error;
	Durin::FPackagePath Path;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath, Path));
	const auto Data = Durin::FindPackageExact(Path);
	ASSERT_TRUE(Data);
	auto Renderer = Durin::MakeThumbnailRenderer<Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer>();
	const auto Input = Durin::CaptureThumbnailKeyInput(Data->TopLevelAssets.front(), Renderer->GetClass());
	ASSERT_TRUE(Input) << Input.error();
	EXPECT_TRUE(ContainsDependency(*Input, Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(*Input, Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
}

TEST(FStaticMeshThumbnailRendererTests,
	SharedCacheQueuesStaticMeshAndLeavesUnsupportedClassUnrequested)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto RendererRegistration = RegisterStaticMeshThumbnailRenderer();
	Durin::FPackagePath StaticMeshPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::FAssetThumbnailPool Cache;
	Durin::ProcessAsyncLoading();

	Cache.AccessTexture(
		Data->TopLevelAssets.front(), Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(MakeAssetPath(StaticMeshPath)).State,
		Durin::EAssetThumbnailState::Pending);
	Cache.Tick();
	ASSERT_TRUE(AdvanceCacheUntil(Cache, [&] {
		return Cache.Find(MakeAssetPath(StaticMeshPath)).State == Durin::EAssetThumbnailState::Failed;
	}));
	const Durin::FAssetThumbnailView Routed = Cache.Find(MakeAssetPath(StaticMeshPath));
	EXPECT_EQ(Routed.State, Durin::EAssetThumbnailState::Failed);
	EXPECT_NE(Routed.Diagnostic.find("unavailable"), std::string::npos);

	Durin::FPackagePath UnsupportedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/ThumbnailFixtures/Unsupported/A_Unsupported",
		UnsupportedPath));
	Cache.AccessTexture({.AssetPath = MakeAssetPath(UnsupportedPath),
		.AssetClassName = "DUnsupportedThumbnailAsset"}, Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(MakeAssetPath(UnsupportedPath)).State,
		Durin::EAssetThumbnailState::None);
	Cache.CancelPendingRequests();
}

TEST(FStaticMeshThumbnailRendererTests,
	WarmPersistentHitDoesNotLoadStaticMeshOrCreatePreviewScene)
{
	if (Durin::GetRenderCommandAdmissionState()
		!= Durin::ERenderCommandAdmissionState::Running)
		Durin::InitRenderingThread();
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto RendererRegistration = RegisterStaticMeshThumbnailRenderer();
	Durin::FPackagePath StaticMeshPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::FAssetData AssetData = Data->TopLevelAssets.front();
	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer>();
	auto& Renderer = *RendererOwner;

	const std::string CacheKey =
		CaptureKey(Renderer, AssetData, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("WarmHit");
	{
		Durin::FThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, MakeThumbnailPngBytes(Durin::FAssetThumbnailOutputSettings{}.Width, Durin::FAssetThumbnailOutputSettings{}.Height)));
	}
	ASSERT_TRUE(Durin::UnloadPackage(StaticMeshPath));
	ASSERT_EQ(Durin::FindResidentPackage(StaticMeshPath), nullptr);

	{
		Durin::FAssetThumbnailPool Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Durin::ProcessAsyncLoading();

		Cache.AccessTexture(AssetData, Durin::EAssetThumbnailPriority::Visible);
		Cache.Tick();
		ASSERT_TRUE(AdvanceCacheUntil(Cache, [&] { return Cache.GetStats().UploadsQueued == 1; }));
		const Durin::FAssetThumbnailPoolStats Stats = Cache.GetStats();
		EXPECT_EQ(Stats.Generation.DiskHits, 1u);
		EXPECT_TRUE(Cache.Find(AssetData.AssetPath).bHasTransparency);
		EXPECT_EQ(Stats.Generation.Loads, 0u);
		EXPECT_EQ(Stats.Generation.Renders, 0u);
		EXPECT_EQ(Stats.Generation.Readbacks, 0u);
		EXPECT_EQ(Stats.CaptureOutputCreations, 0u);
		EXPECT_EQ(Stats.CapturesStarted, 0u);
		EXPECT_EQ(Stats.UploadsQueued, 1u);
		EXPECT_FALSE(Stats.bHasCaptureOutput);
		EXPECT_EQ(Durin::FindResidentPackage(StaticMeshPath), nullptr);
		Cache.CancelPendingRequests();
		Durin::FlushRenderingCommands();
		Durin::ProcessAsyncLoading();

		EXPECT_EQ(
			Cache.Find(MakeAssetPath(StaticMeshPath)).State,
			Durin::EAssetThumbnailState::None);
		EXPECT_EQ(Cache.GetStats().UploadsCompleted, 0u);
		EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
		Cache.Tick();
		Cache.Clear();
	}
}

TEST(FStaticMeshThumbnailRendererTests,
	CorruptPersistentHitIsInvalidatedAndRetriedThroughColdPath)
{
	if (Durin::GetRenderCommandAdmissionState()
		!= Durin::ERenderCommandAdmissionState::Running)
		Durin::InitRenderingThread();
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto RendererRegistration = RegisterStaticMeshThumbnailRenderer();
	Durin::FPackagePath StaticMeshPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::FAssetData AssetData = Data->TopLevelAssets.front();
	auto RendererOwner = Durin::MakeThumbnailRenderer<Durin::StaticMeshEditor::DStaticMeshThumbnailRenderer>();
	auto& Renderer = *RendererOwner;

	const std::string CacheKey =
		CaptureKey(Renderer, AssetData, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("CorruptRecovery");
	{
		const std::array<uint8, 8> Corrupt = {0, 1, 2, 3, 4, 5, 6, 7};
		Durin::FThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, std::as_bytes(std::span{Corrupt})));
	}
	{
		Durin::FAssetThumbnailPool Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Durin::ProcessAsyncLoading();

		Cache.AccessTexture(AssetData, Durin::EAssetThumbnailPriority::Visible);
		Cache.Tick();
		ASSERT_TRUE(AdvanceCacheUntil(Cache, [&] { return Cache.GetStats().Generation.Retries == 1; }));
		EXPECT_EQ(Cache.GetStats().Generation.DiskHits, 0u);
		EXPECT_EQ(Cache.GetStats().Generation.Retries, 1u);
		EXPECT_EQ(Cache.GetStats().Generation.Loads, 1u);
		EXPECT_NE(Durin::FindResidentPackage(StaticMeshPath), nullptr);
		{
			Durin::FThumbnailObjectStore Store({
				.CacheRoot = CacheRoot,
				.ObjectExtension = ".png"});
			Durin::FByteBuffer Encoded;
			EXPECT_EQ(
				Store.Load(CacheKey, Encoded),
				Durin::EThumbnailObjectLoadResult::Miss);
		}
		Cache.Clear();
	}
	Durin::FlushRenderingCommands();
	ASSERT_TRUE(Durin::UnloadPackage(StaticMeshPath));
}

TEST(FStaticMeshThumbnailRendererTests,
	ContentBrowserMutationAndCloseLifecycleRejectsStaleStaticMeshWork)
{
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto RendererRegistration = RegisterStaticMeshThumbnailRenderer();
	Durin::FPackagePath StaticMeshPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::FPackageCatalogEntry Data =
		Durin::FindPackageExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	using Durin::ContentBrowser::FContentBrowserItem;
	using Durin::ContentBrowser::EContentBrowserItemKind;
	using namespace Durin::ContentBrowser::Private;
	FNativeContentBrowserTestQuery Provider;
	FContentBrowserNativeItem NativeInput{
		.Kind = EContentBrowserItemKind::Asset,
		.VirtualPath = Data->TopLevelAssets.front().AssetPath.ToString(),
		.PackagePath = StaticMeshPath,
		.PhysicalPath = TestPhysicalPath(*(Data)),
		.FileSize = TestFileSize(*(Data))};
	const FContentBrowserItem Item = Provider.BindItemForTesting(NativeInput);
	FContentBrowserThumbnailReferences Cache;
	Durin::ProcessAsyncLoading();
	Cache.BeginFrame();
	Cache.Request(Item, Durin::EAssetThumbnailPriority::Prefetch);
	EXPECT_EQ(Cache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::Pending);
	Cache.CancelPendingRequests();
	EXPECT_EQ(Cache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::None);

	// A stale display snapshot cannot supply the package-cache fingerprint.
	auto StaleInput = NativeInput; ++StaleInput.FileSize;
	const auto StaleDisplay = Provider.BindItemForTesting(StaleInput);
	Cache.Request(StaleDisplay, Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(Cache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::Pending);
	Cache.ApplyContentChanges({.Changes = {{Durin::EContentChangeKind::Modified,
		TestPhysicalPath(Item), TestPhysicalPath(Item)}}});
	EXPECT_EQ(Cache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::None);
	Cache.Request(Item, Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(Cache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::Pending);
	Cache.Clear();
	EXPECT_EQ(Cache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::None);

	// An unregistered identity cannot borrow a different asset's fingerprint.
	auto MissingInput = NativeInput;
	MissingInput.VirtualPath = "/ThumbnailFixtures/Meshes/Missing.Missing";
	const auto Missing = Provider.BindItemForTesting(MissingInput);
	Cache.Request(Missing, Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(Cache.GetAssetReferenceCountForTesting(), 0u);
	{
		FContentBrowserThumbnailReferences ClosingCache;
		ClosingCache.BeginFrame();
		ClosingCache.Request(Item, Durin::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(ClosingCache.Find(Item.GetItemKey()).State, Durin::EAssetThumbnailState::Pending);
	}
}

TEST(FStaticMeshThumbnailRendererTests,
	MixedEditorModulesUnloadQueuedThumbnailsWithoutCrossModuleLoss)
{
	InitializeDObjectSystem();
	if (!Durin::FAssetCompilingManager::Get().IsAcceptingRequests())
		Durin::InitializeAssetCompilingManager();
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;

	Durin::FEditorDocumentManager Manager;
	auto& ThumbnailManager = Durin::GetDefaultThumbnailManager();
	Durin::FMaterialEditorModule MaterialModule;
	Durin::FTextureEditorModule TextureModule;
	Durin::FStaticMeshEditorModule StaticMeshModule;
	Durin::FModuleTestHarness MaterialHarness("MaterialEditor");
	Durin::FModuleTestHarness TextureHarness("TextureEditor");
	Durin::FModuleTestHarness StaticMeshHarness("StaticMeshEditor");
	MaterialHarness.Start(MaterialModule);
	TextureHarness.Start(TextureModule);
	StaticMeshHarness.Start(StaticMeshModule);
	// Material, Material Function, Material Parameter Collection, Texture,
	// Volume Texture, and Static Mesh.
	EXPECT_TRUE(!Manager.GetToolkit());

	const std::string MaterialClass =
		Durin::DMaterial::StaticClass()->GetQualifiedName().ToString();
	const std::string MaterialInstanceClass =
		Durin::DMaterialInstance::StaticClass()->GetQualifiedName().ToString();
	const std::string Texture2DClass =
		Durin::DTexture2D::StaticClass()->GetQualifiedName().ToString();
	const std::string TextureCubeClass =
		Durin::DTextureCube::StaticClass()->GetQualifiedName().ToString();
	const std::string StaticMeshClass =
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	EXPECT_TRUE(ThumbnailManager.Find(MaterialClass));
	EXPECT_TRUE(ThumbnailManager.Find(MaterialInstanceClass));
	EXPECT_TRUE(ThumbnailManager.Find(Texture2DClass));
	EXPECT_TRUE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_TRUE(ThumbnailManager.Find(StaticMeshClass));

	Durin::FAssetThumbnailPool Cache(ThumbnailManager);
	Durin::ProcessAsyncLoading();

	for (const std::string_view Path : {
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath,
		Durin::Tests::FAssetThumbnailFixtureSet::DirectionalCubePath,
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath})
	{
		Durin::FPackagePath AssetPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(Path, AssetPath));
		const Durin::FPackageCatalogEntry Data =
			Durin::FindPackageExact(AssetPath);
		ASSERT_NE(Data, nullptr);
		Cache.AccessTexture(Data->TopLevelAssets.front(), Durin::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Cache.Find(MakeAssetPath(AssetPath)).State, Durin::EAssetThumbnailState::Pending);
	}

	// MainFrame shutdown removes concrete modules in reverse composition order.
	// Removing one renderer drains its queued lease without disturbing the others.
	StaticMeshHarness.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(StaticMeshClass));
	EXPECT_TRUE(ThumbnailManager.Find(MaterialClass));
	EXPECT_TRUE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_EQ(
		Manager.FindToolkit(Durin::FAssetEditorToolkitId("StaticMeshEditor")),
		nullptr);
	Cache.Tick();

	TextureHarness.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(Texture2DClass));
	EXPECT_FALSE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_TRUE(ThumbnailManager.Find(MaterialClass));
	EXPECT_EQ(
		Manager.FindToolkit(Durin::FAssetEditorToolkitId("TextureEditor")),
		nullptr);
	EXPECT_EQ(
		Manager.FindToolkit(Durin::FAssetEditorToolkitId("VolumeTextureEditor")),
		nullptr);

	MaterialHarness.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(MaterialClass));
	EXPECT_FALSE(ThumbnailManager.Find(MaterialInstanceClass));
	EXPECT_EQ(
		Manager.FindToolkit(Durin::FAssetEditorToolkitId("MaterialEditor")),
		nullptr);
	EXPECT_TRUE(!Manager.GetToolkit());

	Cache.CancelPendingRequests();
	Cache.Clear();
	ThumbnailManager.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(StaticMeshClass));
}
