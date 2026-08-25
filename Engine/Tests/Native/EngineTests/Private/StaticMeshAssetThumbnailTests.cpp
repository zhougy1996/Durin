#include "Thumbnail/StaticMeshAssetThumbnail.h"

#include "Assets/ContentBrowserThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailCache.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Thumbnail/AssetThumbnailObjectStore.h"

#include "AssetTools.h"
#include "AssetForge/ImportService.h"
#include "AssetForgeBuiltinsAuthoringFeatures.h"
#include "AssetForgeBuiltinsProviderTestFixture.h"
#include "DObject/Class.h"
#include "Editor/WorkspaceManager.h"
#include "MaterialEditorModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "NativeTestSupport.h"
#include "Misc/Paths.h"
#include "Modules/ModuleTestSupport.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMeshEditorModule.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "TextureEditorModule.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace
{
	auto RegisterStaticMeshThumbnailProvider()
		-> Durin::Editor::FAssetThumbnailProviderRegistrationHandle
	{
		std::string Error;
		auto Handle = Durin::Editor::GetDefaultRenderedAssetThumbnailService().RegisterScoped(
			std::make_unique<Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider>(), Error);
		EXPECT_TRUE(Handle) << Error;
		return Handle;
	}

	auto MakeFingerprint(const Durin::Asset::FAssetData& Data)
		-> Durin::Editor::FAssetThumbnailPackageFingerprint
	{
		return {
			.VirtualPath = Data.PackagePath,
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
		Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider& Provider,
		const Durin::Editor::FAssetThumbnailPackageFingerprint& Asset,
		Durin::Editor::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::Editor::FAssetThumbnailProviderRegistration Registration =
			Provider.GetRegistration();
		if (!Provider.CaptureGenerationRequest(
				MakeRequest(Asset), 7, OutRequest, OutError))
		{
			return {};
		}
		OutRequest.KeyInput.Asset = Asset;
		OutRequest.KeyInput.ProviderName = Registration.ProviderName;
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
				return Dependency.VirtualPath.GetView() == Path;
			});
	}

	auto ThumbnailPngBytes() -> std::span<const std::byte>
	{
		static constexpr uint8 Bytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
			0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240,
			159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
		return std::as_bytes(std::span{Bytes});
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

TEST(FStaticMeshAssetThumbnailTests,
	MissingDerivedDataWaitsForAsyncRecoveryBeforeBoundsValidation)
{
	InitializeDObjectSystem();
	Durin::PathUtilities::FScopedMountRegistryFixture MountRegistry;
	Durin::PathUtilities::InitDefaultMountPoints();
	ASSERT_TRUE(Durin::Asset::RefreshAssetCatalog());
	Durin::FModuleTestOwner AuthoringContext("StaticMeshThumbnailTests.Recovery");
	Durin::AssetForge::Builtins::FAssetForgeBuiltinsAuthoringFeatures AuthoringFeatures;
	auto StaticMeshAuthoring =
		AuthoringContext.RegisterFeature<Durin::IStaticMeshAuthoringFeature>(
			AuthoringFeatures);
	ASSERT_TRUE(StaticMeshAuthoring.IsValid());
	Durin::Tests::FScopedAssetForgeBuiltinsProviders Providers;
	std::string Error;
	ASSERT_TRUE(Providers.Register(Error)) << Error;

	Durin::FAssetPath SplineBoxPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/Engine/Models/SplineBox", SplineBoxPath));
	(void)Durin::Asset::UnloadPackage(
		SplineBoxPath, Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
	FScopedThumbnailDerivedDataCache DerivedDataCache("SplineBoxRecovery");
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(SplineBoxPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	ASSERT_FALSE(CaptureKey(Provider, MakeFingerprint(*Data), Request, Error).empty())
		<< Error;
	std::unique_ptr<Durin::Editor::IRenderedAssetThumbnailGenerationSession> Session =
		Provider.CreateGenerationSession(Request, *Request.Input, Error);
	ASSERT_NE(Session, nullptr) << Error;
	const Durin::Editor::FRenderedAssetThumbnailSessionUpdate Initial = Session->Load();
	EXPECT_EQ(Initial.State,
		Durin::Editor::ERenderedAssetThumbnailSessionState::WaitingForResources);
	EXPECT_TRUE(Initial.Diagnostic.empty());
	EXPECT_TRUE(Durin::AssetForge::GetImportService().HasActiveImportClaim(
		SplineBoxPath.ToString()));

	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (Durin::AssetForge::GetImportService().HasActiveImportClaim(
			SplineBoxPath.ToString())
		&& std::chrono::steady_clock::now() < Deadline)
	{
		(void)Durin::AssetForge::GetImportService().PumpImportOperations();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	ASSERT_FALSE(Durin::AssetForge::GetImportService().HasActiveImportClaim(
		SplineBoxPath.ToString()));
	Durin::DStaticMesh* Mesh = nullptr;
	const Durin::Asset::FAssetResult ReloadResult =
		Durin::Asset::LoadAsset(SplineBoxPath, Mesh);
	ASSERT_TRUE(ReloadResult) << ReloadResult.Message;
	ASSERT_NE(Mesh, nullptr);
	EXPECT_TRUE(Mesh->GetLOD0LocalBounds().has_value());
	const Durin::Editor::FRenderedAssetThumbnailSessionUpdate AfterRecovery =
		Session->PollResources();
	EXPECT_EQ(AfterRecovery.Diagnostic.find("non-degenerate LOD 0 bounds"),
		std::string::npos);
	Session.reset();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(
		SplineBoxPath, Durin::Asset::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshAssetThumbnailTests,
	ProviderCapturesExactClassSortedDependenciesAndFrozenInput)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint =
		MakeFingerprint(*Data);

	Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider Provider;
	const Durin::Editor::FAssetThumbnailProviderRegistration Registration =
		Provider.GetRegistration();
	EXPECT_EQ(
		Registration.AssetClassName,
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(
		Registration.ProviderName,
		Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailContract::ProviderName);
	EXPECT_EQ(
		Registration.GeneratorSchemaVersion,
		Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailContract::GeneratorSchemaVersion);

	Durin::Editor::FAssetThumbnailGenerationRequest FirstRequest;
	const std::string FirstKey =
		CaptureKey(Provider, Fingerprint, FirstRequest, Error);
	ASSERT_FALSE(FirstKey.empty()) << Error;
	EXPECT_TRUE(std::ranges::is_sorted(
		FirstRequest.KeyInput.Dependencies,
		{},
		[](const Durin::Editor::FAssetThumbnailPackageFingerprint& Dependency) {
			return Dependency.VirtualPath.GetView();
		}));
	EXPECT_TRUE(ContainsDependency(
		FirstRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(
		FirstRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath));
	const auto Input = std::dynamic_pointer_cast<
		const Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailGenerationInput>(FirstRequest.Input);
	ASSERT_NE(Input, nullptr);
	EXPECT_EQ(Input->AssetPath, StaticMeshPath);
	EXPECT_EQ(Input->VisualContract.Output, FirstRequest.KeyInput.Output);
	EXPECT_FALSE(Input->VisualContract.bOutputOpaque);
	EXPECT_EQ(Input->VisualContract.CameraDirectionX, 2.4f);
	EXPECT_EQ(Input->VisualContract.CameraDirectionY, -3.2f);
	EXPECT_EQ(Input->VisualContract.CameraDirectionZ, 2.4f);

	Durin::Editor::FAssetThumbnailGenerationRequest RepeatedRequest;
	EXPECT_EQ(
		CaptureKey(Provider, Fingerprint, RepeatedRequest, Error),
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
	EXPECT_FALSE(Provider.CaptureGenerationRequest(
		MakeRequest(WrongClass), 7, WrongClassRequest, Error));
	EXPECT_NE(Error.find("wrong asset class"), std::string::npos);
}

TEST(FStaticMeshAssetThumbnailTests,
	ProviderRejectsMissingAndStaleRegistrySnapshots)
{
	Durin::Tests::RegisterRenderedAssetThumbnailFixtureMount();
	Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider Provider;
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Meshes/SM_Missing", MissingPath));
	Durin::Editor::FAssetThumbnailPackageFingerprint Missing = {
		.VirtualPath = MissingPath,
		.AssetClassName = Durin::DStaticMesh::StaticClass()
			->GetQualifiedName().ToString(),
		.PackageFormatVersion = 1,
		.FileSize = 1,
		.LastWriteTimeTicks = 1};
	Durin::Editor::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Provider.CaptureGenerationRequest(
		MakeRequest(Missing), 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);

	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	Durin::Editor::FAssetThumbnailPackageFingerprint Stale = MakeFingerprint(*Data);
	++Stale.FileSize;
	EXPECT_FALSE(Provider.CaptureGenerationRequest(
		MakeRequest(Stale), 1, Captured, Error));
	EXPECT_NE(Error.find("changed"), std::string::npos);
}

TEST(FStaticMeshAssetThumbnailTests,
	GeneratorFixtureAndShaderVersionsChangePersistentKey)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailGenerationRequest Request;
	const std::string Baseline =
		CaptureKey(Provider, MakeFingerprint(*Data), Request, Error);
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

TEST(FStaticMeshAssetThumbnailTests,
	SharedCacheQueuesStaticMeshAndLeavesUnsupportedClassUnrequested)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::Editor::FRenderedAssetThumbnailCache Cache;
	Cache.BeginFrame();
	Cache.Request(
		MakeFingerprint(*Data), Durin::Editor::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(StaticMeshPath).State,
		Durin::Editor::EAssetThumbnailState::Queued);
	Cache.EndFrame();
	const Durin::Editor::FAssetThumbnailView Routed = Cache.Find(StaticMeshPath);
	EXPECT_EQ(Routed.State, Durin::Editor::EAssetThumbnailState::Failed);
	EXPECT_NE(Routed.Diagnostic.find("unavailable"), std::string::npos);

	Durin::FAssetPath UnsupportedPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Unsupported/A_Unsupported",
		UnsupportedPath));
	Cache.Request({
		.VirtualPath = UnsupportedPath,
		.AssetClassName = "DUnsupportedThumbnailAsset",
		.PackageFormatVersion = 1,
		.FileSize = 1,
		.LastWriteTimeTicks = 1}, Durin::Editor::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(UnsupportedPath).State,
		Durin::Editor::EAssetThumbnailState::NotRequested);
	Cache.CancelPendingRequests();
}

TEST(FStaticMeshAssetThumbnailTests,
	WarmPersistentHitDoesNotLoadStaticMeshOrCreatePreviewScene)
{
	if (Durin::GetRenderCommandAdmissionState()
		!= Durin::ERenderCommandAdmissionState::Running)
		Durin::InitRenderingThread();
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint = MakeFingerprint(*Data);
	Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;
	const std::string CacheKey =
		CaptureKey(Provider, Fingerprint, GenerationRequest, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("WarmHit");
	{
		Durin::Editor::FAssetThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, ThumbnailPngBytes()));
	}
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StaticMeshPath));
	ASSERT_EQ(Durin::Asset::FindResidentPackage(StaticMeshPath), nullptr);

	{
		Durin::Editor::FRenderedAssetThumbnailCache Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Cache.BeginFrame();
		Cache.Request(Fingerprint, Durin::Editor::EAssetThumbnailPriority::Visible);
		Cache.EndFrame();
		const Durin::Editor::FRenderedAssetThumbnailCacheStats Stats = Cache.GetStats();
		EXPECT_EQ(Stats.Pipeline.DiskHits, 1u);
		EXPECT_EQ(Stats.Pipeline.Loads, 0u);
		EXPECT_EQ(Stats.Pipeline.Renders, 0u);
		EXPECT_EQ(Stats.Pipeline.Readbacks, 0u);
		EXPECT_EQ(Stats.PreviewSceneCreations, 0u);
		EXPECT_EQ(Stats.PreviewSceneAssignments, 0u);
		EXPECT_EQ(Stats.UploadsQueued, 1u);
		EXPECT_FALSE(Stats.bHasPreviewScene);
		EXPECT_EQ(Durin::Asset::FindResidentPackage(StaticMeshPath), nullptr);
		Cache.CancelPendingRequests();
		Durin::FlushRenderingCommands();
		Cache.BeginFrame();
		EXPECT_EQ(
			Cache.Find(StaticMeshPath).State,
			Durin::Editor::EAssetThumbnailState::NotRequested);
		EXPECT_EQ(Cache.GetStats().UploadsCompleted, 0u);
		EXPECT_EQ(Cache.GetStats().LiveGpuTextures, 0u);
		Cache.EndFrame();
		Cache.Clear();
	}
}

TEST(FStaticMeshAssetThumbnailTests,
	CorruptPersistentHitIsInvalidatedAndRetriedThroughColdPath)
{
	if (Durin::GetRenderCommandAdmissionState()
		!= Durin::ERenderCommandAdmissionState::Running)
		Durin::InitRenderingThread();
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Fingerprint = MakeFingerprint(*Data);
	Durin::Editor::StaticMesh::FStaticMeshAssetThumbnailProvider Provider;
	Durin::Editor::FAssetThumbnailGenerationRequest GenerationRequest;
	const std::string CacheKey =
		CaptureKey(Provider, Fingerprint, GenerationRequest, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("CorruptRecovery");
	{
		const std::array<uint8, 8> Corrupt = {0, 1, 2, 3, 4, 5, 6, 7};
		Durin::Editor::FAssetThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, std::as_bytes(std::span{Corrupt})));
	}
	{
		Durin::Editor::FRenderedAssetThumbnailCache Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Cache.BeginFrame();
		Cache.Request(Fingerprint, Durin::Editor::EAssetThumbnailPriority::Visible);
		Cache.EndFrame();
		EXPECT_EQ(Cache.Find(StaticMeshPath).State, Durin::Editor::EAssetThumbnailState::Queued);
		EXPECT_EQ(Cache.GetStats().Pipeline.DiskHits, 1u);
		EXPECT_EQ(Cache.GetStats().Pipeline.Retries, 1u);
		EXPECT_EQ(Cache.GetStats().Pipeline.Loads, 0u);

		Cache.BeginFrame();
		Cache.EndFrame();
		EXPECT_EQ(Cache.GetStats().Pipeline.Loads, 1u);
		EXPECT_NE(Durin::Asset::FindResidentPackage(StaticMeshPath), nullptr);
		{
			Durin::Editor::FAssetThumbnailObjectStore Store({
				.CacheRoot = CacheRoot,
				.ObjectExtension = ".png"});
			std::vector<std::byte> Encoded;
			EXPECT_EQ(
				Store.Load(CacheKey, Encoded),
				Durin::Editor::EAssetThumbnailObjectLoadResult::Miss);
		}
		Cache.Clear();
	}
	Durin::FlushRenderingCommands();
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StaticMeshPath));
}

