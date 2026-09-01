#include "Thumbnail/StaticMeshThumbnailRenderer.h"

#include "Assets/ContentBrowserThumbnailReferences.h"
#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/AssetThumbnailTestFixtures.h"
#include "Thumbnail/ThumbnailStorage.h"

#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCook.h"
#include "DObject/Class.h"
#include "Editor/WorkspaceManager.h"
#include "MaterialEditorModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "NativeTestSupport.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMeshEditorModule.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "TextureEditorModule.h"

#include <gtest/gtest.h>

#include "NativeDObjectTestSupport.h"

namespace
{
	auto MakeAssetPath(const Durin::FPackagePath& PackagePath)
		-> Durin::FTopLevelAssetPath
	{
		return Durin::Testing::MakePackageLeafTopLevelAssetPathForTests(PackagePath);
	}

	auto RegisterStaticMeshThumbnailRenderer()
		-> Durin::Editor::FThumbnailRendererRegistrationHandle
	{
		std::string Error;
		auto Handle = Durin::Editor::GetDefaultThumbnailManager().RegisterScoped(
			std::make_unique<Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer>(), Error);
		EXPECT_TRUE(Handle) << Error;
		return Handle;
	}

	auto MakeFingerprint(const Durin::FAssetData& Data)
		-> Durin::Editor::FAssetThumbnailPackageFingerprint
	{
		return {
			.AssetPath = MakeAssetPath(Data.PackagePath),
			.PackagePath = Data.PackagePath,
			.AssetClassName = Data.AssetClassName,
			.PackageFormatVersion = Data.FormatVersion,
			.FileSize = static_cast<uint64>(Data.FileSize),
			.LastWriteTimeTicks = Data.LastWriteTimeTicks};
	}

	auto MakeRequest(
		const Durin::Editor::FAssetThumbnailPackageFingerprint& Asset,
		uint64 Serial = 1) -> Durin::Editor::FAssetThumbnailRequest
	{
		return {
			.Asset = Asset,
			.Priority = Durin::Editor::EAssetThumbnailPriority::Visible,
			.RequestSerial = Serial};
	}

	auto CaptureKey(
		Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer& Renderer,
		const Durin::Editor::FAssetThumbnailPackageFingerprint& Asset,
		Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::Editor::FThumbnailRenderingInfo Registration =
			Renderer.GetRegistration();
		if (!Renderer.CaptureGenerationRequest(
				MakeRequest(Asset), 7, OutRequest, OutError))
		{
			return {};
		}
		OutRequest.KeyInput.Asset = Asset;
		OutRequest.KeyInput.RendererName = Registration.RendererName;
		OutRequest.KeyInput.GeneratorSchemaVersion =
			Registration.GeneratorSchemaVersion;
		return Durin::Editor::BuildAssetThumbnailCacheKey(OutRequest.KeyInput);
	}

	auto ContainsDependency(
		const Durin::Editor::FAssetThumbnailGenerationRequest& Request,
		std::string_view Path) -> bool
	{
		return std::ranges::any_of(
			Request.KeyInput.Dependencies,
			[Path](const Durin::Editor::FAssetThumbnailPackageFingerprint& Dependency) {
				return Dependency.PackagePath.GetView() == Path;
			});
	}

	auto LoadThumbnailPngBytes() -> Durin::FByteArray
	{
		Durin::FByteArray Bytes;
		EXPECT_TRUE(Durin::FFileHelper::LoadFileToArray(
			Bytes,
			std::filesystem::path(Durin::FPaths::EngineContentDir())
				/ "Editor/Branding/DurinEditorLogoUI.png"));
		return Bytes;
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
	InitializeDObjectSystem();
	Durin::Testing::FScopedMountRegistryFixture MountRegistry;
	Durin::FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::RefreshAssetRegistry());
	Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	std::string Error;

