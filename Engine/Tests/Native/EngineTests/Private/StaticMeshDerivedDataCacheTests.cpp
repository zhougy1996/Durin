#include <gtest/gtest.h>
#include "NativeAssetRuntimeTestSupport.h"

#include "NativeDObjectTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Property.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineMeshComponent.h"
#include "EngineTestSupport.h"
#include "Hash/XxHash.h"
#include "Serialization/Archive.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Modules/ModuleManager.h"
#include "Modules/ModuleTestSupport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include <condition_variable>
#include <thread>
#include "Threading/RunnableThread.h"
#if defined(__APPLE__)
#include <malloc/malloc.h>
#include <sys/resource.h>
#endif
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Runtime/Engine/Private/StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshResources.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"

namespace
{
	class FScopedStaticMeshProviderRestore
	{
	public:
		~FScopedStaticMeshProviderRestore()
		{
			Durin::FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
		}
	};

	class FScopedDerivedDataCacheRestore
	{
	public:
		FScopedDerivedDataCacheRestore()
			: PreviousDirectory(Durin::FPaths::DerivedDataCacheDir())
		{
		}

		~FScopedDerivedDataCacheRestore()
		{
			Durin::FPaths::SetDerivedDataCacheDirForTests(PreviousDirectory);
		}

	private:
		std::string PreviousDirectory;
	};

	struct FStaticMeshCacheFixture
	{
		std::filesystem::path Root;
		std::filesystem::path CacheRoot;
		std::filesystem::path SourcePath;
		Durin::FPackagePath AssetPath;
		Durin::DStaticMesh* Mesh = nullptr;
	};

	auto GetStaticMeshKey(const Durin::DStaticMesh& Mesh) -> std::string
	{
		std::string Error;
		const Durin::FCacheKeyProxy Key = Durin::BuildStaticMeshDerivedDataKey({
			.ImportedDataHash = Mesh.GetImportedData().GetIdentity(),
			.ReconciliationHash = Durin::BuildStaticMeshReconciliationHash(
				Mesh.GetMaterialSlots(), Mesh.GetNormalizedSize()),
			.TargetPlatform = Durin::EStaticMeshTargetPlatform::Win64}, Error);
		EXPECT_TRUE(Key.IsValid()) << Error;
		return Key.ToString();
	}

	auto ImportCacheFixture(std::string_view Name) -> FStaticMeshCacheFixture
	{
		InitializeDObjectSystem();
		FStaticMeshCacheFixture Fixture;
		Fixture.Root = Durin::Testing::GetTestWorkDirectory() / std::string(Name);
		Fixture.CacheRoot = Fixture.Root / "DerivedDataCache";
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root);
		std::filesystem::create_directories(Fixture.Root / "Content");
		const std::string Mount = std::format("/{}/", Name);
		Durin::Testing::RegisterMountPointForTests(
			Mount, (Fixture.Root / "Content").generic_string() + "/");
		Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
		EXPECT_TRUE(Durin::FPackagePath::TryCreate(Mount + "Mesh", Fixture.AssetPath));
		const auto Source = std::filesystem::path(DURIN_TEST_DATA_DIR) / "MultiSection.gltf";
		Fixture.SourcePath = Fixture.Root / "Sources/Mesh.gltf";
		std::filesystem::create_directories(Fixture.SourcePath.parent_path());
		std::filesystem::copy_file(Source, Fixture.SourcePath,
			std::filesystem::copy_options::overwrite_existing);
		const Durin::Testing::TFactoryImportResult<Durin::DStaticMesh> Import = Durin::AssetForge::Builtins::ImportStaticMeshForTest(
			Fixture.SourcePath.generic_string(), Fixture.AssetPath.ToString());
		EXPECT_TRUE(Import) << Import.Message;
		Fixture.Mesh = Import.Asset;
		if (Fixture.Mesh)
			EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
		return Fixture;
	}

	auto GetObjectPath(const FStaticMeshCacheFixture& Fixture, std::string_view Key) -> std::filesystem::path
	{
		return Fixture.CacheRoot / "StaticMesh" / "Objects"
			/ std::string(Key.substr(0, 2)) / (std::string(Key) + ".bin");
	}

	auto WriteU32(Durin::FByteBuffer& Bytes, size_t Offset, uint32 Value) -> void
	{
		ASSERT_LE(Offset + 4, Bytes.size());
		for (uint32 Byte = 0; Byte < 4; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto WriteU64(Durin::FByteBuffer& Bytes, size_t Offset, uint64 Value) -> void
	{
		ASSERT_LE(Offset + 8, Bytes.size());
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Bytes[Offset + Byte] = static_cast<std::byte>(Value >> (Byte * 8));
	}

	auto ReadU64(const Durin::FByteBuffer& Bytes, size_t Offset) -> uint64
	{
		uint64 Value = 0;
		for (uint32 Byte = 0; Byte < 8; ++Byte)
			Value |= std::to_integer<uint64>(Bytes[Offset + Byte]) << (Byte * 8);
		return Value;
	}

	auto RefreshEnvelopeHeaderHash(Durin::FByteBuffer& Bytes) -> void
	{
		const uint64 HeaderBytes = ReadU64(Bytes, 32);
		std::ranges::fill(std::span(Bytes).subspan(48, 16), std::byte{});
		const Durin::FXxHash128 Hash = Durin::FXxHash128::HashBuffer(
			std::span(Bytes).first(static_cast<size_t>(HeaderBytes)));
		WriteU64(Bytes, 48, Hash.HashLow);
		WriteU64(Bytes, 56, Hash.HashHigh);
	}

}

TEST(FStaticMeshDerivedDataCacheTests, EngineProviderPathPreservesKeysAndRecoversCorruption)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshEngineProvider");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string BaselineKey = GetStaticMeshKey(*Fixture.Mesh);
	FStaticMeshBuildRequest Request{
		.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.ImportedData = Fixture.Mesh->GetImportedData()};
	FStaticMeshBuildResult Product;
	std::string Error;
	// The runtime loader carries only authored metadata until a cache miss.
	Request.ImportedData.ReleaseGeometry();
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_TRUE(Product.DiagnosticMessage.empty());
	EXPECT_TRUE(Request.ImportedData.IsValid());
	ASSERT_NE(Product.RenderData, nullptr);
	const std::array<std::byte, 4> Corrupt{};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, GetObjectPath(Fixture, BaselineKey)));
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::Rebuilt);
	EXPECT_TRUE(Request.ImportedData.IsValid());
	EXPECT_FALSE(Product.DiagnosticMessage.empty());
	EXPECT_TRUE(Error.empty());
	Request.Reconciliation.MaterialSlots.clear();
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.MaterialSlots.size(), Fixture.Mesh->GetMaterialSlots().size());

	FStaticMeshCollisionBuildResult ColdCollision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(
		*Fixture.Mesh->GetRenderData(), EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, ColdCollision, Error)) << Error;
	FStaticMeshCollisionBuildResult Collision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error)) << Error;
	EXPECT_EQ(Collision.DerivedDataKey, ColdCollision.DerivedDataKey);
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Collision.PayloadBytes, ColdCollision.PayloadBytes);
	const auto CollisionPath = Fixture.CacheRoot / "StaticMeshCollision/Objects"
		/ Collision.DerivedDataKey.ToString().substr(0, 2)
		/ (Collision.DerivedDataKey.ToString() + ".bin");
	ASSERT_TRUE(std::filesystem::remove(CollisionPath));
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error, false)) << Error;
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::Rebuilt);
	EXPECT_FALSE(std::filesystem::exists(CollisionPath));
	const auto BlockedRoot = Fixture.Root / "BlockedCollisionCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, BlockedRoot));
	FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	// A read failure remains observable even when no Put is attempted.
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error, false)) << Error;
	EXPECT_TRUE(Collision.Complex);
	EXPECT_FALSE(Collision.Diagnostic.empty());
	EXPECT_EQ(Collision.CacheWriteNanoseconds, 0u);
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error)) << Error;
	EXPECT_TRUE(Collision.Complex);
	EXPECT_FALSE(Collision.Diagnostic.empty());
	EXPECT_LE(Collision.Diagnostic.size(), 2048u);
	EXPECT_NE(Collision.Diagnostic.find("Read: "), std::string::npos);
	EXPECT_NE(Collision.Diagnostic.find("; Put: "), std::string::npos);
	FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, InvalidDetachedReplacementPreservesLiveState)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshReplacementRollback");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const FStaticMeshRenderData* Original = Fixture.Mesh->GetRenderData();
	const FXxHash128 ImportedIdentity = Fixture.Mesh->GetImportedData().GetIdentity();
	const uint64 Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	FStaticMeshBuildResult Result;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData({
		.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.ImportedData = Fixture.Mesh->GetImportedData()}, Result, Error)) << Error;
	ASSERT_FALSE(Result.RenderData->LODResources.empty());
	Result.RenderData->LODResources.front().IndexBuffer.GetMutableIndices().front() =
		std::numeric_limits<uint32>::max();
	EXPECT_FALSE(ApplyStaticMeshBuildResult(*Fixture.Mesh, Fixture.Mesh->GetImportedData(), std::move(Result), Error));
	EXPECT_FALSE(Error.empty());
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), Original);
	EXPECT_EQ(Fixture.Mesh->GetImportedData().GetIdentity(), ImportedIdentity);
	EXPECT_EQ(Fixture.Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, ColdWarmAndSourceUnavailableLoadsFollowEditorPolicy)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCachePolicy");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string ImportedKey = GetStaticMeshKey(*Fixture.Mesh);
	ASSERT_EQ(ImportedKey.size(), 32u);
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, ImportedKey);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
	ASSERT_TRUE(std::filesystem::remove(ObjectPath));

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(GetStaticMeshKey(*Fixture.Mesh), ImportedKey);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(std::filesystem::is_regular_file(ObjectPath));
	ASSERT_TRUE(Durin::UnloadPackage(
		Fixture.AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));

	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, SourceAndSettingsChangesMissDeterministically)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheInvalidation");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string InitialKey = GetStaticMeshKey(*Fixture.Mesh);
	{
		std::ofstream Stream(Fixture.SourcePath, std::ios::binary | std::ios::app);
		ASSERT_TRUE(Stream.is_open());
		Stream << "\n";
	}
	std::string Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh, Error)) << Error;
	const std::string SourceChangedKey = GetStaticMeshKey(*Fixture.Mesh);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(SourceChangedKey, InitialKey);

	auto* ImportData = dynamic_cast<Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Fixture.Mesh->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	auto State = ImportData->GetStaticMeshState();
	State.ImportSettings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	ASSERT_TRUE(ImportData->SetState(std::move(State), Error)) << Error;
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh, Error)) << Error;
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_NE(GetStaticMeshKey(*Fixture.Mesh), SourceChangedKey);
	ASSERT_TRUE(Durin::UnloadPackage(
		Fixture.AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, CorruptionRecoveryIsNonPersistentAndFailurePreservesLiveData)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCacheRecovery");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string Key = GetStaticMeshKey(*Fixture.Mesh);
	const std::filesystem::path ObjectPath = GetObjectPath(Fixture, Key);
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
	const std::array<uint8, 4> Corrupt{1, 2, 3, 4};
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), ObjectPath));
	const std::filesystem::path PackagePath = Fixture.Root / "Content" / "Mesh.dasset";
	Durin::FByteBuffer PackageBytesBeforeRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesBeforeRecovery, PackagePath));
	const auto PackageTimeBeforeRecovery =
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(PackagePath, PackageTimeBeforeRecovery);

	ASSERT_TRUE(Durin::LoadObject(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	const Durin::FStaticMeshRenderData* CompleteRenderData = Fixture.Mesh->GetRenderData();
	ASSERT_NE(CompleteRenderData, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	Durin::FByteBuffer PackageBytesAfterRecovery;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		PackageBytesAfterRecovery, PackagePath));
	EXPECT_EQ(PackageBytesAfterRecovery, PackageBytesBeforeRecovery);
	EXPECT_EQ(std::filesystem::last_write_time(PackagePath), PackageTimeBeforeRecovery);

	const std::filesystem::path BlockedCacheRoot = Fixture.Root / "BlockedCacheRoot";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), BlockedCacheRoot));
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	std::string Error;
	EXPECT_TRUE(Fixture.Mesh->PostLoad(Error)) << Error;
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	EXPECT_TRUE(Error.empty());
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());

	Durin::FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(Durin::UnloadPackage(
		Fixture.AssetPath,
		Durin::EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, CookedCollisionCompanionIsDeterministicAndRequiredAtRuntime)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCookedCollisionConsumer");
	ASSERT_NE(Fixture.Mesh, nullptr);
	std::string Error;
	ASSERT_TRUE(Fixture.Mesh->SetCollisionSourceMode(
		Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error)) << Error;
	ASSERT_NE(Fixture.Mesh->GetBodySetup(), nullptr);
	Durin::FCollisionGeometryRef AuthoredGeometry;
	ASSERT_TRUE(Fixture.Mesh->GetBodySetup()->BuildComplexGeometry(AuthoredGeometry));
	const uint32 AuthoredVertices = AuthoredGeometry.GetVertexCount();
	const uint32 AuthoredTriangles = AuthoredGeometry.GetTriangleCount();
	const uint32 AuthoredNodes = AuthoredGeometry.GetNodeCount();
	const uint64 AuthoredBytes = AuthoredGeometry.GetRetainedBytes();

	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	const std::filesystem::path CookRoot = std::filesystem::absolute(Fixture.Root / "CookCollision");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Fixture.Root / "CookCollisionSecond");
	for (const std::filesystem::path& Root : {CookRoot, SecondCookRoot})
	{
		Durin::FCookContext Context(
			Root, Durin::ECookTargetPlatform::Win64,
			Durin::ECookTargetProfile::Game);
		ASSERT_TRUE(Durin::ContributeEngineCookAsset(
			*Fixture.Mesh, "/Game/CookedCollisionMesh", Context, Error)) << Error;
		ASSERT_TRUE(Context.Publish(&Error)) << Error;
	}
	Durin::FByteBuffer FirstPackage, SecondPackage, FirstBulk, SecondBulk, FirstManifest, SecondManifest;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstPackage, (CookRoot / "Game/CookedCollisionMesh.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondPackage, (SecondCookRoot / "Game/CookedCollisionMesh.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(FirstManifest, (CookRoot / "CookManifest.bin")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(SecondManifest, (SecondCookRoot / "CookManifest.bin")));
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstManifest, SecondManifest);
	if (!Durin::FFileHelper::LoadFileToArray(
		FirstBulk, CookRoot / "Game/CookedCollisionMesh.dbulk"))
	{
		EXPECT_FALSE(std::filesystem::exists(
			SecondCookRoot / "Game/CookedCollisionMesh.dbulk"));
		Durin::FAssetPackageInspection Inspection;
		Durin::FPackagePath CookedPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
			"/Game/CookedCollisionMesh", CookedPath));
		ASSERT_TRUE(Durin::InspectAssetPackage(
			(CookRoot / "Game/CookedCollisionMesh.dasset").generic_string(),
			CookedPath, Inspection));
		EXPECT_NE(Inspection.FindField("RenderData"), nullptr);
		EXPECT_NE(Inspection.FindField("CollisionData"), nullptr);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "Content" / "Models");
		const FScopedStaticMeshProviderRestore ProviderRestore;
		ASSERT_TRUE(Durin::FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
		Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
		ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedCollisionMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
		ASSERT_FALSE(Durin::FModuleManager::Get().IsModuleLoaded("StaticMeshBuild"));
		const Durin::FAssetResult Loaded =
			Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
				Path, Fixture.AssetPath.GetPackageName()), CookedMesh);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		const Durin::FCookedMeshBlockingResult LoadResult =
			CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(LoadResult) << LoadResult.Message;
		ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
		Durin::FCollisionGeometryRef Geometry;
		ASSERT_TRUE(CookedMesh->GetBodySetup()->BuildComplexGeometry(Geometry));
		EXPECT_EQ(Geometry.GetVertexCount(), AuthoredVertices);
		EXPECT_EQ(Geometry.GetTriangleCount(), AuthoredTriangles);
		EXPECT_EQ(Geometry.GetNodeCount(), AuthoredNodes);
		EXPECT_EQ(Geometry.GetRetainedBytes(), AuthoredBytes);
		ASSERT_TRUE(Durin::UnloadPackage(Path));
		ASSERT_TRUE(AssetRuntime.Restore());
		return;
	}
}

