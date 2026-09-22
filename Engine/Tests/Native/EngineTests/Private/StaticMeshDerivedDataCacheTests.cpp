#include "Diagnostics/StaticMeshPayloadInspection.h"
#include "StaticMesh/StaticMeshCustomVersion.h"
#include "DObject/PackageFormat.h"
#include "StaticMesh/StaticMeshBuildTestSupport.h"
#include <gtest/gtest.h>
#include "NativeAssetRuntimeTestSupport.h"

#include "NativeDObjectTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Runtime/Engine/Private/Asset/AssetDerivedDataCache.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "Asset/CookedMeshLoadManager.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
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
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Runtime/Engine/Private/StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshResources.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"

using namespace StaticMeshBuildTestSupport;

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

	auto InspectCompilationOperation(const Durin::DStaticMesh& Mesh) -> Durin::FStaticMeshCompilationDiagnostic
	{
		const auto Snapshot = Durin::InspectStaticMeshPayloads(Mesh);
		EXPECT_EQ(Snapshot.bOperationSourceMatches, Snapshot.Operation.RequestId != 0
			&& Snapshot.Operation.SourceIdentity == Mesh.GetSource().GetIdentity());
		EXPECT_LE(Durin::FormatStaticMeshCompilationDiagnostic(Snapshot.Operation).size(), 4096u);
		return Snapshot.Operation;
	}

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
		const Durin::FCacheKeyProxy Key = Durin::BuildStaticMeshDerivedDataKey({
			.SourceHash = Mesh.GetSource().GetIdentity(),
			.ReconciliationHash = Durin::BuildStaticMeshReconciliationHash(
				Mesh.GetMaterialSlots(), Mesh.GetNormalizedSize()),
			.TargetPlatform = Durin::EStaticMeshTargetPlatform::Win64}).value();
		EXPECT_TRUE(Key.IsValid());
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
		.Source = Fixture.Mesh->GetSource()};
	FStaticMeshBuildResult Product;
	std::string Error;
	// The runtime loader carries only authored metadata until a cache miss.
	Request.Source.ReleaseGeometry();
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Product.CacheDiagnostics.Read.Code, EAssetCacheError::None);
	EXPECT_EQ(Product.CacheDiagnostics.Write.Code, EAssetCacheError::None);
	EXPECT_TRUE(Request.Source.IsValid());
	ASSERT_NE(Product.RenderData, nullptr);
	const std::array<std::byte, 4> Corrupt{};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, GetObjectPath(Fixture, BaselineKey)));
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::Rebuilt);
	EXPECT_TRUE(Request.Source.IsValid());
	EXPECT_EQ(Product.CacheDiagnostics.Read.Code, EAssetCacheError::Read);
	ASSERT_NE(Product.CacheDiagnostics.Read.ReadCause, nullptr);
	EXPECT_EQ(Product.CacheDiagnostics.Read.ReadCause->Code, DerivedData::ECacheError::Corrupt);
	EXPECT_TRUE(Error.empty());
	Request.Reconciliation.MaterialSlots.clear();
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product)) << Error;
	EXPECT_EQ(Product.DerivedDataKey.ToString(), BaselineKey);
	EXPECT_EQ(Product.MaterialSlots.size(), Fixture.Mesh->GetMaterialSlots().size());

	FStaticMeshCollisionBuildResult ColdCollision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(
		*Fixture.Mesh->GetRenderData(), EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, ColdCollision)) << Error;
	FStaticMeshCollisionBuildResult Collision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision)) << Error;
	EXPECT_EQ(Collision.DerivedDataKey, ColdCollision.DerivedDataKey);
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Collision.PayloadBytes, ColdCollision.PayloadBytes);
	const auto CollisionPath = Fixture.CacheRoot / "StaticMeshCollision/Objects"
		/ Collision.DerivedDataKey.ToString().substr(0, 2)
		/ (Collision.DerivedDataKey.ToString() + ".bin");
	ASSERT_TRUE(std::filesystem::remove(CollisionPath));
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, false)) << Error;
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::Rebuilt);
	EXPECT_FALSE(std::filesystem::exists(CollisionPath));
	const auto BlockedRoot = Fixture.Root / "BlockedCollisionCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, BlockedRoot));
	FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	// A read failure remains observable even when no Put is attempted.
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, false)) << Error;
	EXPECT_TRUE(Collision.Complex);
	EXPECT_EQ(Collision.CacheDiagnostics.Read.Code, EAssetCacheError::Read);
	EXPECT_EQ(Collision.CacheWriteNanoseconds, 0u);
	EXPECT_TRUE(Error.empty());
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision)) << Error;
	EXPECT_TRUE(Collision.Complex);
	EXPECT_EQ(Collision.CacheDiagnostics.Read.Code, EAssetCacheError::Read);
	EXPECT_LE(Durin::FormatStaticMeshCacheDiagnostics(Collision.CacheDiagnostics).size(), 2048u);
	EXPECT_EQ(Collision.CacheDiagnostics.Write.Code, EAssetCacheError::Write);
	ASSERT_NE(Collision.CacheDiagnostics.Write.WriteCause, nullptr);
	EXPECT_EQ(Collision.CacheDiagnostics.Write.WriteCause->Code, DerivedData::ECacheError::StorageFailure);
	FPaths::SetDerivedDataCacheDirForTests(Fixture.CacheRoot.generic_string());
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshDerivedDataCacheTests, InvalidDetachedReplacementInvalidatesLiveData)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshReplacementFailure");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const FXxHash128 ImportedIdentity = Fixture.Mesh->GetSource().GetIdentity();
	const uint64 Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	FStaticMeshBuildResult Result;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData({
		.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.Source = Fixture.Mesh->GetSource()}, Result)) << Error;
	ASSERT_FALSE(Result.RenderData->LODResources.empty());
	Result.RenderData->LODResources.front().IndexBuffer.GetMutableIndices().front() =
		std::numeric_limits<uint32>::max();
	const auto Applied = ApplyStaticMeshBuildResult(*Fixture.Mesh, Fixture.Mesh->GetSource(), std::move(Result));
	EXPECT_EQ(Applied.error().Code, EStaticMeshDirectBuildError::Render);
	ASSERT_TRUE(Applied.error().RenderCause);
	EXPECT_EQ(Applied.error().RenderCause->Code, EStaticMeshReplacementError::Payload);
	ASSERT_TRUE(Applied.error().RenderCause->PayloadCause);
	EXPECT_EQ(Applied.error().RenderCause->PayloadCause->Code, EStaticMeshPayloadError::IndexRange);
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetIdentity(), ImportedIdentity);
	EXPECT_GT(Fixture.Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_TRUE(Fixture.Mesh->GetPackage()->IsDirty());
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

	{
		auto LoadedValue = Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
		Fixture.Mesh = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
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
	{
		auto LoadedValue = Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
		Fixture.Mesh = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
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
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh));
	const std::string SourceChangedKey = GetStaticMeshKey(*Fixture.Mesh);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(SourceChangedKey, InitialKey);

	auto* ImportData = dynamic_cast<Durin::AssetForge::Builtins::DStaticMeshImportData*>(
		Fixture.Mesh->GetAssetImportData());
	ASSERT_NE(ImportData, nullptr);
	auto State = ImportData->GetStaticMeshState();
	State.ImportSettings = Durin::FStaticMeshImportSettings::MakeYUpNegativeZForward();
	State.SourceData.Normalize();
	ASSERT_TRUE(State.Validate());
	ImportData->SetState(std::move(State));
	ASSERT_TRUE(Durin::AssetForge::Builtins::ReimportStaticMesh(
		*Fixture.Mesh));
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
	auto PackageBytesBeforeRecoveryRead = Durin::FFileHelper::LoadFileToArray(PackagePath);
	ASSERT_TRUE(PackageBytesBeforeRecoveryRead) << PackageBytesBeforeRecoveryRead.error().ToString();
	PackageBytesBeforeRecovery = std::move(*PackageBytesBeforeRecoveryRead);
	const auto PackageTimeBeforeRecovery =
		std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
	std::filesystem::last_write_time(PackagePath, PackageTimeBeforeRecovery);

	{
		auto LoadedValue = Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
		Fixture.Mesh = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	Durin::FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	EXPECT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	const Durin::FStaticMeshRenderData* CompleteRenderData = Fixture.Mesh->GetRenderData();
	ASSERT_NE(CompleteRenderData, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	Durin::FByteBuffer PackageBytesAfterRecovery;
	auto PackageBytesAfterRecoveryRead = Durin::FFileHelper::LoadFileToArray(PackagePath);
	ASSERT_TRUE(PackageBytesAfterRecoveryRead) << PackageBytesAfterRecoveryRead.error().ToString();
	PackageBytesAfterRecovery = std::move(*PackageBytesAfterRecoveryRead);
	EXPECT_EQ(PackageBytesAfterRecovery, PackageBytesBeforeRecovery);
	EXPECT_EQ(std::filesystem::last_write_time(PackagePath), PackageTimeBeforeRecovery);

	const std::filesystem::path BlockedCacheRoot = Fixture.Root / "BlockedCacheRoot";
	ASSERT_TRUE(Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Corrupt)), BlockedCacheRoot));
	Durin::FPaths::SetDerivedDataCacheDirForTests(BlockedCacheRoot.generic_string());
	std::string Error;
	Fixture.Mesh->PostLoad();
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
	Fixture.Mesh->SetCollisionSourceMode(Durin::EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_EQ(Fixture.Mesh->GetCollisionBuildStatus(), Durin::EStaticMeshCollisionBuildStatus::Ready)
		<< Durin::FormatStaticMeshCollisionError(Fixture.Mesh->GetCollisionBuildError());
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
		Durin::FCookContext Context(Durin::ECookTargetPlatform::Win64,
			Durin::ECookTargetProfile::Game);
		ASSERT_TRUE(Durin::ContributeEngineCookAsset(
			*Fixture.Mesh, "/Game/CookedCollisionMesh", Context)) << Error;
		ASSERT_TRUE(Durin::PublishCookContext(Context, Root)) << Error;
	}
	Durin::FByteBuffer FirstPackage, SecondPackage, SecondBulk, FirstManifest, SecondManifest;
	auto FirstPackageRead = Durin::FFileHelper::LoadFileToArray((CookRoot / "Game/CookedCollisionMesh.dasset"));
	ASSERT_TRUE(FirstPackageRead) << FirstPackageRead.error().ToString();
	FirstPackage = std::move(*FirstPackageRead);
	auto SecondPackageRead = Durin::FFileHelper::LoadFileToArray((SecondCookRoot / "Game/CookedCollisionMesh.dasset"));
	ASSERT_TRUE(SecondPackageRead) << SecondPackageRead.error().ToString();
	SecondPackage = std::move(*SecondPackageRead);
	auto FirstManifestRead = Durin::FFileHelper::LoadFileToArray((CookRoot / "CookManifest.bin"));
	ASSERT_TRUE(FirstManifestRead) << FirstManifestRead.error().ToString();
	FirstManifest = std::move(*FirstManifestRead);
	auto SecondManifestRead = Durin::FFileHelper::LoadFileToArray((SecondCookRoot / "CookManifest.bin"));
	ASSERT_TRUE(SecondManifestRead) << SecondManifestRead.error().ToString();
	SecondManifest = std::move(*SecondManifestRead);
	EXPECT_EQ(FirstPackage, SecondPackage);
	EXPECT_EQ(FirstManifest, SecondManifest);
	auto FirstBulk = Durin::FFileHelper::LoadFileToArray(CookRoot / "Game/CookedCollisionMesh.dbulk");
	if (!FirstBulk)
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
		const auto Loaded =
			Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
				Path, Fixture.AssetPath.GetPackageName()));
			CookedMesh = Loaded.value_or(nullptr);
		ASSERT_TRUE(Loaded) << (Loaded ? std::string{} : Loaded.error().Message);
		const Durin::FCookedMeshBlockingResult LoadResult =
			CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(LoadResult) << Durin::FormatCookedMeshLoadError(LoadResult.Error);
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
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", First)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(First, CookRoot)) << Error;

	Durin::FCookContext Second(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::ContributeEngineCookAsset(
		*Fixture.Mesh, "/Game/CookedMesh", Second)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Second, SecondCookRoot)) << Error;
	Durin::FByteBuffer FirstPackage;
	Durin::FByteBuffer SecondPackage;
	Durin::FByteBuffer SecondBulk;
	auto FirstPackageRead = Durin::FFileHelper::LoadFileToArray((CookRoot / "Game/CookedMesh.dasset"));
	ASSERT_TRUE(FirstPackageRead) << FirstPackageRead.error().ToString();
	FirstPackage = std::move(*FirstPackageRead);
	auto SecondPackageRead = Durin::FFileHelper::LoadFileToArray((SecondCookRoot / "Game/CookedMesh.dasset"));
	ASSERT_TRUE(SecondPackageRead) << SecondPackageRead.error().ToString();
	SecondPackage = std::move(*SecondPackageRead);
	EXPECT_EQ(FirstPackage, SecondPackage);
	auto FirstBulk = Durin::FFileHelper::LoadFileToArray(CookRoot / "Game/CookedMesh.dbulk");
	if (!FirstBulk)
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
		const auto Loaded =
			Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakeTopLevelAssetObjectPathForTests(
				Path, Fixture.AssetPath.GetPackageName()));
			CookedMesh = Loaded.value_or(nullptr);
		ASSERT_TRUE(Loaded) << (Loaded ? std::string{} : Loaded.error().Message);
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
		for (int Poll = 0; Poll < 10; ++Poll)
		{
			const auto Snapshot = Durin::InspectStaticMeshPayloads(*CookedMesh);
			EXPECT_EQ(Snapshot.CookedLoad.CpuPhase, CancelledStatus.CpuPhase);
			EXPECT_EQ(Snapshot.CookedLoad.Generation, CancelledStatus.Generation);
			EXPECT_FALSE(Snapshot.bCpuResident);
		}
		ASSERT_TRUE(Durin::InitializeCookedMeshLoadManager());
		const Durin::FCookedMeshBlockingResult RetryResult =
			CookedMesh->EnsureRenderDataLoadedBlocking();
		ASSERT_TRUE(RetryResult) << Durin::FormatCookedMeshLoadError(RetryResult.Error);
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
		EXPECT_FALSE(CookedMesh->GetSource().IsValid());
		EXPECT_FALSE(CookedMesh->GetSource().IsGeometryResident());
		EXPECT_NE(CookedMesh->GetCookedRenderData().GetMetadata().LogicalSize, 0u);
		ASSERT_TRUE(Durin::UnloadPackage(Path));
		ASSERT_TRUE(AssetRuntime.Restore());
		return;
	}
}

