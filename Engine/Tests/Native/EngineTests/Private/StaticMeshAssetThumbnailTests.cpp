#include "Thumbnail/StaticMeshAssetThumbnail.h"

#include "Assets/ContentBrowserThumbnailCache.h"
#include "Thumbnail/MaterialAssetThumbnail.h"
#include "Thumbnail/RenderedAssetThumbnailTestFixtures.h"
#include "Thumbnail/AssetThumbnailCache.h"

#include "AssetSystem.h"
#include "NativeTestSupport.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"

#include <gtest/gtest.h>

namespace
{
	auto MakeFingerprint(const Durin::Asset::FAssetData& Data)
		-> Durin::FAssetThumbnailPackageFingerprint
	{
		return {
			.VirtualPath = Data.PackagePath,
			.AssetClassName = Data.AssetClassName,
			.PackageFormatVersion = Data.FormatVersion,
			.FileSize = static_cast<Durin::uint64>(Data.FileSize),
			.LastWriteTimeTicks = Data.LastWriteTimeTicks};
	}

	auto MakeRequest(
		const Durin::FAssetThumbnailPackageFingerprint& Asset,
		Durin::uint64 Serial = 1) -> Durin::FAssetThumbnailRequest
	{
		return {
			.Asset = Asset,
			.Priority = Durin::EAssetThumbnailPriority::Visible,
			.RequestSerial = Serial};
	}

	auto CaptureKey(
		Durin::FStaticMeshAssetThumbnailProvider& Provider,
		const Durin::FAssetThumbnailPackageFingerprint& Asset,
		Durin::FAssetThumbnailGenerationRequest& OutRequest,
		std::string& OutError) -> std::string
	{
		const Durin::FAssetThumbnailProviderRegistration Registration =
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
		return Durin::BuildAssetThumbnailCacheKey(OutRequest.KeyInput);
	}

	auto ContainsDependency(
		const Durin::FAssetThumbnailGenerationRequest& Request,
		std::string_view Path) -> bool
	{
		return std::ranges::any_of(
			Request.KeyInput.Dependencies,
			[Path](const Durin::FAssetThumbnailPackageFingerprint& Dependency) {
				return Dependency.VirtualPath.GetView() == Path;
			});
	}

	auto ThumbnailPngBytes() -> std::span<const Durin::uint8>
	{
		static constexpr Durin::uint8 Bytes[] = {
			137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 73, 72, 68, 82,
			0, 0, 0, 2, 0, 0, 0, 1, 8, 6, 0, 0, 0, 244, 34, 127, 138,
			0, 0, 0, 17, 73, 68, 65, 84, 120, 156, 99, 248, 207, 192, 240,
			159, 129, 129, 129, 1, 0, 12, 252, 1, 255, 253, 45, 119, 109,
			0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
		return Bytes;
	}

	auto MakeCacheRoot(std::string_view Name) -> std::filesystem::path
	{
		const std::filesystem::path Root =
			Durin::Testing::GetTestWorkDirectory() / "StaticMeshThumbnailCache" / Name;
		Durin::Testing::RemoveTestWorkDirectory(Root);
		return Root;
	}
}

TEST(FStaticMeshAssetThumbnailTests,
	ProviderCapturesExactClassSortedDependenciesAndFrozenInput)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::FAssetThumbnailPackageFingerprint Fingerprint =
		MakeFingerprint(*Data);

	Durin::FStaticMeshAssetThumbnailProvider Provider;
	const Durin::FAssetThumbnailProviderRegistration Registration =
		Provider.GetRegistration();
	EXPECT_EQ(
		Registration.AssetClassName,
		Durin::DStaticMesh::StaticClass()->GetQualifiedName().ToString());
	EXPECT_EQ(
		Registration.ProviderName,
		Durin::FStaticMeshAssetThumbnailContract::ProviderName);
	EXPECT_EQ(
		Registration.GeneratorSchemaVersion,
		Durin::FStaticMeshAssetThumbnailContract::GeneratorSchemaVersion);