TEST(FStaticMeshDerivedDataCacheTests, CookedPackageLoadsWithoutSourceOrDerivedDataFallback)
{
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshCookedConsumer");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::filesystem::path CookRoot = std::filesystem::absolute(Fixture.Root / "Cook");
	const std::filesystem::path SecondCookRoot = std::filesystem::absolute(Fixture.Root / "CookSecond");
	Durin::FCookContext First(
		CookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", First, Error)) << Error;
	ASSERT_TRUE(First.Publish(&Error)) << Error;

	Durin::FCookContext Second(
		SecondCookRoot,
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", Second, Error)) << Error;
	ASSERT_TRUE(Second.Publish(&Error)) << Error;
	Durin::FByteBuffer FirstPackage;
	Durin::FByteBuffer SecondPackage;
	Durin::FByteBuffer FirstBulk;
	Durin::FByteBuffer SecondBulk;
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		FirstPackage, (CookRoot / "Game/CookedMesh.dasset")));
	ASSERT_TRUE(Durin::FFileHelper::LoadFileToArray(
		SecondPackage, (SecondCookRoot / "Game/CookedMesh.dasset")));
	EXPECT_EQ(FirstPackage, SecondPackage);
	if (!Durin::FFileHelper::LoadFileToArray(
		FirstBulk, CookRoot / "Game/CookedMesh.dbulk"))
	{
		EXPECT_FALSE(std::filesystem::exists(
			SecondCookRoot / "Game/CookedMesh.dbulk"));
		Durin::FAssetPackageInspection Inspection;
		Durin::FPackagePath CookedPath;
		ASSERT_TRUE(Durin::FPackagePath::TryCreateProjectContent(
			"/Game/CookedMesh", CookedPath));
		ASSERT_TRUE(Durin::InspectAssetPackage(
			(CookRoot / "Game/CookedMesh.dasset").generic_string(),
			CookedPath, Inspection));
		EXPECT_NE(Inspection.FindField("RenderData"), nullptr);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
		Durin::Testing::RemoveTestWorkDirectory(Fixture.Root / "Content" / "Models");
		const FScopedStaticMeshProviderRestore ProviderRestore;
		ASSERT_TRUE(Durin::FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
		Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
		ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
		ASSERT_FALSE(Durin::FModuleManager::Get().IsModuleLoaded("StaticMeshBuild"));
		const Durin::FAssetResult Loaded =
			Durin::LoadObject(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
				Path, Fixture.AssetPath.GetPackageName()), CookedMesh);
		ASSERT_TRUE(Loaded) << Loaded.Message;
		ASSERT_EQ(CookedMesh->GetRenderData(), nullptr);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Unloaded);
		auto* FirstConsumer = Durin::NewObject<Durin::DStaticMeshComponent>(
			nullptr, Durin::FName("CookedStaticMeshFirstConsumer"));
		FirstConsumer->SetStaticMesh(CookedMesh);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::Unloaded);
		ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
		FirstConsumer->RegisterComponent();
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::IoQueued);
		EXPECT_EQ(FirstConsumer->CreateSceneProxy(), nullptr);
		Durin::ShutdownCookedMeshLoadManager();
		const auto CancelledStatus = CookedMesh->RequestRenderDataAndResources();
		EXPECT_EQ(CancelledStatus.CpuPhase, Durin::ECookedMeshCpuPhase::Cancelled);
		ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
		const Durin::FCookedMeshBlockingResult RetryResult =
			CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(RetryResult) << RetryResult.Message;
		EXPECT_GT(RetryResult.Status.Generation, CancelledStatus.Generation);
		auto FirstProxy = FirstConsumer->CreateSceneProxy();
		const Durin::FCookedMeshLoadStatus RecoveredStatus =
			CookedMesh->RequestRenderDataAndResources();
		const Durin::FCookedMeshLoadDiagnostics RecoveredDiagnostics =
			Durin::GetCookedMeshLoadManager()->GetDiagnostics();
		if (!FirstProxy) Durin::ShutdownCookedMeshLoadManager();
		ASSERT_NE(FirstProxy, nullptr)
			<< "cpu_phase=" << static_cast<uint32>(RecoveredStatus.CpuPhase)
			<< " failed=" << RecoveredDiagnostics.FailedCount
			<< " stale=" << RecoveredDiagnostics.StaleCount
			<< " in_flight=" << RecoveredDiagnostics.InFlightCount;
		ASSERT_NE(CookedMesh->GetRenderData(), nullptr);
		EXPECT_EQ(CookedMesh->RequestRenderDataAndResources().CpuPhase,
			Durin::ECookedMeshCpuPhase::CpuReady);
		auto* SplineConsumer = Durin::NewObject<Durin::DSplineMeshComponent>(
			nullptr, Durin::FName("CookedSplineMeshConsumer"));
		SplineConsumer->SetStaticMesh(CookedMesh);
		ASSERT_NE(SplineConsumer->CreateSceneProxy(), nullptr);
		Durin::ShutdownCookedMeshLoadManager();
		EXPECT_EQ(CookedMesh->GetAssetImportData(), nullptr);
		EXPECT_FALSE(CookedMesh->GetImportedData().IsValid());
		EXPECT_FALSE(CookedMesh->GetImportedData().IsGeometryResident());
		EXPECT_NE(CookedMesh->GetCookedRenderData().GetMetadata().LogicalSize, 0u);
		ASSERT_TRUE(Durin::UnloadPackage(Path));
		ASSERT_TRUE(AssetRuntime.Restore());
		return;
	}
}

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif
TEST(FStaticMeshSourceResidencyTests, RepresentativeGeometry)
{
	using namespace Durin;
	for (const uint32 Triangles : {1u, 100000u})
	{
		#if defined(__APPLE__)
		std::atomic<size_t> PeakBytes{0};
		std::jthread Sampler([&](std::stop_token Stop) {
			while (!Stop.stop_requested())
			{
				malloc_statistics_t Stats{};
				malloc_zone_statistics(malloc_default_zone(), &Stats);
				PeakBytes.store(std::max(PeakBytes.load(), Stats.size_in_use));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		});
#endif
		FStaticMeshDecodedGeometry Input;
		Input.MaterialSlots.push_back({"Material", 0, "Material"});
		auto& Mesh = Input.Meshes.emplace_back();
		Mesh.Name = "Fixture";
		for (uint32 Triangle = 0; Triangle < Triangles; ++Triangle)
		{
			const float X = static_cast<float>(Triangle);
			Mesh.Positions.insert(Mesh.Positions.end(), {{X, 0, 0}, {X + 1, 0, 0}, {X, 1, 0}});
			Mesh.Indices.insert(Mesh.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
		}
		std::string Error;
		FStaticMeshImportedData Source;
		ASSERT_TRUE(Source.Initialize(std::move(Input), Error)) << Error;
		auto Decoded = Source.AcquireGeometry(Error);
		ASSERT_TRUE(Error.empty()) << Error;
		EXPECT_EQ(Decoded->Meshes.front().Positions.size(), Triangles * 3);
		EXPECT_EQ(Source.AcquireGeometry(Error), Decoded);
		EXPECT_EQ(Source.GetGeometryBulk().GetPayloadSize(), Triangles == 1 ? 195u : 4800147u);
		EXPECT_EQ(Source.GetIdentity().HashLow, Triangles == 1 ? 4982799754724307949ull : 17565407108445809865ull);
		EXPECT_EQ(Source.GetIdentity().HashHigh, Triangles == 1 ? 10298414200299834774ull : 892654471079648671ull);
		FStaticMeshBuildResult Product;
		ASSERT_TRUE(BuildStaticMeshDerivedData({.ImportedData = Source, .bPersistDerivedData = false}, Product, Error)) << Error;
		const uint64 Retained = Mesh.Positions.capacity() * sizeof(FVector3f)
			+ Mesh.Indices.capacity() * sizeof(uint32);
		std::cout << "residency_fixture triangles=" << Triangles << " retained_array_capacity_bytes=" << Retained
			<< " payload_bytes=" << Source.GetGeometryBulk().GetPayloadSize()
			<< " identity=" << Source.GetIdentity().HashLow << ":" << Source.GetIdentity().HashHigh;
#if defined(__APPLE__)
		Sampler.request_stop();
		Sampler.join();
		std::cout << " process_sampled_peak_allocated_bytes=" << PeakBytes.load();
#endif
		std::cout << std::endl;
	}
}

// Records detached recipe and acceleration costs without imposing host timing thresholds.
TEST(FStaticMeshAuthoredCompilationTests, RepresentativeCandidateBudgets)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests(
		(Testing::GetTestWorkDirectory() / "AuthoredBudgetCache").generic_string());
	for (const uint32 TriangleCount : {1u, 100000u})
	{
		FStaticMeshDecodedGeometry Geometry;
		Geometry.MaterialSlots.push_back({"Material", 0, "Material"});
		auto& Section = Geometry.Meshes.emplace_back();
		Section.Name = "BudgetFixture";
		Section.Positions.reserve(TriangleCount * 3);
		Section.Indices.reserve(TriangleCount * 3);
		for (uint32 Triangle = 0; Triangle < TriangleCount; ++Triangle)
		{
			const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100),
				float(Triangle / 10000));
			Section.Positions.insert(Section.Positions.end(),
				{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
			Section.Indices.insert(Section.Indices.end(),
				{Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
		}
		std::string Error;
		FStaticMeshImportedData Source;
		ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error)) << Error;
		Source.ReleaseGeometry();
		const auto Start = std::chrono::steady_clock::now();
		FStaticMeshBuildResult Render;
		ASSERT_TRUE(BuildStaticMeshDerivedData(
			{.ImportedData = Source, .bPersistDerivedData = false}, Render, Error)) << Error;
		ASSERT_EQ(Render.Origin, EStaticMeshBuildOrigin::Rebuilt);
		const auto RenderEnd = std::chrono::steady_clock::now();
		ASSERT_NE(Render.RenderData, nullptr);
		const auto& LOD = Render.RenderData->LODResources.front();
		const auto Acceleration = BuildStaticMeshRayQueryAcceleration(LOD);
		ASSERT_NE(Acceleration, nullptr);
		FStaticMeshCollisionBuildResult Collision;
		const auto CollisionStart = std::chrono::steady_clock::now();
		ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Render.RenderData,
			EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
			EBodySetupCollisionQueryPolicy::SimpleAndComplex,
			Collision, Error, false)) << Error;
		ASSERT_TRUE(Collision.Complex);
		const auto CollisionEnd = std::chrono::steady_clock::now();
		const auto Nanoseconds = [](auto Duration) {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count();
		};
		std::cout << "authored_budget_fixture triangles=" << TriangleCount
			<< " canonical_bytes=" << Source.GetGeometryBulk().GetPayloadSize()
			<< " render_payload_bytes=" << Render.PayloadBytes
			<< " ray_retained_bytes=" << Acceleration->RetainedBytes
			<< " collision_retained_bytes=" << Collision.Complex.GetRetainedBytes()
			<< " decode_render_ns=" << Nanoseconds(RenderEnd - Start)
			<< " ray_ns=" << Acceleration->BuildNanoseconds
			<< " collision_ns=" << Nanoseconds(CollisionEnd - CollisionStart) << std::endl;
		EXPECT_FALSE(Source.IsGeometryResident());
		EXPECT_LE(Acceleration->RetainedBytes,
			std::max<uint64>(1024, uint64(TriangleCount) * 96));
	}
}