TEST(FStaticMeshSourceResidencyTests, SmallGeometryPreservesIdentityAndResidency)
{
	CheckSourceResidency(false);
}

TEST(FStaticMeshAuthoredCompilationTests, SmallCandidateBuildsRenderRayAndCollisionProducts)
{
	CheckCandidateBudgets(false);
}

namespace
{
	// Exercises the persisted field boundary without exposing mutable bulk in the source API.
	auto GetReflectedSourceBulk(Durin::FStaticMeshSource& Source) -> Durin::FEditorBulkData&
	{
		auto* Property = Durin::FStaticMeshSource::StaticStruct()->FindPropertyByName("Geometry");
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
			if (bFail) return std::unexpected(Durin::FPackageResourceReadError{.Status = Durin::EPackageResourceReadStatus::IoError,
				.Reason = Durin::EPackageResourceReadReason::Open});
			return Bytes.MakeView(Offset, Size);
		}
		Durin::FSharedByteBuffer Bytes;
		bool bFail;
	};

	// Models large package metadata without allocating or decoding the advertised payload.
	class FReservationSourceResource final : public Durin::FPackageResource
	{
	public:
		explicit FReservationSourceResource(uint64 Size) : FPackageResource(Size) {}

	private:
		auto ReadRangeImpl(uint64, uint64, const std::atomic_bool&)
			-> Durin::FPackageResourceReadResult override
		{
			ADD_FAILURE() << "Admission and pre-build cancellation must not read source payloads.";
			return std::unexpected(Durin::FPackageResourceReadError{.Status = Durin::EPackageResourceReadStatus::IoError});
		}
	};

	auto AttachReservationSource(Durin::FStaticMeshSource& Source, uint64 Size)
		-> std::shared_ptr<FReservationSourceResource>
	{
		auto Resource = std::make_shared<FReservationSourceResource>(Size);
		const auto& Bulk = Source.GetGeometryBulk();
		Durin::FEditorBulkData Attached;
		std::string Error;
		{
			auto ValueResult = Durin::FEditorBulkData::TryCreatePackageBacked(Bulk.GetInstanceId(), Bulk.GetPayloadId(), Size, {.Resource = Resource, .StoredSize = Size});
			if (!ValueResult)
			{
				ADD_FAILURE() << Durin::FormatEditorBulkDataError(ValueResult.error());
				return {};
			}
			Attached = std::move(*ValueResult);
		}
		Source.ReleaseGeometry();
		GetReflectedSourceBulk(Source) = std::move(Attached);
		return Resource;
	}

	auto AttachResidencyProbe(Durin::FStaticMeshSource& Source, bool bFail = false)
		-> std::shared_ptr<FResidencyReadProbe>
	{
		const auto& Bulk = Source.GetGeometryBulk();
		auto Resource = std::make_shared<FResidencyReadProbe>(Bulk.GetPayload().Wait().value(), bFail);
		Durin::FEditorBulkData Attached;
		std::string Error;
		{
			auto ValueResult = Durin::FEditorBulkData::TryCreatePackageBacked(Bulk.GetInstanceId(), Bulk.GetPayloadId(), Bulk.GetPayloadSize(), {.Resource = Resource, .StoredSize = Bulk.GetPayloadSize()});
			if (!ValueResult)
			{
				ADD_FAILURE() << Durin::FormatEditorBulkDataError(ValueResult.error());
				return {};
			}
			Attached = std::move(*ValueResult);
		}
		Source.ReleaseGeometry();
		GetReflectedSourceBulk(Source) = std::move(Attached);
		return Resource;
	}
}

// The provider returns owned CPU values that outlive the borrowed source and invocation.
TEST(FStaticMeshBuildProviderTests, ReturnsDetachedCPUStreamsAndDiscardsCancelledProduct)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	auto Source = std::make_shared<const FStaticMeshDecodedGeometry>(MakeResidencyGeometry());
	std::string Error;
	const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
		[&](IStaticMeshBuildProvider& Provider) {
			return Provider.BuildRender({.Geometry = Source, .NormalizedSize = 2.0f});
		});
	ASSERT_EQ(Invocation.Status, EFeatureInvokeStatus::Invoked);
	ASSERT_TRUE(Invocation.Value.has_value());
	ASSERT_TRUE(*Invocation.Value);
	const auto& Product = **Invocation.Value;
	Source.reset();
	ASSERT_EQ(Product.LODs.size(), 1u);
	const auto& LOD = Product.LODs.front();
	ASSERT_EQ(LOD.Positions.size(), 3u);
	EXPECT_EQ(LOD.Positions[0], FVector3f(-1, -1, 0));
	EXPECT_EQ(LOD.Positions[1], FVector3f(1, -1, 0));
	EXPECT_EQ(LOD.Positions[2], FVector3f(-1, 1, 0));
	EXPECT_EQ(LOD.Indices, (std::vector<uint32>{0, 1, 2}));
	EXPECT_EQ(LOD.Normals.size(), 3u);
	EXPECT_EQ(LOD.Tangents.size(), 3u);
	EXPECT_EQ(LOD.Colors, std::vector<FVector4f>(3, FVector4f(1.0f)));
	for (const auto& Channel : LOD.TexCoords)
		EXPECT_EQ(Channel, std::vector<FVector2f>(3, FVector2f(0.0f)));
	EXPECT_EQ(LOD.NumTexCoords, 0u);
	EXPECT_FALSE(LOD.bHasColorVertexData);
	EXPECT_EQ(LOD.ScreenSize, 0.0f);
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].Name, "Fixture");
	EXPECT_EQ(LOD.Sections[0].LocalBounds.Min, FVector3(-1, -1, 0));
	EXPECT_EQ(Product.LocalBounds.Max, FVector3(1, 1, 0));
	ASSERT_EQ(Product.MaterialSlots.size(), 1u);
	EXPECT_EQ(Product.MaterialSlots[0].Name, FName("Material"));
	const auto Cancelled = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
		[&](IStaticMeshBuildProvider& Provider) {
			return Provider.BuildRender({}, {.ShouldCancel = [] { return true; }});
		});
	ASSERT_EQ(Cancelled.Status, EFeatureInvokeStatus::Invoked);
	ASSERT_TRUE(Cancelled.Value.has_value());
	ASSERT_FALSE(*Cancelled.Value);
	EXPECT_EQ(Cancelled.Value->error().Code, EStaticMeshRecipeError::Cancelled);
	EXPECT_EQ(Product.LODs.size(), 1u);
	EXPECT_EQ(Product.MaterialSlots.size(), 1u);
}

TEST(FStaticMeshSourceResidencyTests, SharesConcurrentReadsAndSurvivesReleaseCopyAndReplacement)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
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
			const auto Read = Source.AcquireGeometry();
			EXPECT_TRUE(Read);
			Handle = *Read;
		});
	Readers.clear();
	ASSERT_TRUE(Handles.front());
	for (const auto& Handle : Handles) EXPECT_EQ(Handle, Handles.front());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	FStaticMeshSource Copy = Source;
	Source.ReleaseGeometry();
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_EQ(Copy.AcquireGeometry().value(), Handles.front());
	const auto Reload = Source.AcquireGeometry().value();
	ASSERT_TRUE(Reload) << Error;
	EXPECT_NE(Reload, Handles.front());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 2u);
	EXPECT_EQ(Source.GetIdentity(), Identity);
	std::weak_ptr<const FStaticMeshDecodedGeometry> Weak = Handles.front();
	Handles.fill({});
	EXPECT_FALSE(Weak.expired());
	Copy.ReleaseGeometry();
	EXPECT_TRUE(Weak.expired());
	const auto Old = Source.AcquireGeometry().value();
	auto Replacement = MakeResidencyGeometry();
	Replacement.Meshes.front().Positions.front().x = 7;
	ASSERT_TRUE(Source.Initialize(std::move(Replacement)));
	EXPECT_NE(Source.GetIdentity(), Identity);
	EXPECT_EQ(Old->Meshes.front().Positions.front().x, 0);
	EXPECT_EQ(Source.AcquireGeometry().value()->Meshes.front().Positions.front().x, 7);
	std::jthread Releaser([&] { for (int Index = 0; Index < 32; ++Index) Source.ReleaseGeometry(); });
	for (int Index = 0; Index < 32; ++Index)
	{
		const auto Handle = Source.AcquireGeometry().value();
		ASSERT_TRUE(Handle) << Error;
		EXPECT_EQ(Handle->Meshes.front().Positions.front().x, 7);
	}
}

TEST(FStaticMeshSourceResidencyTests, InvalidCompleteInitializationPreservesIdentityAndReaders)
{
	using namespace Durin;
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto Original = Source.AcquireGeometry().value();
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
		const auto Rejected = Source.Initialize(std::move(Invalid));
		EXPECT_FALSE(Rejected);
		EXPECT_EQ(Rejected.error().Code, Case == 0 ? EStaticMeshSourceError::IndexRange
			: Case == 5 ? EStaticMeshSourceError::Limit : Case == 6 ? EStaticMeshSourceError::Counts
			: EStaticMeshSourceError::ChannelLength);
		if (Case == 5)
		{
			EXPECT_EQ(Rejected.error().Field, "MeshName");
			EXPECT_EQ(Rejected.error().Actual, 4097u);
			EXPECT_EQ(Rejected.error().Expected, 4096u);
		}
		EXPECT_EQ(Source.GetIdentity(), Identity);
		EXPECT_EQ(Source.AcquireGeometry().value(), Original);
	}
}

TEST(FStaticMeshSourceResidencyTests, MalformedCanonicalBytesNeverPublishPartialResidency)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto Payload = Source.GetGeometryBulk().GetPayload().Wait();
	const FByteBuffer Original(Payload->begin(), Payload->end());
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
		const auto Rejected = Source.AcquireGeometry();
		EXPECT_FALSE(Rejected);
		EXPECT_EQ(Rejected.error().Code, Case == 3 ? EStaticMeshSourceError::IndexRange
			: Case == 4 ? EStaticMeshSourceError::ChannelLength : EStaticMeshSourceError::Archive);
		if (Case == 3)
		{
			EXPECT_EQ(Rejected.error().Actual, 3u);
			EXPECT_EQ(Rejected.error().Expected, 3u);
			EXPECT_EQ(Rejected.error().Index, 2u);
		}
		if (Case == 4)
		{
			EXPECT_EQ(Rejected.error().Field, "Normals");
			EXPECT_EQ(Rejected.error().Actual, 1u);
			EXPECT_EQ(Rejected.error().Expected, 3u);
		}
		EXPECT_FALSE(Source.IsGeometryResident());
	}
	ASSERT_TRUE(GetReflectedSourceBulk(Source).UpdatePayload(Original));
	ASSERT_TRUE(Source.AcquireGeometry().value()) << Error;
}

TEST(FStaticMeshSourceResidencyTests, WarmCacheSkipsUnreadableBulkAndMissPreservesReadDiagnostic)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	const auto Fixture = ImportCacheFixture("StaticMeshUnreadableResidency");
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	FStaticMeshBuildRequest Request{.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.Source = Fixture.Mesh->GetSource()};
	const auto Resource = AttachResidencyProbe(Request.Source, true);
	FStaticMeshBuildResult Product;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Product)) << Error;
	EXPECT_EQ(Product.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	ASSERT_TRUE(std::filesystem::remove(GetObjectPath(Fixture, GetStaticMeshKey(*Fixture.Mesh))));
	const auto FailedRead = BuildStaticMeshDerivedData(Request, Product);
	EXPECT_FALSE(FailedRead);
	EXPECT_EQ(FailedRead.error().Code, EStaticMeshDerivedDataError::Source);
	ASSERT_TRUE(FailedRead.error().SourceCause);
	ASSERT_TRUE(FailedRead.error().SourceCause->ReadCause);
	EXPECT_EQ(FailedRead.error().SourceCause->ReadCause->error().Status, EPackageResourceReadStatus::IoError);
	EXPECT_EQ(FailedRead.error().SourceCause->ReadCause->error().Reason, EPackageResourceReadReason::Open);
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	EXPECT_FALSE(Request.Source.IsGeometryResident());
	EXPECT_FALSE(BuildStaticMeshDerivedData(Request, Product));
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 2u);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshSourceVersionTests, AuthoredLoadRequiresFileVersionAndPreservesSourceIdentity)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshSourceVersion");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const auto Identity = Fixture.Mesh->GetSource().GetIdentity();
	const auto Key = GetStaticMeshKey(*Fixture.Mesh);
	FByteBuffer Original;
	const auto File = Fixture.Root / "Content/Mesh.dasset";
	auto OriginalRead = FFileHelper::LoadFileToArray(File);
	ASSERT_TRUE(OriginalRead) << OriginalRead.error().ToString();
	Original = std::move(*OriginalRead);
	ObjectPackage::FLinkerTables Saved;
	ASSERT_TRUE(ObjectPackage::ReadPackage(Original, {}, Fixture.AssetPath, Saved));
	ASSERT_EQ(Saved.CustomVersions, (std::vector<FCustomVersion>{{FStaticMeshSourceVersion::Guid, FStaticMeshSourceVersion::CurrentVersion}}));
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
	for (const int32 Version : {-1, 0, 2})
	{
		auto Candidate = Saved;
		if (Version < 0) Candidate.CustomVersions.clear();
		else Candidate.CustomVersions.front().Version = Version;
		FByteBuffer Bytes, Bulk;
		ASSERT_TRUE(ObjectPackage::WritePackage(Candidate, Bytes, Bulk));
		ASSERT_TRUE(Bulk.empty());
		ASSERT_TRUE(FFileHelper::SaveArrayToFile(Bytes, File));
		DStaticMesh* Loaded = nullptr;
		const auto Result = LoadObject<Durin::DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
		Loaded = Result.value_or(nullptr);
		EXPECT_EQ((Result ? Durin::EAssetReadError::None : Result.error().Code), EAssetReadError::UnsupportedVersion) << (Result ? std::string{} : Result.error().Message);
		EXPECT_EQ(Loaded, nullptr);
		EXPECT_EQ(FindResidentPackage(Fixture.AssetPath), nullptr);
		FByteBuffer Unchanged;
		auto UnchangedRead = FFileHelper::LoadFileToArray(File);
		ASSERT_TRUE(UnchangedRead) << UnchangedRead.error().ToString();
		Unchanged = std::move(*UnchangedRead);
		EXPECT_EQ(Unchanged, Bytes);
	}
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Original, File));
	DStaticMesh* Loaded = nullptr;
	const auto Result = LoadObject<Durin::DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
	Loaded = Result.value_or(nullptr);
	ASSERT_TRUE(Result) << (Result ? std::string{} : Result.error().Message);
	EXPECT_EQ(Loaded->GetSource().GetIdentity(), Identity);
	EXPECT_EQ(GetStaticMeshKey(*Loaded), Key);
	std::string Error;
	ASSERT_TRUE(Loaded->GetSource().AcquireGeometry().value()) << Error;
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshSourceVersionTests, AuthoredSourceLoadsAfterRestart)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshSourceRestart");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const auto Identity = Fixture.Mesh->GetSource().GetIdentity();
	const auto Key = GetStaticMeshKey(*Fixture.Mesh);
	ASSERT_TRUE(SavePackage(Fixture.Mesh->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
	Fixture.Mesh = nullptr;
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
	const auto Scan = RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation);
	ASSERT_TRUE(Scan) << (Scan.Errors.empty() ? "Incomplete registry" : Durin::FormatAssetRegistryError(Scan.Errors.front()));
	DStaticMesh* Mesh = nullptr;
	const auto Result = LoadObject<Durin::DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
	Mesh = Result.value_or(nullptr);
	ASSERT_TRUE(Result) << (Result ? std::string{} : Result.error().Message);
	ASSERT_NE(Mesh, nullptr);
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), Identity);
	EXPECT_EQ(GetStaticMeshKey(*Mesh), Key);
	std::string Error;
	ASSERT_TRUE(Mesh->GetSource().AcquireGeometry().value()) << Error;
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
	ShutdownAssetManager();
	CollectGarbage();
	ASSERT_TRUE(InitializeAssetManager());
}