	Durin::FPackagePath SplineBoxPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/Engine/Models/SplineBox", SplineBoxPath));
	(void)Durin::UnloadPackage(
		SplineBoxPath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved);
	FScopedThumbnailDerivedDataCache DerivedDataCache("SplineBoxRecovery");
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(SplineBoxPath);
	ASSERT_NE(Data, nullptr);
	Durin::DStaticMesh* Mesh = nullptr;
	const Durin::FAssetResult LoadResult =
		Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(SplineBoxPath), Mesh);
	ASSERT_TRUE(LoadResult) << LoadResult.Message;
	ASSERT_NE(Mesh, nullptr);
	ASSERT_TRUE(Mesh->GetLOD0LocalBounds().has_value());
	ASSERT_NE(Mesh->GetRenderData(), nullptr);

	Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	ASSERT_FALSE(CaptureKey(Renderer, MakeFingerprint(*Data), Request, Error).empty())
		<< Error;
	std::unique_ptr<Durin::Editor::IThumbnailRendererSession> Session =
		Renderer.CreateGenerationSession(Request, *Request.Input, Error);
	ASSERT_NE(Session, nullptr) << Error;
	const Durin::Editor::FThumbnailRendererSessionUpdate Initial = Session->Load();
	EXPECT_EQ(Initial.Diagnostic.find("non-degenerate LOD 0 bounds"),
		std::string::npos);
	Session.reset();
	ASSERT_TRUE(Durin::UnloadPackage(
		SplineBoxPath, Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshThumbnailRendererTests,
	RendererCapturesExactClassSortedDependenciesAndFrozenInput)
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
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint =
		MakeFingerprint(*Data);

	Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer Renderer;
	const Durin::Editor::FThumbnailRenderingInfo Registration =
		Renderer.GetRegistration();
	EXPECT_EQ(
		Registration.AssetClassName,
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(
		Registration.RendererName,
		Durin::Editor::StaticMesh::FStaticMeshThumbnailRendererContract::RendererName);
	EXPECT_EQ(
		Registration.GeneratorSchemaVersion,
		Durin::Editor::StaticMesh::FStaticMeshThumbnailRendererContract::GeneratorSchemaVersion);

	Durin::Editor::FAssetThumbnailGenerationRequest FirstRequest;
	const std::string FirstKey =
		CaptureKey(Renderer, Fingerprint, FirstRequest, Error);
	ASSERT_FALSE(FirstKey.empty()) << Error;
	EXPECT_TRUE(std::ranges::is_sorted(
		FirstRequest.KeyInput.Dependencies,
		{},
		[](const Durin::Editor::FAssetThumbnailPackageFingerprint& Dependency) {
			return Dependency.PackagePath.GetView();
		}));
	EXPECT_TRUE(ContainsDependency(
		FirstRequest,
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(
		FirstRequest,
		Durin::Tests::FAssetThumbnailFixtureSet::ParentTexturePath));
	const auto Input = std::dynamic_pointer_cast<
		const Durin::Editor::StaticMesh::FStaticMeshThumbnailRendererGenerationInput>(FirstRequest.Input);
	ASSERT_NE(Input, nullptr);
	EXPECT_EQ(Input->AssetPath, MakeAssetPath(StaticMeshPath));
	EXPECT_EQ(Input->VisualContract.Output, FirstRequest.KeyInput.Output);
	EXPECT_FALSE(Input->VisualContract.bOutputOpaque);
	EXPECT_EQ(Input->VisualContract.CameraDirectionX, 2.4f);
	EXPECT_EQ(Input->VisualContract.CameraDirectionY, -3.2f);
	EXPECT_EQ(Input->VisualContract.CameraDirectionZ, 2.4f);

	Durin::Editor::FAssetThumbnailGenerationRequest RepeatedRequest;
	EXPECT_EQ(
		CaptureKey(Renderer, Fingerprint, RepeatedRequest, Error),
		FirstKey);
	EXPECT_EQ(RepeatedRequest.KeyInput.Dependencies,
		FirstRequest.KeyInput.Dependencies);

	Durin::Editor::FAssetThumbnailKeyInput ChangedDependency = FirstRequest.KeyInput;
	ASSERT_FALSE(ChangedDependency.Dependencies.empty());
	++ChangedDependency.Dependencies.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::Editor::BuildAssetThumbnailCacheKey(ChangedDependency),
		FirstKey);

	Durin::Editor::FAssetThumbnailGenerationRequest WrongClassRequest;
	Durin::Editor::FAssetThumbnailPackageFingerprint WrongClass = Fingerprint;
	WrongClass.AssetClassName = "DMaterial";
	EXPECT_FALSE(Renderer.CaptureGenerationRequest(
		MakeRequest(WrongClass), 7, WrongClassRequest, Error));
	EXPECT_NE(Error.find("wrong asset class"), std::string::npos);
}

TEST(FStaticMeshThumbnailRendererTests,
	RendererRejectsMissingAndStaleRegistrySnapshots)
{
	Durin::Tests::RegisterAssetThumbnailFixtureMount();
	Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer Renderer;
	Durin::FPackagePath MissingPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/ThumbnailFixtures/Meshes/SM_Missing", MissingPath));
	Durin::Editor::FAssetThumbnailPackageFingerprint Missing = {
		.AssetPath = MakeAssetPath(MissingPath),
		.PackagePath = MissingPath,
		.AssetClassName = Durin::DStaticMesh::StaticClass()
			->GetQualifiedName().ToString(),
		.PackageFormatVersion = 1,
		.FileSize = 1,
		.LastWriteTimeTicks = 1};
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Renderer.CaptureGenerationRequest(
		MakeRequest(Missing), 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);

	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto RendererRegistration = RegisterStaticMeshThumbnailRenderer();
	Durin::FPackagePath StaticMeshPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	Durin::Editor::FAssetThumbnailPackageFingerprint Stale = MakeFingerprint(*Data);
	++Stale.FileSize;
	EXPECT_FALSE(Renderer.CaptureGenerationRequest(
		MakeRequest(Stale), 1, Captured, Error));
	EXPECT_NE(Error.find("changed"), std::string::npos);
}

TEST(FStaticMeshThumbnailRendererTests,
	GeneratorFixtureAndShaderVersionsChangePersistentKey)
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
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	const std::string Baseline =
		CaptureKey(Renderer, MakeFingerprint(*Data), Request, Error);
	ASSERT_FALSE(Baseline.empty()) << Error;
	for (const auto Mutate : {
			+[](Durin::Editor::FAssetThumbnailKeyInput& Input) {
				++Input.GeneratorSchemaVersion;
			},
			+[](Durin::Editor::FAssetThumbnailKeyInput& Input) {
				++Input.PreviewFixtureVersion;
			},
			+[](Durin::Editor::FAssetThumbnailKeyInput& Input) {
				++Input.ShaderContractVersion;
			}})
	{
		Durin::Editor::FAssetThumbnailKeyInput Changed = Request.KeyInput;
		Mutate(Changed);
		EXPECT_NE(Durin::Editor::BuildAssetThumbnailCacheKey(Changed), Baseline);
	}
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
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::FAssetThumbnailPool Cache;
	Cache.BeginFrame();
	Cache.Request(
		MakeFingerprint(*Data), Durin::Editor::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(MakeAssetPath(StaticMeshPath)).State,
		Durin::Editor::EAssetThumbnailState::Queued);
	Cache.EndFrame();
	const Durin::Editor::FAssetThumbnailView Routed = Cache.Find(MakeAssetPath(StaticMeshPath));
	EXPECT_EQ(Routed.State, Durin::Editor::EAssetThumbnailState::Failed);
	EXPECT_NE(Routed.Diagnostic.find("unavailable"), std::string::npos);

	Durin::FPackagePath UnsupportedPath;
	ASSERT_TRUE(Durin::FPackagePath::TryCreate(
		"/ThumbnailFixtures/Unsupported/A_Unsupported",
		UnsupportedPath));
	Cache.Request({
		.AssetPath = MakeAssetPath(UnsupportedPath),
		.PackagePath = UnsupportedPath,
		.AssetClassName = "DUnsupportedThumbnailAsset",
		.PackageFormatVersion = 1,
		.FileSize = 1,
		.LastWriteTimeTicks = 1}, Durin::Editor::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(MakeAssetPath(UnsupportedPath)).State,
		Durin::Editor::EAssetThumbnailState::NotRequested);
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
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint = MakeFingerprint(*Data);
	Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;
	const std::string CacheKey =
		CaptureKey(Renderer, Fingerprint, GenerationRequest, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("WarmHit");
	{
		Durin::Editor::FThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, LoadThumbnailPngBytes()));
	}
	ASSERT_TRUE(Durin::UnloadPackage(StaticMeshPath));
	ASSERT_EQ(Durin::FindResidentPackage(StaticMeshPath), nullptr);

	{
		Durin::Editor::FAssetThumbnailPool Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Cache.BeginFrame();
		Cache.Request(Fingerprint, Durin::Editor::EAssetThumbnailPriority::Visible);
		Cache.EndFrame();
		const Durin::Editor::FAssetThumbnailPoolStats Stats = Cache.GetStats();
		EXPECT_EQ(Stats.Generation.DiskHits, 1u);
		EXPECT_EQ(Stats.Generation.Loads, 0u);
		EXPECT_EQ(Stats.Generation.Renders, 0u);
		EXPECT_EQ(Stats.Generation.Readbacks, 0u);
		EXPECT_EQ(Stats.PreviewSceneCreations, 0u);
		EXPECT_EQ(Stats.PreviewSceneAssignments, 0u);
		EXPECT_EQ(Stats.UploadsQueued, 1u);
		EXPECT_FALSE(Stats.bHasPreviewScene);
		EXPECT_EQ(Durin::FindResidentPackage(StaticMeshPath), nullptr);
		Cache.CancelPendingRequests();
		Durin::FlushRenderingCommands();
		Cache.BeginFrame();
		EXPECT_EQ(
			Cache.Find(MakeAssetPath(StaticMeshPath)).State,
			Durin::Editor::EAssetThumbnailState::NotRequested);
		EXPECT_EQ(Cache.GetStats().UploadsCompleted, 0u);
		EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
		Cache.EndFrame();
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
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint = MakeFingerprint(*Data);
	Durin::Editor::StaticMesh::DStaticMeshThumbnailRenderer Renderer;
	Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;
	const std::string CacheKey =
		CaptureKey(Renderer, Fingerprint, GenerationRequest, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("CorruptRecovery");
	{
		const std::array<uint8, 8> Corrupt = {0, 1, 2, 3, 4, 5, 6, 7};
		Durin::Editor::FThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, std::as_bytes(std::span{Corrupt})));
	}
	{
		Durin::Editor::FAssetThumbnailPool Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Cache.BeginFrame();
		Cache.Request(Fingerprint, Durin::Editor::EAssetThumbnailPriority::Visible);
		Cache.EndFrame();
		EXPECT_EQ(Cache.Find(MakeAssetPath(StaticMeshPath)).State, Durin::Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(Cache.GetStats().Generation.DiskHits, 1u);
		EXPECT_EQ(Cache.GetStats().Generation.Retries, 1u);
		EXPECT_EQ(Cache.GetStats().Generation.Loads, 0u);

		Cache.BeginFrame();
		Cache.EndFrame();
		EXPECT_EQ(Cache.GetStats().Generation.Loads, 1u);
		EXPECT_NE(Durin::FindResidentPackage(StaticMeshPath), nullptr);
		{
			Durin::Editor::FThumbnailObjectStore Store({
				.CacheRoot = CacheRoot,
				.ObjectExtension = ".png"});
			Durin::FByteArray Encoded;
			EXPECT_EQ(
				Store.Load(CacheKey, Encoded),
				Durin::Editor::EThumbnailObjectLoadResult::Miss);
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
	const Durin::FAssetCatalogEntry Data =
		Durin::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Current = MakeFingerprint(*Data);

	Durin::Editor::ContentBrowser::Private::FContentBrowserThumbnailReferences Cache;
	Cache.BeginFrame();
	Cache.Request({
		.Identity = "/ThumbnailFixtures/Meshes/SM_OldIdentity",
		.Asset = Current,
		.Priority = Durin::Editor::EAssetThumbnailPriority::Prefetch});
	EXPECT_EQ(
		Cache.Find("/ThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::Editor::EAssetThumbnailState::Queued);
	// Refresh, rename, and move all cancel the old visible snapshot before rebinding identities.
	Cache.CancelPendingRequests();
	EXPECT_EQ(
		Cache.Find("/ThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::Editor::EAssetThumbnailState::NotRequested);

	Durin::Editor::FAssetThumbnailPackageFingerprint StaleReimport = Current;
	++StaleReimport.LastWriteTimeTicks;
	Cache.Request({
		.Identity = "/ThumbnailFixtures/Meshes/SM_NewIdentity",
		.Asset = StaleReimport,
		.Priority = Durin::Editor::EAssetThumbnailPriority::Visible});
	Cache.EndFrame();
	EXPECT_EQ(
		Cache.Find("/ThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::Editor::EAssetThumbnailState::Invalid);
	Cache.BeginFrame();
	Cache.Request({
		.Identity = "/ThumbnailFixtures/Meshes/SM_NewIdentity",
		.Asset = Current,
		.Priority = Durin::Editor::EAssetThumbnailPriority::Visible});
	EXPECT_EQ(
		Cache.Find("/ThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::Editor::EAssetThumbnailState::Queued);
	// Delete and panel close clear identity bindings and pending work without rendering.
	Cache.Clear();
	EXPECT_EQ(
		Cache.Find("/ThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::Editor::EAssetThumbnailState::NotRequested);
	EXPECT_EQ(
		Cache.Find("/ThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::Editor::EAssetThumbnailState::NotRequested);

	{
		Durin::Editor::ContentBrowser::Private::FContentBrowserThumbnailReferences ClosingCache;
		ClosingCache.BeginFrame();
		ClosingCache.Request({
			.Identity = StaticMeshPath.GetView(),
			.Asset = Current,
			.Priority = Durin::Editor::EAssetThumbnailPriority::Visible});
		EXPECT_EQ(
			ClosingCache.Find(StaticMeshPath.GetView()).State,
			Durin::Editor::EAssetThumbnailState::Queued);
	}
}

TEST(FStaticMeshThumbnailRendererTests,
	MixedEditorModulesUnloadQueuedThumbnailsWithoutCrossModuleLoss)
{
	InitializeDObjectSystem();
	Durin::Tests::FAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateAssetThumbnailFixtures(Fixtures, Error))
		<< Error;

	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::DThumbnailManager ThumbnailManager;
	Durin::FMaterialEditorModule MaterialModule;
	Durin::FTextureEditorModule TextureModule;
	Durin::FStaticMeshEditorModule StaticMeshModule;
	Durin::FModuleTestHarness MaterialHarness("MaterialEditor");
	Durin::FModuleTestHarness TextureHarness("TextureEditor");
	Durin::FModuleTestHarness StaticMeshHarness("StaticMeshEditor");
	MaterialHarness.Start(MaterialModule);
	TextureHarness.Start(TextureModule);
	StaticMeshHarness.Start(StaticMeshModule);
	ASSERT_TRUE(MaterialModule.RegisterMaterialEditor(Manager, ThumbnailManager));
	ASSERT_TRUE(TextureModule.RegisterTextureEditor(Manager, ThumbnailManager));
	ASSERT_TRUE(StaticMeshModule.RegisterStaticMeshEditor(Manager, ThumbnailManager));
	EXPECT_EQ(Manager.GetWorkspaceDescriptors().size(), 4u);

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

	Durin::Editor::FAssetThumbnailPool Cache(ThumbnailManager);
	Cache.BeginFrame();
	for (const std::string_view Path : {
		Durin::Tests::FAssetThumbnailFixtureSet::MaterialPath,
		Durin::Tests::FAssetThumbnailFixtureSet::DirectionalCubePath,
		Durin::Tests::FAssetThumbnailFixtureSet::StaticMeshPath})
	{
		Durin::FPackagePath AssetPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate(Path, AssetPath));
		const Durin::FAssetCatalogEntry Data =
			Durin::FindAssetExact(AssetPath);
		ASSERT_NE(Data, nullptr);
		Cache.Request(MakeFingerprint(*Data), Durin::Editor::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Cache.Find(MakeAssetPath(AssetPath)).State, Durin::Editor::EAssetThumbnailState::Queued);
	}

	// MainFrame shutdown removes concrete modules in reverse composition order.
	// Removing one renderer drains its queued lease without disturbing the others.
	StaticMeshModule.UnregisterStaticMeshEditor();
	EXPECT_FALSE(ThumbnailManager.Find(StaticMeshClass));
	EXPECT_TRUE(ThumbnailManager.Find(MaterialClass));
	EXPECT_TRUE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("StaticMeshEditor")),
		nullptr);
	Cache.EndFrame();

	TextureModule.UnregisterTextureEditor();
	EXPECT_FALSE(ThumbnailManager.Find(Texture2DClass));
	EXPECT_FALSE(ThumbnailManager.Find(TextureCubeClass));
	EXPECT_TRUE(ThumbnailManager.Find(MaterialClass));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("TextureEditor")),
		nullptr);
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("VolumeTextureEditor")),
		nullptr);

	MaterialModule.UnregisterMaterialEditor();
	EXPECT_FALSE(ThumbnailManager.Find(MaterialClass));
	EXPECT_FALSE(ThumbnailManager.Find(MaterialInstanceClass));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")),
		nullptr);
	EXPECT_TRUE(Manager.GetWorkspaceDescriptors().empty());

	Cache.CancelPendingRequests();
	Cache.Clear();
	ThumbnailManager.Shutdown();
	EXPECT_FALSE(ThumbnailManager.Find(StaticMeshClass));
	StaticMeshHarness.Shutdown();
	TextureHarness.Shutdown();
	MaterialHarness.Shutdown();
}