namespace
{
	auto MakeResidencyGeometry() -> Durin::FStaticMeshDecodedGeometry
	{
		Durin::FStaticMeshDecodedGeometry Geometry;
		Geometry.MaterialSlots.push_back({"Material", 0, "Material"});
		auto& Mesh = Geometry.Meshes.emplace_back();
		Mesh.Name = "Fixture";
		Mesh.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
		Mesh.Indices = {0, 1, 2};
		return Geometry;
	}

	// Exercises the persisted field boundary without exposing mutable bulk in the source API.
	auto GetReflectedSourceBulk(Durin::FStaticMeshImportedData& Source) -> Durin::FEditorBulkData&
	{
		auto* Property = Durin::FStaticMeshImportedData::StaticStruct()->FindPropertyByName("Geometry");
		return *Property->ContainerPtrToValuePtr<Durin::FEditorBulkData>(&Source);
	}

	class FResidencyReadProbe final : public Durin::FPackageResource
	{
	public:
		explicit FResidencyReadProbe(Durin::FSharedByteBuffer InBytes, bool InFail = false)
			: FPackageResource(InBytes.GetSize()), Bytes(std::move(InBytes)), bFail(InFail) {}

	private:
		auto ReadRangeImpl(uint64 Offset, uint64 Size, const std::atomic_bool&)
			-> Durin::FPackageResourceReadResult override
		{
			if (bFail) return {.Status = Durin::EPackageResourceReadStatus::IoError,
				.Message = "Deliberate residency source read failure."};
			return {.Status = Durin::EPackageResourceReadStatus::Success,
				.Buffer = Bytes.MakeView(Offset, Size)};
		}
		Durin::FSharedByteBuffer Bytes;
		bool bFail;
	};

	auto AttachResidencyProbe(Durin::FStaticMeshImportedData& Source, bool bFail = false)
		-> std::shared_ptr<FResidencyReadProbe>
	{
		const auto& Bulk = Source.GetGeometryBulk();
		auto Resource = std::make_shared<FResidencyReadProbe>(Bulk.GetPayload().Wait().Buffer, bFail);
		Durin::FEditorBulkData Attached;
		std::string Error;
		EXPECT_TRUE(Durin::FEditorBulkData::TryCreatePackageBacked(Bulk.GetInstanceId(),
			Bulk.GetPayloadId(), Bulk.GetPayloadSize(),
			{.Resource = Resource, .StoredSize = Bulk.GetPayloadSize()}, Attached, &Error)) << Error;
		Source.ReleaseGeometry();
		GetReflectedSourceBulk(Source) = std::move(Attached);
		return Resource;
	}
}

TEST(FStaticMeshSourceResidencyTests, SharesConcurrentReadsAndSurvivesReleaseCopyAndReplacement)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error)) << Error;
	ASSERT_TRUE(Source.IsGeometryResident());
	const auto Resource = AttachResidencyProbe(Source);
	const auto Identity = Source.GetIdentity();
	EXPECT_TRUE(Source.IsValid());
	EXPECT_EQ(Source.GetMeshCount(), 1u);
	EXPECT_EQ(Source.GetMaterialSlotCount(), 1u);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	std::array<FStaticMeshGeometryReadHandle, 16> Handles;
	std::vector<std::jthread> Readers;
	for (auto& Handle : Handles)
		Readers.emplace_back([&Source, &Handle] {
			std::string ReadError;
			Handle = Source.AcquireGeometry(ReadError);
			EXPECT_TRUE(ReadError.empty()) << ReadError;
		});
	Readers.clear();
	ASSERT_TRUE(Handles.front());
	for (const auto& Handle : Handles) EXPECT_EQ(Handle, Handles.front());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	FStaticMeshImportedData Copy = Source;
	Source.ReleaseGeometry();
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_EQ(Copy.AcquireGeometry(Error), Handles.front());
	const auto Reload = Source.AcquireGeometry(Error);
	ASSERT_TRUE(Reload) << Error;
	EXPECT_NE(Reload, Handles.front());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 2u);
	EXPECT_EQ(Source.GetIdentity(), Identity);
	std::weak_ptr<const FStaticMeshDecodedGeometry> Weak = Handles.front();
	Handles.fill({});
	EXPECT_FALSE(Weak.expired());
	Copy.ReleaseGeometry();
	EXPECT_TRUE(Weak.expired());
	const auto Old = Source.AcquireGeometry(Error);
	auto Replacement = MakeResidencyGeometry();
	Replacement.Meshes.front().Positions.front().x = 7;
	ASSERT_TRUE(Source.Initialize(std::move(Replacement), Error)) << Error;
	EXPECT_NE(Source.GetIdentity(), Identity);
	EXPECT_EQ(Old->Meshes.front().Positions.front().x, 0);
	EXPECT_EQ(Source.AcquireGeometry(Error)->Meshes.front().Positions.front().x, 7);
	std::jthread Releaser([&] { for (int Index = 0; Index < 32; ++Index) Source.ReleaseGeometry(); });
	for (int Index = 0; Index < 32; ++Index)
	{
		const auto Handle = Source.AcquireGeometry(Error);
		ASSERT_TRUE(Handle) << Error;
		EXPECT_EQ(Handle->Meshes.front().Positions.front().x, 7);
	}
}