TEST(FStaticMeshSourceResidencyTests, ExistingAuthoredPackageAndDuplicateRetainCanonicalSource)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Testing::FScopedMountRegistryFixture MountRegistry;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshSourceDuplicate");
	ASSERT_NE(Fixture.Mesh, nullptr);
	ASSERT_TRUE(SavePackage(Fixture.Mesh->GetPackage()));
	const auto Path = Fixture.AssetPath;
	ASSERT_TRUE(UnloadPackage(Path));
	Fixture.Mesh = nullptr;
	DStaticMesh* Mesh = nullptr;
	{
		auto LoadedValue = LoadObject<Durin::DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Path));
		Mesh = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_NE(Mesh, nullptr);
	std::string Error;
	const auto Geometry = Mesh->GetSource().AcquireGeometry().value();
	ASSERT_TRUE(Geometry) << Error;
	auto* Duplicate = Cast<DStaticMesh>(DuplicateObject(Mesh, nullptr, "ResidencyDuplicate").value());
	ASSERT_NE(Duplicate, nullptr);
	EXPECT_EQ(Duplicate->GetSource().GetIdentity(), Mesh->GetSource().GetIdentity());
	Mesh->GetSource().ReleaseGeometry();
	const auto Copied = Duplicate->GetSource().AcquireGeometry().value();
	ASSERT_TRUE(Copied) << Error;
	EXPECT_EQ(Copied->Meshes.size(), Geometry->Meshes.size());
	EXPECT_EQ(Copied->Meshes.front().Indices, Geometry->Meshes.front().Indices);
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationIsTypedAndDiscardsProducts)
{
	using namespace Durin;
	FStaticMeshBuildResult Render;
	Render.RenderData = std::make_unique<FStaticMeshRenderData>();
	std::string Error;
	const auto Cancelled = BuildStaticMeshDerivedData({}, Render,
		{.ShouldCancel = [] { return true; }});
	ASSERT_FALSE(Cancelled);
	EXPECT_EQ(Cancelled.error().Code, EStaticMeshDerivedDataError::Cancelled);
	EXPECT_EQ(Render.RenderData, nullptr);

	const auto Invalid = BuildStaticMeshDerivedData({}, Render);
	ASSERT_FALSE(Invalid);
	EXPECT_NE(Invalid.error().Code, EStaticMeshDerivedDataError::Cancelled);
	EXPECT_NE(Invalid.error().Code, EStaticMeshDerivedDataError::None);

	FStaticMeshCollisionBuildResult Collision;
	FStaticMeshRenderData EmptyRender;
	uint32 Checks = 0;
	const auto LateCancellation = BuildStaticMeshCollisionDerivedData(EmptyRender,
		EBodySetupCollisionSourceMode::None,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, false,
		{.ShouldCancel = [&] { return ++Checks == 2; }});
	ASSERT_FALSE(LateCancellation);
	EXPECT_EQ(LateCancellation.error().Code, EStaticMeshDerivedDataError::Cancelled);
	EXPECT_FALSE(Collision.Simple);
	EXPECT_FALSE(Collision.Complex);
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
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Input)));
	for (const uint64 StopAt : {8ull, 32ull, 128ull})
	{
		FStaticMeshBuildExecutionMetrics Metrics;
		FStaticMeshBuildResult Product;
		const auto Outcome = BuildStaticMeshDerivedData(
			{.Source = Source, .bPersistDerivedData = false}, Product,
			{.ShouldCancel = [&] { return Metrics.CancellationCheckpoints >= StopAt; },
				.Metrics = &Metrics});
		ASSERT_FALSE(Outcome) << StopAt;
		EXPECT_EQ(Outcome.error().Code, EStaticMeshDerivedDataError::Cancelled) << StopAt;
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
	const auto SynchronousBuild1 = BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry());
	ASSERT_TRUE(SynchronousBuild1) << Durin::FormatStaticMeshSynchronousError(SynchronousBuild1.error());
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Mesh->GetCollisionBuildStatus(), Durin::EStaticMeshCollisionBuildStatus::Failed)
		<< Durin::FormatStaticMeshCollisionError(Mesh->GetCollisionBuildError());
	const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
	auto Built = BuildStaticMeshAuthoredCandidate(MakeStaticMeshAuthoredBuildRequest(
		Mesh->GetSource(), Snapshot));
	ASSERT_TRUE(Built) << FormatStaticMeshAuthoredBuildError(Built.error());
	auto Candidate = std::move(*Built);
	const auto Ray = Candidate->GetRenderData()->LODResources.front().RayQueryAcceleration;
	const auto CollisionIdentity = Candidate->GetCollision().Complex.GetIdentity();
	ASSERT_NE(Ray, nullptr);
	ASSERT_NE(CollisionIdentity, 0u);
	FScopedStaticMeshProviderRestore RestoreProvider;
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
	const auto Start = std::chrono::steady_clock::now();
	ASSERT_TRUE(ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Candidate), Snapshot)) << Error;
	const auto Duration = std::chrono::steady_clock::now() - Start;
	EXPECT_EQ(Mesh->GetRenderData()->LODResources.front().RayQueryAcceleration, Ray);
	FCollisionGeometryRef PublishedCollision;
	ASSERT_TRUE(Mesh->GetBodySetup()->BuildComplexGeometry(PublishedCollision));
	EXPECT_EQ(PublishedCollision.GetIdentity(), CollisionIdentity);
	EXPECT_FALSE(Mesh->GetSource().IsGeometryResident());
	std::cout << "authored_candidate_application_ns="
		<< std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count() << std::endl;
}

TEST(FStaticMeshAuthoredCompilationTests, CancelledAndStaleCandidatesPreserveLiveState)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("AuthoredCandidateTest"));
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry()));
	for (const uint32 Scenario : {0u, 1u, 2u})
	{
		const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
		auto Built = BuildStaticMeshAuthoredCandidate(MakeStaticMeshAuthoredBuildRequest(
			Mesh->GetSource(), Snapshot));
		ASSERT_TRUE(Built) << FormatStaticMeshAuthoredBuildError(Built.error());
		auto Candidate = std::move(*Built);
		if (Scenario == 1) ASSERT_TRUE(Mesh->RenameMaterialSlot(0, FName("Changed")));
		if (Scenario == 2) Mesh->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::SimpleOnly);
		const auto* Original = Mesh->GetRenderData();
		const auto SourceIdentity = Mesh->GetSource().GetIdentity();
		const auto Revision = Mesh->GetRenderResourceStatus().Revision;
		const auto Outcome = ApplyStaticMeshAuthoredCandidate(*Mesh, std::move(Candidate), Snapshot,
			true, {.ShouldCancel = [Scenario] { return Scenario == 0; }});
		ASSERT_FALSE(Outcome);
		EXPECT_EQ(Outcome.error().Owner, FObjectKey(Mesh));
		EXPECT_EQ(Outcome.error().Code, Scenario == 0 ? EStaticMeshApplicationError::Cancelled
			: Scenario == 1 ? EStaticMeshApplicationError::MaterialBindings : EStaticMeshApplicationError::OwnerChanged);
		if (Scenario == 1)
		{
			ASSERT_TRUE(Outcome.error().ExpectedSlot);
			ASSERT_TRUE(Outcome.error().CurrentSlot);
			ASSERT_TRUE(Outcome.error().InputSlot);
			EXPECT_EQ(Outcome.error().SlotIndex, 0u);
			EXPECT_EQ(Outcome.error().ExpectedSlot->Name, Snapshot.MaterialSlots.front().Name.ToString());
			EXPECT_EQ(Outcome.error().CurrentSlot->Name, "Changed");
			EXPECT_EQ(Outcome.error().InputSlot->Name, Outcome.error().ExpectedSlot->Name);
		}
		if (Scenario == 2)
		{
			ASSERT_TRUE(Outcome.error().Expected);
			ASSERT_TRUE(Outcome.error().Current);
			EXPECT_EQ(Outcome.error().Expected->CollisionPolicy, Snapshot.CollisionPolicy);
			EXPECT_EQ(Outcome.error().Current->CollisionPolicy, EBodySetupCollisionQueryPolicy::SimpleOnly);
		}
		EXPECT_EQ(Mesh->GetRenderData(), Original);
		EXPECT_EQ(Mesh->GetSource().GetIdentity(), SourceIdentity);
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Revision);
		if (Scenario == 1)
		{
			ASSERT_TRUE(Mesh->RenameMaterialSlot(0, FName("AfterFailure")));
			EXPECT_EQ(Outcome.error().CurrentSlot->Name, "Changed");
		}
	}
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationAbandonsDecodeAndRayScratch)
{
	using namespace Durin;
	auto Geometry = MakeResidencyGeometry();
	for (uint32 Index = 0; Index < 4096; ++Index)
		Geometry.Meshes.front().Indices.insert(Geometry.Meshes.front().Indices.end(), {0, 1, 2});
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	Source.ReleaseGeometry();
	uint32 Checks = 0;
	const auto CancelledRead = Source.AcquireGeometry([&] { return ++Checks == 8; });
	EXPECT_FALSE(CancelledRead);
	EXPECT_EQ(CancelledRead.error().Code, EStaticMeshSourceError::Cancelled);
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_EQ(Checks, 8u);
	ASSERT_TRUE(Source.AcquireGeometry().value()) << Error;
	FStaticMeshBuildResult Render;
	ASSERT_TRUE(BuildStaticMeshDerivedData({.Source = Source}, Render));
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
		auto BuildRender(const Durin::FStaticMeshRecipeBuildRequest&,
			const Durin::FStaticMeshBuildExecutionControl&) -> std::expected<Durin::FStaticMeshRecipeBuildProduct, Durin::FStaticMeshRecipeError> override
		{
			ADD_FAILURE();
			return std::unexpected(Durin::FStaticMeshRecipeError{.Code = Durin::EStaticMeshRecipeError::MissingGeometry});
		}
		auto BuildCollision(const Durin::FStaticMeshCollisionRecipeRequest&,
			const Durin::FStaticMeshBuildExecutionControl&) -> std::expected<Durin::FStaticMeshCollisionRecipeProduct, Durin::FStaticMeshRecipeError> override
		{
			ADD_FAILURE();
			return std::unexpected(Durin::FStaticMeshRecipeError{.Code = Durin::EStaticMeshRecipeError::MissingGeometry});
		}
	};
}