TEST(FStaticMeshAssetThumbnailTests,
	ContentBrowserMutationAndCloseLifecycleRejectsStaleStaticMeshWork)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	auto ProviderRegistration = RegisterStaticMeshThumbnailProvider();
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetCatalogEntry Data =
		Durin::Asset::FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::Editor::FAssetThumbnailPackageFingerprint Current = MakeFingerprint(*Data);

	Durin::Editor::ContentBrowser::Private::FContentBrowserThumbnailCache Cache;
	Cache.BeginFrame();
	Cache.Request({
		.Identity = "/RenderedThumbnailFixtures/Meshes/SM_OldIdentity",
		.Asset = Current,
		.Priority = Durin::Editor::EAssetThumbnailPriority::Prefetch});
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::Editor::EAssetThumbnailState::Queued);
	// Refresh, rename, and move all cancel the old visible snapshot before rebinding identities.
	Cache.CancelPendingRequests();
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::Editor::EAssetThumbnailState::NotRequested);

	Durin::Editor::FAssetThumbnailPackageFingerprint StaleReimport = Current;
	++StaleReimport.LastWriteTimeTicks;
	Cache.Request({
		.Identity = "/RenderedThumbnailFixtures/Meshes/SM_NewIdentity",
		.Asset = StaleReimport,
		.Priority = Durin::Editor::EAssetThumbnailPriority::Visible});
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::Editor::EAssetThumbnailState::Invalid);
	Cache.Request({
		.Identity = "/RenderedThumbnailFixtures/Meshes/SM_NewIdentity",
		.Asset = Current,
		.Priority = Durin::Editor::EAssetThumbnailPriority::Visible});
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::Editor::EAssetThumbnailState::Queued);
	// Delete and panel close clear identity bindings and pending work without rendering.
	Cache.Clear();
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::Editor::EAssetThumbnailState::NotRequested);
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::Editor::EAssetThumbnailState::NotRequested);

	{
		Durin::Editor::ContentBrowser::Private::FContentBrowserThumbnailCache ClosingCache;
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

TEST(FStaticMeshAssetThumbnailTests,
	MixedEditorModulesUnloadQueuedThumbnailsWithoutCrossModuleLoss)
{
	InitializeDObjectSystem();
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;

	Durin::Editor::FWorkspaceManager Manager;
	Durin::Editor::FRenderedAssetThumbnailService Service;
	Durin::FMaterialEditorModule MaterialModule;
	Durin::FTextureEditorModule TextureModule;
	Durin::FStaticMeshEditorModule StaticMeshModule;
	ASSERT_TRUE(MaterialModule.RegisterMaterialEditor(Manager, Service));
	ASSERT_TRUE(TextureModule.RegisterTextureEditor(Manager, Service));
	ASSERT_TRUE(StaticMeshModule.RegisterStaticMeshEditor(Manager, Service));
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
	EXPECT_TRUE(Service.Find(MaterialClass));
	EXPECT_TRUE(Service.Find(MaterialInstanceClass));
	EXPECT_TRUE(Service.Find(Texture2DClass));
	EXPECT_TRUE(Service.UsesSourceImage(Texture2DClass));
	EXPECT_TRUE(Service.Find(TextureCubeClass));
	EXPECT_TRUE(Service.Find(StaticMeshClass));

	Durin::Editor::FRenderedAssetThumbnailCache Cache(Service);
	Cache.BeginFrame();
	for (const std::string_view Path : {
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::DirectionalCubePath,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath})
	{
		Durin::FAssetPath AssetPath;
		ASSERT_TRUE(Durin::FAssetPath::TryCreate(Path, AssetPath));
		const Durin::Asset::FAssetCatalogEntry Data =
			Durin::Asset::FindAssetExact(AssetPath);
		ASSERT_NE(Data, nullptr);
		Cache.Request(MakeFingerprint(*Data), Durin::Editor::EAssetThumbnailPriority::Visible);
		EXPECT_EQ(Cache.Find(AssetPath).State, Durin::Editor::EAssetThumbnailState::Queued);
	}

	// MainFrame shutdown removes concrete modules in reverse composition order.
	// Removing one provider drains its queued lease without disturbing the others.
	StaticMeshModule.UnregisterStaticMeshEditor();
	EXPECT_FALSE(Service.Find(StaticMeshClass));
	EXPECT_TRUE(Service.Find(MaterialClass));
	EXPECT_TRUE(Service.Find(TextureCubeClass));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("StaticMeshEditor")),
		nullptr);
	Cache.EndFrame();

	TextureModule.UnregisterTextureEditor();
	EXPECT_FALSE(Service.Find(Texture2DClass));
	EXPECT_FALSE(Service.Find(TextureCubeClass));
	EXPECT_TRUE(Service.Find(MaterialClass));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("TextureEditor")),
		nullptr);
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("VolumeTextureEditor")),
		nullptr);

	MaterialModule.UnregisterMaterialEditor();
	EXPECT_FALSE(Service.Find(MaterialClass));
	EXPECT_FALSE(Service.Find(MaterialInstanceClass));
	EXPECT_EQ(
		Manager.FindWorkspace(Durin::Editor::FWorkspaceTypeId("MaterialEditor")),
		nullptr);
	EXPECT_TRUE(Manager.GetWorkspaceDescriptors().empty());

	Cache.CancelPendingRequests();
	Cache.Clear();
	Service.Shutdown();
	EXPECT_FALSE(Service.Find(StaticMeshClass));
}