TEST(FStaticMeshSourceResidencyTests, InvalidCompleteInitializationPreservesIdentityAndReaders)
{
	using namespace Durin;
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error)) << Error;
	const auto Original = Source.AcquireGeometry(Error);
	const auto Identity = Source.GetIdentity();
	for (uint32 Case = 0; Case < 7; ++Case)
	{
		auto Invalid = MakeResidencyGeometry();
		auto& Mesh = Invalid.Meshes.front();
		switch (Case)
		{
		case 0: Mesh.Indices.front() = 3; break;
		case 1: Mesh.Normals.resize(1); break;
		case 2: Mesh.UVChannels[3].resize(2); break;
		case 3: Mesh.Colors.resize(1); break;
		case 4: Mesh.Tangents.resize(1); break;
		case 5: Mesh.Name.resize(4097); break;
		case 6: Invalid.MaterialSlots.clear(); break;
		}
		EXPECT_FALSE(Source.Initialize(std::move(Invalid), Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_EQ(Source.GetIdentity(), Identity);
		EXPECT_EQ(Source.AcquireGeometry(Error), Original);
	}
}

TEST(FStaticMeshSourceResidencyTests, MalformedCanonicalBytesNeverPublishPartialResidency)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error)) << Error;
	const auto Payload = Source.GetGeometryBulk().GetPayload().Wait();
	const FByteBuffer Original(Payload.Buffer.begin(), Payload.Buffer.end());
	ASSERT_EQ(Original.size(), 195u);
	for (uint32 Case = 0; Case < 6; ++Case)
	{
		FByteBuffer Bytes = Original;
		switch (Case)
		{
		case 0: Bytes.pop_back(); break;
		case 1: Bytes.push_back(std::byte{}); break;
		case 2: WriteU64(Bytes, 75, 50'000'000); break;
		case 3: WriteU32(Bytes, Bytes.size() - 4, 3); break;
		case 5: WriteU64(Bytes, 75, std::numeric_limits<uint64>::max()); break;
		case 4:
			WriteU64(Bytes, 119, 1);
			Bytes.insert(Bytes.begin() + 127, 12, std::byte{});
			break;
		}
		ASSERT_TRUE(GetReflectedSourceBulk(Source).UpdatePayload(Bytes));
		Error = "stale diagnostic";
		EXPECT_FALSE(Source.AcquireGeometry(Error));
		EXPECT_FALSE(Error.empty());
		EXPECT_NE(Error, "stale diagnostic");
		EXPECT_FALSE(Source.IsGeometryResident());
	}
	ASSERT_TRUE(GetReflectedSourceBulk(Source).UpdatePayload(Original));
	ASSERT_TRUE(Source.AcquireGeometry(Error)) << Error;
}

TEST(FStaticMeshSourceResidencyTests, WarmCacheSkipsUnreadableBulkAndMissPreservesReadDiagnostic)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	const auto Fixture = ImportCacheFixture("StaticMeshUnreadableResidency");
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetImportedData().IsGeometryResident());
	FStaticMeshBuildRequest Request{.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.ImportedData = Fixture.Mesh->GetImportedData()};
	const auto Resource = AttachResidencyProbe(Request.ImportedData, true);
	FStaticMeshBuildResult Product;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product, Error)) << Error;
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	ASSERT_TRUE(std::filesystem::remove(GetObjectPath(Fixture, GetStaticMeshKey(*Fixture.Mesh))));
	EXPECT_FALSE(BuildStaticMeshDerivedData(Request, Product, Error));
	EXPECT_EQ(Error, "Deliberate residency source read failure.");
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	EXPECT_FALSE(Request.ImportedData.IsGeometryResident());
	EXPECT_FALSE(BuildStaticMeshDerivedData(Request, Product, Error));
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 2u);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshSourceResidencyTests, ExistingAuthoredPackageAndDuplicateRetainCanonicalSource)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	FMountPaths::InitDefaultMountPoints();
	ASSERT_TRUE(RefreshAssetRegistry());
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/Engine/Models/Box", Path));
	DStaticMesh* Mesh = nullptr;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Path), Mesh));
	ASSERT_NE(Mesh, nullptr);
	std::string Error;
	const auto Geometry = Mesh->GetImportedData().AcquireGeometry(Error);
	ASSERT_TRUE(Geometry) << Error;
	auto* Duplicate = Cast<DStaticMesh>(DuplicateObject(Mesh, nullptr, "ResidencyDuplicate"));
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetImportedData().GetIdentity(), Mesh->GetImportedData().GetIdentity());
	Mesh->GetImportedData().ReleaseGeometry();
	const auto Copied = Duplicate->GetImportedData().AcquireGeometry(Error);
	ASSERT_TRUE(Copied) << Error;
	EXPECT_EQ(Copied->Meshes.size(), Geometry->Meshes.size());
	EXPECT_EQ(Copied->Meshes.front().Indices, Geometry->Meshes.front().Indices);
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationIsTypedAndDiscardsProducts)
{
	using namespace Durin;
	FStaticMeshBuildResult Render;
	Render.RenderData = std::make_unique<FStaticMeshRenderData>();
	std::string Error;
	const auto Cancelled = BuildStaticMeshDerivedData({}, Render, Error,
		{.ShouldCancel = [] { return true; }});
	EXPECT_EQ(Cancelled.Status, EStaticMeshBuildStatus::Cancelled);
	EXPECT_EQ(Render.RenderData, nullptr);
	EXPECT_EQ(Cancelled.Diagnostic, Error);

	const auto Invalid = BuildStaticMeshDerivedData({}, Render, Error);
	EXPECT_EQ(Invalid.Status, EStaticMeshBuildStatus::Failed);
	EXPECT_NE(Invalid.Status, Cancelled.Status);
	EXPECT_FALSE(Invalid.Diagnostic.empty());

	FStaticMeshCollisionBuildResult Collision;
	FStaticMeshRenderData EmptyRender;
	uint32 Checks = 0;
	const auto LateCancellation = BuildStaticMeshCollisionDerivedData(EmptyRender,
		EBodySetupCollisionSourceMode::None,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error, false,
		{.ShouldCancel = [&] { return ++Checks == 2; }});
	EXPECT_EQ(LateCancellation.Status, EStaticMeshBuildStatus::Cancelled);
	EXPECT_FALSE(Collision.Simple);
	EXPECT_FALSE(Collision.Complex);
	EXPECT_LE(FStaticMeshBuildOutcome(EStaticMeshBuildStatus::Failed,
		std::string(8192, 'x')).Diagnostic.size(), MaximumStaticMeshBuildDiagnosticBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationInterruptsGeometryLoops)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests(
		(Testing::GetTestWorkDirectory() / "AuthoredCancellationCache").generic_string());
	FStaticMeshDecodedGeometry Input = MakeResidencyGeometry();
	auto& Section = Input.Meshes.front();
	Section.Positions.clear();
	Section.Indices.clear();
	for (uint32 Triangle = 0; Triangle < 4096; ++Triangle)
	{
		const FVector3f Base(float(Triangle % 64), float(Triangle / 64), 0);
		Section.Positions.insert(Section.Positions.end(),
			{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
		Section.Indices.insert(Section.Indices.end(),
			{Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
	}
	std::vector<FVector3> CollisionPositions;
	for (const auto& Position : Section.Positions) CollisionPositions.emplace_back(Position);
	const auto Indices = Section.Indices;
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Input), Error)) << Error;
	for (const uint64 StopAt : {8ull, 32ull, 128ull})
	{
		FStaticMeshBuildExecutionMetrics Metrics;
		FStaticMeshBuildResult Product;
		const auto Outcome = BuildStaticMeshDerivedData(
			{.ImportedData = Source, .bPersistDerivedData = false}, Product, Error,
			{.ShouldCancel = [&] { return Metrics.CancellationCheckpoints >= StopAt; },
				.Metrics = &Metrics});
		EXPECT_EQ(Outcome.Status, EStaticMeshBuildStatus::Cancelled) << StopAt;
		EXPECT_EQ(Product.RenderData, nullptr);
		EXPECT_LE(Metrics.CancellationCheckpoints, StopAt + 1);
	}
	for (const uint32 StopAt : {2u, 64u, 256u})
	{
		uint32 Checks = 0;
		FCollisionGeometryBuildDiagnostics Diagnostic;
		const auto Geometry = FCollisionGeometryRef::BuildTriangleMesh(
			CollisionPositions, Indices, &Diagnostic, [&] { return ++Checks >= StopAt; });
		EXPECT_FALSE(Geometry) << StopAt;
		EXPECT_EQ(Diagnostic.Status, ECollisionGeometryBuildStatus::Cancelled);
		EXPECT_EQ(Checks, StopAt);
	}
	const std::array<FVector3, 4> Tetrahedron{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	FCollisionGeometryBuildDiagnostics Diagnostic;
	EXPECT_FALSE(FCollisionGeometryRef::BuildConvexHull(Tetrahedron, &Diagnostic,
		[] { return true; }));
	EXPECT_EQ(Diagnostic.Status, ECollisionGeometryBuildStatus::Cancelled);
}

TEST(FStaticMeshAuthoredCompilationTests, SealedCandidatePublishesWithoutProviderOrReconstruction)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("AuthoredCandidateTest"));
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry(), Error)) << Error;
	ASSERT_TRUE(Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error));
	const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
	std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate;
	ASSERT_TRUE(BuildStaticMeshAuthoredCandidate(MakeStaticMeshAuthoredBuildRequest(
		Mesh->GetImportedData(), Snapshot), Candidate, Error)) << Error;
	const auto Ray = Candidate->GetRenderData()->LODResources.front().RayQueryAcceleration;
	const auto CollisionIdentity = Candidate->GetCollision().Complex.GetIdentity();
	ASSERT_NE(Ray, nullptr);
	ASSERT_NE(CollisionIdentity, 0u);
	FScopedStaticMeshProviderRestore RestoreProvider;
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
	const auto Start = std::chrono::steady_clock::now();
	ASSERT_TRUE(ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Candidate), Snapshot, Error)) << Error;
	const auto Duration = std::chrono::steady_clock::now() - Start;
	EXPECT_EQ(Mesh->GetRenderData()->LODResources.front().RayQueryAcceleration, Ray);
	FCollisionGeometryRef PublishedCollision;
	ASSERT_TRUE(Mesh->GetBodySetup()->BuildComplexGeometry(PublishedCollision));
	EXPECT_EQ(PublishedCollision.GetIdentity(), CollisionIdentity);
	EXPECT_FALSE(Mesh->GetImportedData().IsGeometryResident());
	std::cout << "authored_candidate_application_ns="
		<< std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count() << std::endl;
}