TEST(FStaticMeshAuthoredCompilationTests, ProviderFailuresAndPersistenceDiagnosticsRemainDistinct)
{
	using namespace Durin;
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("SubmissionProviderFailure"));
	FStaticMeshBuildResult Render;
	{
		FModuleTestOwner Owner("StaticMeshAmbiguousProvider");
		FUnexpectedStaticMeshProvider Provider;
		auto Registration = Owner.RegisterFeature(Provider);
		ASSERT_TRUE(Registration.IsValid());
		const auto Ambiguous = BuildStaticMeshDerivedData({.Source = Source}, Render);
		EXPECT_EQ(Ambiguous.error().Code, EStaticMeshDerivedDataError::ProviderInvocation);
		EXPECT_EQ(Ambiguous.error().InvocationStatus, EFeatureInvokeStatus::Ambiguous);
		const auto Submitted = SubmitStaticMeshCompilation(*Mesh, {.Source = Source});
		EXPECT_EQ(Submitted.error().Code, EStaticMeshSubmissionError::Provider);
		EXPECT_EQ(Submitted.error().InvocationStatus, EFeatureInvokeStatus::Ambiguous);
		EXPECT_EQ(Render.RenderData, nullptr);
	}
	{
		FScopedStaticMeshProviderRestore Restore;
		ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
		const auto Unavailable = BuildStaticMeshDerivedData({.Source = Source}, Render);
		EXPECT_EQ(Unavailable.error().Code, EStaticMeshDerivedDataError::ProviderInvocation);
		EXPECT_EQ(Unavailable.error().InvocationStatus, EFeatureInvokeStatus::Unavailable);
		const auto Submitted = SubmitStaticMeshCompilation(*Mesh, {.Source = Source});
		EXPECT_EQ(Submitted.error().Code, EStaticMeshSubmissionError::Provider);
		EXPECT_EQ(Submitted.error().InvocationStatus, EFeatureInvokeStatus::Unavailable);
		EXPECT_EQ(Render.RenderData, nullptr);
	}
	FScopedDerivedDataCacheRestore RestoreCache;
	const auto CacheFile = Testing::GetTestWorkDirectory() / "StaticMeshBlockedCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	EXPECT_TRUE(BuildStaticMeshDerivedData({.Source = Source}, Render));
	EXPECT_NE(Render.RenderData, nullptr);
	EXPECT_EQ(Render.CacheDiagnostics.Read.Code, EAssetCacheError::Read);
	EXPECT_EQ(Render.CacheDiagnostics.Write.Code, EAssetCacheError::Write);
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
	for (uint32 Triangle = 0; Triangle < 512; ++Triangle)
	{
		const FVector3f Base(float(Triangle % 64), float(Triangle / 64), 0);
		Section.Positions.insert(Section.Positions.end(),
			{Base, Base + FVector3f(0.5f, 0, 0), Base + FVector3f(0, 0.5f, 0)});
		Section.Indices.insert(Section.Indices.end(), {Triangle * 3, Triangle * 3 + 1, Triangle * 3 + 2});
	}
	std::string Error;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(std::move(Input)));
	FStaticMeshBuildResult Render;
	ASSERT_TRUE(BuildStaticMeshDerivedData({.Source = Source}, Render)) << Error;
	FStaticMeshCollisionBuildResult Collision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Render.RenderData,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision)) << Error;
	FStaticMeshPayloadData Payload;
	ASSERT_TRUE(MakeStaticMeshPayloadData(*Render.RenderData, Payload));
	FStaticMeshCollisionPayloadData CollisionPayload;
	ASSERT_TRUE(MakeStaticMeshCollisionPayloadData(Collision.Complex,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, CollisionPayload));

	// Measure callback counts first, then interrupt each codec in the middle of its work.
	// A one-shot callback also verifies that a caught cancellation cannot become a cache miss.
	const auto CheckCodec = [&](auto& Value) {
		FByteBuffer Bytes;
		uint32 Count = 0;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
		Value.Serialize(Writer, [&] { ++Count; return false; });
		ASSERT_FALSE(Writer.IsError());
		ASSERT_GT(Count, 20u);
		const uint32 StopAt = Count / 2;
		Count = 0;
		FByteBuffer CancelledBytes;
		FCanonicalMemoryWriter CancelledWriter(CancelledBytes,
			EArchivePurpose::DerivedDataPayload,
			{.Target = {"Win64", "Game"}});
		Value.Serialize(CancelledWriter, [&] { return ++Count == StopAt; });
		EXPECT_TRUE(CancelledWriter.IsError());
		EXPECT_TRUE(CancelledBytes.empty());

		using TPayload = std::remove_cvref_t<decltype(Value)>;
		TPayload Decoded;
		Count = 0;
		FCanonicalMemoryReader Reader(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
		Decoded.Serialize(Reader, [&] { ++Count; return false; });
		ASSERT_FALSE(Reader.IsError());
		ASSERT_GT(Count, 20u);
		const uint32 ReadStopAt = Count / 2;
		Count = 0;
		TPayload CancelledDecoded;
		FCanonicalMemoryReader CancelledReader(Bytes,
			EArchivePurpose::DerivedDataPayload,
			{.Target = {"Win64", "Game"}});
		CancelledDecoded.Serialize(CancelledReader,
			[&] { return ++Count == ReadStopAt; });
		EXPECT_TRUE(CancelledReader.IsError());
		EXPECT_NE(CancelledReader.GetError().find("cancelled"), std::string_view::npos);
		// Ordinary loading may leave partial storage. The operation owns and
		// discards that unpublished candidate, then may retry into fresh storage.
		CancelledDecoded = {};
		FCanonicalMemoryReader Retry(Bytes, EArchivePurpose::DerivedDataPayload, {.Target = {"Win64", "Game"}});
		CancelledDecoded.Serialize(Retry);
		EXPECT_FALSE(Retry.IsError()) << Retry.GetError();
		EXPECT_TRUE(RequireArchiveEnd(Retry));
	};
	CheckCodec(Payload);
	CheckCodec(CollisionPayload);
	uint32 Checks = 0;
	FStaticMeshPayloadData Unchanged;
	Unchanged.MaterialSlotCount = 123;
	EXPECT_FALSE(MakeStaticMeshPayloadData(*Render.RenderData, Unchanged,
		[&] { return ++Checks == 8; }));
	EXPECT_EQ(Unchanged.MaterialSlotCount, 123u);
	EXPECT_TRUE(Unchanged.LODs.empty());
	Checks = 0;
	FCollisionGeometryRef UnchangedGeometry = Collision.Complex;
	EXPECT_FALSE(MakeStaticMeshCollisionGeometry(CollisionPayload, UnchangedGeometry,
		[&] { return ++Checks == 8; }));
	EXPECT_EQ(UnchangedGeometry.GetIdentity(), Collision.Complex.GetIdentity());
	Checks = 0;
	EXPECT_FALSE(Render.RenderData->RecalculateBounds([&] { return ++Checks == 8; }));
	EXPECT_EQ(Checks, 8u);
	Render.RenderData->RecalculateBounds();

	FStaticMeshBuildRequest CachedRequest;
	CachedRequest.Source = Source;
	CachedRequest.Reconciliation.MaterialSlots = Render.MaterialSlots;
	Checks = 0;
	const auto CancelledRender = BuildStaticMeshDerivedData(CachedRequest, Render,
		{.ShouldCancel = [&] { return ++Checks == 8; }});
	ASSERT_FALSE(CancelledRender);
	EXPECT_EQ(CancelledRender.error().Code, EStaticMeshDerivedDataError::Cancelled);
	EXPECT_FALSE(Render.RenderData);
	ASSERT_TRUE(BuildStaticMeshDerivedData(CachedRequest, Render));
	EXPECT_EQ(Render.Origin, EStaticMeshBuildOrigin::CacheHit);
	Checks = 0;
	const auto CancelledCollision = BuildStaticMeshCollisionDerivedData(*Render.RenderData,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, true,
		{.ShouldCancel = [&] { return ++Checks == 110; }});
	ASSERT_FALSE(CancelledCollision);
	EXPECT_EQ(CancelledCollision.error().Code, EStaticMeshDerivedDataError::Cancelled);
	EXPECT_FALSE(Collision.Complex);
}

TEST(FStaticMeshAuthoredCompilationTests, SmallCandidatePublishesAndDiscardsCheckpointCancellations)
{
	CheckCandidatePublicationAndCancellation(false);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerSupersedesOnceAndRetainsLateWorkers)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerSupersession"));
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	std::vector<EStaticMeshCompilationStatus> Terminals;
	const auto Completed = [&](const FStaticMeshCompilationDiagnostic& Value) { Terminals.push_back(Value.Status); };
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Completed)) << Error;
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}, Completed)) << Error;
	ASSERT_TRUE(Barrier.Wait(2));
	EXPECT_TRUE(Terminals.empty());
	const uint64 Reserved = GetStaticMeshCompilationManagerDiagnostics().ReservedBytes;
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	ASSERT_EQ(1u, Terminals.size());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, Terminals.front());
	EXPECT_EQ(2u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	EXPECT_EQ(Reserved, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	const auto InvalidSource = SubmitStaticMeshCompilation(*Mesh, {}, Completed);
	EXPECT_EQ(InvalidSource.error().Code, EStaticMeshSubmissionError::Source);
	EXPECT_EQ(InvalidSource.error().Owner, FObjectKey(Mesh));
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
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	uint32 Completions = 0;
	for (uint32 Index = 0; Index < 32; ++Index)
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source},
			[&](const FStaticMeshCompilationDiagnostic&) { ++Completions; })) << Error;
	EXPECT_EQ(0u, Completions);
	EXPECT_EQ(32u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	const auto Latest = InspectCompilationOperation(*Mesh).RequestId;
	const auto Rejected = SubmitStaticMeshCompilation(*Mesh, {.Source = Source});
	EXPECT_EQ(Rejected.error().Code, EStaticMeshSubmissionError::AdmissionBudget);
	EXPECT_EQ(Rejected.error().RecordCount, 32u);
	EXPECT_EQ(Rejected.error().RecordLimit, 32u);
	EXPECT_EQ(Rejected.error().ReservedBytes, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	EXPECT_GT(Rejected.error().RequestedBytes, 0u);
	EXPECT_EQ(Latest, InspectCompilationOperation(*Mesh).RequestId);
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
	ASSERT_TRUE(BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry()));
	const auto* Original = Mesh->GetRenderData();
	FStaticMeshWorkerBarrier Barrier;
	std::vector<EStaticMeshCompilationStatus> Terminals;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Mesh->GetSource()},
		[&](const auto& Value) { Terminals.push_back(Value.Status); }));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(Mesh->RenameMaterialSlot(0, "MutationWhileBuilding"));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	ASSERT_EQ(1u, Terminals.size());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, Terminals.front());
	EXPECT_EQ(Original, Mesh->GetRenderData());
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Mesh->GetSource()},
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
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FScopedStaticMeshProviderRestore Restore;
	FStaticMeshWorkerBarrier Barrier(EStaticMeshCompilationPhase::Mailbox);
	std::optional<EStaticMeshCompilationStatus> Terminal;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source},
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
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(Geometry));
	const auto SourceResource = AttachReservationSource(Source, 4800000);
	ASSERT_TRUE(Source.IsValid());
	FStaticMeshWorkerBarrier Barrier;
	uint32 Accepted = 0;
	while (SubmitStaticMeshCompilation(*Mesh, {.Source = Source})) ++Accepted;
	EXPECT_GT(Accepted, 2u);
	EXPECT_LT(Accepted, 32u);
	const auto State = GetStaticMeshCompilationManagerDiagnostics();
	EXPECT_EQ(Accepted, State.OutstandingRecords);
	EXPECT_LE(State.ReservedBytes, 1024ull * 1024 * 1024);
	const auto Latest = InspectCompilationOperation(*Mesh).RequestId;
	FStaticMeshSource Oversized;
	ASSERT_TRUE(Oversized.Initialize(std::move(Geometry)));
	const auto OversizedResource = AttachReservationSource(Oversized, 9600000);
	ASSERT_TRUE(Oversized.IsValid());
	const auto Rejected = SubmitStaticMeshCompilation(*Mesh, {.Source = Oversized});
	EXPECT_EQ(Rejected.error().Code, EStaticMeshSubmissionError::RequestBudget);
	ASSERT_TRUE(Rejected.error().MemoryCause);
	EXPECT_EQ(Rejected.error().MemoryCause->Limit, 512ull * 1024 * 1024);
	EXPECT_EQ(Rejected.error().MemoryCause->Bytes, 1024ull * 1024);
	EXPECT_EQ(Rejected.error().MemoryCause->RejectedCount, 9600000u);
	EXPECT_EQ(Rejected.error().MemoryCause->RejectedWidth, 64u);
	EXPECT_EQ(Latest, InspectCompilationOperation(*Mesh).RequestId);
	CancelStaticMeshCompilation(*Mesh);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
	EXPECT_FALSE(BuildStaticMeshAuthoredCandidate(MakeStaticMeshAuthoredBuildRequest(Source, {}),
		{.MaximumWorkingSetBytes = 1024}));
	EXPECT_EQ(SourceResource->GetReadStats().RequestCount, 0u);
	EXPECT_EQ(OversizedResource->GetReadStats().RequestCount, 0u);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerShutdownDrainsAndCanRestart)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerRestart"));
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::optional<EStaticMeshCompilationStatus> Terminal;
	{
		FStaticMeshWorkerBarrier Barrier;
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source},
			[&](const auto& Value) { Terminal = Value.Status; }));
		ASSERT_TRUE(Barrier.Wait(1));
		Barrier.Release();
		AssetPrivate::CreateStaticMeshCompilingManager()->Shutdown();
	}
	ASSERT_TRUE(Terminal.has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, *Terminal);
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	ASSERT_TRUE(AssetPrivate::CreateStaticMeshCompilingManager()->Start());
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Mesh).Status);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerFairnessAndHistoryStayBounded)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::vector<uint64> Dispatches;
	std::vector<DStaticMesh*> Meshes;
	for (uint32 Index = 0; Index < 8; ++Index)
		Meshes.push_back(NewObject<DStaticMesh>(nullptr, FName(std::format("ManagerFairness{}", Index))));
	{
		FStaticMeshWorkerBarrier Barrier;
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[0], {.Source = Source}));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[1], {.Source = Source}));
		ASSERT_TRUE(Barrier.Wait(2));
		for (uint32 Index = 2; Index < 7; ++Index)
			ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[Index],
				{.Source = Source, .Priority = EStaticMeshCompilationPriority::Interactive}));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[7], {.Source = Source}));
		const uint64 BackgroundId = InspectCompilationOperation(*Meshes[7]).RequestId;
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
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[0], {.Source = Source}));
		FAssetCompilingManager::Get().FinishCompilationForObject(*Meshes[0]);
	}
	const auto Diagnostics = GetStaticMeshCompilationManagerDiagnostics();
	EXPECT_EQ(128u, Diagnostics.RetainedDiagnostics);
	EXPECT_EQ(0u, InspectCompilationOperation(*Meshes[7]).RequestId);
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
	const auto Owner = FObjectKey(Fixture.Mesh);
	std::optional<EStaticMeshCompilationStatus> Terminal;
	std::string Error;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Fixture.Mesh, {.Source = Fixture.Mesh->GetSource()},
		[&](const auto& Value) { Terminal = Value.Status; }));
	ASSERT_TRUE(Barrier.Wait(1));
	const auto Unloaded = UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved);
	EXPECT_TRUE(Unloaded) << Unloaded.Message;
	EXPECT_EQ(nullptr, ResolveObjectKey(Owner));
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
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::optional<EStaticMeshCompilationStatus> Terminal;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source},
		[&](const auto& Value) { Terminal = Value.Status; }));
	ASSERT_TRUE(Barrier.Wait(1));
	auto* Property = DStaticMesh::StaticClass()->FindPropertyByName("NormalizedSize");
	ASSERT_NE(nullptr, Property);
	*Property->ContainerPtrToValuePtr<float>(Mesh) = 3.0f;
	Barrier.Release();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	ASSERT_TRUE(Terminal.has_value());
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, *Terminal);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Mesh).Status);
	EXPECT_FLOAT_EQ(3.0f, Mesh->GetNormalizedSize());
	EXPECT_NE(nullptr, Mesh->GetRenderData());

	auto* Initial = NewObject<DStaticMesh>(nullptr, FName("ManagerInitialCollision"));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Initial, {.Source = Source}));
	Initial->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Initial->GetCollisionBuildStatus(), Durin::EStaticMeshCollisionBuildStatus::Failed)
		<< Durin::FormatStaticMeshCollisionError(Initial->GetCollisionBuildError());
	FAssetCompilingManager::Get().FinishCompilationForObject(*Initial);
	FCollisionGeometryRef Collision;
	ASSERT_NE(nullptr, Initial->GetBodySetup());
	EXPECT_TRUE(Initial->GetBodySetup()->BuildComplexGeometry(Collision));
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Initial).Status);
}

TEST(FStaticMeshAuthoredCompilationTests, PostLoadSchedulesAndJoinsWithoutDiscardingResidentData)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("AsyncPostLoadContract");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto* Original = Fixture.Mesh->GetRenderData();
	const auto Identity = Fixture.Mesh->GetSource().GetIdentity();
	std::string Error;
	FStaticMeshWorkerBarrier Barrier;
	Fixture.Mesh->PostLoad();
	ASSERT_TRUE(Barrier.Wait(1));
	const auto Request = InspectCompilationOperation(*Fixture.Mesh).RequestId;
	EXPECT_TRUE(HasPendingStaticMeshCompilation(*Fixture.Mesh));
	EXPECT_EQ(Original, Fixture.Mesh->GetRenderData());
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	Fixture.Mesh->PostLoad();
	EXPECT_EQ(Request, InspectCompilationOperation(*Fixture.Mesh).RequestId);
	Barrier.Release();
	const auto SynchronousBuild2 = BuildStaticMeshSynchronously(*Fixture.Mesh, Fixture.Mesh->GetSource());
	EXPECT_TRUE(SynchronousBuild2) << Durin::FormatStaticMeshSynchronousError(SynchronousBuild2.error());
	EXPECT_EQ(Request, InspectCompilationOperation(*Fixture.Mesh).RequestId);
	EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
}