	Durin::FAssetThumbnailGenerationRequest FirstRequest;
	const std::string FirstKey =
		CaptureKey(Provider, Fingerprint, FirstRequest, Error);
	ASSERT_FALSE(FirstKey.empty()) << Error;
	EXPECT_TRUE(std::ranges::is_sorted(
		FirstRequest.KeyInput.Dependencies,
		{},
		[](const Durin::FAssetThumbnailPackageFingerprint& Dependency) {
			return Dependency.VirtualPath.GetView();
		}));
	EXPECT_TRUE(ContainsDependency(
		FirstRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::MaterialPath));
	EXPECT_TRUE(ContainsDependency(
		FirstRequest,
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::ParentTexturePath));
	const auto Input = std::dynamic_pointer_cast<
		const Durin::FStaticMeshAssetThumbnailGenerationInput>(FirstRequest.Input);
	ASSERT_NE(Input, nullptr);
	EXPECT_EQ(Input->AssetPath, StaticMeshPath);
	EXPECT_EQ(Input->VisualContract.Output, FirstRequest.KeyInput.Output);
	EXPECT_FALSE(Input->VisualContract.bOutputOpaque);
	EXPECT_EQ(Input->VisualContract.CameraDirectionX, 2.4f);
	EXPECT_EQ(Input->VisualContract.CameraDirectionY, -3.2f);
	EXPECT_EQ(Input->VisualContract.CameraDirectionZ, 2.4f);

	Durin::FAssetThumbnailGenerationRequest RepeatedRequest;
	EXPECT_EQ(
		CaptureKey(Provider, Fingerprint, RepeatedRequest, Error),
		FirstKey);
	EXPECT_EQ(RepeatedRequest.KeyInput.Dependencies,
		FirstRequest.KeyInput.Dependencies);

	Durin::FAssetThumbnailKeyInput ChangedDependency = FirstRequest.KeyInput;
	ASSERT_FALSE(ChangedDependency.Dependencies.empty());
	++ChangedDependency.Dependencies.front().LastWriteTimeTicks;
	EXPECT_NE(
		Durin::BuildAssetThumbnailCacheKey(ChangedDependency),
		FirstKey);

	Durin::FAssetThumbnailGenerationRequest WrongClassRequest;
	Durin::FAssetThumbnailPackageFingerprint WrongClass = Fingerprint;
	WrongClass.AssetClassName = "DMaterial";
	EXPECT_FALSE(Provider.CaptureGenerationRequest(
		MakeRequest(WrongClass), 7, WrongClassRequest, Error));
	EXPECT_NE(Error.find("wrong asset class"), std::string::npos);
}

TEST(FStaticMeshAssetThumbnailTests,
	ProviderRejectsMissingAndStaleRegistrySnapshots)
{
	Durin::Tests::RegisterRenderedAssetThumbnailFixtureMount();
	Durin::FStaticMeshAssetThumbnailProvider Provider;
	Durin::FAssetPath MissingPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		"/RenderedThumbnailFixtures/Meshes/SM_Missing", MissingPath));
	Durin::FAssetThumbnailPackageFingerprint Missing = {
		.VirtualPath = MissingPath,
		.AssetClassName = Durin::DStaticMesh::StaticClass()
			->GetQualifiedName().ToString(),
		.PackageFormatVersion = 1,
		.FileSize = 1,
		.LastWriteTimeTicks = 1};
	Durin::FAssetThumbnailGenerationRequest Captured;
	std::string Error;
	EXPECT_FALSE(Provider.CaptureGenerationRequest(
		MakeRequest(Missing), 1, Captured, Error));
	EXPECT_NE(Error.find(MissingPath.ToString()), std::string::npos);

	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	Durin::FAssetThumbnailPackageFingerprint Stale = MakeFingerprint(*Data);
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
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::FStaticMeshAssetThumbnailProvider Provider;
	Durin::FAssetThumbnailGenerationRequest Request;
	const std::string Baseline =
		CaptureKey(Provider, MakeFingerprint(*Data), Request, Error);
	ASSERT_FALSE(Baseline.empty()) << Error;
	for (const auto Mutate : {
			+[](Durin::FAssetThumbnailKeyInput& Input) {
				++Input.GeneratorSchemaVersion;
			},
			+[](Durin::FAssetThumbnailKeyInput& Input) {
				++Input.PreviewFixtureVersion;
			},
			+[](Durin::FAssetThumbnailKeyInput& Input) {
				++Input.ShaderContractVersion;
			}})
	{
		Durin::FAssetThumbnailKeyInput Changed = Request.KeyInput;
		Mutate(Changed);
		EXPECT_NE(Durin::BuildAssetThumbnailCacheKey(Changed), Baseline);
	}
}