TEST(FStaticMeshAuthoredCompilationTests, CancelledAndStaleCandidatesPreserveLiveState)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("AuthoredCandidateTest"));
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry(), Error));
	for (const uint32 Scenario : {0u, 1u, 2u})
	{
		const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
		std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate;
		ASSERT_TRUE(BuildStaticMeshAuthoredCandidate(MakeStaticMeshAuthoredBuildRequest(
			Mesh->GetImportedData(), Snapshot), Candidate, Error)) << Error;
		if (Scenario == 1) ASSERT_TRUE(Mesh->RenameMaterialSlot(0, FName("Changed"), Error));
		if (Scenario == 2) ASSERT_TRUE(Mesh->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::SimpleOnly, Error));
		const auto* Original = Mesh->GetRenderData();
		const auto SourceIdentity = Mesh->GetImportedData().GetIdentity();
		const auto Revision = Mesh->GetRenderResourceStatus().Revision;
		const auto Outcome = ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Candidate), Snapshot, Error,
			true, {.ShouldCancel = [Scenario] { return Scenario == 0; }});
		EXPECT_EQ(Outcome.Status, Scenario == 0 ? EStaticMeshBuildStatus::Cancelled : EStaticMeshBuildStatus::Failed);
		EXPECT_EQ(Mesh->GetRenderData(), Original);
		EXPECT_EQ(Mesh->GetImportedData().GetIdentity(), SourceIdentity);
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Revision);
	}
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationAbandonsDecodeAndRayScratch)
{
	using namespace Durin;
	auto Geometry = MakeResidencyGeometry();
	for (uint32 Index = 0; Index < 4096; ++Index)
		Geometry.Meshes.front().Indices.insert(Geometry.Meshes.front().Indices.end(), {0, 1, 2});
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error));
	Source.ReleaseGeometry();
	uint32 Checks = 0;
	EXPECT_FALSE(Source.AcquireGeometry(Error, [&] { return ++Checks == 8; }));
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_EQ(Checks, 8u);
	ASSERT_TRUE(Source.AcquireGeometry(Error)) << Error;
	FStaticMeshBuildResult Render;
	ASSERT_TRUE(BuildStaticMeshDerivedData({.ImportedData = Source}, Render, Error));
	for (const uint32 StopAt : {1u, 16u, 64u})
	{
		Checks = 0;
		EXPECT_EQ(BuildStaticMeshRayQueryAcceleration(Render.RenderData->LODResources.front(),
			[&] { return ++Checks == StopAt; }), nullptr);
		EXPECT_EQ(Checks, StopAt);
	}
}

namespace
{
	class FUnexpectedStaticMeshProvider final : public Durin::IStaticMeshBuildProvider
	{
	public:
		auto GetDescriptor() const -> Durin::FStaticMeshBuildProviderDescriptor override
		{
			ADD_FAILURE() << "Ambiguous providers must not be invoked.";
			return {};
		}
		auto BuildRender(const Durin::FStaticMeshRecipeBuildRequest&, Durin::FStaticMeshRecipeBuildProduct&,
			std::string&, const Durin::FStaticMeshBuildExecutionControl&) -> Durin::FStaticMeshBuildOutcome override
		{
			ADD_FAILURE();
			return {Durin::EStaticMeshBuildStatus::Failed};
		}
		auto BuildCollision(const Durin::FStaticMeshCollisionRecipeRequest&, Durin::FStaticMeshCollisionRecipeProduct&,
			std::string&, const Durin::FStaticMeshBuildExecutionControl&) -> Durin::FStaticMeshBuildOutcome override
		{
			ADD_FAILURE();
			return {Durin::EStaticMeshBuildStatus::Failed};
		}
	};
}

TEST(FStaticMeshAuthoredCompilationTests, ProviderFailuresAndPersistenceDiagnosticsRemainDistinct)
{
	using namespace Durin;
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	FStaticMeshBuildResult Render;
	{
		FModuleTestOwner Owner("StaticMeshAmbiguousProvider");
		FUnexpectedStaticMeshProvider Provider;
		auto Registration = Owner.RegisterFeature(Provider);
		ASSERT_TRUE(Registration.IsValid());
		EXPECT_EQ(BuildStaticMeshDerivedData({.ImportedData = Source}, Render, Error).Status,
			EStaticMeshBuildStatus::Failed);
		EXPECT_EQ(Render.RenderData, nullptr);
	}
	{
		FScopedStaticMeshProviderRestore Restore;
		ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
		EXPECT_EQ(BuildStaticMeshDerivedData({.ImportedData = Source}, Render, Error).Status,
			EStaticMeshBuildStatus::Failed);
		EXPECT_EQ(Render.RenderData, nullptr);
	}
	FScopedDerivedDataCacheRestore RestoreCache;
	const auto CacheFile = Testing::GetTestWorkDirectory() / "StaticMeshBlockedCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	EXPECT_EQ(BuildStaticMeshDerivedData({.ImportedData = Source}, Render, Error).Status,
		EStaticMeshBuildStatus::Succeeded);
	EXPECT_NE(Render.RenderData, nullptr);
	EXPECT_FALSE(Render.DiagnosticMessage.empty());
	EXPECT_TRUE(Error.empty());
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationDiscardsPayloadAndFinalizationScratch)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests(
		(Testing::GetTestWorkDirectory() / "PayloadCancellationCache").generic_string());
	auto Input = MakeResidencyGeometry();
	auto& Section = Input.Meshes.front();
	Section.Positions.clear();
	Section.Indices.clear();
	for (uint32 Triangle = 0; Triangle < 4096; ++Triangle)
	{
		const FVector3f Base(float(Triangle % 64), float(Triangle / 64), 0);
		Section.Positions.insert(Section.Positions.end(),
			{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
		Section.Indices.insert(Section.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
	}
	std::string Error;
	FStaticMeshImportedData Source;
	ASSERT_TRUE(Source.Initialize(std::move(Input), Error));
	FStaticMeshBuildResult Render;
	ASSERT_TRUE(BuildStaticMeshDerivedData({.ImportedData = Source}, Render, Error)) << Error;
	FStaticMeshCollisionBuildResult Collision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Render.RenderData,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error)) << Error;
	FStaticMeshPayloadData Payload;
	ASSERT_TRUE(MakeStaticMeshPayloadData(*Render.RenderData, Payload, Error));
	FStaticMeshCollisionPayloadData CollisionPayload;
	ASSERT_TRUE(MakeStaticMeshCollisionPayloadData(Collision.Complex,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, CollisionPayload, Error));

	// Measure callback counts first, then interrupt each codec in the middle of its work.
	// A one-shot callback also verifies that a caught cancellation cannot become a cache miss.
	const auto CheckCodec = [&](auto& Value) {
		FByteBuffer Bytes;
		uint32 Count = 0;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::DerivedDataPayload);
		Value.Serialize(Writer, EStaticMeshTargetPlatform::Win64, [&] { ++Count; return false; });
		ASSERT_FALSE(Writer.HasError());
		ASSERT_GT(Count, 20u);
		const uint32 StopAt = Count / 2;
		Count = 0;
		FByteBuffer CancelledBytes;
		FCanonicalMemoryWriter CancelledWriter(CancelledBytes, EArchivePurpose::DerivedDataPayload);
		Value.Serialize(CancelledWriter, EStaticMeshTargetPlatform::Win64, [&] { return ++Count == StopAt; });
		EXPECT_TRUE(CancelledWriter.HasError());
		EXPECT_TRUE(CancelledBytes.empty());

		using TPayload = std::remove_cvref_t<decltype(Value)>;
		TPayload Decoded;
		Count = 0;
		FCanonicalMemoryReader Reader(Bytes, EArchivePurpose::DerivedDataPayload);
		Decoded.Serialize(Reader, EStaticMeshTargetPlatform::Win64, [&] { ++Count; return false; });
		ASSERT_FALSE(Reader.HasError());
		ASSERT_GT(Count, 20u);
		const uint32 ReadStopAt = Count / 2;
		Count = 0;
		TPayload CancelledDecoded;
		FCanonicalMemoryReader CancelledReader(Bytes, EArchivePurpose::DerivedDataPayload);
		CancelledDecoded.Serialize(CancelledReader, EStaticMeshTargetPlatform::Win64,
			[&] { return ++Count == ReadStopAt; });
		EXPECT_TRUE(CancelledReader.HasError());
		if constexpr (std::is_same_v<TPayload, FStaticMeshPayloadData>)
			EXPECT_TRUE(CancelledDecoded.LODs.empty());
		else
			EXPECT_TRUE(CancelledDecoded.Positions.empty());
	};
	CheckCodec(Payload);
	CheckCodec(CollisionPayload);
	uint32 Checks = 0;
	FStaticMeshPayloadData Unchanged;
	Unchanged.MaterialSlotCount = 123;
	EXPECT_FALSE(MakeStaticMeshPayloadData(*Render.RenderData, Unchanged, Error,
		[&] { return ++Checks == 8; }));
	EXPECT_EQ(Unchanged.MaterialSlotCount, 123u);
	EXPECT_TRUE(Unchanged.LODs.empty());
	Checks = 0;
	FCollisionGeometryRef UnchangedGeometry = Collision.Complex;
	EXPECT_FALSE(MakeStaticMeshCollisionGeometry(CollisionPayload, UnchangedGeometry, Error,
		[&] { return ++Checks == 8; }));
	EXPECT_EQ(UnchangedGeometry.GetIdentity(), Collision.Complex.GetIdentity());
	Checks = 0;
	EXPECT_FALSE(Render.RenderData->RecalculateBounds([&] { return ++Checks == 8; }));
	EXPECT_EQ(Checks, 8u);
	Render.RenderData->RecalculateBounds();

	FStaticMeshBuildRequest CachedRequest;
	CachedRequest.ImportedData = Source;
	CachedRequest.Reconciliation.MaterialSlots = Render.MaterialSlots;
	Checks = 0;
	const auto CancelledRender = BuildStaticMeshDerivedData(CachedRequest, Render, Error,
		{.ShouldCancel = [&] { return ++Checks == 8; }});
	EXPECT_EQ(CancelledRender.Status, EStaticMeshBuildStatus::Cancelled);
	EXPECT_FALSE(Render.RenderData);
	ASSERT_TRUE(BuildStaticMeshDerivedData(CachedRequest, Render, Error));
	EXPECT_EQ(Render.Origin, EStaticMeshBuildOrigin::CacheHit);
	Checks = 0;
	const auto CancelledCollision = BuildStaticMeshCollisionDerivedData(*Render.RenderData,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, Error, true,
		{.ShouldCancel = [&] { return ++Checks == 110; }});
	EXPECT_EQ(CancelledCollision.Status, EStaticMeshBuildStatus::Cancelled);
	EXPECT_FALSE(Collision.Complex);
}