TEST(FStaticMeshAuthoredCompilationTests, CacheAdapterRetainsReadWriteStatusAndRequestContext)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("CacheAdapterCause");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto Observation = InspectCompilationOperation(*Fixture.Mesh);
	ASSERT_TRUE(Observation.Render);
	const auto Key = Observation.Render->DerivedDataKey;
	const auto CacheFile = Fixture.Root / "BlockedAdapterCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	AssetDerivedDataCache::FOperationDiagnostic Diagnostic;
	FSharedByteBuffer Bytes;
	EXPECT_EQ(AssetDerivedDataCache::Load(Key, MaximumStaticMeshPayloadBytes, Bytes, Diagnostic),
		AssetDerivedDataCache::ELoadResult::Miss);
	ASSERT_TRUE(Diagnostic.ReadCause);
	EXPECT_EQ(Diagnostic.ReadCause->Code, DerivedData::ECacheError::StorageFailure);
	EXPECT_FALSE(Diagnostic.WriteCause);
	EXPECT_EQ(Diagnostic.Key, Key);
	EXPECT_EQ(Diagnostic.MaximumValueBytes, MaximumStaticMeshPayloadBytes);
	const FByteBuffer Payload{std::byte{1}};
	EXPECT_FALSE(AssetDerivedDataCache::Store(Key, Payload, MaximumStaticMeshPayloadBytes, Diagnostic));
	EXPECT_FALSE(Diagnostic.ReadCause);
	ASSERT_TRUE(Diagnostic.WriteCause);
	EXPECT_EQ(Diagnostic.WriteCause->Code, DerivedData::ECacheError::StorageFailure);
	EXPECT_EQ(Diagnostic.Key, Key);
	EXPECT_EQ(Diagnostic.MaximumValueBytes, MaximumStaticMeshPayloadBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, FactoryAdmissionRetainsSourceCountAndMissingImportData)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("ReimportAdmissionCause");
	ASSERT_NE(nullptr, Fixture.Mesh);
	auto* Factory = NewObject<DStaticMeshFactory>(nullptr, "ReimportAdmissionFactory");
	std::optional<FReimportResult> Result;
	uint32 Calls = 0;
	auto Complete = [&](FReimportResult Value) { ++Calls; Result = std::move(Value); };
	const std::vector<std::string> Files{"first.obj", "second.obj"};
	Factory->ReimportFromFiles(*Fixture.Mesh, Files, Complete);
	ASSERT_TRUE(Result);
	EXPECT_EQ(Calls, 1u);
	const auto* Detail = dynamic_cast<const FStaticMeshFactoryError*>(Result->FactoryCause.get());
	ASSERT_NE(Detail, nullptr);
	const auto* Cause = std::get_if<FStaticMeshRebuildError>(&Detail->Cause);
	ASSERT_NE(Cause, nullptr);
	EXPECT_EQ(Cause->Code, EStaticMeshRebuildError::SourceCount);
	EXPECT_EQ(Cause->SourceCount, 2u);
	EXPECT_EQ(Cause->ObjectPath, Fixture.Mesh->GetObjectPath());
	auto* Empty = NewObject<DStaticMesh>(nullptr, "MissingImportDataMesh");
	ASSERT_NE(Empty, nullptr);
	Factory->Reimport(*Empty, Complete);
	EXPECT_EQ(Calls, 2u);
	Detail = dynamic_cast<const FStaticMeshFactoryError*>(Result->FactoryCause.get());
	ASSERT_NE(Detail, nullptr);
	Cause = std::get_if<FStaticMeshRebuildError>(&Detail->Cause);
	ASSERT_NE(Cause, nullptr);
	EXPECT_EQ(Cause->Code, EStaticMeshRebuildError::ImportData);
	EXPECT_EQ(Cause->ObjectPath, Empty->GetObjectPath());
	MarkAsGarbage(Empty);
}

TEST(FStaticMeshAuthoredCompilationTests, TransientCreationRetainsSettingsCauseAndClearsOutput)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("TransientCreationCause");
	ASSERT_NE(nullptr, Fixture.Mesh);
	FStaticMeshImportSettings Settings;
	Settings.RightAxis = Settings.ForwardAxis;
	const auto Rejected = CreateTransientStaticMeshFromFile(Fixture.SourcePath.generic_string(), nullptr, "RejectedTransientMesh", Settings);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().Code, EStaticMeshRebuildError::Settings);
	EXPECT_EQ(Rejected.error().ObjectName, "RejectedTransientMesh");
	EXPECT_EQ(Rejected.error().Filename, Fixture.SourcePath.generic_string());
	ASSERT_TRUE(Rejected.error().SettingsCause);
	EXPECT_EQ(Rejected.error().SettingsCause->Code, EStaticMeshImportSettingsError::RepeatedAxis);
	EXPECT_EQ(Rejected.error().SettingsCause->RightAxis, Settings.RightAxis);
	const auto MissingPath = Fixture.SourcePath.generic_string() + ".missing";
	const auto Missing = CreateTransientStaticMeshFromFile(MissingPath, nullptr, "MissingTransientMesh");
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().Code, EStaticMeshRebuildError::SourceFile);
	EXPECT_EQ(Missing.error().Filename, MissingPath);
	EXPECT_EQ(Missing.error().ObjectName, "MissingTransientMesh");
}

TEST(FStaticMeshAuthoredCompilationTests, ReimportRetainsDecodeCauseWithoutPublishing)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("ReimportDecodeCause");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto* Original = Fixture.Mesh->GetRenderData();
	const auto* ImportData = Fixture.Mesh->GetAssetImportData();
	const auto Identity = Fixture.Mesh->GetSource().GetIdentity();
	{
		std::ofstream Stream(Fixture.SourcePath, std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(Stream.is_open());
		Stream << "invalid geometry source";
	}
	const auto Result = ReimportStaticMesh(*Fixture.Mesh);
	ASSERT_FALSE(Result);
	EXPECT_EQ(Result.error().Code, EStaticMeshRebuildError::Decode);
	EXPECT_EQ(Result.error().ObjectPath, Fixture.Mesh->GetObjectPath());
	ASSERT_FALSE(Result.error().DecodeCauses.empty());
	EXPECT_EQ(Result.error().DecodeCauses.front().Category, ESceneImportDiagnosticCategory::InvalidValue);
	auto* Factory = NewObject<DStaticMeshFactory>(nullptr, "ReimportDecodeCauseFactory");
	std::optional<FReimportResult> Completion;
	uint32 CompletionCount = 0;
	Factory->Reimport(*Fixture.Mesh, [&](FReimportResult Value) { ++CompletionCount; Completion = std::move(Value); });
	ASSERT_TRUE(Completion);
	EXPECT_EQ(CompletionCount, 1u);
	const auto* Detail = dynamic_cast<const FStaticMeshFactoryError*>(Completion->FactoryCause.get());
	ASSERT_NE(Detail, nullptr);
	const auto* Cause = std::get_if<FStaticMeshRebuildError>(&Detail->Cause);
	ASSERT_NE(Cause, nullptr);
	EXPECT_EQ(Cause->Code, EStaticMeshRebuildError::Decode);
	ASSERT_FALSE(Cause->DecodeCauses.empty());
	EXPECT_EQ(Cause->DecodeCauses.front().Category, Result.error().DecodeCauses.front().Category);
	EXPECT_EQ(Original, Fixture.Mesh->GetRenderData());
	EXPECT_EQ(ImportData, Fixture.Mesh->GetAssetImportData());
	EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
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
	const auto Identity = Fixture.Mesh->GetSource().GetIdentity();
	auto* Factory = NewObject<DStaticMeshFactory>(nullptr, "AsyncReimportFactory");
	std::optional<FReimportResult> Result;
	{
		FStaticMeshWorkerBarrier Barrier;
		Factory->Reimport(*Fixture.Mesh, [&](FReimportResult Value) { Result = std::move(Value); });
		ASSERT_TRUE(Barrier.Wait(1));
		const auto RequestId = InspectCompilationOperation(*Fixture.Mesh).RequestId;
		EXPECT_FALSE(Result.has_value());
		EXPECT_EQ(Original, Fixture.Mesh->GetRenderData());
		EXPECT_EQ(ImportData, Fixture.Mesh->GetAssetImportData());
		CancelStaticMeshCompilation(*Fixture.Mesh);
		Barrier.Release();
		FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
		ASSERT_TRUE(Result.has_value());
		EXPECT_FALSE(Result->Succeeded());
		const auto* Detail = dynamic_cast<const FStaticMeshFactoryError*>(Result->FactoryCause.get());
		ASSERT_NE(Detail, nullptr);
		EXPECT_EQ(std::get<FStaticMeshCompilationDiagnostic>(Detail->Cause).RequestId, RequestId);
		EXPECT_EQ(std::get<FStaticMeshCompilationDiagnostic>(Detail->Cause).Status, EStaticMeshCompilationStatus::Cancelled);
		EXPECT_EQ(std::get<FStaticMeshCompilationDiagnostic>(Detail->Cause).Phase, EStaticMeshCompilationPhase::Terminal);
		EXPECT_EQ(ImportData, Fixture.Mesh->GetAssetImportData());
		EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
		EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	}
	uint32 SaveAttempts = 0;
	const auto SaveFailure = ReimportStaticMesh(*Fixture.Mesh, {.ShouldFail = [&](EAssetBundleSavePhase Phase, size_t) {
		if (Phase != EAssetBundleSavePhase::StagePackage) return false;
		++SaveAttempts;
		return true;
	}});
	ASSERT_FALSE(SaveFailure);
	EXPECT_EQ(SaveFailure.error().Code, EStaticMeshRebuildError::Completion);
	ASSERT_TRUE(SaveFailure.error().CompletionCause);
	EXPECT_EQ(SaveFailure.error().CompletionCause->Error.Code, EStaticMeshCompletionError::Save);
	ASSERT_NE(SaveFailure.error().CompletionCause->Error.SaveCause, nullptr);
	EXPECT_EQ(1u, SaveAttempts);
	EXPECT_TRUE(Fixture.Mesh->GetPackage()->IsDirty());
	EXPECT_NE(ImportData, Fixture.Mesh->GetAssetImportData());
	EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
	EXPECT_NE(nullptr, Fixture.Mesh->GetRenderData());
	const auto AppliedImport = Fixture.Mesh->GetAssetImportData();
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	const auto Missing = ReimportStaticMesh(*Fixture.Mesh);
	ASSERT_FALSE(Missing);
	EXPECT_EQ(Missing.error().Code, EStaticMeshRebuildError::SourceFile);
	EXPECT_EQ(Missing.error().ObjectPath, Fixture.Mesh->GetObjectPath());
	EXPECT_EQ(Missing.error().Filename, Fixture.SourcePath.generic_string());
	EXPECT_EQ(AppliedImport, Fixture.Mesh->GetAssetImportData());
	const auto SynchronousBuild3 = BuildStaticMeshSynchronously(*Fixture.Mesh, Fixture.Mesh->GetSource());
	EXPECT_TRUE(SynchronousBuild3) << Durin::FormatStaticMeshSynchronousError(SynchronousBuild3.error());
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
	{
		auto LoadedValue = LoadObject<Durin::DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
		Fixture.Mesh = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_EQ(nullptr, Fixture.Mesh->GetRenderData());
	const auto Identity = Fixture.Mesh->GetSource().GetIdentity();
	const auto Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	FByteBuffer Before, After;
	ASSERT_TRUE(SerializeAssetPackageBytes(Fixture.Mesh->GetPackage(), Before));
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	std::string Error;
	ASSERT_TRUE(ContributeEngineCookAsset(*Fixture.Mesh, "/Game/Projection", Context)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Context, std::filesystem::absolute(Fixture.Root / "Cook"))) << Error;
	EXPECT_EQ(nullptr, Fixture.Mesh->GetRenderData());
	EXPECT_EQ(Revision, Fixture.Mesh->GetRenderResourceStatus().Revision);
	EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(SerializeAssetPackageBytes(Fixture.Mesh->GetPackage(), After));
	EXPECT_EQ(Before, After);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
}

TEST(FStaticMeshAuthoredCompilationTests, ConcurrentCandidatesRetainCancelledBytesUntilWorkerRelease)
{
	CheckConcurrentCandidates(false);
}