TEST(FStaticMeshAssetThumbnailTests,
	SharedCacheQueuesStaticMeshAndLeavesUnsupportedClassUnrequested)
{
	Durin::Tests::FRenderedAssetThumbnailFixtureSet Fixtures;
	std::string Error;
	ASSERT_TRUE(Durin::Tests::CreateRenderedAssetThumbnailFixtures(Fixtures, Error))
		<< Error;
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);

	Durin::FRenderedAssetThumbnailCache Cache;
	Cache.BeginFrame();
	Cache.Request(
		MakeFingerprint(*Data), Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(StaticMeshPath).State,
		Durin::EAssetThumbnailState::Queued);
	Cache.EndFrame();
	const Durin::FAssetThumbnailView Routed = Cache.Find(StaticMeshPath);
	EXPECT_EQ(Routed.State, Durin::EAssetThumbnailState::Failed);
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
		.LastWriteTimeTicks = 1}, Durin::EAssetThumbnailPriority::Visible);
	EXPECT_EQ(
		Cache.Find(UnsupportedPath).State,
		Durin::EAssetThumbnailState::NotRequested);
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
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::FAssetThumbnailPackageFingerprint Fingerprint = MakeFingerprint(*Data);
	Durin::FStaticMeshAssetThumbnailProvider Provider;
	Durin::FAssetThumbnailGenerationRequest GenerationRequest;
	const std::string CacheKey =
		CaptureKey(Provider, Fingerprint, GenerationRequest, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("WarmHit");
	{
		Durin::FAssetThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, ThumbnailPngBytes()));
	}
	ASSERT_TRUE(Durin::Asset::UnloadPackage(StaticMeshPath));
	ASSERT_EQ(Durin::Asset::FindLoadedPackage(StaticMeshPath), nullptr);

	{
		Durin::FRenderedAssetThumbnailCache Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Cache.BeginFrame();
		Cache.Request(Fingerprint, Durin::EAssetThumbnailPriority::Visible);
		Cache.EndFrame();
		const Durin::FRenderedAssetThumbnailCacheStats Stats = Cache.GetStats();
		EXPECT_EQ(Stats.Pipeline.DiskHits, 1u);
		EXPECT_EQ(Stats.Pipeline.Loads, 0u);
		EXPECT_EQ(Stats.Pipeline.Renders, 0u);
		EXPECT_EQ(Stats.Pipeline.Readbacks, 0u);
		EXPECT_EQ(Stats.PreviewSceneCreations, 0u);
		EXPECT_EQ(Stats.PreviewSceneAssignments, 0u);
		EXPECT_EQ(Stats.UploadsQueued, 1u);
		EXPECT_FALSE(Stats.bHasPreviewScene);
		EXPECT_EQ(Durin::Asset::FindLoadedPackage(StaticMeshPath), nullptr);
		Cache.CancelPendingRequests();
		Durin::FlushRenderingCommands();
		Cache.BeginFrame();
		EXPECT_EQ(
			Cache.Find(StaticMeshPath).State,
			Durin::EAssetThumbnailState::NotRequested);
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
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::FAssetThumbnailPackageFingerprint Fingerprint = MakeFingerprint(*Data);
	Durin::FStaticMeshAssetThumbnailProvider Provider;
	Durin::FAssetThumbnailGenerationRequest GenerationRequest;
	const std::string CacheKey =
		CaptureKey(Provider, Fingerprint, GenerationRequest, Error);
	ASSERT_FALSE(CacheKey.empty()) << Error;
	const std::filesystem::path CacheRoot = MakeCacheRoot("CorruptRecovery");
	{
		const std::array<Durin::uint8, 8> Corrupt = {0, 1, 2, 3, 4, 5, 6, 7};
		Durin::FAssetThumbnailObjectStore Store({
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		ASSERT_TRUE(Store.Store(CacheKey, Corrupt));
	}
	{
		Durin::FRenderedAssetThumbnailCache Cache({}, {
			.CacheRoot = CacheRoot,
			.ObjectExtension = ".png"});
		Cache.BeginFrame();
		Cache.Request(Fingerprint, Durin::EAssetThumbnailPriority::Visible);
		Cache.EndFrame();
		EXPECT_EQ(Cache.Find(StaticMeshPath).State, Durin::EAssetThumbnailState::Queued);
		EXPECT_EQ(Cache.GetStats().Pipeline.DiskHits, 1u);
		EXPECT_EQ(Cache.GetStats().Pipeline.Retries, 1u);
		EXPECT_EQ(Cache.GetStats().Pipeline.Loads, 0u);

		Cache.BeginFrame();
		Cache.EndFrame();
		EXPECT_EQ(Cache.GetStats().Pipeline.Loads, 1u);
		EXPECT_NE(Durin::Asset::FindLoadedPackage(StaticMeshPath), nullptr);
		{
			Durin::FAssetThumbnailObjectStore Store({
				.CacheRoot = CacheRoot,
				.ObjectExtension = ".png"});
			std::vector<Durin::uint8> Encoded;
			EXPECT_EQ(
				Store.Load(CacheKey, Encoded),
				Durin::EAssetThumbnailObjectLoadResult::Miss);
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
	Durin::FAssetPath StaticMeshPath;
	ASSERT_TRUE(Durin::FAssetPath::TryCreate(
		Durin::Tests::FRenderedAssetThumbnailFixtureSet::StaticMeshPath,
		StaticMeshPath));
	const Durin::Asset::FAssetData* Data =
		Durin::Asset::GetAssetRegistry().FindAssetExact(StaticMeshPath);
	ASSERT_NE(Data, nullptr);
	const Durin::FAssetThumbnailPackageFingerprint Current = MakeFingerprint(*Data);

	Durin::FContentBrowserThumbnailCache Cache;
	Cache.BeginFrame();
	Cache.Request({
		.Identity = "/RenderedThumbnailFixtures/Meshes/SM_OldIdentity",
		.Asset = Current,
		.Priority = Durin::EAssetThumbnailPriority::Prefetch});
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::EAssetThumbnailState::Queued);
	// Refresh, rename, and move all cancel the old visible snapshot before rebinding identities.
	Cache.CancelPendingRequests();
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::EAssetThumbnailState::NotRequested);

	Durin::FAssetThumbnailPackageFingerprint StaleReimport = Current;
	++StaleReimport.LastWriteTimeTicks;
	Cache.Request({
		.Identity = "/RenderedThumbnailFixtures/Meshes/SM_NewIdentity",
		.Asset = StaleReimport,
		.Priority = Durin::EAssetThumbnailPriority::Visible});
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::EAssetThumbnailState::Invalid);
	Cache.Request({
		.Identity = "/RenderedThumbnailFixtures/Meshes/SM_NewIdentity",
		.Asset = Current,
		.Priority = Durin::EAssetThumbnailPriority::Visible});
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::EAssetThumbnailState::Queued);
	// Delete and panel close clear identity bindings and pending work without rendering.
	Cache.Clear();
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_OldIdentity").State,
		Durin::EAssetThumbnailState::NotRequested);
	EXPECT_EQ(
		Cache.Find("/RenderedThumbnailFixtures/Meshes/SM_NewIdentity").State,
		Durin::EAssetThumbnailState::NotRequested);

	{
		Durin::FContentBrowserThumbnailCache ClosingCache;
		ClosingCache.BeginFrame();
		ClosingCache.Request({
			.Identity = StaticMeshPath.GetView(),
			.Asset = Current,
			.Priority = Durin::EAssetThumbnailPriority::Visible});
		EXPECT_EQ(
			ClosingCache.Find(StaticMeshPath.GetView()).State,
			Durin::EAssetThumbnailState::Queued);
	}
}