TEST(FStaticMeshAuthoredCompilationTests, MeasuresCompleteCandidateAndPublicationSeparately)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests(
		(Testing::GetTestWorkDirectory() / "CompleteCandidateTimingCache").generic_string());
	FStaticMeshDecodedGeometry Geometry = MakeResidencyGeometry();
	auto& Section = Geometry.Meshes.front();
	Section.Positions.clear();
	Section.Indices.clear();
	for (uint32 Triangle = 0; Triangle < 100000; ++Triangle)
	{
		const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100), float(Triangle / 10000));
		Section.Positions.insert(Section.Positions.end(),
			{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
		Section.Indices.insert(Section.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
	}
	std::string Error;
	FStaticMeshImportedData Source;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error));
	Source.ReleaseGeometry();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("CompleteCandidateTiming"));
	auto* Body = NewObject<DBodySetup>(Mesh, FName("BodySetup"));
	ASSERT_TRUE(Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0));
	ASSERT_TRUE(Mesh->SetBodySetup(Body));
	const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
	auto Request = MakeStaticMeshAuthoredBuildRequest(Source, Snapshot);
	Request.bPersistDerivedData = false;
	std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate;
	FStaticMeshBuildExecutionMetrics Metrics;
	const auto Start = std::chrono::steady_clock::now();
	auto LastCheckpoint = Start;
	uint64 MaximumGapNanoseconds = 0;
	const auto Outcome = BuildStaticMeshAuthoredCandidate(std::move(Request), Candidate, Error,
		{.ShouldCancel = [&] {
			const auto Now = std::chrono::steady_clock::now();
			MaximumGapNanoseconds = std::max(MaximumGapNanoseconds, static_cast<uint64>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Now - LastCheckpoint).count()));
			LastCheckpoint = Now;
			return false;
		}, .Metrics = &Metrics});
	const auto Built = std::chrono::steady_clock::now();
	ASSERT_TRUE(Outcome) << Error;
	ASSERT_NE(Candidate, nullptr);
	const auto Ray = Candidate->GetRenderData()->LODResources.front().RayQueryAcceleration;
	ASSERT_TRUE(ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Candidate), Snapshot, Error)) << Error;
	const auto Published = std::chrono::steady_clock::now();
	EXPECT_EQ(Mesh->GetRenderData()->LODResources.front().RayQueryAcceleration, Ray);
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_GT(Metrics.CancellationCheckpoints, 1000u);
	std::cout << "complete_candidate_fixture triangles=100000 build_ns="
		<< std::chrono::duration_cast<std::chrono::nanoseconds>(Built - Start).count()
		<< " publication_ns=" << std::chrono::duration_cast<std::chrono::nanoseconds>(Published - Built).count()
		<< " checkpoints=" << Metrics.CancellationCheckpoints
		<< " maximum_checkpoint_gap_ns=" << MaximumGapNanoseconds << std::endl;
	uint64 MaximumCancellationNanoseconds = 0;
	for (const uint64 StopAfter : {Metrics.CancellationCheckpoints / 4,
		Metrics.CancellationCheckpoints / 2, Metrics.CancellationCheckpoints * 3 / 4})
	{
		auto CancelRequest = MakeStaticMeshAuthoredBuildRequest(Source, Snapshot);
		CancelRequest.bPersistDerivedData = false;
		uint64 Checks = 0;
		bool bRequested = false;
		std::chrono::steady_clock::time_point RequestedAt;
		std::unique_ptr<FStaticMeshAuthoredCandidate> CancelledCandidate;
		const auto Cancelled = BuildStaticMeshAuthoredCandidate(std::move(CancelRequest),
			CancelledCandidate, Error, {.ShouldCancel = [&] {
				if (bRequested) return true;
				if (++Checks == StopAfter)
				{
					bRequested = true;
					RequestedAt = std::chrono::steady_clock::now();
				}
				return false;
			}});
		ASSERT_TRUE(bRequested);
		EXPECT_EQ(Cancelled.Status, EStaticMeshBuildStatus::Cancelled);
		EXPECT_FALSE(CancelledCandidate);
		MaximumCancellationNanoseconds = std::max(MaximumCancellationNanoseconds,
			static_cast<uint64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - RequestedAt).count()));
	}
	std::cout << "complete_candidate_cancellation_fixture samples=3 maximum_cancel_to_return_ns="
		<< MaximumCancellationNanoseconds << std::endl;

}

namespace
{
	struct FStaticMeshWorkerBarrier
	{
		std::mutex Mutex;
		std::condition_variable Changed;
		uint32 Entered = 0;
		bool bReleased = false;
		explicit FStaticMeshWorkerBarrier(Durin::EStaticMeshCompilationPhase Selected = Durin::EStaticMeshCompilationPhase::Building, bool bFirstOnly = false)
		{
			Durin::AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([this, Selected, bFirstOnly](uint64, Durin::EStaticMeshCompilationPhase Phase) {
				if (Phase != Selected) return;
				EXPECT_FALSE(Durin::IsInGameThread());
				std::unique_lock Lock(Mutex);
				if (bFirstOnly && Entered != 0) return;
				++Entered;
				Changed.notify_all();
				Changed.wait(Lock, [&] { return bReleased; });
			});
		}
		auto Wait(uint32 Count, std::chrono::seconds Timeout = std::chrono::seconds(5)) -> bool
		{
			std::unique_lock Lock(Mutex);
			return Changed.wait_for(Lock, Timeout, [&] { return Entered >= Count; });
		}
		auto Release() -> void
		{
			std::lock_guard Lock(Mutex);
			bReleased = true;
			Changed.notify_all();
		}
		~FStaticMeshWorkerBarrier()
		{
			Release();
			Durin::FAssetCompilingManager::Get().FinishAllCompilation();
			Durin::AssetPrivate::SetStaticMeshCompilationPhaseHookForTests({});
		}
	};
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerSupersedesOnceAndRetainsLateWorkers)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerSupersession"));
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	FStaticMeshWorkerBarrier Barrier;
	std::vector<EStaticMeshCompilationStatus> Terminals;
	const auto Completed = [&](const FStaticMeshCompilationDiagnostic& Value) { Terminals.push_back(Value.Status); };
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error, Completed)) << Error;
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error, Completed)) << Error;
	ASSERT_TRUE(Barrier.Wait(2));
	EXPECT_TRUE(Terminals.empty());
	const uint64 Reserved = GetStaticMeshCompilationManagerDiagnostics().ReservedBytes;
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	ASSERT_EQ(1u, Terminals.size());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, Terminals.front());
	EXPECT_EQ(2u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	EXPECT_EQ(Reserved, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	EXPECT_FALSE(SubmitStaticMeshCompilation(*Mesh, {}, Error, Completed));
	Barrier.Release();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	ASSERT_EQ(2u, Terminals.size());
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, Terminals.back());
	EXPECT_NE(nullptr, Mesh->GetRenderData());
	EXPECT_FALSE(HasPendingStaticMeshCompilation(*Mesh));
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(2u, Terminals.size());
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerBoundsAcceptedRecordsAndDefersCancellation)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerCountBounds"));
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	FStaticMeshWorkerBarrier Barrier;
	uint32 Completions = 0;
	for (uint32 Index = 0; Index < 32; ++Index)
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error,
			[&](const FStaticMeshCompilationDiagnostic&) { ++Completions; })) << Error;
	EXPECT_EQ(0u, Completions);
	EXPECT_EQ(32u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	const auto Latest = GetStaticMeshCompilationDiagnostic(*Mesh).RequestId;
	EXPECT_FALSE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error));
	EXPECT_EQ(Latest, GetStaticMeshCompilationDiagnostic(*Mesh).RequestId);
	CancelStaticMeshCompilation(*Mesh);
	EXPECT_EQ(0u, Completions);
	const auto Batch = FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_LE(Batch.ProcessedCompletionCount, 2u);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	EXPECT_EQ(32u, Completions);
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	EXPECT_EQ(nullptr, Mesh->GetRenderData());
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerRejectsMutationAndDestructionDuringBuild)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerOwnerMutation"));
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry(), Error));
	const auto* Original = Mesh->GetRenderData();
	FStaticMeshWorkerBarrier Barrier;
	std::vector<EStaticMeshCompilationStatus> Terminals;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Mesh->GetImportedData()}, Error,
		[&](const auto& Value) { Terminals.push_back(Value.Status); }));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(Mesh->RenameMaterialSlot(0, "MutationWhileBuilding", Error));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	ASSERT_EQ(1u, Terminals.size());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, Terminals.front());
	EXPECT_EQ(Original, Mesh->GetRenderData());
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Mesh->GetImportedData()}, Error,
		[&](const auto& Value) { Terminals.push_back(Value.Status); }));
	ASSERT_TRUE(Barrier.Wait(2));
	Mesh->BeginDestroy();
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_EQ(2u, Terminals.size());
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, Terminals.back());
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerRejectsRetiredProviderBeforePublication)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerRetiredProvider"));
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	FScopedStaticMeshProviderRestore Restore;
	FStaticMeshWorkerBarrier Barrier(EStaticMeshCompilationPhase::Mailbox);
	std::optional<EStaticMeshCompilationStatus> Terminal;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error,
		[&](const auto& Value) { Terminal = Value.Status; }));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	Barrier.Release();
	const auto Result = FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	ASSERT_TRUE(Terminal.has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, *Terminal);
	EXPECT_TRUE(Result.SuccessfullyCompiledAssets.empty());
	EXPECT_EQ(nullptr, Mesh->GetRenderData());
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerEnforcesByteReservationBeforeSupersession)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerByteBounds"));
	auto Geometry = MakeResidencyGeometry();
	Geometry.Meshes.front().Positions.resize(100000);
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(Geometry, Error));
	FStaticMeshWorkerBarrier Barrier;
	uint32 Accepted = 0;
	while (SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error)) ++Accepted;
	EXPECT_GT(Accepted, 2u);
	EXPECT_LT(Accepted, 32u);
	const auto State = GetStaticMeshCompilationManagerDiagnostics();
	EXPECT_EQ(Accepted, State.OutstandingRecords);
	EXPECT_LE(State.ReservedBytes, 1024ull * 1024 * 1024);
	const auto Latest = GetStaticMeshCompilationDiagnostic(*Mesh).RequestId;
	Geometry.Meshes.front().Positions.resize(800000);
	FStaticMeshImportedData Oversized;
	ASSERT_TRUE(Oversized.Initialize(std::move(Geometry), Error));
	EXPECT_FALSE(SubmitStaticMeshCompilation(*Mesh, {.Source = Oversized}, Error));
	EXPECT_EQ(Latest, GetStaticMeshCompilationDiagnostic(*Mesh).RequestId);
	CancelStaticMeshCompilation(*Mesh);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	std::unique_ptr<FStaticMeshAuthoredCandidate> Candidate;
	EXPECT_FALSE(BuildStaticMeshAuthoredCandidate(MakeStaticMeshAuthoredBuildRequest(Source, {}), Candidate, Error,
		{.MaximumWorkingSetBytes = 1024}));
	EXPECT_EQ(nullptr, Candidate);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerShutdownDrainsAndCanRestart)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerRestart"));
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	std::optional<EStaticMeshCompilationStatus> Terminal;
	{
		FStaticMeshWorkerBarrier Barrier;
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error,
			[&](const auto& Value) { Terminal = Value.Status; }));
		ASSERT_TRUE(Barrier.Wait(1));
		Barrier.Release();
		AssetPrivate::CreateStaticMeshCompilingManager()->Shutdown();
	}
	ASSERT_TRUE(Terminal.has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, *Terminal);
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	ASSERT_TRUE(AssetPrivate::CreateStaticMeshCompilingManager()->Start(&Error));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(*Mesh).Status);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerFairnessAndHistoryStayBounded)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	std::vector<uint64> Dispatches;
	std::vector<DStaticMesh*> Meshes;
	for (uint32 Index = 0; Index < 8; ++Index)
		Meshes.push_back(NewObject<DStaticMesh>(nullptr, FName(std::format("ManagerFairness{}", Index))));
	{
		FStaticMeshWorkerBarrier Barrier;
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[0], {.Source = Source}, Error));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[1], {.Source = Source}, Error));
		ASSERT_TRUE(Barrier.Wait(2));
		for (uint32 Index = 2; Index < 7; ++Index)
			ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[Index],
				{.Source = Source, .Priority = EStaticMeshCompilationPriority::Interactive}, Error));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[7], {.Source = Source}, Error));
		const uint64 BackgroundId = GetStaticMeshCompilationDiagnostic(*Meshes[7]).RequestId;
		AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([&](uint64 Id, EStaticMeshCompilationPhase Phase) {
			if (Phase == EStaticMeshCompilationPhase::Queued) Dispatches.push_back(Id);
		});
		Barrier.Release();
		FAssetCompilingManager::Get().FinishAllCompilation();
		ASSERT_EQ(6u, Dispatches.size());
		EXPECT_EQ(BackgroundId, Dispatches[4]);
	}
	for (uint32 Index = 0; Index < 130; ++Index)
	{
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[0], {.Source = Source}, Error));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Meshes[0]);
	}
	const auto Diagnostics = GetStaticMeshCompilationManagerDiagnostics();
	EXPECT_EQ(128u, Diagnostics.RetainedDiagnostics);
	EXPECT_EQ(0u, GetStaticMeshCompilationDiagnostic(*Meshes[7]).RequestId);
	EXPECT_EQ(0u, Diagnostics.OutstandingRecords);
	EXPECT_EQ(0u, Diagnostics.ReservedBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerDoesNotKeepUnloadedPackageAlive)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	const auto Fixture = ImportCacheFixture("ManagerPackageRetirement");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto Owner = MakeObjectHandle(Fixture.Mesh);
	std::optional<EStaticMeshCompilationStatus> Terminal;
	std::string Error;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Fixture.Mesh, {.Source = Fixture.Mesh->GetImportedData()}, Error,
		[&](const auto& Value) { Terminal = Value.Status; }));
	ASSERT_TRUE(Barrier.Wait(1));
	const auto Unloaded = UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved);
	EXPECT_TRUE(Unloaded) << Unloaded.Message;
	EXPECT_EQ(nullptr, ResolveObjectHandle(Owner));
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Terminal.has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, *Terminal);
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerRecapturesReflectedFactsAndInitialCollisionEdits)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerReflectedFacts"));
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	std::optional<EStaticMeshCompilationStatus> Terminal;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error,
		[&](const auto& Value) { Terminal = Value.Status; }));
	ASSERT_TRUE(Barrier.Wait(1));
	auto* Property = DStaticMesh::StaticClass()->FindPropertyByName("NormalizedSize");
	ASSERT_NE(nullptr, Property);
	*Property->ContainerPtrToValuePtr<float>(Mesh) = 3.0f;
	Barrier.Release();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	ASSERT_TRUE(Terminal.has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, *Terminal);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(*Mesh).Status);
	EXPECT_FLOAT_EQ(3.0f, Mesh->GetNormalizedSize());
	EXPECT_NE(nullptr, Mesh->GetRenderData());

	auto* Initial = NewObject<DStaticMesh>(nullptr, FName("ManagerInitialCollision"));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Initial, {.Source = Source}, Error));
	ASSERT_TRUE(Initial->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Initial);
	FCollisionGeometryRef Collision;
	ASSERT_NE(nullptr, Initial->GetBodySetup());
	EXPECT_TRUE(Initial->GetBodySetup()->BuildComplexGeometry(Collision));
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(*Initial).Status);
}