TEST(FStaticMeshAuthoredCompilationTests, InitialMutationRequeuesAtCapacityAndExplicitCancelStopsReplacement)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	std::string Error;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::array<DStaticMesh*, 32> Meshes;
	FStaticMeshWorkerBarrier Barrier;
	for (size_t Index = 0; Index < Meshes.size(); ++Index)
	{
		Meshes[Index] = NewObject<DStaticMesh>(nullptr, FName(std::format("CapacityMutation{}", Index)));
		ASSERT_TRUE(SubmitStaticMeshCompilation(*Meshes[Index], {.Source = Source, .bPersistDerivedData = false}));
	}
	ASSERT_TRUE(Barrier.Wait(2));
	Meshes[0]->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Meshes[0]->GetCollisionBuildStatus(), Durin::EStaticMeshCollisionBuildStatus::Failed)
		<< Durin::FormatStaticMeshCollisionError(Meshes[0]->GetCollisionBuildError());
	Meshes[1]->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Meshes[1]->GetCollisionBuildStatus(), Durin::EStaticMeshCollisionBuildStatus::Failed)
		<< Durin::FormatStaticMeshCollisionError(Meshes[1]->GetCollisionBuildError());
	CancelStaticMeshCompilation(*Meshes[1]);
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(32u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_NE(nullptr, Meshes[0]->GetRenderData());
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Meshes[0]).Status);
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
	const auto Cold = InspectCompilationOperation(*Fixture.Mesh);
	ASSERT_TRUE(Cold.Render.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::Rebuilt, Cold.Render->Origin);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetIdentity(), Cold.SourceIdentity);
	std::string Error;
	Fixture.Mesh->PostLoad();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	const auto Warm = InspectCompilationOperation(*Fixture.Mesh);
	ASSERT_TRUE(Warm.Render.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::CacheHit, Warm.Render->Origin);
	EXPECT_EQ(Warm.PersistenceDiagnostic.Render.Read.Code, EAssetCacheError::None);
	EXPECT_EQ(Warm.PersistenceDiagnostic.Render.Write.Code, EAssetCacheError::None);
	EXPECT_EQ(Warm.Error.Code, EStaticMeshCompletionError::None);
	EXPECT_EQ(Cold.Render->DerivedDataKey, Warm.Render->DerivedDataKey);
	const auto Revision = Fixture.Mesh->GetPackage()->GetEditRevision();
	for (uint32 Index = 0; Index < 10; ++Index)
		EXPECT_EQ(Warm.RequestId, InspectCompilationOperation(*Fixture.Mesh).RequestId);
	EXPECT_EQ(Revision, Fixture.Mesh->GetPackage()->GetEditRevision());
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	EXPECT_FALSE(HasPendingStaticMeshCompilation(*Fixture.Mesh));
	const auto CacheFile = Fixture.Root / "BlockedManagerCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	Fixture.Mesh->PostLoad();
	FAssetCompilingManager::Get().FinishCompilationForObject(*Fixture.Mesh);
	const auto FailedCache = InspectCompilationOperation(*Fixture.Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, FailedCache.Status);
	ASSERT_TRUE(FailedCache.Render.has_value());
	EXPECT_EQ(EStaticMeshBuildOrigin::Rebuilt, FailedCache.Render->Origin);
	EXPECT_EQ(FailedCache.PersistenceDiagnostic.Render.Write.Code, EAssetCacheError::Write);
	ASSERT_NE(FailedCache.PersistenceDiagnostic.Render.Write.WriteCause, nullptr);
	EXPECT_EQ(FailedCache.PersistenceDiagnostic.Render.Write.WriteCause->Code, DerivedData::ECacheError::StorageFailure);
	EXPECT_EQ(FailedCache.Error.Code, EStaticMeshCompletionError::None);
	EXPECT_LE(FormatStaticMeshCompilationDiagnostic(FailedCache).size() + FailedCache.Descriptor.ProducerIdentity.size(), 4096u);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	const auto Synchronous = BuildStaticMeshSynchronously(*Fixture.Mesh, Fixture.Mesh->GetSource());
	EXPECT_TRUE(Synchronous);
	ASSERT_TRUE(Synchronous);
	EXPECT_EQ(Synchronous->Render.Write.Code, EAssetCacheError::Write);
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
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("AllChannelManagerFixture"));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source, .bPersistDerivedData = false}));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	ASSERT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Mesh).Status);
	ASSERT_NE(nullptr, Mesh->GetRenderData());
	const auto* Original = Mesh->GetRenderData();
	EXPECT_EQ(MaxStaticMeshUVChannels, Original->LODResources.front().NumTexCoords);
	EXPECT_EQ(128u, Original->LODResources.front().Sections.size());
	auto* Body = NewObject<DBodySetup>(Mesh, FName("ConvexLimitBody"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
	ASSERT_TRUE(Mesh->SetBodySetup(Body));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source, .bPersistDerivedData = false}));
	FAssetCompilingManager::Get().FinishCompilationForObject(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Failed, InspectCompilationOperation(*Mesh).Status);
	EXPECT_EQ(Original, Mesh->GetRenderData());
	EXPECT_EQ(0u, GetStaticMeshCompilationManagerDiagnostics().ReservedBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, LatestCompletedObservationWinsOverRetainedOlderWorker)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("LatestObservationWhileOldWorkerRetained"));
	FStaticMeshWorkerBarrier Barrier(EStaticMeshCompilationPhase::Building, true);
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source}));
	const auto Latest = InspectCompilationOperation(*Mesh).RequestId;
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (InspectCompilationOperation(*Mesh).Phase != EStaticMeshCompilationPhase::Terminal
		&& std::chrono::steady_clock::now() < Deadline)
	{
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	const auto Completed = InspectCompilationOperation(*Mesh);
	EXPECT_EQ(Latest, Completed.RequestId);
	EXPECT_EQ(EStaticMeshCompilationPhase::Terminal, Completed.Phase);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, Completed.Status);
	EXPECT_EQ(1u, GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords);
	EXPECT_GT(GetStaticMeshCompilationManagerDiagnostics().ReservedBytes, 0u);
	EXPECT_NE(nullptr, Mesh->GetRenderData());
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(Latest, InspectCompilationOperation(*Mesh).RequestId);
}

TEST(FStaticMeshPayloadInspectionTests, UnreadableSourcePollingDoesNotAcquireOrMutate)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	const auto Fixture = ImportCacheFixture("StaticMeshInspectionReadProbe");
	ASSERT_NE(Fixture.Mesh, nullptr);
	auto& Source = const_cast<FStaticMeshSource&>(Fixture.Mesh->GetSource());
	const auto Resource = AttachResidencyProbe(Source, true);
	const auto Identity = Source.GetIdentity();
	const auto Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	const auto* Cpu = Fixture.Mesh->GetRenderData();
	const auto Dirty = Fixture.Mesh->GetPackage()->IsDirty();
	const auto Before = GetStaticMeshCompilationManagerDiagnostics();
	const auto ObjectCount = GDObjectArray.GetNum();
	const FScopedStaticMeshProviderRestore ProviderRestore;
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
	for (int Index = 0; Index < 100; ++Index)
	{
		const auto Snapshot = InspectStaticMeshPayloads(*Fixture.Mesh);
		EXPECT_FALSE(Snapshot.bSourceResident);
		EXPECT_EQ(Snapshot.Fields[0].Identity, Identity);
		EXPECT_EQ(Snapshot.Fields[0].State, "Metadata present");
		EXPECT_EQ(Snapshot.Gpu.Revision, Revision);
		EXPECT_TRUE(Snapshot.bCpuResident);
	}
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
	EXPECT_FALSE(FModuleManager::Get().IsModuleLoaded("StaticMeshBuild"));
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), Cpu);
	EXPECT_EQ(Fixture.Mesh->GetPackage()->IsDirty(), Dirty);
	EXPECT_EQ(GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords, Before.OutstandingRecords);
	EXPECT_EQ(GetStaticMeshCompilationManagerDiagnostics().ReservedBytes, Before.ReservedBytes);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshPayloadInspectionTests, PackageInspectionPreservesAbsentMalformedAndUnsupportedStates)
{
	using namespace Durin;
	FAssetPackageInspection Package;
	Package.Header.AssetClassName = DStaticMesh::StaticClass()->GetQualifiedName().ToString();
	Package.PhysicalPath = "/nonexistent/metadata-only/Mesh.dasset";
	Package.Objects.emplace_back();
	FStaticMeshPayloadInspection Snapshot;
	const auto ObjectCount = GDObjectArray.GetNum();
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
	ASSERT_EQ(Snapshot.Fields.size(), 3u);
	EXPECT_TRUE(Snapshot.bConstructFree);
	for (const auto& Field : Snapshot.Fields) EXPECT_EQ(Field.State, "Absent");
	Package.Objects[0].Fields.push_back({.Name = "RenderData", .SourceFormatVersion = 10});
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(Snapshot.Fields[1].State, "Malformed");
	Package.Objects[0].Fields[0].SourceFormatVersion = 8;
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(Snapshot.Fields[1].State, "Unsupported");
	Package.Objects[0].Fields.push_back({.Name = "Source", .SourceFormatVersion = 10});
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(Snapshot.Fields[0].State, "Malformed");
	Package.Header.AssetClassName = "Unsupported";
	std::string Error;
	EXPECT_FALSE(InspectStaticMeshPayloadPackage(Package, Snapshot, &Error));
	EXPECT_FALSE(Error.empty());
}

TEST(FStaticMeshPayloadInspectionTests, AuthoredAndCookedMetadataDoesNotDependOnCompanionAvailability)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	const auto Fixture = ImportCacheFixture("StaticMeshInspectionPackages");
	ASSERT_NE(Fixture.Mesh, nullptr);
	FAssetPackageInspection Package;
	ASSERT_TRUE(InspectAssetPackage((Fixture.Root / "Content/Mesh.dasset").generic_string(), Fixture.AssetPath, Package));
	FStaticMeshPayloadInspection Snapshot;
	const auto ObjectCount = GDObjectArray.GetNum();
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
	EXPECT_EQ(Snapshot.Fields[0].State, "Metadata present");
	EXPECT_EQ(Snapshot.Fields[1].State, "Absent");
	const auto SourceVersions = Package.CustomVersions;
	ASSERT_EQ(SourceVersions, (std::vector<FCustomVersion>{{FStaticMeshSourceVersion::Guid, FStaticMeshSourceVersion::CurrentVersion}}));
	std::vector<FAssetPackageField> SourceFields;
	ASSERT_TRUE(Package.FindField("Source")->TryInspectStructFields(SourceFields));
	EXPECT_EQ(std::ranges::find(SourceFields, "SchemaVersion", &FAssetPackageField::Name), SourceFields.end());
	for (const int32 Version : {-1, 0, 2})
	{
		Package.CustomVersions = SourceVersions;
		if (Version < 0) Package.CustomVersions.clear();
		else Package.CustomVersions.front().Version = Version;
		ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
		EXPECT_EQ(Snapshot.Fields[0].State, "Unsupported");
		EXPECT_EQ(GDObjectArray.GetNum(), ObjectCount);
	}
	Package.CustomVersions = SourceVersions;
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	const auto Identity = Snapshot.Fields[0].Identity;
	Package.PhysicalPath = "/nonexistent/absent-companion/Mesh.dasset";
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(Snapshot.Fields[0].Identity, Identity);
	EXPECT_EQ(Snapshot.Fields[0].State, "Metadata present");
	std::string Error;
	Fixture.Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Fixture.Mesh->GetCollisionBuildStatus(), Durin::EStaticMeshCollisionBuildStatus::Failed)
		<< Durin::FormatStaticMeshCollisionError(Fixture.Mesh->GetCollisionBuildError());
	const auto CookRoot = std::filesystem::absolute(Fixture.Root / "Cook");
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(ContributeEngineCookAsset(*Fixture.Mesh, "/Game/InspectionMesh", Context)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Context, CookRoot)) << Error;
	FPackagePath CookedPath;
	ASSERT_TRUE(FPackagePath::TryCreateProjectContent("/Game/InspectionMesh", CookedPath));
	ASSERT_TRUE(InspectAssetPackage((CookRoot / "Game/InspectionMesh.dasset").generic_string(), CookedPath, Package));
	EXPECT_EQ(std::ranges::find(Package.CustomVersions, FStaticMeshSourceVersion::Guid, &FCustomVersion::Guid), Package.CustomVersions.end());
	Package.PhysicalPath = "/nonexistent/cooked/Mesh.dasset";
	ASSERT_TRUE(InspectStaticMeshPayloadPackage(Package, Snapshot));
	EXPECT_EQ(Snapshot.Fields[0].State, "Absent");
	EXPECT_EQ(Snapshot.Fields[1].State, "Metadata present");
	EXPECT_EQ(Snapshot.Fields[2].State, "Metadata present");
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshPayloadInspectionTests, MetadataModeDoesNotHashInlinePayloadOrRequireExternalStorage)
{
	using namespace Durin;
	FAssetPackageField Field{.Name = "RenderData", .Kind = DurinCodeGen::EPropertyGenFlags::BulkData, .SourceFormatVersion = 10};
	auto Append = [&](const auto& Value) {
		const auto Bytes = std::as_bytes(std::span(&Value, 1));
		Field.Payload.insert(Field.Payload.end(), Bytes.begin(), Bytes.end());
	};
	Append(uint32{1}); Append(uint8{0}); Append(uint8{0}); Append(uint16{1});
	Append(uint32{1}); Append(uint64{1}); Append(FGuid{1, 2, 3, 4});
	Append(uint64{1}); Append(uint64{2}); Append(uint64{1}); Append(uint64{1}); Append(uint64{0});
	Field.Payload.push_back(std::byte{0x42});
	FPackageBulkStorageDescriptor Descriptor;
	EXPECT_FALSE(Field.TryReadBulkDataStorageDescriptor(Descriptor));
	EXPECT_TRUE(Field.TryReadBulkDataStorageDescriptor(Descriptor, false));
	Field.Payload[4] = std::byte{1};
	Field.Payload.pop_back();
	EXPECT_TRUE(Field.TryReadBulkDataStorageDescriptor(Descriptor, false));
	EXPECT_EQ(Descriptor.StorageKind, EPackageBulkStorageKind::External);
}

TEST(FStaticMeshReplacementTests, FailureDropsDerivedDataAndValidSourceCanBeRebuilt)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshReplacementRetry");
	auto* Mesh = Fixture.Mesh;
	ASSERT_NE(Mesh, nullptr);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Ready);
	const auto Revision = Mesh->GetRenderResourceStatus().Revision;
	Mesh->ReplaceRenderData(nullptr, {});
	EXPECT_NE(Mesh->GetRenderDataUpdateError().Code, Durin::EStaticMeshReplacementError::None);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_GT(Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_EQ(Mesh->GetRenderDataLoadStatus().CpuPhase, ECookedMeshCpuPhase::Failed);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Unavailable);
	EXPECT_FALSE(Mesh->GetBodySetup()->GetResidentComplexGeometry());
	Mesh->RebuildCollision();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Failed);
	const auto MissingCollision = Mesh->GetCollisionBuildError();
	EXPECT_EQ(MissingCollision.Code, EStaticMeshCollisionError::MissingRenderData);
	EXPECT_EQ(MissingCollision.Mode, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);

	FStaticMeshSource Source;
	std::string Error;
	auto Geometry = MakeResidencyGeometry();
	Geometry.Meshes.front().Positions.front().x = 7;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	Mesh->ReplaceSourceRenderData(Source, nullptr, {}, 2.0f);
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), Source.GetIdentity());
	EXPECT_EQ(Mesh->GetNormalizedSize(), 2.0f);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_NE(Mesh->GetRenderDataUpdateError().Code, Durin::EStaticMeshReplacementError::None);

	FStaticMeshBuildResult Product;
	ASSERT_TRUE(BuildStaticMeshDerivedData({
		.Reconciliation = CaptureStaticMeshReconciliation(*Mesh),
		.Source = Source}, Product)) << Error;
	Mesh->ReplaceSourceRenderData(Source, std::move(Product.RenderData),
		std::move(Product.MaterialSlots), Product.NormalizedSize);
	EXPECT_EQ(Mesh->GetRenderDataUpdateError().Code, Durin::EStaticMeshReplacementError::None);
	EXPECT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Mesh->GetRenderDataLoadStatus().CpuPhase, ECookedMeshCpuPhase::CpuReady);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Ready);
	EXPECT_EQ(Mesh->GetCollisionBuildError().Code, Durin::EStaticMeshCollisionError::None);
	EXPECT_EQ(MissingCollision.Code, EStaticMeshCollisionError::MissingRenderData);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshReplacementTests, CollisionConfigurationCanPrecedeCpuData)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, "DeferredCollisionMesh");
	Mesh->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::ComplexOnly);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Unavailable);
	EXPECT_EQ(Mesh->GetCollisionBuildError().Code, Durin::EStaticMeshCollisionError::None);
	EXPECT_EQ(Mesh->GetBodySetup()->GetCollisionQueryPolicy(), EBodySetupCollisionQueryPolicy::ComplexOnly);
	Mesh->RebuildCollision();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Failed);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EStaticMeshCollisionBuildStatus::Ready);
	EXPECT_EQ(Mesh->GetCollisionBuildError().Code, Durin::EStaticMeshCollisionError::None);
	MarkObjectHierarchyAsGarbage(Mesh);
}

TEST(FStaticMeshRecipeTests, RejectionOwnsMeshIndexWithoutProduct)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	auto Source = std::make_shared<FStaticMeshDecodedGeometry>(MakeResidencyGeometry());
	Source->Meshes[0].Name = "RejectedMesh";
	Source->Meshes[0].Indices[1] = 99;
	const auto Invocation = FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
		[&](IStaticMeshBuildProvider& Provider) { return Provider.BuildRender({.Geometry = Source}); });
	ASSERT_TRUE(Invocation.Value.has_value());
	const auto Result = *Invocation.Value;
	EXPECT_FALSE(Result);
	Source.reset();
	EXPECT_EQ(Result.error().Code, EStaticMeshRecipeError::IndexRange);
	EXPECT_EQ(Result.error().Kind, EStaticMeshRecipeKind::Render);
	EXPECT_EQ(Result.error().MeshName, "RejectedMesh");
	EXPECT_EQ(Result.error().Index, 1u);
	EXPECT_EQ(Result.error().Actual, 99u);
	EXPECT_EQ(Result.error().Expected, 3u);
}

TEST(FStaticMeshRecipeTests, CollisionFailureAndCancellationRetainPhysicsCause)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	const std::array Positions{FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0)};
	const std::array<uint32, 3> Indices{0, 1, 99};
	const auto Invoke = [&](const FStaticMeshBuildExecutionControl& Control) {
		return FModularFeatureRegistry::Get().InvokeSingle<IStaticMeshBuildProvider>(
			[&](IStaticMeshBuildProvider& Provider) {
				return Provider.BuildCollision({Positions, Indices, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0}, Control);
			});
	};
	const auto Failed = Invoke({});
	ASSERT_TRUE(Failed.Value.has_value());
	EXPECT_FALSE(*Failed.Value);
	EXPECT_EQ(Failed.Value->error().Code, EStaticMeshRecipeError::CollisionBuild);
	EXPECT_EQ(Failed.Value->error().Kind, EStaticMeshRecipeKind::Collision);
	ASSERT_TRUE(Failed.Value->error().CollisionCause);
	EXPECT_EQ(Failed.Value->error().CollisionCause->Status, ECollisionGeometryBuildStatus::InvalidInput);
	uint32 Checks = 0;
	const auto Cancelled = Invoke({.ShouldCancel = [&] { return ++Checks == 3; }});
	ASSERT_TRUE(Cancelled.Value.has_value());
	ASSERT_FALSE(*Cancelled.Value);
	EXPECT_EQ(Cancelled.Value->error().Code, EStaticMeshRecipeError::Cancelled);
	ASSERT_TRUE(Cancelled.Value->error().CollisionCause);
	EXPECT_EQ(Cancelled.Value->error().CollisionCause->Status, ECollisionGeometryBuildStatus::Cancelled);
	const auto Budget = Invoke({.MaximumWorkingSetBytes = 1});
	ASSERT_TRUE(Budget.Value.has_value());
	EXPECT_EQ(Budget.Value->error().Code, EStaticMeshRecipeError::WorkingSet);
	EXPECT_EQ(Budget.Value->error().Expected, 1u);
	EXPECT_EQ(Budget.Value->error().VertexCount, 3u);
	EXPECT_EQ(Budget.Value->error().IndexCount, 3u);
}

TEST(FStaticMeshSourceResidencyTests, TypedReadFailureOwnsCauseAndRejectedIndex)
{
	using namespace Durin;
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto Resource = AttachResidencyProbe(Source, true);
	const auto Read = Source.AcquireGeometry();
	EXPECT_FALSE(Read);
	EXPECT_EQ(Read.error().Code, EStaticMeshSourceError::Read);
	ASSERT_TRUE(Read.error().ReadCause);
	EXPECT_EQ(Read.error().ReadCause->error().Status, EPackageResourceReadStatus::IoError);
	EXPECT_EQ(Read.error().ReadCause->error().Reason, EPackageResourceReadReason::Open);
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);

	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto Payload = Source.GetGeometryBulk().GetPayload().Wait();
	FByteBuffer Bytes(Payload->begin(), Payload->end());
	WriteU32(Bytes, Bytes.size() - 4, 99);
	ASSERT_TRUE(GetReflectedSourceBulk(Source).UpdatePayload(Bytes));
	const auto Invalid = Source.AcquireGeometry();
	EXPECT_FALSE(Invalid);
	EXPECT_FALSE(Source.IsGeometryResident());
	Source = {};
	EXPECT_EQ(Invalid.error().Code, EStaticMeshSourceError::IndexRange);
	EXPECT_EQ(Invalid.error().MeshName, "Fixture");
	EXPECT_EQ(Invalid.error().Index, 2u);
	EXPECT_EQ(Invalid.error().Actual, 99u);
	EXPECT_EQ(Invalid.error().Expected, 3u);
	EXPECT_EQ(Read.error().ReadCause->error().Reason, EPackageResourceReadReason::Open);
	EXPECT_EQ(Source.AcquireGeometry().error().Code, EStaticMeshSourceError::InvalidHeader);
}

TEST(FStaticMeshSourceResidencyTests, InitializationErrorsOwnValuesAndPreserveSource)
{
	using namespace Durin;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto Original = Source.AcquireGeometry().value();
	const auto Identity = Source.GetIdentity();
	auto Invalid = MakeResidencyGeometry();
	Invalid.Meshes[0].Name = "RejectedSource";
	Invalid.Meshes[0].Positions[1].z = std::numeric_limits<float>::infinity();
	const auto NonFinite = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(NonFinite);
	EXPECT_EQ(NonFinite.error().Code, EStaticMeshSourceError::NonFinitePosition);
	EXPECT_EQ(NonFinite.error().MeshName, "RejectedSource");
	EXPECT_EQ(NonFinite.error().Index, 1u);
	EXPECT_TRUE(std::isinf(NonFinite.error().Position.z));
	Invalid = MakeResidencyGeometry();
	Invalid.Meshes[0].Indices.clear();
	const auto Empty = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(Empty);
	EXPECT_EQ(Empty.error().Code, EStaticMeshSourceError::EmptyMesh);
	EXPECT_EQ(Empty.error().Field, "Indices");
	EXPECT_EQ(Empty.error().Actual, 0u);
	EXPECT_EQ(Empty.error().Expected, 1u);
	Invalid = MakeResidencyGeometry();
	Invalid.Meshes[0].Indices.pop_back();
	const auto Triangles = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(Triangles);
	EXPECT_EQ(Triangles.error().Code, EStaticMeshSourceError::TriangleList);
	EXPECT_EQ(Triangles.error().Actual, 2u);
	EXPECT_EQ(Triangles.error().Expected, 3u);
	Invalid = MakeResidencyGeometry();
	Invalid.MaterialSlots.push_back(Invalid.MaterialSlots.front());
	const auto Duplicate = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(Duplicate);
	EXPECT_EQ(Duplicate.error().Code, EStaticMeshSourceError::DuplicateMaterial);
	EXPECT_EQ(Duplicate.error().Actual, 0u);
	EXPECT_EQ(Source.GetIdentity(), Identity);
	EXPECT_EQ(Source.AcquireGeometry().value(), Original);
	EXPECT_EQ(Original->Meshes[0].Name, "Fixture");
}

TEST(FStaticMeshDerivedDataCacheTests, PayloadRebuildRetainsTypedRenderAndCollisionRejection)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshTypedCacheRejection");
	ASSERT_NE(Fixture.Mesh, nullptr);
	FStaticMeshBuildRequest Request{.Reconciliation = CaptureStaticMeshReconciliation(*Fixture.Mesh),
		.Source = Fixture.Mesh->GetSource()};
	FStaticMeshBuildResult Render;
	std::string Error;
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Render));
	ASSERT_TRUE(std::filesystem::remove(GetObjectPath(Fixture, Render.DerivedDataKey.ToString())));
	const std::array<std::byte, 4> Invalid{};
	AssetDerivedDataCache::FOperationDiagnostic Diagnostic;
	ASSERT_TRUE(AssetDerivedDataCache::Store(Render.DerivedDataKey, Invalid, MaximumStaticMeshPayloadBytes, Diagnostic));
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Render)) << Error;
	EXPECT_EQ(Render.Origin, EStaticMeshBuildOrigin::Rebuilt);
	ASSERT_TRUE(Render.CacheDiagnostics.DecodeCause);
	const auto RenderRejection = *Render.CacheDiagnostics.DecodeCause;
	EXPECT_EQ(RenderRejection.Code, EStaticMeshCacheCodecError::Archive);
	EXPECT_EQ(RenderRejection.Operation, EStaticMeshCacheCodecOperation::DecodeRender);
	EXPECT_EQ(RenderRejection.ArchiveCode, EArchiveFailureCode::TruncatedPayload);
	ASSERT_TRUE(BuildStaticMeshDerivedData(Request, Render));
	EXPECT_EQ(Render.Origin, EStaticMeshBuildOrigin::CacheHit);
	EXPECT_FALSE(Render.CacheDiagnostics.DecodeCause);

	FStaticMeshCollisionBuildResult Collision;
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision));
	const auto Key = Collision.DerivedDataKey;
	const auto Path = Fixture.CacheRoot / "StaticMeshCollision/Objects"
		/ Key.ToString().substr(0, 2) / (Key.ToString() + ".bin");
	ASSERT_TRUE(std::filesystem::remove(Path));
	ASSERT_TRUE(AssetDerivedDataCache::Store(Key, Invalid, MaximumStaticMeshCollisionPayloadBytes, Diagnostic));
	ASSERT_TRUE(BuildStaticMeshCollisionDerivedData(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision));
	EXPECT_EQ(Collision.Origin, EStaticMeshBuildOrigin::Rebuilt);
	ASSERT_TRUE(Collision.CacheDiagnostics.DecodeCause);
	const auto CollisionRejection = *Collision.CacheDiagnostics.DecodeCause;
	Collision = {};
	EXPECT_EQ(CollisionRejection.Code, EStaticMeshCacheCodecError::Archive);
	EXPECT_EQ(CollisionRejection.Operation, EStaticMeshCacheCodecOperation::DecodeCollision);
	EXPECT_EQ(CollisionRejection.ArchiveCode, EArchiveFailureCode::TruncatedPayload);
	EXPECT_EQ(RenderRejection.Operation, EStaticMeshCacheCodecOperation::DecodeRender);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, BuildBoundariesTranslateProviderFailureAndCancellation)
{
	using namespace Durin;
	class FInvalidProductProvider final : public IStaticMeshBuildProvider
	{
	public:
		bool bCancel = false;
		auto GetDescriptor() const -> FStaticMeshBuildProviderDescriptor override
		{
			return {.ProducerIdentity = "InvalidProductFixture", .RenderBuilderVersion = 777, .CollisionBuilderVersion = 777};
		}
		auto BuildRender(const FStaticMeshRecipeBuildRequest&,
			const FStaticMeshBuildExecutionControl&) -> std::expected<FStaticMeshRecipeBuildProduct, FStaticMeshRecipeError> override
		{
			if (bCancel) return std::unexpected(FStaticMeshRecipeError{.Code = EStaticMeshRecipeError::Cancelled});
			return FStaticMeshRecipeBuildProduct{};
		}
		auto BuildCollision(const FStaticMeshCollisionRecipeRequest&,
			const FStaticMeshBuildExecutionControl&) -> std::expected<FStaticMeshCollisionRecipeProduct, FStaticMeshRecipeError> override
		{
			ADD_FAILURE() << "Render rejection must not invoke a collision recipe.";
			return std::unexpected(FStaticMeshRecipeError{.Code = EStaticMeshRecipeError::CollisionInput});
		}
	};
	const FScopedDerivedDataCacheRestore CacheRestore;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "InvalidProductCache").generic_string());
	FScopedStaticMeshProviderRestore RestoreProvider;
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	ASSERT_TRUE(FModuleManager::Get().UnloadModule("StaticMeshBuild").Succeeded());
	FModuleTestOwner Owner("InvalidProductProvider");
	FInvalidProductProvider Provider;
	auto Registration = Owner.RegisterFeature(Provider);
	ASSERT_TRUE(Registration.IsValid());
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshBuildResult Product;
	std::string Error;
	const auto Outcome = BuildStaticMeshDerivedData({.Source = Source, .bPersistDerivedData = false}, Product);
	EXPECT_FALSE(Outcome);
	ASSERT_FALSE(Outcome);
	EXPECT_NE(Outcome.error().Code, EStaticMeshDerivedDataError::Cancelled);
	ASSERT_TRUE(Outcome.error().PayloadCause);
	EXPECT_EQ(Outcome.error().PayloadCause->Code, EStaticMeshCacheCodecError::RenderPayload);
	EXPECT_EQ(Outcome.error().PayloadCause->Operation, EStaticMeshCacheCodecOperation::EncodeRender);
	ASSERT_TRUE(Outcome.error().PayloadCause->RenderCause);
	EXPECT_NE(Outcome.error().PayloadCause->RenderCause->Code, EStaticMeshPayloadError::None);
	EXPECT_EQ(Product.RenderData, nullptr);
	const auto Authored = BuildStaticMeshAuthoredCandidate({.Source = Source, .bPersistDerivedData = false});
	ASSERT_FALSE(Authored);
	EXPECT_EQ(Authored.error().Code, EStaticMeshAuthoredBuildError::RenderBuild);
	EXPECT_EQ(Authored.error().Message, FormatStaticMeshDerivedDataError(Outcome.error()));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("TypedWorkerFailure"));
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Source, .bPersistDerivedData = false})) << Error;
	FAssetCompilingManager::Get().FinishAllCompilation();
	const auto Diagnostic = GetStaticMeshCompilationDiagnostic(*Mesh);
	EXPECT_EQ(Diagnostic.Error.Code, EStaticMeshCompletionError::Build);
	ASSERT_TRUE(Diagnostic.Error.BuildCause);
	EXPECT_EQ(Diagnostic.Error.BuildCause->Code, EStaticMeshAuthoredBuildError::RenderBuild);
	EXPECT_EQ(Diagnostic.Error.BuildCause->Message, Authored.error().Message);
	const auto Synchronous = BuildStaticMeshSynchronously(*Mesh, Source);
	ASSERT_FALSE(Synchronous);
	EXPECT_EQ(Synchronous.error().Code, EStaticMeshSynchronousError::Completion);
	EXPECT_EQ(Synchronous.error().Message, FormatStaticMeshCompilationDiagnostic(Diagnostic));
	Provider.bCancel = true;
	const auto Cancelled = BuildStaticMeshSynchronously(*Mesh, Source);
	ASSERT_FALSE(Cancelled);
	EXPECT_EQ(Cancelled.error().Code, EStaticMeshSynchronousError::Cancelled);
	EXPECT_FALSE(Cancelled.error().Message.empty());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_FALSE(HasPendingStaticMeshCompilation(*Mesh));
}