TEST(FStaticMeshAuthoredCompilationTests, PostLoadSchedulesAndJoinsWithoutDiscardingResidentData)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("AsyncPostLoadContract");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto* Original = Fixture.Mesh->GetRenderData();
	const auto Identity = Fixture.Mesh->GetImportedData().GetIdentity();
	std::string Error;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(Fixture.Mesh->PostLoad(Error));
	ASSERT_TRUE(Barrier.Wait(1));
	const auto Request = GetStaticMeshCompilationDiagnostic(*Fixture.Mesh).RequestId;
	EXPECT_TRUE(HasPendingStaticMeshCompilation(*Fixture.Mesh));
	EXPECT_EQ(Original, Fixture.Mesh->GetRenderData());
	EXPECT_FALSE(Fixture.Mesh->GetImportedData().IsGeometryResident());
	EXPECT_TRUE(Fixture.Mesh->PostLoad(Error));
	EXPECT_EQ(Request, GetStaticMeshCompilationDiagnostic(*Fixture.Mesh).RequestId);
	Barrier.Release();
	EXPECT_TRUE(BuildStaticMeshSynchronously(*Fixture.Mesh, Fixture.Mesh->GetImportedData(), Error)) << Error;
	EXPECT_EQ(Request, GetStaticMeshCompilationDiagnostic(*Fixture.Mesh).RequestId);
	EXPECT_EQ(Identity, Fixture.Mesh->GetImportedData().GetIdentity());
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
}

TEST(FStaticMeshAuthoredCompilationTests, ReimportDefersProvenanceAndSaveFailureKeepsAppliedStateDirty)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("AsyncReimportContract");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto* Original = Fixture.Mesh->GetRenderData();
	const auto* ImportData = Fixture.Mesh->GetAssetImportData();
	const auto Identity = Fixture.Mesh->GetImportedData().GetIdentity();
	auto* Factory = NewObject<DStaticMeshFactory>(nullptr, "AsyncReimportFactory");
	std::optional<FReimportResult> Result;
	{
		FStaticMeshWorkerBarrier Barrier;
		Factory->Reimport(*Fixture.Mesh, [&](FReimportResult Value) { Result = std::move(Value); });
		ASSERT_TRUE(Barrier.Wait(1));
		EXPECT_FALSE(Result.has_value());
		EXPECT_EQ(Original, Fixture.Mesh->GetRenderData());
		EXPECT_EQ(ImportData, Fixture.Mesh->GetAssetImportData());
		CancelStaticMeshCompilation(*Fixture.Mesh);
		Barrier.Release();
		FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
		ASSERT_TRUE(Result.has_value());
		EXPECT_FALSE(Result->Succeeded());
		EXPECT_EQ(ImportData, Fixture.Mesh->GetAssetImportData());
		EXPECT_EQ(Identity, Fixture.Mesh->GetImportedData().GetIdentity());
		EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	}
	std::string Error;
	uint32 SaveAttempts = 0;
	EXPECT_FALSE(ReimportStaticMesh(*Fixture.Mesh, Error, {.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
		if (Phase != EAssetBundleSavePhase::StagePackage) return false;
		++SaveAttempts;
		return true;
	}}));
	EXPECT_EQ(1u, SaveAttempts);
	EXPECT_FALSE(Error.empty());
	EXPECT_TRUE(Fixture.Mesh->GetPackage()->IsDirty());
	EXPECT_NE(ImportData, Fixture.Mesh->GetAssetImportData());
	EXPECT_EQ(Identity, Fixture.Mesh->GetImportedData().GetIdentity());
	EXPECT_NE(nullptr, Fixture.Mesh->GetRenderData());
	const auto AppliedImport = Fixture.Mesh->GetAssetImportData();
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	EXPECT_FALSE(ReimportStaticMesh(*Fixture.Mesh, Error));
	EXPECT_EQ(AppliedImport, Fixture.Mesh->GetAssetImportData());
	EXPECT_TRUE(BuildStaticMeshSynchronously(*Fixture.Mesh, Fixture.Mesh->GetImportedData(), Error)) << Error;
}

TEST(FStaticMeshAuthoredCompilationTests, CookProjectsMissingCpuDataWithoutPublishingAuthoredState)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("DetachedCookProjection");
	ASSERT_NE(nullptr, Fixture.Mesh);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(LoadObject(Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath), Fixture.Mesh));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_EQ(nullptr, Fixture.Mesh->GetRenderData());
	const auto Identity = Fixture.Mesh->GetImportedData().GetIdentity();
	const auto Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	FByteBuffer Before, After;
	ASSERT_TRUE(SerializeAssetPackageBytes(Fixture.Mesh->GetPackage(), Before));
	FCookContext Context(std::filesystem::absolute(Fixture.Root / "Cook"), ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(ContributeEngineCookAsset(*Fixture.Mesh, "/Game/Projection", Context, Error)) << Error;
	ASSERT_TRUE(Context.Publish(&Error)) << Error;
	EXPECT_EQ(nullptr, Fixture.Mesh->GetRenderData());
	EXPECT_EQ(Revision, Fixture.Mesh->GetRenderResourceStatus().Revision);
	EXPECT_EQ(Identity, Fixture.Mesh->GetImportedData().GetIdentity());
	EXPECT_FALSE(Fixture.Mesh->GetImportedData().IsGeometryResident());
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(SerializeAssetPackageBytes(Fixture.Mesh->GetPackage(), After));
	EXPECT_EQ(Before, After);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
}