TEST(FStaticMeshAuthoredCompilationTests, DerivedResultRetainsSourceCancellationAndGeometryCounts)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("StaticMeshBuild");
	const FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "SourceCancellationCache").generic_string());
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshBuildResult Product;
	uint32 Checks = 0;
	const auto Cancelled = BuildStaticMeshDerivedData({.Source = Source, .bPersistDerivedData = false}, Product,
		{.ShouldCancel = [&] { return ++Checks == 3; }});
	EXPECT_FALSE(Cancelled);
	EXPECT_EQ(Cancelled.error().Code, EStaticMeshDerivedDataError::Cancelled);
	ASSERT_TRUE(Cancelled.error().SourceCause);
	EXPECT_EQ(Cancelled.error().SourceCause->Code, EStaticMeshSourceError::Cancelled);
	EXPECT_EQ(Product.RenderData, nullptr);
	FStaticMeshRenderData Invalid;
	Invalid.LODResources.resize(1);
	Invalid.LODResources[0].IndexBuffer.Init(std::vector<uint32>{0, 1});
	FStaticMeshCollisionBuildResult Collision;
	const auto Rejected = BuildStaticMeshCollisionDerivedData(Invalid,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex, Collision, false);
	EXPECT_FALSE(Rejected);
	Invalid.LODResources.clear();
	EXPECT_EQ(Rejected.error().Code, EStaticMeshDerivedDataError::InvalidGeometry);
	EXPECT_EQ(Rejected.error().Kind, EStaticMeshRecipeKind::Collision);
	EXPECT_EQ(Rejected.error().VertexCount, 0u);
	EXPECT_EQ(Rejected.error().IndexCount, 2u);
	EXPECT_FALSE(Collision.Simple);
	EXPECT_FALSE(Collision.Complex);
}

TEST(FStaticMeshAuthoredCompilationTests, BuildErrorsOwnInputAndRejectedReservation)
{
	using namespace Durin;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshAuthoredBuildRequest Request{.Source = Source};
	Request.NormalizedSize = std::numeric_limits<float>::quiet_NaN();
	const auto Invalid = BuildStaticMeshAuthoredCandidate(Request);
	Request.NormalizedSize = 1.5f;
	ASSERT_FALSE(Invalid);
	EXPECT_EQ(Invalid.error().Code, EStaticMeshAuthoredBuildError::Input);
	EXPECT_NE(Invalid.error().Message.find("normalized size nan"), std::string::npos);
	const auto PayloadBytes = Source.GetGeometryBulk().GetPayloadSize();
	const auto Budget = BuildStaticMeshAuthoredCandidate(Request, {.MaximumWorkingSetBytes = 1});
	Request.Source = {};
	Source = {};
	ASSERT_FALSE(Budget);
	EXPECT_EQ(Budget.error().Code, EStaticMeshAuthoredBuildError::SourceBudget);
	EXPECT_NE(Budget.error().Message.find(std::format("Limit 1, accumulated 0, rejected {} x 8 bytes", PayloadBytes)), std::string::npos);
	EXPECT_LE(Budget.error().Message.size(), MaximumStaticMeshBuildDiagnosticBytes);
	const auto Cancelled = BuildStaticMeshAuthoredCandidate({}, {.ShouldCancel = [] { return true; }});
	ASSERT_FALSE(Cancelled);
	EXPECT_EQ(Cancelled.error().Code, EStaticMeshAuthoredBuildError::Cancelled);
}

TEST(FStaticMeshAuthoredCompilationTests, ApplicationFailureRetainsImportOwnershipThroughCompletion)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ApplicationCauseMesh"));
	const auto Invalid = ApplyStaticMeshAuthoredCandidate(*Mesh, {}, {});
	EXPECT_EQ(Invalid.error().Code, EStaticMeshApplicationError::InvalidCandidate);
	EXPECT_TRUE(Invalid.error().OwnerValid);
	EXPECT_FALSE(Invalid.error().CandidatePresent);
	EXPECT_FALSE(Invalid.error().RenderDataPresent);
	std::string Error;
	const auto SynchronousBuild4 = BuildStaticMeshSynchronously(*Mesh, MakeResidencyGeometry());
	ASSERT_TRUE(SynchronousBuild4) << Durin::FormatStaticMeshSynchronousError(SynchronousBuild4.error());
	auto* Other = NewObject<DStaticMesh>(nullptr, FName("ForeignImportOwner"));
	auto* ForeignImport = NewObject<DAssetImportData>(Other, FName("ForeignImport"));
	const auto* OriginalRender = Mesh->GetRenderData();
	const auto OriginalRevision = Mesh->GetRenderResourceStatus().Revision;
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Mesh->GetSource(),
		.PreparePublication = [ForeignImport](DStaticMesh&, DAssetImportData*& OutImport) -> std::expected<void, FStaticMeshApplicationError> {
			OutImport = ForeignImport;
			return {};
		}})) << Error;
	FAssetCompilingManager::Get().FinishAllCompilation();
	const auto Diagnostic = GetStaticMeshCompilationDiagnostic(*Mesh);
	EXPECT_EQ(Diagnostic.Status, EStaticMeshCompilationStatus::Failed);
	EXPECT_EQ(Diagnostic.Error.Code, EStaticMeshCompletionError::Application);
	ASSERT_TRUE(Diagnostic.Error.ApplicationCause);
	EXPECT_EQ(Diagnostic.Error.ApplicationCause->Code, EStaticMeshApplicationError::ImportOwnership);
	EXPECT_EQ(Diagnostic.Error.ApplicationCause->Owner, FObjectKey(Mesh));
	EXPECT_EQ(Diagnostic.Error.ApplicationCause->ImportData, FObjectKey(ForeignImport));
	EXPECT_EQ(Diagnostic.Error.ApplicationCause->ImportOuter, FObjectKey(Other));
	EXPECT_EQ(Mesh->GetRenderData(), OriginalRender);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, OriginalRevision);
	EXPECT_EQ(Mesh->GetAssetImportData(), nullptr);
	std::string RejectedClass = "RejectedImportData";
	ASSERT_TRUE(SubmitStaticMeshCompilation(*Mesh, {.Source = Mesh->GetSource(),
		.PreparePublication = [&RejectedClass](DStaticMesh& Target, DAssetImportData*&) -> std::expected<void, FStaticMeshApplicationError> {
			return std::unexpected(FStaticMeshApplicationError{.Code = EStaticMeshApplicationError::ImportAllocation,
				.Owner = FObjectKey(&Target), .ImportClass = RejectedClass});
		}}));
	FAssetCompilingManager::Get().FinishAllCompilation();
	RejectedClass.clear();
	const auto Prepared = GetStaticMeshCompilationDiagnostic(*Mesh);
	EXPECT_EQ(Prepared.Status, EStaticMeshCompilationStatus::Failed);
	EXPECT_EQ(Prepared.Error.Code, EStaticMeshCompletionError::Application);
	ASSERT_TRUE(Prepared.Error.ApplicationCause);
	EXPECT_EQ(Prepared.Error.ApplicationCause->Code, EStaticMeshApplicationError::ImportAllocation);
	EXPECT_EQ(Prepared.Error.ApplicationCause->ImportClass, "RejectedImportData");
	EXPECT_EQ(Mesh->GetRenderData(), OriginalRender);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, OriginalRevision);
}

TEST(FStaticMeshAuthoredCompilationTests, SynchronousFailureExposesBoundaryCodeAndMessage)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("SynchronousTypedFailure"));
	auto Geometry = MakeResidencyGeometry();
	Geometry.Meshes.front().Indices.back() = 77;
	const auto SourceFailure = BuildStaticMeshSynchronously(*Mesh, std::move(Geometry));
	ASSERT_FALSE(SourceFailure);
	EXPECT_EQ(SourceFailure.error().Code, EStaticMeshSynchronousError::Source);
	EXPECT_FALSE(SourceFailure.error().Message.empty());
	EXPECT_LE(SourceFailure.error().Message.size(), MaximumStaticMeshBuildDiagnosticBytes);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	const auto SubmissionFailure = BuildStaticMeshSynchronously(*Mesh, FStaticMeshSource{});
	ASSERT_FALSE(SubmissionFailure);
	EXPECT_EQ(SubmissionFailure.error().Code, EStaticMeshSynchronousError::Submission);
	EXPECT_FALSE(SubmissionFailure.error().Message.empty());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_FALSE(HasPendingStaticMeshCompilation(*Mesh));
}

TEST(FStaticMeshReplacementTests, ErrorsOwnRejectedSourceSlotAndUVValues)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("TypedReplacementValues"));
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto InvalidSource = Mesh->ReplaceSourceRenderData(Source, nullptr, {}, std::numeric_limits<float>::quiet_NaN());
	EXPECT_EQ(InvalidSource.error().Code, EStaticMeshReplacementError::Source);
	EXPECT_TRUE(InvalidSource.error().SourceValid);
	EXPECT_TRUE(std::isnan(InvalidSource.error().NormalizedSize));
	auto DuplicateRender = std::make_unique<FStaticMeshRenderData>();
	DuplicateRender->MaterialSlots = {{"Duplicate", 0}, {"Duplicate", 1}};
	std::vector<FMeshMaterialSlotDefinition> Slots{
		{.Name = FName("Duplicate")}, {.Name = FName("Duplicate")}};
	const auto Duplicate = Mesh->ReplaceRenderData(std::move(DuplicateRender), std::move(Slots));
	EXPECT_EQ(Duplicate.error().Code, EStaticMeshReplacementError::SlotName);
	EXPECT_EQ(Duplicate.error().Index, 1u);
	EXPECT_EQ(Duplicate.error().SlotName, "Duplicate");
	auto UVRender = std::make_unique<FStaticMeshRenderData>();
	UVRender->MaterialSlots = {{"Slot", 0}};
	UVRender->LODResources.emplace_back().NumTexCoords = MaxStaticMeshUVChannels + 1;
	const auto UV = Mesh->ReplaceRenderData(std::move(UVRender), {{.Name = FName("Slot")}});
	EXPECT_EQ(UV.error().Code, EStaticMeshReplacementError::UVChannels);
	EXPECT_EQ(UV.error().Index, 0u);
	EXPECT_EQ(UV.error().Actual, MaxStaticMeshUVChannels + 1);
	EXPECT_EQ(UV.error().Expected, MaxStaticMeshUVChannels);
	const auto Missing = Mesh->ReplaceRenderData(nullptr, {});
	EXPECT_EQ(Missing.error().Code, EStaticMeshReplacementError::MaterialSlots);
	EXPECT_FALSE(Missing.error().RenderDataPresent);
	EXPECT_EQ(Mesh->GetRenderDataUpdateError().Code, Missing.error().Code);
	EXPECT_EQ(Duplicate.error().SlotName, "Duplicate");
	EXPECT_TRUE(std::isnan(InvalidSource.error().NormalizedSize));
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
}

TEST(FStaticMeshCookedLoadTests, BlockingFailureRetainsDecodeCauseAcrossRetry)
{
	using namespace Durin;
	Testing::FScopedAssetRuntimeForTests Runtime;
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "TypedCookedFailure");
	std::filesystem::create_directories(Root);
	ASSERT_TRUE(Runtime.RestartCooked(Root));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("TypedCookedFailureMesh"));
	const FObjectKey Owner(Mesh);
	// Inject a malformed detached payload into the otherwise ordinary runtime object.
	auto& Payload = const_cast<FBulkData&>(Mesh->GetCookedRenderData());
	const FByteBuffer Bytes{std::byte{0xff}};
	{
		auto ValueResult = FBulkData::TryCreateDetached(Bytes);
		ASSERT_TRUE(ValueResult);
		Payload = std::move(*ValueResult);
	}
	const auto Failed = Mesh->EnsureRenderDataLoadedBlocking();
	ASSERT_FALSE(Failed);
	EXPECT_EQ(Failed.Status.CpuPhase, ECookedMeshCpuPhase::Failed);
	EXPECT_EQ(Failed.Error.Code, ECookedMeshLoadError::Product);
	EXPECT_EQ(Failed.Error.Owner, Owner);
	ASSERT_NE(Failed.Error.ProductCause, nullptr);
	EXPECT_EQ(Failed.Error.ProductCause->Code, ECookedMeshProductError::RenderArchive);
	EXPECT_TRUE(Failed.Error.ProductCause->ArchiveCode.has_value());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(InitializeCookedMeshLoadManager());
	const auto AsyncFailed = Mesh->EnsureRenderDataLoadedBlocking();
	EXPECT_FALSE(AsyncFailed);
	EXPECT_EQ(AsyncFailed.Error.Code, ECookedMeshLoadError::Product);
	EXPECT_EQ(AsyncFailed.Error.Owner, Owner);
	ASSERT_NE(AsyncFailed.Error.ProductCause, nullptr);
	EXPECT_EQ(AsyncFailed.Error.ProductCause->Code, ECookedMeshProductError::RenderArchive);
	ShutdownCookedMeshLoadManager();
	Payload = FBulkData{};
	const auto Retried = Mesh->EnsureRenderDataLoadedBlocking();
	EXPECT_FALSE(Retried);
	EXPECT_EQ(Retried.Error.Code, ECookedMeshLoadError::Unavailable);
	EXPECT_EQ(Retried.Error.Owner, Owner);
	EXPECT_GT(Retried.Status.Generation, Failed.Status.Generation);
	ASSERT_TRUE(Runtime.Restore());
	EXPECT_EQ(Failed.Error.Owner, Owner);
	EXPECT_EQ(Failed.Error.ProductCause->Code, ECookedMeshProductError::RenderArchive);
	EXPECT_FALSE(FormatCookedMeshLoadError(Failed.Error).empty());
}