TEST(FStaticMeshAuthoredCompilationTests, ConcurrentLargeCandidatesSeparateCostsAndRetainCancelledBytes)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "ConcurrentAuthoredQualification").generic_string());
	auto Geometry = MakeResidencyGeometry();
	auto& Section = Geometry.Meshes.front();
	Section.Positions.clear();
	Section.Indices.clear();
	for (uint32 Triangle = 0; Triangle < 100000; ++Triangle)
	{
		const FVector3f Base(float(Triangle % 100), float((Triangle / 100) % 100), float(Triangle / 10000));
		Section.Positions.insert(Section.Positions.end(),
			{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
		Section.Indices.insert(Section.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
	}
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error));
	Source.ReleaseGeometry();
#if defined(__APPLE__)
	std::atomic<size_t> SampledAllocationPeak = 0;
	std::jthread AllocationSampler([&](std::stop_token Stop) {
		while (!Stop.stop_requested())
		{
			malloc_statistics_t Stats{};
			malloc_zone_statistics(nullptr, &Stats);
			SampledAllocationPeak.store(std::max(SampledAllocationPeak.load(), Stats.size_in_use));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});
#endif
	std::array<DStaticMesh*, 2> Meshes;
	std::array<std::optional<FStaticMeshCompilationDiagnostic>, 2> Results;
	FStaticMeshWorkerBarrier Barrier(EStaticMeshCompilationPhase::Mailbox);
	for (size_t Index = 0; Index < Meshes.size(); ++Index)
	{
		Meshes[Index] = NewObject<DStaticMesh>(nullptr, FName(std::format("ConcurrentCandidate{}", Index)));
		auto* Body = NewObject<DBodySetup>(Meshes[Index], FName("BodySetup"));
		ASSERT_TRUE(Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0));
		ASSERT_TRUE(Meshes[Index]->SetBodySetup(Body));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[Index], {.Source = Source, .bPersistDerivedData = false}, Error,
			[&, Index](const auto& Result) { EXPECT_TRUE(IsInGameThread()); Results[Index] = Result; })) << Error;
	}
	ASSERT_TRUE(Barrier.Wait(2, std::chrono::seconds(30)));
	const auto Held = GetStaticMeshCompilationManagerDiagnostics();
	EXPECT_EQ(2u, Held.RunningWorkers);
	EXPECT_EQ(2u, Held.OutstandingRecords);
	EXPECT_LE(Held.ReservedBytes, 1024ull * 1024 * 1024);
#if defined(__APPLE__)
	malloc_statistics_t AllocationStats{};
	malloc_zone_statistics(nullptr, &AllocationStats);
	rusage Usage{};
	ASSERT_EQ(0, getrusage(RUSAGE_SELF, &Usage));
	std::cout << "concurrent_candidate_memory reserved_bytes=" << Held.ReservedBytes
		<< " default_zone_in_use=" << AllocationStats.size_in_use
		<< " process_peak_rss_bytes=" << Usage.ru_maxrss << std::endl;
#endif
	CancelStaticMeshCompilation(*Meshes[0]);
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	ASSERT_TRUE(Results[0].has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, Results[0]->Status);
	EXPECT_FALSE(Results[1].has_value());
	EXPECT_EQ(Held.ReservedBytes, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	EXPECT_EQ(nullptr, Meshes[0]->GetRenderData());
	EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	ASSERT_TRUE(Results[1].has_value());
#if defined(__APPLE__)
	AllocationSampler.request_stop();
	AllocationSampler.join();
	std::cout << "concurrent_candidate_allocation sampled_default_zone_peak_bytes="
		<< SampledAllocationPeak.load() << " sample_interval_ms=1" << std::endl;
#endif
	const auto& Completed = *Results[1];
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, Completed.Status);
	ASSERT_TRUE(Completed.Render.has_value());
	ASSERT_TRUE(Completed.Collision.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::Rebuilt, Completed.Render->Origin);
	EXPECT_TRUE(Completed.Render->DerivedDataKey.IsValid());
	EXPECT_GT(Completed.Render->PayloadBytes, 0u);
	EXPECT_GT(Completed.CaptureNanoseconds, 0u);
	EXPECT_GT(Completed.WorkerNanoseconds, 0u);
	EXPECT_GT(Completed.PublicationNanoseconds, 0u);
	FCollisionGeometryRef Collision;
	EXPECT_TRUE(Meshes[1]->GetBodySetup()->BuildComplexGeometry(Collision));
	EXPECT_FALSE(Meshes[1]->GetImportedData().IsGeometryResident());
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	std::cout << "concurrent_candidate_cost capture_ns=" << Completed.CaptureNanoseconds
		<< " worker_ns=" << Completed.WorkerNanoseconds
		<< " publication_ns=" << Completed.PublicationNanoseconds << std::endl;
}

TEST(FStaticMeshAuthoredCompilationTests, InitialMutationRequeuesAtCapacityAndExplicitCancelStopsReplacement)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	std::string Error;
	FStaticMeshImportedData Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	std::array<DStaticMesh*, 32> Meshes;
	FStaticMeshWorkerBarrier Barrier;
	for (size_t Index = 0; Index < Meshes.size(); ++Index)
	{
		Meshes[Index] = NewObject<DStaticMesh>(nullptr, FName(std::format("CapacityMutation{}", Index)));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[Index], {.Source = Source, .bPersistDerivedData = false}, Error));
	}
	ASSERT_TRUE(Barrier.Wait(2));
	ASSERT_TRUE(Meshes[0]->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error));
	ASSERT_TRUE(Meshes[1]->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, Error));
	CancelStaticMeshCompilation(*Meshes[1]);
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(32u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_NE(nullptr, Meshes[0]->GetRenderData());
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(*Meshes[0]).Status);
	EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, DiagnosticsExposeColdWarmAndPersistenceFailureWithoutPollingIo)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("ManagerObservationContract");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto Cold = GetStaticMeshCompilationDiagnostic(*Fixture.Mesh);
	ASSERT_TRUE(Cold.Render.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::Rebuilt, Cold.Render->Origin);
	EXPECT_EQ(Fixture.Mesh->GetImportedData().GetIdentity(), Cold.SourceIdentity);
	std::string Error;
	ASSERT_TRUE(Fixture.Mesh->PostLoad(Error));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	const auto Warm = GetStaticMeshCompilationDiagnostic(*Fixture.Mesh);
	ASSERT_TRUE(Warm.Render.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::CacheHit, Warm.Render->Origin);
	EXPECT_TRUE(Warm.Message.empty());
	EXPECT_EQ(Cold.Render->DerivedDataKey, Warm.Render->DerivedDataKey);
	const auto Revision = Fixture.Mesh->GetPackage()->GetEditRevision();
	for (uint32 Index = 0; Index < 10; ++Index)
		EXPECT_EQ(Warm.RequestId, GetStaticMeshCompilationDiagnostic(*Fixture.Mesh).RequestId);
	EXPECT_EQ(Revision, Fixture.Mesh->GetPackage()->GetEditRevision());
	EXPECT_FALSE(Fixture.Mesh->GetImportedData().IsGeometryResident());
	EXPECT_FALSE(HasPendingStaticMeshCompilation(*Fixture.Mesh));
	const auto CacheFile = Fixture.Root / "BlockedManagerCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	ASSERT_TRUE(Fixture.Mesh->PostLoad(Error));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	const auto FailedCache = GetStaticMeshCompilationDiagnostic(*Fixture.Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, FailedCache.Status);
	ASSERT_TRUE(FailedCache.Render.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::Rebuilt, FailedCache.Render->Origin);
	EXPECT_FALSE(FailedCache.Message.empty());
	EXPECT_LE(FailedCache.Message.size() + FailedCache.Descriptor.ProducerIdentity.size(), 4096u);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerQualifiesAllChannelsManySectionsAndConvexLimits)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto Geometry = MakeResidencyGeometry();
	const auto Triangle = Geometry.Meshes.front();
	Geometry.Meshes.clear();
	for (uint32 Index = 0; Index < 128; ++Index)
	{
		auto& Section = Geometry.Meshes.emplace_back(Triangle);
		Section.Name = std::format("AllChannels{}", Index);
		for (auto& Position : Section.Positions) Position += FVector3f(float(Index), 0, float(Index % 7));
		Section.Normals.assign(Section.Positions.size(), FVector3f(0, 0, 1));
		Section.Tangents.assign(Section.Positions.size(), FVector4f(1, 0, 0, 1));
		Section.Colors.assign(Section.Positions.size(), FVector4f(1));
		for (auto& UV : Section.UVChannels) UV.assign(Section.Positions.size(), FVector2f(0));
	}
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry), Error));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("AllChannelManagerFixture"));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source, .bPersistDerivedData = false}, Error));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	ASSERT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(*Mesh).Status);
	ASSERT_NE(nullptr, Mesh->GetRenderData());
	const auto* Original = Mesh->GetRenderData();
	EXPECT_EQ(MaxStaticMeshUVChannels, Original->LODResources.front().NumTexCoords);
	EXPECT_EQ(128u, Original->LODResources.front().Sections.size());
	auto* Body = NewObject<DBodySetup>(Mesh, FName("ConvexLimitBody"));
	ASSERT_TRUE(Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::ConvexHullFromLOD0));
	ASSERT_TRUE(Mesh->SetBodySetup(Body));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source, .bPersistDerivedData = false}, Error));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Failed, GetStaticMeshCompilationDiagnostic(*Mesh).Status);
	EXPECT_EQ(Original, Mesh->GetRenderData());
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, LatestCompletedObservationWinsOverRetainedOlderWorker)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FStaticMeshImportedData Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry(), Error));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("LatestObservationWhileOldWorkerRetained"));
	FStaticMeshWorkerBarrier Barrier(EStaticMeshCompilationPhase::Building, true);
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Error));
	const auto Latest = GetStaticMeshCompilationDiagnostic(*Mesh).RequestId;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (GetStaticMeshCompilationDiagnostic(*Mesh).Phase != EStaticMeshCompilationPhase::Terminal
		&& std::chrono::steady_clock::now() < Deadline)
	{
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto Completed = GetStaticMeshCompilationDiagnostic(*Mesh);
	EXPECT_EQ(Latest, Completed.RequestId);
	EXPECT_EQ(EStaticMeshCompilationPhase::Terminal, Completed.Phase);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, Completed.Status);
	EXPECT_EQ(1u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	EXPECT_GT(GetStaticMeshCompilationManagerDiagnostics().ReservedBytes, 0u);
	EXPECT_NE(nullptr, Mesh->GetRenderData());
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(Latest, GetStaticMeshCompilationDiagnostic(*Mesh).RequestId);
}
