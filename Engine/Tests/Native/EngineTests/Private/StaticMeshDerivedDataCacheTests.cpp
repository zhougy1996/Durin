#include "StaticMesh/StaticMeshDerivedDataKeyTestSupport.h"

#include "StaticMesh/StaticMeshAttributes.h"
#include "Math/Operations.h"
#include "StaticMesh/StaticMeshImportSettings.h"
#include "StaticMesh/StaticMeshCompilationTestSupport.h"
#include "DerivedDataCache/DerivedDataCache.h"
#include "Threading/ThreadEvent.h"
#include "Runtime/Engine/Private/Physics/PhysicsCookDerivedDataKey.h"
#include "Runtime/Engine/Private/Asset/AssetBuildScheduler.h"
#include "../../../../Source/Developer/DerivedDataCache/Private/DerivedDataCacheStorage.h"
#include "Physics/PhysicsDerivedData.h"
#include "Physics/PhysicsMeshInputTask.h"
#include "Physics/PhysicsCookHelper.h"
#include "StaticMeshTestAccess.h"
#include "Diagnostics/StaticMeshPayloadInspection.h"
#include "StaticMesh/StaticMeshCustomVersion.h"
#include "DObject/PackageFormat.h"
#include "StaticMesh/StaticMeshBuildTestSupport.h"
#include <gtest/gtest.h>
#include "NativeAssetRuntimeTestSupport.h"

#include "NativeDObjectTestSupport.h"

#include "Asset/PackageSerialization.h"
#include "Asset/Mutation.h"
#include "Asset/AssetCook.h"
#include "StaticMesh/StaticMeshBuildPrivate.h"
#include "StaticMesh/StaticMeshSharedOutput.h"
#include "StaticMesh/StaticMeshSourceCodec.h"
#include "Physics/PhysicsBuildFunction.h"
#include "Physics/PhysicsCookInputPrivate.h"
#include "Physics/PhysicsSharedOutput.h"
#include "DerivedDataBuildSession.h"
#include "Asset/CookedMeshLoadManager.h"
#include "Asset/CookedMeshProducts.h"
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
#include "Serialization/BinaryFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "NativeTestSupport.h"
#include "Modules/ModuleManager.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/IMeshBuilderModule.h"
#include "StaticMesh/StaticMeshCompilation.h"
#include "Threading/RunnableThread.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "Runtime/Engine/Private/StaticMesh/StaticMeshDerivedDataKey.h"
#include "StaticMesh/StaticMeshResources.h"
#include "AssetForge/Builtins/StaticMeshImport.h"
#include "AssetForge/Builtins/StaticMeshFactory.h"
#include "StaticMesh/StaticMeshFactoryTestSupport.h"
#include "AssetForge/Builtins/StaticMeshImportData.h"
#include "Import/EditorReimportHandler.h"

using namespace StaticMeshBuildTestSupport;

using namespace Durin::Testing;

namespace
{
	inline auto GetCollisionKey(const Durin::FStaticMeshRenderData& Render)
	{
		using namespace Durin;
		const auto& LOD = Render.LODResources.front();
		FBinaryWriter Bytes;
		const auto& Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
		Bytes.WriteU64(Positions.size());
		for (const auto& Position : Positions)
			for (uint32 Axis = 0; Axis < 3; ++Axis) Bytes.WriteU32(std::bit_cast<uint32>(Position[Axis]));
		const auto& Indices = LOD.IndexBuffer.GetIndices();
		Bytes.WriteU64(Indices.size());
		for (auto Index : Indices) Bytes.WriteU32(Index);
		return MakePhysicsCookBuildAction({.GeometryHash = FXxHash128::HashBuffer(Bytes.GetBytes()),
			.SourceMode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0,
			.BuilderVersion = FCollisionCookedData::GetBuildVersion(),
			.TargetPlatform = EAssetPayloadTargetPlatform::Win64}).value().GetKey();
	}

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
		const Durin::FCacheKeyProxy Key = Durin::Testing::BuildStaticMeshDerivedDataKey({
			.SourceHash = Mesh.GetSource().GetIdentity(),
			.ReconciliationHash = Durin::BuildStaticMeshReconciliationHash(
				Mesh.GetMaterialSlots(), Mesh.GetNormalizedSize()),
			.BuilderVersion = Durin::IMeshBuilderModule::Get()->GetBuildVersion(),
			.MaterialSlotCount = uint32(Mesh.GetMaterialSlots().size()),
			.TargetPlatform = Durin::EAssetPayloadTargetPlatform::Win64}).value();
		EXPECT_TRUE(Key.IsValid());
		return Key.ToString();
	}

	auto SaveMeshFixtureText(std::string_view Text, const std::filesystem::path& Path)
	{
		return Durin::FFileHelper::SaveArrayToFile(std::as_bytes(std::span(Text.data(), Text.size())), Path);
	}

	auto ImportCacheFixture(std::string_view Name, bool bStandalone = false) -> FStaticMeshCacheFixture
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
		Fixture.SourcePath = Fixture.Root / (bStandalone ? "Sources/Mesh.obj" : "Sources/Mesh.gltf");
		std::filesystem::create_directories(Fixture.SourcePath.parent_path());
		if (bStandalone)
			EXPECT_TRUE(SaveMeshFixtureText("o Original\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", Fixture.SourcePath.generic_string()));
		else
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
		return Fixture.CacheRoot / "records" / "v4" / "StaticMesh"
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

	auto HasCacheError(const FCacheLogCapture& Capture, std::string_view Operation) -> bool
	{
		return Capture.Has(Operation);
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
namespace
{
	auto MakeAuthoredLODDescriptions() -> std::vector<Durin::FMeshDescription>
	{
		auto Primary = MakeResidencyGeometry();
		Primary.Sections.front().Positions = {{10, 10, 0}, {14, 10, 0}, {14, 14, 0}, {10, 14, 0}};
		Primary.Sections.front().Indices = {0, 1, 2, 0, 2, 3};
		auto Lower = MakeResidencyGeometry();
		Lower.Sections.front().Positions = {{11, 11, 0.5f}, {12, 11, 0.5f}, {11, 12, 0.5f}};
		return {std::move(Primary), std::move(Lower)};
	}

	auto BuildAuthoredLODs(Durin::DStaticMesh* Mesh, std::vector<Durin::FMeshDescription> LODs) -> void
	{
		Durin::FStaticMeshSource Source;
		ASSERT_TRUE(Source.InitializeLODs(std::move(LODs)));
		const auto Built = Mesh->BuildFromSource(Durin::EStaticMeshBuildMode::Synchronous,
			{.Source = Source, .PreparedMaterialSlots = Durin::FStaticMeshTestAccess::MakeSlots(Source)});
		ASSERT_TRUE(Built) << Built.error();
	}
}

TEST(FStaticMeshLODTests, SourceRoundTripsEveryLODAndRejectsInvalidReplacementAtomically)
{
	using namespace Durin;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.InitializeLODs(MakeAuthoredLODDescriptions()));
	EXPECT_EQ(Source.GetNumLODs(), 2u);
	EXPECT_EQ(Source.GetMeshCount(), 2u);
	const auto Identity = Source.GetIdentity();
	const auto Original = Source.AcquireLODs();
	ASSERT_TRUE(Original);
	auto Copy = Source;
	Source.ReleaseGeometry();
	EXPECT_FALSE(Source.IsGeometryResident());
	EXPECT_TRUE(Copy.IsGeometryResident());
	const auto Loaded = Source.AcquireLODs();
	ASSERT_TRUE(Loaded);
	ASSERT_EQ(Loaded->size(), 2u);
	for (uint32 LOD = 0; LOD < 2; ++LOD)
		EXPECT_EQ((*Loaded)[LOD]->Sections.front().Positions, (*Original)[LOD]->Sections.front().Positions);
	EXPECT_FALSE(Source.AcquireLODGeometry(2));
	const auto Cancelled = Source.AcquireLODs([] { return true; });
	ASSERT_FALSE(Cancelled);
	EXPECT_EQ(Cancelled.error().Code, EStaticMeshSourceError::Cancelled);
	auto Invalid = MakeAuthoredLODDescriptions();
	Invalid.back().Sections.front().Indices[0] = 99;
	EXPECT_FALSE(Source.InitializeLODs(std::move(Invalid)));
	EXPECT_EQ(Source.GetIdentity(), Identity);
	EXPECT_FALSE(Source.InitializeLODs({}));
	EXPECT_FALSE(Source.InitializeLODs(std::vector<FMeshDescription>(9, MakeResidencyGeometry())));
	EXPECT_EQ(Source.GetIdentity(), Identity);
	const auto Payload = Source.GetGeometryBulk().GetPayload().Wait();
	ASSERT_TRUE(Payload);
	FByteBuffer Corrupt(Payload->GetBytes().begin(), Payload->GetBytes().end());
	WriteU32(Corrupt, 4, 9);
	EXPECT_FALSE(StaticMeshPrivate::DecodeSourceLODs(Corrupt, Source.GetMaterialSlotCount(), Source.GetMeshCount()));
	Corrupt.assign(Payload->GetBytes().begin(), Payload->GetBytes().end());
	Corrupt.pop_back();
	EXPECT_FALSE(StaticMeshPrivate::DecodeSourceLODs(Corrupt, Source.GetMaterialSlotCount(), Source.GetMeshCount()));
	// The legacy one-LOD representation retains its original semantic identity.
	FStaticMeshSource Single;
	ASSERT_TRUE(Single.Initialize(MakeResidencyGeometry()));
	FStaticMeshSource SingleList;
	ASSERT_TRUE(SingleList.InitializeLODs({MakeResidencyGeometry()}));
	EXPECT_EQ(Single.GetIdentity(), SingleList.GetIdentity());
	FStaticMeshSource MaximumLODs;
	ASSERT_TRUE(MaximumLODs.InitializeLODs(std::vector<FMeshDescription>(8, MakeResidencyGeometry())));
	MaximumLODs.ReleaseGeometry();
	EXPECT_EQ(MaximumLODs.AcquireLODs()->size(), 8u);
}

TEST(FStaticMeshLODTests, BuildAndCacheKeepSharedCoordinatesAndMaterialsAcrossLODs)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshAuthoredLODCache", true);
	ASSERT_NE(Fixture.Mesh, nullptr);
	auto Descriptions = MakeAuthoredLODDescriptions();
	Descriptions.back().PolygonGroups.front() = {"LowerMaterial", 7, "LowerMaterial"};
	Descriptions.back().Sections.front().SourceMaterialIndex = 7;
	BuildAuthoredLODs(Fixture.Mesh, Descriptions);
	const auto* Render = Fixture.Mesh->GetRenderData();
	ASSERT_NE(Render, nullptr);
	ASSERT_EQ(Render->LODResources.size(), 2u);
	EXPECT_EQ(Render->LODResources[0].IndexBuffer.GetNumIndices(), 6u);
	EXPECT_EQ(Render->LODResources[1].IndexBuffer.GetNumIndices(), 3u);
	EXPECT_EQ(Render->LODResources[1].VertexBuffers.PositionVertexBuffer.GetPositions().front(), FVector3f(-0.375f, -0.375f, 0.1875f));
	EXPECT_GT(Render->LODResources[0].ScreenSize, Render->LODResources[1].ScreenSize);
	EXPECT_EQ(Render->MaterialSlots.size(), 2u);
	EXPECT_EQ(Render->LODResources[1].Sections.front().MaterialSlotIndex, 1u);
	const auto FirstKey = GetStaticMeshKey(*Fixture.Mesh);
	const auto FirstPositions = Render->LODResources[0].VertexBuffers.PositionVertexBuffer.GetPositions();
	const std::vector<FVector3f> SavedPositions(FirstPositions.begin(), FirstPositions.end());
	ASSERT_TRUE(Fixture.Mesh->Build(EStaticMeshBuildMode::Synchronous));
	EXPECT_EQ(GetStaticMeshKey(*Fixture.Mesh), FirstKey);
	ASSERT_EQ(Fixture.Mesh->GetRenderData()->LODResources.size(), 2u);
	Descriptions.back().Sections.front().Positions[0].z += 0.25f;
	BuildAuthoredLODs(Fixture.Mesh, std::move(Descriptions));
	EXPECT_NE(GetStaticMeshKey(*Fixture.Mesh), FirstKey);
	const auto& After = Fixture.Mesh->GetRenderData()->LODResources[0].VertexBuffers.PositionVertexBuffer.GetPositions();
	EXPECT_EQ(std::vector<FVector3f>(After.begin(), After.end()), SavedPositions);
	BuildAuthoredLODs(Fixture.Mesh, std::vector<FMeshDescription>(8, MakeResidencyGeometry()));
	EXPECT_EQ(Fixture.Mesh->GetRenderData()->LODResources.size(), 8u);
}

TEST(FStaticMeshLODTests, FileImportReimportsAllSourcesAndFailedLODLeavesAcceptedAssetUnchanged)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshAuthoredLODImport", true);
	ASSERT_NE(Fixture.Mesh, nullptr);
	const auto LowerPath = Fixture.Root / "Sources/Lower.obj";
	ASSERT_TRUE(SaveMeshFixtureText("o Lower\nv 0.25 0.25 0.2\nv 0.5 0.25 0.2\nv 0.25 0.5 0.2\nf 1 2 3\n", LowerPath));
	EXPECT_EQ(AssetForge::Builtins::ImportStaticMeshLODFromFile(*Fixture.Mesh, 2, LowerPath.generic_string()).Status, EReimportResult::Failed);
	const auto Imported = AssetForge::Builtins::ImportStaticMeshLODFromFile(*Fixture.Mesh, 1, LowerPath.generic_string());
	ASSERT_EQ(Imported.Status, EReimportResult::Succeeded) << Imported.Message;
	FinishMeshBuildForTests(*Fixture.Mesh);
	ASSERT_EQ(Fixture.Mesh->GetSource().GetNumLODs(), 2u);
	ASSERT_EQ(Fixture.Mesh->GetRenderData()->LODResources.size(), 2u);
	ASSERT_NE(Fixture.Mesh->GetAssetImportData()->GetSourceData().FindByRole("lod1"), nullptr);
	const auto Old = Fixture.Mesh->GetSource().AcquireLODGeometry(1);
	ASSERT_TRUE(Old);
	ASSERT_TRUE(SaveMeshFixtureText("o Lower\nv 0.3 0.3 0.4\nv 0.6 0.3 0.4\nv 0.3 0.6 0.4\nf 1 2 3\n", LowerPath));
	const auto Reimported = AssetForge::Builtins::ReimportStaticMesh(*Fixture.Mesh);
	ASSERT_EQ(Reimported.Status, EReimportResult::Succeeded) << Reimported.Message;
	FinishMeshBuildForTests(*Fixture.Mesh);
	EXPECT_NE(Fixture.Mesh->GetSource().AcquireLODGeometry(1).value()->Sections.front().Positions, (*Old)->Sections.front().Positions);
	const auto AcceptedIdentity = Fixture.Mesh->GetSource().GetIdentity();
	const auto* AcceptedRender = Fixture.Mesh->GetRenderData();
	ASSERT_TRUE(std::filesystem::remove(LowerPath));
	EXPECT_EQ(AssetForge::Builtins::ReimportStaticMesh(*Fixture.Mesh).Status, EReimportResult::Failed);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetIdentity(), AcceptedIdentity);
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), AcceptedRender);
	ASSERT_TRUE(SavePackage(Fixture.Mesh->GetPackage()));
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
	const auto Reloaded = LoadObject<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
	ASSERT_TRUE(Reloaded);
	Fixture.Mesh = *Reloaded;
	FinishMeshBuildForTests(*Fixture.Mesh);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetIdentity(), AcceptedIdentity);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetNumLODs(), 2u);
	EXPECT_EQ(Fixture.Mesh->GetRenderData()->LODResources.size(), 2u);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
}

TEST(FStaticMeshLODTests, CookedCollisionLODLoadsWithoutEditorSourceAndDrivesRuntimeSplineDeformation)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Fixture = ImportCacheFixture("StaticMeshCookedCollisionLOD", true);
	ASSERT_NE(Fixture.Mesh, nullptr);
	BuildAuthoredLODs(Fixture.Mesh, MakeAuthoredLODDescriptions());
	Fixture.Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Fixture.Mesh->SetLODForCollision(1);
	FinishMeshBuildForTests(*Fixture.Mesh);
	const auto Expected = Fixture.Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Expected);
	ASSERT_EQ(Fixture.Mesh->GetBodySetup()->GetResidentComplexGeometry().GetTriangleCount(), 1u);
	const auto CollisionInspection = InspectStaticMeshCollision(*Fixture.Mesh);
	EXPECT_TRUE(CollisionInspection.bTriangleCountsComparable);
	EXPECT_EQ(CollisionInspection.SourceTriangles, 1u);
	EXPECT_EQ(CollisionInspection.RemovedTriangles, 0u);
	const auto CookRoot = std::filesystem::absolute(Fixture.Root / "Cook");
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(SaveCookPackage(Fixture.Mesh->GetPackage(), "/Game/CollisionLOD", Context));
	ASSERT_TRUE(PublishCookContext(Context, CookRoot));
	Testing::RemoveTestWorkDirectory(Fixture.CacheRoot);
	Testing::FScopedAssetRuntimeForTests Runtime;
	ASSERT_TRUE(Runtime.RestartCooked(CookRoot));
	Testing::RegisterMountPointForTests("/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/CollisionLOD", Path));
	const auto Loaded = LoadObject<DStaticMesh>(Testing::MakeTopLevelAssetObjectPathForTests(Path, Fixture.AssetPath.GetPackageName()));
	ASSERT_TRUE(Loaded) << (Loaded ? std::string{} : Loaded.error().Message);
	auto* Mesh = *Loaded;
	EXPECT_FALSE(Mesh->GetSource().IsValid());
	EXPECT_EQ(Mesh->GetLODForCollision(), 1);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Mesh->GetPhysicsTriMeshData()->Positions, Expected->Positions);
	EXPECT_EQ(Mesh->GetPhysicsTriMeshData()->Indices, Expected->Indices);
	ASSERT_TRUE(Mesh->EnsureRenderDataLoadedBlocking());
	ASSERT_EQ(Mesh->GetRenderData()->LODResources.size(), 2u);
	EXPECT_EQ(Mesh->GetRenderData()->LODResources[0].IndexBuffer.GetNumIndices(), 6u);
	EXPECT_EQ(Mesh->GetRenderData()->LODResources[1].IndexBuffer.GetNumIndices(), 3u);
	EXPECT_EQ(Mesh->GetRenderData()->GetNumInitializedResources(), 0u);
	EXPECT_EQ(Mesh->GetBodySetup()->GetResidentComplexGeometry().GetTriangleCount(), 1u);
	Mesh->SetLODForCollision(0);
	EXPECT_EQ(Mesh->GetLODForCollision(), 1); // Cook fixes selection; there is no runtime source rebuild.
	auto* Component = NewObject<DSplineMeshComponent>(nullptr, "CookedCollisionLODSpline");
	Component->SetStaticMesh(Mesh);
	Component->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_TRUE(Component->GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_EQ(Component->GetBodySetup()->GetResidentComplexGeometry().GetTriangleCount(), 1u);
	EXPECT_DOUBLE_EQ(Component->GetDerivedState()->Params.SourceForwardMin, -0.75);
	EXPECT_DOUBLE_EQ(Component->GetDerivedState()->Params.SourceForwardMax, 0.75);
	EXPECT_EQ(Mesh->GetRenderData()->GetNumInitializedResources(), 0u);
	Component->SetStaticMesh(nullptr);
	MarkAsGarbage(Component);
	ASSERT_TRUE(UnloadPackage(Path));
	ASSERT_TRUE(Runtime.Restore());
}

TEST(FStaticMeshAuthoredCompilationTests, BuildRequiresAuthoredSlotsAndPreservesExistingMetadata)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("FixedBuildSlots"));
	const auto Geometry = MakeResidencyGeometry();
	EXPECT_FALSE(FStaticMeshTestAccess::BuildCandidate(Mesh, Geometry));
	EXPECT_EQ(Mesh->GetNumMaterialSlots(), 0u);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	auto Slots = FStaticMeshTestAccess::MakeSlots(Geometry);
	Slots[0].Name = FName("UserSlot");
	Slots[0].SourceName = "Authored source label";
	ASSERT_TRUE(FStaticMeshTestAccess::BuildCandidate(Mesh, Geometry, Slots));
	ASSERT_TRUE(FStaticMeshTestAccess::BuildCandidate(Mesh, Geometry));
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].Name, FName("UserSlot"));
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].SourceName, "Authored source label");
	EXPECT_EQ(Mesh->GetRenderData()->LODResources[0].Sections[0].MaterialSlotIndex, 0u);
	const auto* PreviousRender = Mesh->GetRenderData();
	const auto PreviousSource = Mesh->GetSource().GetIdentity();
	auto Changed = Geometry;
	Changed.PolygonGroups[0].SourceMaterialIndex = 7;
	Changed.Sections[0].SourceMaterialIndex = 7;
	EXPECT_FALSE(FStaticMeshTestAccess::BuildCandidate(Mesh, Changed));
	EXPECT_EQ(Mesh->GetRenderData(), PreviousRender);
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), PreviousSource);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].SourceMaterialIndex, 0u);
	Slots[0].Name = FName("Uncommitted");
	Changed.Sections[0].Indices[0] = 100;
	EXPECT_FALSE(FStaticMeshTestAccess::BuildCandidate(Mesh, Changed, Slots));
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].Name, FName("UserSlot"));
	EXPECT_EQ(Mesh->GetRenderData(), PreviousRender);
}

TEST(FStaticMeshAuthoredCompilationTests, PreparedSlotsAreDiscardedWhenOwnerChanges)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("PreparedSlotRejection"));
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	const auto* PreviousRender = Mesh->GetRenderData();
	auto Slots = CaptureStaticMeshReconciliation(*Mesh).MaterialSlots;
	Slots[0].Name = FName("ImportedSlot");
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Mesh->GetSource(), .PreparedMaterialSlots = Slots}));
	EXPECT_FALSE(CanJoinStaticMeshCompilation(*Mesh, Mesh->GetSource()));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(Mesh->RenameMaterialSlot(0, FName("UserEdit")));
	Barrier.Release();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_EQ(Mesh->GetCompilationDiagnostic().Status, EStaticMeshCompilationStatus::Superseded);
	EXPECT_EQ(Mesh->GetMaterialSlots()[0].Name, FName("UserEdit"));
	EXPECT_EQ(Mesh->GetRenderData(), PreviousRender);
	EXPECT_FALSE(Mesh->IsCompiling());
}

TEST(FStaticMeshDerivedDataCacheTests, EngineProviderPathPreservesKeysAndRecoversCorruption)
{
	FCacheLogCapture CollisionLog;
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshEngineProvider");
	ASSERT_NE(Fixture.Mesh, nullptr);
	const std::string BaselineKey = GetStaticMeshKey(*Fixture.Mesh);
	FStaticMeshBuildRequest Request{
		.Settings = MakeStaticMeshBuildSettings(Fixture.Mesh->GetMaterialSlots(), Fixture.Mesh->GetNormalizedSize()),
		.Source = Fixture.Mesh->GetSource()};
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Product;
	FCacheLogCapture RenderCacheLog;
	std::string Error;
	// The runtime loader carries only authored metadata until a cache miss.
	Request.Source.ReleaseGeometry();
	ASSERT_TRUE((Product = BuildRenderForTest(Request, {}, &RenderCacheLog))) << Error;
	EXPECT_TRUE(RenderCacheLog.empty());
	EXPECT_TRUE(Request.Source.IsValid());
	ASSERT_NE((*Product), nullptr);
	const std::array<std::byte, 4> Corrupt{};
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, GetObjectPath(Fixture, BaselineKey)));
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE((Product = BuildRenderForTest(Request, {}, &RenderCacheLog))) << Error;
	EXPECT_TRUE(Request.Source.IsValid());
	EXPECT_TRUE(HasCacheError(RenderCacheLog, "read"));
	EXPECT_TRUE(Error.empty());
	Request.Settings.MaterialSlots.clear();
	EXPECT_FALSE(BuildStaticMeshRenderData(Request));

	std::expected<FPhysicsCookResult, std::string> ColdCollision;
	ASSERT_TRUE((ColdCollision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(
		*Fixture.Mesh->GetRenderData(), EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)))) << Error;
	std::expected<FPhysicsCookResult, std::string> Collision;
	CollisionLog.Reset();
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)))) << Error;
	const auto CollisionPath = Fixture.CacheRoot / "records" / "v4" / "StaticMeshCollision"
		/ GetCollisionKey(*Fixture.Mesh->GetRenderData()).ToString().substr(0, 2)
		/ (GetCollisionKey(*Fixture.Mesh->GetRenderData()).ToString() + ".bin");
	ASSERT_TRUE(std::filesystem::remove(CollisionPath));
	CollisionLog.Reset();
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, false)))) << Error;
	EXPECT_FALSE(std::filesystem::exists(CollisionPath));
	const auto BlockedRoot = Fixture.Root / "BlockedCollisionCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(Corrupt, BlockedRoot));
	FPaths::SetDerivedDataCacheDirForTests(BlockedRoot.generic_string());
	// A read failure remains observable even when no Put is attempted.
	CollisionLog.Reset();
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, false)))) << Error;
	EXPECT_TRUE(Collision->Complex);
	EXPECT_TRUE(HasCacheError(CollisionLog, "read"));
	EXPECT_TRUE(Error.empty());
	CollisionLog.Reset();
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)))) << Error;
	EXPECT_TRUE(Collision->Complex);
	EXPECT_TRUE(HasCacheError(CollisionLog, "read"));
	EXPECT_LE(CollisionLog.size(), 2u);
	EXPECT_TRUE(HasCacheError(CollisionLog, "write"));
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
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Result;
	std::string Error;
	ASSERT_TRUE((Result = BuildRenderForTest({
		.Settings = MakeStaticMeshBuildSettings(Fixture.Mesh->GetMaterialSlots(), Fixture.Mesh->GetNormalizedSize()),
		.Source = Fixture.Mesh->GetSource()}))) << Error;
	ASSERT_FALSE((*Result)->LODResources.empty());
	(*Result)->LODResources.front().IndexBuffer.GetMutableIndices().front() =
		std::numeric_limits<uint32>::max();
	const auto Applied = FStaticMeshTestAccess::ReplaceSourceRenderData(Fixture.Mesh, Fixture.Mesh->GetSource(),
		std::move((*Result)), FStaticMeshTestAccess::MakeSlots(Fixture.Mesh->GetSource()), 1.5f);
	ASSERT_FALSE(Applied);
	EXPECT_FALSE(Applied.error().empty());
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetIdentity(), ImportedIdentity);
	EXPECT_GT(Fixture.Mesh->GetRenderResourceStatus().Revision, Revision);
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

	{
		auto LoadedValue = Durin::LoadObject<Durin::DStaticMesh>(Durin::Testing::MakePackageLeafAssetObjectPathForTests(Fixture.AssetPath));
		Fixture.Mesh = LoadedValue.value_or(nullptr);
		ASSERT_TRUE(LoadedValue);
	}
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
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
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
	ASSERT_NE(Fixture.Mesh->GetRenderData(), nullptr);
	ASSERT_TRUE(Durin::UnloadPackage(Fixture.AssetPath));
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
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
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
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
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
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
	ASSERT_EQ(Fixture.Mesh->GetCollisionBuildStatus(), Durin::EPhysicsMeshBuildStatus::Ready)
		<< (Fixture.Mesh->GetCollisionBuildError() ? *Fixture.Mesh->GetCollisionBuildError() : std::string{});
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
		ASSERT_TRUE(Durin::SaveCookPackage((*Fixture.Mesh).GetPackage(), "/Game/CookedCollisionMesh", Context)) << Error;
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
		Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
		ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedCollisionMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
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
		// Cold cooking retains recipe capacity; package loading allocates exact array sizes.
		EXPECT_LE(Geometry.GetRetainedBytes(), AuthoredBytes);
		for (uint32 Index = 0; Index < AuthoredVertices; ++Index)
			EXPECT_EQ(*Geometry.GetVertex(Index), *AuthoredGeometry.GetVertex(Index));
		for (uint32 Index = 0; Index < AuthoredTriangles; ++Index)
		{
			const auto& Actual = *Geometry.GetTriangle(Index);
			const auto& Expected = *AuthoredGeometry.GetTriangle(Index);
			EXPECT_EQ(Actual.First, Expected.First);
			EXPECT_EQ(Actual.Second, Expected.Second);
			EXPECT_EQ(Actual.Third, Expected.Third);
			EXPECT_EQ(Actual.SourceOrdinal, Expected.SourceOrdinal);
		}
		for (uint32 Index = 0; Index < AuthoredNodes; ++Index)
		{
			const auto& Actual = *Geometry.GetNode(Index);
			const auto& Expected = *AuthoredGeometry.GetNode(Index);
			EXPECT_EQ(Actual.Minimum, Expected.Minimum);
			EXPECT_EQ(Actual.Maximum, Expected.Maximum);
			EXPECT_EQ(Actual.First, Expected.First);
			EXPECT_EQ(Actual.CountOrSecond, Expected.CountOrSecond);
		}
		ASSERT_EQ(Geometry.GetLeafTriangleCount(), AuthoredGeometry.GetLeafTriangleCount());
		for (uint32 Index = 0; Index < Geometry.GetLeafTriangleCount(); ++Index)
			EXPECT_EQ(Geometry.GetLeafTriangle(Index), AuthoredGeometry.GetLeafTriangle(Index));
		ASSERT_TRUE(Durin::UnloadPackage(Path));
		ASSERT_TRUE(AssetRuntime.Restore());
		return;
	}
}

TEST(FStaticMeshDerivedDataCacheTests, ComplexProxyRoundTripsAndCookedSplineNeedsNeitherProxyNorEditorSource)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Primary = ImportCacheFixture("StaticMeshComplexProxyPrimary", true);
	auto Proxy = ImportCacheFixture("StaticMeshComplexProxySource", true);
	ASSERT_NE(Primary.Mesh, nullptr);
	ASSERT_NE(Proxy.Mesh, nullptr);
	auto ProxyGeometry = Proxy.Mesh->GetSource().AcquireGeometry();
	ASSERT_TRUE(ProxyGeometry);
	auto Changed = **ProxyGeometry;
	for (auto& Section : Changed.Sections)
		for (auto& Position : Section.Positions) Position = Position * 0.25f + FVector3f(0.25f, 0.25f, 0.1f);
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Proxy.Mesh, std::move(Changed)));
	ASSERT_TRUE(SavePackage(Proxy.Mesh->GetPackage()));
	ASSERT_TRUE(Primary.Mesh->SetComplexCollisionMesh(Proxy.Mesh));
	Primary.Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	FinishMeshBuildForTests(*Primary.Mesh);
	const auto Expected = Primary.Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Expected);
	ASSERT_TRUE(SavePackage(Primary.Mesh->GetPackage()));
	FObjectCookContext Dependencies;
	Primary.Mesh->OnCookEvent(ECookEvent::PlatformCookDependencies, Dependencies);
	EXPECT_TRUE(std::ranges::any_of(Dependencies.Dependencies, [&](const auto& Dependency) {
		return Dependency.Kind == ECookBuildDependencyKind::DirectPackage
			&& Dependency.LogicalName == Proxy.AssetPath.ToString();
	}));
	ASSERT_TRUE(UnloadPackage(Primary.AssetPath));
	const auto Reloaded = LoadObject<DStaticMesh>(Testing::MakePackageLeafAssetObjectPathForTests(Primary.AssetPath));
	ASSERT_TRUE(Reloaded);
	Primary.Mesh = *Reloaded;
	FinishMeshBuildForTests(*Primary.Mesh);
	ASSERT_NE(Primary.Mesh->GetComplexCollisionMesh(), nullptr);
	EXPECT_EQ(Primary.Mesh->GetPhysicsTriMeshData()->Positions, Expected->Positions);
	EXPECT_FALSE(Primary.Mesh->GetPackage()->IsDirty());
	const auto CookRoot = std::filesystem::absolute(Primary.Root / "Cook");
	// The primary need not enable static collision for runtime spline consumers.
	Primary.Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	auto* SelectedProxy = Primary.Mesh->GetComplexCollisionMesh();
	auto* EmptyProxy = NewObject<DStaticMesh>(nullptr, "UnusableCookProxy");
	ASSERT_TRUE(Primary.Mesh->SetComplexCollisionMesh(EmptyProxy));
	FCookContext RejectedCook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	EXPECT_FALSE(SaveCookPackage(Primary.Mesh->GetPackage(), "/Game/ComplexProxy", RejectedCook));
	ASSERT_TRUE(Primary.Mesh->SetComplexCollisionMesh(SelectedProxy));
	FCookContext Cook(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(SaveCookPackage(Primary.Mesh->GetPackage(), "/Game/ComplexProxy", Cook));
	ASSERT_TRUE(PublishCookContext(Cook, CookRoot));
	// Publish only the primary asset. Editor reference and both source trees are absent.
	EXPECT_FALSE(std::filesystem::exists(CookRoot / "Game/StaticMeshComplexProxySource"));
	Testing::RemoveTestWorkDirectory(Primary.CacheRoot);
	Testing::RemoveTestWorkDirectory(Proxy.CacheRoot);
	Testing::FScopedAssetRuntimeForTests AssetRuntime;
	ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
	Testing::RegisterMountPointForTests("/Game/", (CookRoot / "Game").generic_string() + "/");
	ASSERT_TRUE(RefreshAssetRegistry(EAssetRegistryScanMode::FullValidation));
	FPackagePath CookedPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/Game/ComplexProxy", CookedPath));
	const auto Loaded = LoadObject<DStaticMesh>(Testing::MakeTopLevelAssetObjectPathForTests(
		CookedPath, Primary.AssetPath.GetPackageName()));
	ASSERT_TRUE(Loaded) << (Loaded ? std::string{} : Loaded.error().Message);
	auto* Mesh = *Loaded;
	EXPECT_EQ(Mesh->GetComplexCollisionMesh(), nullptr);
	ASSERT_NE(Mesh->GetBodySetup(), nullptr);
	EXPECT_EQ(Mesh->GetBodySetup()->GetCollisionSourceMode(), EBodySetupCollisionSourceMode::None);
	EXPECT_FALSE(Mesh->GetSource().IsValid());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	const auto InputWithoutRender = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(InputWithoutRender);
	EXPECT_EQ(InputWithoutRender->Positions, Expected->Positions);
	EXPECT_EQ(InputWithoutRender->Indices, Expected->Indices);
	ASSERT_TRUE(Mesh->EnsureRenderDataLoadedBlocking());
	EXPECT_EQ(Mesh->GetRenderData()->GetNumInitializedResources(), 0u);
	auto* Spline = NewObject<DSplineMeshComponent>(nullptr, "CookedProxySpline");
	Spline->SetStaticMesh(Mesh);
	Spline->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	ASSERT_NE(Spline->GetBodySetup(), nullptr);
	EXPECT_TRUE(Spline->GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_TRUE(Spline->GetCollisionBuildError().empty());
	EXPECT_EQ(Mesh->GetRenderData()->GetNumInitializedResources(), 0u);
	Spline->SetStaticMesh(nullptr);
	MarkAsGarbage(Spline);
	ASSERT_TRUE(UnloadPackage(CookedPath));
	ASSERT_TRUE(AssetRuntime.Restore());
}

TEST(FStaticMeshDerivedDataCacheTests, ReimportedProxyUpdatesDependentsWithoutRepublishingVisualGeometry)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	auto Primary = ImportCacheFixture("StaticMeshProxyReimportPrimary", true);
	auto Proxy = ImportCacheFixture("StaticMeshProxyReimportSource", true);
	ASSERT_NE(Primary.Mesh, nullptr);
	ASSERT_NE(Proxy.Mesh, nullptr);
	ASSERT_TRUE(Primary.Mesh->SetComplexCollisionMesh(Proxy.Mesh));
	auto* Spline = NewObject<DSplineMeshComponent>(nullptr, "ReimportedProxySpline");
	Spline->SetStaticMesh(Primary.Mesh);
	Spline->SetSplineMeshCollisionMode(ESplineMeshCollisionMode::DeformedTriangleMesh);
	const auto OldInput = Primary.Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(OldInput);
	const auto OldRender = Spline->GetDerivedState();
	const auto OldBodyRevision = Spline->GetBodySetup()->GetRevision();
	ASSERT_TRUE(SaveMeshFixtureText("o Replacement\nv 0 0 0.2\nv 0.5 0 0.2\nv 0 0.5 0.2\nf 1 2 3\n",
		Proxy.SourcePath));
	const auto Reimported = AssetForge::Builtins::ReimportStaticMesh(*Proxy.Mesh);
	ASSERT_EQ(Reimported.Status, EReimportResult::Succeeded) << Reimported.Message;
	FinishMeshBuildForTests(*Proxy.Mesh);
	const auto NewInput = Primary.Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(NewInput);
	EXPECT_NE(NewInput->Positions, OldInput->Positions);
	EXPECT_EQ(Spline->GetDerivedState(), OldRender);
	EXPECT_GT(Spline->GetBodySetup()->GetRevision(), OldBodyRevision);
	EXPECT_TRUE(Spline->GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_TRUE(Spline->GetCollisionBuildError().empty());
	Spline->SetStaticMesh(nullptr);
	MarkAsGarbage(Spline);
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
	ASSERT_TRUE(Durin::SaveCookPackage((*Fixture.Mesh).GetPackage(), "/Game/CookedMesh", First)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(First, CookRoot)) << Error;

	Durin::FCookContext Second(
		Durin::ECookTargetPlatform::Win64,
		Durin::ECookTargetProfile::Game);
	ASSERT_TRUE(Durin::SaveCookPackage((*Fixture.Mesh).GetPackage(), "/Game/CookedMesh", Second)) << Error;
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
		Durin::Testing::FScopedAssetRuntimeForTests AssetRuntime;
		ASSERT_TRUE(AssetRuntime.RestartCooked(CookRoot));
		Durin::Testing::RegisterMountPointForTests(
			"/Game/", (CookRoot / "Game").generic_string() + "/");
		ASSERT_TRUE(Durin::RefreshAssetRegistry(
			Durin::EAssetRegistryScanMode::FullValidation));
		Durin::FPackagePath Path;
		ASSERT_TRUE(Durin::FPackagePath::TryCreate("/Game/CookedMesh", Path));
		Durin::DStaticMesh* CookedMesh = nullptr;
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

TEST(FStaticMeshAuthoredCompilationTests, DetachedBuildersRespectRenderAndCollisionBudgets)
{
	CheckDetachedBuildBudgets(false);
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
TEST(FStaticMeshBuildModuleTests, ReturnsDetachedCPUStreamsAndDiscardsCancelledProduct)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Source = std::make_shared<const FMeshDescription>(MakeResidencyGeometry());
	std::string Error;
	auto Module = IMeshBuilderModule::Get();
	ASSERT_TRUE(Module);
	FStaticMeshRenderData Product;
	const auto Result = Module->BuildRender(Product, {.Geometry = Source,
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}}, .NormalizedSize = 2.0f});
	ASSERT_TRUE(Result);
	Source.reset();
	ASSERT_EQ(Product.LODResources.size(), 1u);
	EXPECT_FALSE(Product.LODResources.front().VertexBuffers.PositionVertexBuffer.IsInitialized());
	EXPECT_TRUE(Product.LODVertexFactories.empty());
	const auto& LOD = Product.LODResources.front();
	const auto Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
	const auto Normals = LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetNormals();
	const auto Tangents = LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetTangents();
	const auto Colors = LOD.VertexBuffers.ColorVertexBuffer.GetColors();
	ASSERT_EQ(Positions.size(), 3u);
	EXPECT_EQ(Positions[0], FVector3f(-1, -1, 0));
	EXPECT_EQ(Positions[1], FVector3f(1, -1, 0));
	EXPECT_EQ(Positions[2], FVector3f(-1, 1, 0));
	EXPECT_TRUE(std::ranges::equal(LOD.IndexBuffer.GetIndices(), std::vector<uint32>{0, 1, 2}));
	EXPECT_EQ(Normals.size(), 3u);
	EXPECT_EQ(Tangents.size(), 3u);
	EXPECT_TRUE(std::ranges::equal(Colors, std::vector<FVector4f>(3, FVector4f(1.0f))));
	for (const auto& Channel : LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords())
		EXPECT_TRUE(std::ranges::equal(Channel, std::vector<FVector2f>(3, FVector2f(0.0f))));
	EXPECT_EQ(LOD.NumTexCoords, 0u);
	EXPECT_FALSE(LOD.bHasColorVertexData);
	EXPECT_EQ(LOD.ScreenSize, 0.0f);
	ASSERT_EQ(LOD.Sections.size(), 1u);
	EXPECT_EQ(LOD.Sections[0].Name, "Fixture");
	EXPECT_EQ(LOD.Sections[0].LocalBounds.Min, FVector3(-1, -1, 0));
	EXPECT_EQ(Product.LocalBounds.Max, FVector3(1, 1, 0));
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	const auto Cancelled = Module->BuildRender(Product, {.Control = Control});
	ASSERT_FALSE(Cancelled);
	EXPECT_EQ(Product.LODResources.size(), 1u);
}

TEST(FStaticMeshBuildModuleTests, CancellationDuringConstructionPreservesOutputWithoutErrorLog)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FCacheLogCapture LogSession;
	const auto Cursor = FLogger::Get().ReadRecords(0, 1).NewestAvailableSequence + 1;
	auto Geometry = MakeResidencyGeometry();
	for (uint32 Index = 0; Index < 1000; ++Index)
		Geometry.Sections.front().Indices.insert(Geometry.Sections.front().Indices.end(), {0, 1, 2});
	FStaticMeshRenderData Product;
	Product.MaterialSlots.push_back({"Previous", 7});
	uint32 Checks = 0;
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [&] { return ++Checks == 3; }};
	EXPECT_FALSE(IMeshBuilderModule::Get()->BuildRender(Product, {
		.Geometry = std::make_shared<const FMeshDescription>(std::move(Geometry)),
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}},
		.Control = Control}));
	EXPECT_EQ(Checks, 3u);
	EXPECT_TRUE(Product.LODResources.empty());
	ASSERT_EQ(Product.MaterialSlots.size(), 1u);
	EXPECT_EQ(Product.MaterialSlots.front().Name, "Previous");
	FLogger::Get().Flush();
	for (const auto& Record : FLogger::Get().ReadRecords(Cursor).Records)
		EXPECT_FALSE(Record.Module == "MeshBuilder" && Record.Level == ELogLevel::Error) << Record.Message;
}

TEST(FStaticMeshBuildModuleTests, ResolvesReorderedMaterialSlotsAndSkipsEmptySections)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Geometry = MakeResidencyGeometry();
	Geometry.PolygonGroups.push_back({.Name = "Second", .SourceMaterialIndex = 7});
	auto Second = Geometry.Sections.front();
	Second.Name = "Second";
	Second.SourceMaterialIndex = 7;
	Geometry.Sections.push_back(std::move(Second));
	Geometry.Sections.insert(Geometry.Sections.begin(), {.Name = "Empty"});
	FStaticMeshRenderData Product;
	ASSERT_TRUE(IMeshBuilderModule::Get()->BuildRender(Product, {
		.Geometry = std::make_shared<const FMeshDescription>(std::move(Geometry)),
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Second"), "Second", 7},
			FStaticMeshBuildMaterialSlot{FName("First"), "First", 0}}}));
	ASSERT_EQ(Product.LODResources.size(), 1u);
	const auto& LOD = Product.LODResources.front();
	ASSERT_EQ(LOD.Sections.size(), 2u);
	EXPECT_EQ(LOD.Sections[0].MaterialSlotIndex, 1u);
	EXPECT_EQ(LOD.Sections[1].MaterialSlotIndex, 0u);
	EXPECT_EQ(LOD.Sections[0].FirstIndex, 0u);
	EXPECT_EQ(LOD.Sections[1].FirstIndex, 3u);
	EXPECT_EQ(LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices(), 6u);
}

TEST(FStaticMeshBuildModuleTests, RejectsDuplicateAndMissingSourceMaterialsWithoutChangingOutput)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FCacheLogCapture LogSession;
	for (uint32 Scenario = 0; Scenario < 3; ++Scenario)
	{
		auto Geometry = MakeResidencyGeometry();
		if (Scenario == 0) Geometry.PolygonGroups.push_back(Geometry.PolygonGroups.front());
		if (Scenario == 1) Geometry.PolygonGroups.front().SourceMaterialIndex = 7;
		if (Scenario == 2) Geometry.Sections.front().SourceMaterialIndex = 7;
		FStaticMeshRenderData Product;
		Product.MaterialSlots.push_back({"Previous", 7});
		EXPECT_FALSE(IMeshBuilderModule::Get()->BuildRender(Product, {
			.Geometry = std::make_shared<const FMeshDescription>(std::move(Geometry)),
			.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}}})) << Scenario;
		EXPECT_TRUE(Product.LODResources.empty());
		ASSERT_EQ(Product.MaterialSlots.size(), 1u);
		EXPECT_EQ(Product.MaterialSlots.front().Name, "Previous");
	}
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
	std::array<FMeshDescriptionReadHandle, 16> Handles;
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
	std::weak_ptr<const FMeshDescription> Weak = Handles.front();
	Handles.fill({});
	EXPECT_FALSE(Weak.expired());
	Copy.ReleaseGeometry();
	EXPECT_TRUE(Weak.expired());
	const auto Old = Source.AcquireGeometry().value();
	auto Replacement = MakeResidencyGeometry();
	Replacement.Sections.front().Positions.front().x = 7;
	ASSERT_TRUE(Source.Initialize(std::move(Replacement)));
	EXPECT_NE(Source.GetIdentity(), Identity);
	EXPECT_EQ(Old->Sections.front().Positions.front().x, 0);
	EXPECT_EQ(Source.AcquireGeometry().value()->Sections.front().Positions.front().x, 7);
	std::jthread Releaser([&] { for (int Index = 0; Index < 32; ++Index) Source.ReleaseGeometry(); });
	for (int Index = 0; Index < 32; ++Index)
	{
		const auto Handle = Source.AcquireGeometry().value();
		ASSERT_TRUE(Handle) << Error;
		EXPECT_EQ(Handle->Sections.front().Positions.front().x, 7);
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
		auto& Mesh = Invalid.Sections.front();
		switch (Case)
		{
		case 0: Mesh.Indices.front() = 3; break;
		case 1: Mesh.Normals.resize(1); break;
		case 2: Mesh.UVChannels[3].resize(2); break;
		case 3: Mesh.Colors.resize(1); break;
		case 4: Mesh.Tangents.resize(1); break;
		case 5: Mesh.Name.resize(4097); break;
		case 6: Invalid.PolygonGroups.clear(); break;
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

TEST(FStaticMeshSourceResidencyTests, WarmCacheSkipsUnreadableBulkAndMissReturnsGenericFailure)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	const auto Fixture = ImportCacheFixture("StaticMeshUnreadableResidency");
	ASSERT_NE(Fixture.Mesh, nullptr);
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	FStaticMeshBuildRequest Request{.Settings = MakeStaticMeshBuildSettings(Fixture.Mesh->GetMaterialSlots(), Fixture.Mesh->GetNormalizedSize()),
		.Source = Fixture.Mesh->GetSource()};
	const auto Resource = AttachResidencyProbe(Request.Source, true);
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Product;
	FCacheLogCapture RenderCacheLog;
	std::string Error;
	ASSERT_TRUE((Product = BuildRenderForTest(Request, {}, &RenderCacheLog))) << Error;
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 0u);
	ASSERT_TRUE(std::filesystem::remove(GetObjectPath(Fixture, GetStaticMeshKey(*Fixture.Mesh))));
	const auto& FailedRead = (Product = BuildRenderForTest(Request, {}, &RenderCacheLog));
	EXPECT_FALSE(FailedRead);
	EXPECT_FALSE(FailedRead.error().empty());
	EXPECT_EQ(Resource->GetReadStats().RequestCount, 1u);
	EXPECT_FALSE(Request.Source.IsGeometryResident());
	EXPECT_FALSE((Product = BuildRenderForTest(Request, {}, &RenderCacheLog)));
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
	EXPECT_EQ(Copied->Sections.size(), Geometry->Sections.size());
	EXPECT_EQ(Copied->Sections.front().Indices, Geometry->Sections.front().Indices);
	MarkObjectHierarchyAsGarbage(Duplicate);
	ASSERT_TRUE(UnloadPackage(Path));
}

TEST(FAssetBuildCancellationTests, CopiesAndObserversShareMonotonicCancellation)
{
	using namespace Durin;
	uint32 Calls = 0;
	const FAssetBuildTaskContext Control{.Cancellation = [&] { return ++Calls == 1; }};
	const auto Copy = Control;
	EXPECT_TRUE(Copy.IsCancelled());
	EXPECT_TRUE(Control.Cancellation.IsCancelled());
	EXPECT_EQ(Calls, 1u);
	FCancellationToken Request;
	const auto NativeObserver = Request.WithObserver([] { return true; });
	EXPECT_TRUE(NativeObserver.IsCancelled());
	EXPECT_TRUE(Request.IsCancelled());
	FCancellationToken CrossThread;
	std::thread Worker([Copy = CrossThread] { Copy.Cancel(); });
	Worker.join();
	EXPECT_TRUE(CrossThread.IsCancelled());
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationTokenDiscardsProducts)
{
	using namespace Durin;
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Render;
	std::string Error;
	const Durin::FAssetBuildTaskContext RenderControl{.Cancellation = [] { return true; }};
	const auto& Cancelled = (Render = BuildRenderForTest({},
		RenderControl));
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(RenderControl.Cancellation.IsCancelled());
	EXPECT_FALSE(*Render);

	const auto& Invalid = (Render = BuildRenderForTest({}));
	ASSERT_FALSE(Invalid);

	EXPECT_FALSE(Invalid.error().empty());

	std::expected<FPhysicsCookResult, std::string> Collision;
	FStaticMeshRenderData EmptyRender;
	uint32 Checks = 0;
	const Durin::FAssetBuildTaskContext CollisionProbeControl{.Cancellation = [&] { return ++Checks == 2; }};
	const auto& LateCancellation = (Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(EmptyRender,
		EBodySetupCollisionSourceMode::None, false),
		CollisionProbeControl));
	ASSERT_TRUE(LateCancellation);
	EXPECT_TRUE(CollisionProbeControl.Cancellation.IsCancelled());
	EXPECT_FALSE(Collision->Simple || Collision->Complex);
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationInterruptsGeometryLoops)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests(
		(Testing::GetTestWorkDirectory() / "AuthoredCancellationCache").generic_string());
	FMeshDescription Input = MakeResidencyGeometry();
	auto& Section = Input.Sections.front();
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
		FAssetBuildTaskMetrics Metrics;
		std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Product;
		const Durin::FAssetBuildTaskContext Control{.Cancellation = [&] { return Metrics.CancellationCheckpoints >= StopAt; },
				.Metrics = &Metrics};
		const auto& Outcome = (Product = BuildRenderForTest(
			{.Source = Source, .bPersistDerivedData = false},
			Control));
		ASSERT_TRUE(Outcome) << StopAt;
		EXPECT_TRUE(Control.Cancellation.IsCancelled()) << StopAt;
		EXPECT_FALSE(*Product);
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

TEST(FStaticMeshAuthoredCompilationTests, PreparedRenderPublishesWithoutRebuildingOrReconstruction)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("PreparedRenderTest"));
	std::string Error;
	const auto SynchronousBuild1 = FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry());
	ASSERT_TRUE(SynchronousBuild1) << SynchronousBuild1.error();
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Mesh->GetCollisionBuildStatus(), Durin::EPhysicsMeshBuildStatus::Failed)
		<< (Mesh->GetCollisionBuildError() ? *Mesh->GetCollisionBuildError() : std::string{});
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	const auto PhysicsRevision = Mesh->GetBodySetup()->GetRevision();
	const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
	auto Built = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(Snapshot.MaterialSlots, Snapshot.NormalizedSize), .Source = Mesh->GetSource()});
	ASSERT_TRUE(Built) << Built.error();
	auto Candidate = std::move(*Built);
	const auto Start = std::chrono::steady_clock::now();
	ASSERT_TRUE(PublishStaticMeshRenderData(*Mesh, std::move(Candidate))) << Error;
	const auto Duration = std::chrono::steady_clock::now() - Start;
	FCollisionGeometryRef PublishedCollision;
	EXPECT_TRUE(Mesh->GetBodySetup()->BuildComplexGeometry(PublishedCollision));
	EXPECT_EQ(Mesh->GetBodySetup()->GetRevision(), PhysicsRevision);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_FALSE(Mesh->GetSource().IsGeometryResident());
	std::cout << "render_publication_ns="
		<< std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count() << std::endl;
}

TEST(FStaticMeshAuthoredCompilationTests, StaleRenderPreservesLiveState)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("PreparedRenderTest"));
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	const auto Snapshot = CaptureStaticMeshReconciliation(*Mesh);
	auto Built = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(Snapshot.MaterialSlots, Snapshot.NormalizedSize), .Source = Mesh->GetSource()});
	ASSERT_TRUE(Built) << Built.error();
	auto Candidate = std::move(*Built);
	ASSERT_TRUE(Mesh->RenameMaterialSlot(0, FName("Changed")));
	const auto* Original = Mesh->GetRenderData();
	const auto SourceIdentity = Mesh->GetSource().GetIdentity();
	const auto Revision = Mesh->GetRenderResourceStatus().Revision;
	const auto Outcome = CommitStaticMeshBuild(*Mesh, std::move(Candidate), Mesh->GetSource(), Snapshot,
		true);
	ASSERT_FALSE(Outcome);
	EXPECT_NE(Outcome.error().find("material bindings"), std::string::npos);
	EXPECT_NE(Outcome.error().find("slot 0"), std::string::npos);
	EXPECT_NE(Outcome.error().find(Snapshot.MaterialSlots.front().Name.ToString()), std::string::npos);
	EXPECT_NE(Outcome.error().find("current 'Changed'"), std::string::npos);
	EXPECT_EQ(Mesh->GetRenderData(), Original);
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), SourceIdentity);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Revision);
	ASSERT_TRUE(Mesh->RenameMaterialSlot(0, FName("AfterFailure")));
	EXPECT_NE(Outcome.error().find("current 'Changed'"), std::string::npos);
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationAbandonsDecodeScratch)
{
	using namespace Durin;
	auto Geometry = MakeResidencyGeometry();
	for (uint32 Index = 0; Index < 4096; ++Index)
		Geometry.Sections.front().Indices.insert(Geometry.Sections.front().Indices.end(), {0, 1, 2});
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
}

TEST(FStaticMeshAuthoredCompilationTests, CacheWarningsDoNotFailBuild)
{
	using namespace Durin;
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Render;
	FCacheLogCapture RenderCacheLog;

	FScopedDerivedDataCacheRestore RestoreCache;
	const auto CacheFile = Testing::GetTestWorkDirectory() / "StaticMeshBlockedCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	ASSERT_TRUE((Render = BuildRenderForTest({.Source = Source}, {}, &RenderCacheLog)));
	EXPECT_NE((*Render), nullptr);
	EXPECT_TRUE(HasCacheError(RenderCacheLog, "read"));
	EXPECT_TRUE(HasCacheError(RenderCacheLog, "write"));
	EXPECT_TRUE(Error.empty());
}

TEST(FStaticMeshAuthoredCompilationTests, CancellationDiscardsPayloadAndFinalizationScratch)
{
	using namespace Durin;
	FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests(
		(Testing::GetTestWorkDirectory() / "PayloadCancellationCache").generic_string());
	auto Input = MakeResidencyGeometry();
	auto& Section = Input.Sections.front();
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
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Render;
	FCacheLogCapture RenderCacheLog;
	ASSERT_TRUE((Render = BuildRenderForTest({.Source = Source}, {}, &RenderCacheLog))) << Error;
	std::expected<FPhysicsCookResult, std::string> Collision;
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*(*Render),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0)))) << Error;
	FStaticMeshPayloadData Payload;
	ASSERT_TRUE(MakeStaticMeshPayloadData(*(*Render), Payload));
	FPhysicsCollisionPayloadData CollisionPayload;
	ASSERT_TRUE(MakePhysicsCollisionPayloadData(Collision->Complex, CollisionPayload));

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
	EXPECT_FALSE(MakeStaticMeshPayloadData(*(*Render), Unchanged,
		[&] { return ++Checks == 8; }));
	EXPECT_EQ(Unchanged.MaterialSlotCount, 123u);
	EXPECT_TRUE(Unchanged.LODs.empty());
	Checks = 0;
	FCollisionGeometryRef UnchangedGeometry = Collision->Complex;
	EXPECT_FALSE(MakePhysicsCollisionGeometry(CollisionPayload, UnchangedGeometry,
		[&] { return ++Checks == 8; }));
	EXPECT_EQ(UnchangedGeometry.GetIdentity(), Collision->Complex.GetIdentity());
	Checks = 0;
	EXPECT_FALSE((*Render)->RecalculateBounds([&] { return ++Checks == 8; }));
	EXPECT_EQ(Checks, 8u);
	(*Render)->RecalculateBounds();

	FStaticMeshBuildRequest CachedRequest;
	CachedRequest.Source = Source;
	CachedRequest.Settings = MakeStaticMeshBuildSettings(FStaticMeshTestAccess::MakeSlots(Source));
	Checks = 0;
	const Durin::FAssetBuildTaskContext RenderControl{.Cancellation = [&] { return ++Checks == 8; }};
	const auto& CancelledRender = (Render = BuildRenderForTest(CachedRequest,
		RenderControl));
	ASSERT_TRUE(CancelledRender);
	EXPECT_TRUE(RenderControl.Cancellation.IsCancelled());
	EXPECT_FALSE(*Render);
	ASSERT_TRUE((Render = BuildRenderForTest(CachedRequest)));
	Checks = 0;
	const Durin::FAssetBuildTaskContext CollisionProbeControl{.Cancellation = [&] { ++Checks; return false; }};
	ASSERT_TRUE(FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*(*Render),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, true),
		CollisionProbeControl));
	ASSERT_GT(Checks, 10u);
	const auto StopCollisionAt = Checks / 2;
	Checks = 0;
	const Durin::FAssetBuildTaskContext CollisionControl{.Cancellation = [&] { return ++Checks == StopCollisionAt; }};
	const auto& CancelledCollision = (Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*(*Render),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, true),
		CollisionControl));
	ASSERT_TRUE(CancelledCollision);
	EXPECT_TRUE(CollisionControl.Cancellation.IsCancelled());
	EXPECT_FALSE(Collision->Simple || Collision->Complex);
}

TEST(FStaticMeshAuthoredCompilationTests, PreparedRenderPublishesAndDiscardsCheckpointCancellations)
{
	CheckRenderPublicationAndCancellation(false);
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
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())})) << Error;
	const auto FirstId = Mesh->GetCompilationDiagnostic().RequestId;
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())})) << Error;
	const auto SecondId = Mesh->GetCompilationDiagnostic().RequestId;
	ASSERT_TRUE(Barrier.Wait(2));
	const uint64 Reserved = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes;
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_NE(GetStaticMeshCompilationDiagnostic(FirstId).Phase, EStaticMeshCompilationPhase::Terminal);
	EXPECT_FALSE(GetStaticMeshCompilationDiagnostic(FirstId).Status);
	EXPECT_TRUE(GetStaticMeshCompilationDiagnostic(FirstId).Error.empty());
	EXPECT_EQ(2u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	EXPECT_EQ(Reserved, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
	const auto InvalidSource = Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {});
	ASSERT_FALSE(InvalidSource);
	EXPECT_FALSE(InvalidSource.error().empty());
	Barrier.Release();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, GetStaticMeshCompilationDiagnostic(FirstId).Status);
	EXPECT_TRUE(GetStaticMeshCompilationDiagnostic(FirstId).Error.empty());
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, GetStaticMeshCompilationDiagnostic(SecondId).Status);
	EXPECT_NE(nullptr, Mesh->GetRenderData());
	EXPECT_FALSE(Mesh->IsCompiling());
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(Mesh->GetCompilationDiagnostic().RequestId, SecondId);
}

TEST(FStaticMeshAuthoredCompilationTests, QueuedCancellationHasNoFailureOrAdmissionSlot)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	auto* First = NewObject<DStaticMesh>(nullptr, FName("QueuedCancellationFirst"));
	auto* Second = NewObject<DStaticMesh>(nullptr, FName("QueuedCancellationSecond"));
	auto* Queued = NewObject<DStaticMesh>(nullptr, FName("QueuedCancellationPending"));
	EXPECT_FALSE(Queued->GetCompilationDiagnostic().Status);
	for (auto* Mesh : {First, Second, Queued})
		ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous,
			{.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	ASSERT_TRUE(Barrier.Wait(2));
	const auto Pending = Queued->GetCompilationDiagnostic();
	EXPECT_EQ(Pending.Phase, EStaticMeshCompilationPhase::Queued);
	EXPECT_FALSE(Pending.Status);
	EXPECT_TRUE(Pending.Error.empty());
	const auto Before = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
	EXPECT_EQ(Before.AdmittedBuildCount, 2u);
	Queued->CancelCompilation();
	Queued->FinishCompilation();
	const auto Completed = GetStaticMeshCompilationDiagnostic(Pending.RequestId);
	EXPECT_EQ(Completed.Phase, EStaticMeshCompilationPhase::Terminal);
	EXPECT_EQ(Completed.Status, EStaticMeshCompilationStatus::Cancelled);
	EXPECT_TRUE(Completed.Error.empty());
	const auto After = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
	EXPECT_EQ(After.AdmittedBuildCount, 2u);
	EXPECT_EQ(After.OutstandingRecords, 2u);
	EXPECT_EQ(After.EstimatedWorkingSetBytes, Before.EstimatedWorkingSetBytes - Pending.EstimatedWorkingSetBytes);
	First->CancelCompilation();
	Second->CancelCompilation();
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerQueuesBeyondFormerRecordLimitAndDefersCancellation)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerCountBounds"));
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	std::vector<uint64> RequestIds;
	for (uint32 Index = 0; Index < 32; ++Index)
	{
		ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())})) << Error;
		RequestIds.push_back(Mesh->GetCompilationDiagnostic().RequestId);
	}
	EXPECT_EQ(32u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	RequestIds.push_back(Mesh->GetCompilationDiagnostic().RequestId);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords, 33u);
	Mesh->CancelCompilation();
	const auto BeforeExpiredPump = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
	const auto Expired = FAssetCompilingManager::Get().ProcessAsyncTasks({.MaximumCompletions = 5,
		.Deadline = std::chrono::steady_clock::now()});
	EXPECT_EQ(Expired.ProcessedCompletionCount, 0u);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, BeforeExpiredPump.EstimatedWorkingSetBytes);
	const auto Batch = FAssetCompilingManager::Get().ProcessAsyncTasks({.MaximumCompletions = 2});
	EXPECT_EQ(Batch.ProcessedCompletionCount, 2u);
	const auto LargerBatch = FAssetCompilingManager::Get().ProcessAsyncTasks({.MaximumCompletions = 5});
	EXPECT_EQ(LargerBatch.ProcessedCompletionCount, 5u);
	Barrier.Release();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	for (auto Id : RequestIds)
	{
		const auto Diagnostic = GetStaticMeshCompilationDiagnostic(Id);
		EXPECT_EQ(Diagnostic.Phase, EStaticMeshCompilationPhase::Terminal);
		EXPECT_NE(Diagnostic.Status, EStaticMeshCompilationStatus::Succeeded);
		EXPECT_TRUE(Diagnostic.Error.empty());
	}
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
	EXPECT_EQ(nullptr, Mesh->GetRenderData());
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerRejectsMutationAndDestructionDuringBuild)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerOwnerMutation"));
	std::string Error;
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	const auto* Original = Mesh->GetRenderData();
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(Mesh->Build(EStaticMeshBuildMode::Asynchronous, {}));
	const auto FirstId = Mesh->GetCompilationDiagnostic().RequestId;
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(Mesh->RenameMaterialSlot(0, "MutationWhileBuilding"));
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_NE(GetStaticMeshCompilationDiagnostic(FirstId).Phase, EStaticMeshCompilationPhase::Terminal);
	EXPECT_FALSE(GetStaticMeshCompilationDiagnostic(FirstId).Status);
	EXPECT_TRUE(GetStaticMeshCompilationDiagnostic(FirstId).Error.empty());
	EXPECT_EQ(Original, Mesh->GetRenderData());
	ASSERT_TRUE(Mesh->Build(EStaticMeshBuildMode::Asynchronous, {}));
	const auto SecondId = Mesh->GetCompilationDiagnostic().RequestId;
	ASSERT_TRUE(Barrier.Wait(2));
	Mesh->BeginDestroy();
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, GetStaticMeshCompilationDiagnostic(FirstId).Status);
	EXPECT_TRUE(GetStaticMeshCompilationDiagnostic(FirstId).Error.empty());
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, GetStaticMeshCompilationDiagnostic(SecondId).Status);
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerQueuesBeyondFormerByteLimitAndRejectsOversizedInput)
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
	constexpr uint32 Accepted = 6;
	for (uint32 Index = 0; Index < Accepted; ++Index)
		ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	const auto State = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
	EXPECT_EQ(Accepted, State.OutstandingRecords);
	EXPECT_GT(State.EstimatedWorkingSetBytes, 1024ull * 1024 * 1024);
	const auto Latest = InspectCompilationOperation(*Mesh).RequestId;
	FStaticMeshSource Oversized;
	ASSERT_TRUE(Oversized.Initialize(std::move(Geometry)));
	const auto OversizedResource = AttachReservationSource(Oversized, 9600000);
	ASSERT_TRUE(Oversized.IsValid());
	const auto Rejected = Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Oversized});
	ASSERT_FALSE(Rejected);
	EXPECT_FALSE(Rejected.error().empty());
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords, State.OutstandingRecords);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, State.EstimatedWorkingSetBytes);
	EXPECT_EQ(Latest, InspectCompilationOperation(*Mesh).RequestId);
	Mesh->CancelCompilation();
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
	EXPECT_FALSE(BuildStaticMeshRenderData({.Source = Source},
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
	{
		FStaticMeshWorkerBarrier Barrier;
		ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
		ASSERT_TRUE(Barrier.Wait(1));
		Barrier.Release();
		AssetPrivate::CreateStaticMeshCompilingManager()->Shutdown();
	}
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, Mesh->GetCompilationDiagnostic().Status);
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	ASSERT_TRUE(AssetPrivate::CreateStaticMeshCompilingManager()->Start());
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
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
		ASSERT_TRUE(Meshes[0]->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
		ASSERT_TRUE(Meshes[1]->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
		ASSERT_TRUE(Barrier.Wait(2));
		for (uint32 Index = 2; Index < 7; ++Index)
			ASSERT_TRUE(Meshes[Index]->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()), .Priority = EStaticMeshCompilationPriority::Interactive}));
		ASSERT_TRUE(Meshes[7]->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
		const uint64 BackgroundId = InspectCompilationOperation(*Meshes[7]).RequestId;
		AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([&](uint64 Id, EStaticMeshCompilationPhase Phase) {
			if (Phase == EStaticMeshCompilationPhase::Queued) Dispatches.push_back(Id);
		});
		// Keep the fairness assertion about queued submissions, without blocker continuations.
		Meshes[0]->CancelCompilation();
		Meshes[1]->CancelCompilation();
		Barrier.Release();
		FAssetCompilingManager::Get().FinishAllCompilation();
		ASSERT_EQ(6u, Dispatches.size());
		const auto Background = std::ranges::find(Dispatches, BackgroundId);
		ASSERT_NE(Background, Dispatches.end());
		// Interactive result-consumption turns can make the background submission run earlier.
		EXPECT_LE(std::distance(Dispatches.begin(), Background), 4);
	}
	for (uint32 Index = 0; Index < 130; ++Index)
	{
		ASSERT_TRUE(Meshes[0]->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
		StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Meshes[0]);
	}
	const auto Diagnostics = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
	EXPECT_EQ(128u, Diagnostics.RetainedDiagnostics);
	EXPECT_EQ(0u, InspectCompilationOperation(*Meshes[7]).RequestId);
	EXPECT_EQ(0u, Diagnostics.OutstandingRecords);
	EXPECT_EQ(0u, Diagnostics.EstimatedWorkingSetBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerDoesNotKeepUnloadedPackageAlive)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	const auto Fixture = ImportCacheFixture("ManagerPackageRetirement");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto Owner = FObjectKey(Fixture.Mesh);
	std::string Error;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(Fixture.Mesh->Build(EStaticMeshBuildMode::Asynchronous, {}));
	ASSERT_TRUE(Barrier.Wait(1));
	const auto RequestId = Fixture.Mesh->GetCompilationDiagnostic().RequestId;
	const auto Unloaded = UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved);
	EXPECT_TRUE(Unloaded) << Unloaded.Message;
	EXPECT_EQ(nullptr, ResolveObjectKey(Owner));
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, GetStaticMeshCompilationDiagnostic(RequestId).Status);
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, ManagerDiscardsSourceMutationAndAcceptsInitialCollisionEdits)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ManagerReflectedFacts"));
	FStaticMeshSource Source;
	std::string Error;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	FStaticMeshTestAccess::SetMaterialSlots(Mesh, FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source}));
	ASSERT_TRUE(Barrier.Wait(1));
	auto* Property = DStaticMesh::StaticClass()->FindPropertyByName("NormalizedSize");
	ASSERT_NE(nullptr, Property);
	*Property->ContainerPtrToValuePtr<float>(Mesh) = 3.0f;
	Barrier.Release();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, Mesh->GetCompilationDiagnostic().Status);
	EXPECT_EQ(EStaticMeshCompilationStatus::Superseded, InspectCompilationOperation(*Mesh).Status);
	EXPECT_FLOAT_EQ(3.0f, Mesh->GetNormalizedSize());
	EXPECT_EQ(nullptr, Mesh->GetRenderData());

	auto* Initial = NewObject<DStaticMesh>(nullptr, FName("ManagerInitialCollision"));
	FStaticMeshTestAccess::SetMaterialSlots(Initial, FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()));
	ASSERT_TRUE(Initial->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source}));
	Initial->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Initial->GetCollisionBuildStatus(), Durin::EPhysicsMeshBuildStatus::Failed)
		<< (Initial->GetCollisionBuildError() ? *Initial->GetCollisionBuildError() : std::string{});
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Initial);
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
	EXPECT_TRUE(Fixture.Mesh->IsCompiling());
	EXPECT_EQ(Original, Fixture.Mesh->GetRenderData());
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	Fixture.Mesh->PostLoad();
	EXPECT_EQ(Request, InspectCompilationOperation(*Fixture.Mesh).RequestId);
	Barrier.Release();
	const auto SynchronousBuild2 = Fixture.Mesh->Build(EStaticMeshBuildMode::Synchronous);
	EXPECT_TRUE(SynchronousBuild2) << SynchronousBuild2.error();
	EXPECT_EQ(Request, InspectCompilationOperation(*Fixture.Mesh).RequestId);
	EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
	EXPECT_TRUE(Fixture.Mesh->GetPackage()->IsDirty());
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Cancelled, InspectCompilationOperation(*Fixture.Mesh).Status);
}

TEST(FStaticMeshAuthoredCompilationTests, FactoryAdmissionRetainsMissingImportData)
{
	using namespace Durin;
	using namespace Durin::AssetForge::Builtins;
	InitializeDObjectSystem();
	auto* Factory = NewObject<DStaticMeshFactory>(nullptr, "ReimportAdmissionFactory");
	auto* Empty = NewObject<DStaticMesh>(nullptr, "MissingImportDataMesh");
	ASSERT_NE(Empty, nullptr);
	std::optional<FReimportResult> Result;
	uint32 Calls = 0;
	Factory->Reimport(*Empty, [&](FReimportResult Value) { ++Calls; Result = std::move(Value); });
	ASSERT_TRUE(Result);
	EXPECT_EQ(Calls, 1u);
	EXPECT_EQ(Result->Status, EReimportResult::Failed);
	const auto* Detail = dynamic_cast<const FStaticMeshFactoryError*>(Result->FactoryCause.get());
	ASSERT_NE(Detail, nullptr);
	EXPECT_FALSE(Detail->Diagnostic.empty());
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
	EXPECT_FALSE(Rejected.error().empty());
	const auto MissingPath = Fixture.SourcePath.generic_string() + ".missing";
	const auto Missing = CreateTransientStaticMeshFromFile(MissingPath, nullptr, "MissingTransientMesh");
	ASSERT_FALSE(Missing);
	EXPECT_NE(Missing.error().find(MissingPath), std::string::npos);
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
	ASSERT_TRUE(SaveCookPackage((*Fixture.Mesh).GetPackage(), "/Game/Projection", Context)) << Error;
	ASSERT_TRUE(Durin::PublishCookContext(Context, std::filesystem::absolute(Fixture.Root / "Cook"))) << Error;
	EXPECT_EQ(nullptr, Fixture.Mesh->GetRenderData());
	EXPECT_EQ(Revision, Fixture.Mesh->GetRenderResourceStatus().Revision);
	EXPECT_EQ(Identity, Fixture.Mesh->GetSource().GetIdentity());
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	ASSERT_TRUE(SerializeAssetPackageBytes(Fixture.Mesh->GetPackage(), After));
	EXPECT_EQ(Before, After);
	Barrier.Release();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
}

TEST(FStaticMeshAuthoredCompilationTests, ConcurrentRenderBuildsRetainCancelledBytesUntilWorkerRelease)
{
	CheckConcurrentRenderBuilds(false);
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
		FStaticMeshTestAccess::SetMaterialSlots(Meshes[Index], FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()));
		ASSERT_TRUE(Meshes[Index]->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .bPersistDerivedData = false}));
	}
	ASSERT_TRUE(Barrier.Wait(2));
	Meshes[0]->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Meshes[0]->GetCollisionBuildStatus(), Durin::EPhysicsMeshBuildStatus::Failed)
		<< (Meshes[0]->GetCollisionBuildError() ? *Meshes[0]->GetCollisionBuildError() : std::string{});
	Meshes[1]->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_NE(Meshes[1]->GetCollisionBuildStatus(), Durin::EPhysicsMeshBuildStatus::Failed)
		<< (Meshes[1]->GetCollisionBuildError() ? *Meshes[1]->GetCollisionBuildError() : std::string{});
	Meshes[1]->CancelCompilation();
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(32u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_NE(nullptr, Meshes[0]->GetRenderData());
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Meshes[0]).Status);
	EXPECT_EQ(nullptr, Meshes[1]->GetRenderData());
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
}

TEST(FStaticMeshAuthoredCompilationTests, DiagnosticsExposeColdWarmAndPersistenceFailureWithoutPollingIo)
{
	FCacheLogCapture CacheLog;
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("ManagerObservationContract");
	ASSERT_NE(nullptr, Fixture.Mesh);
	const auto Cold = InspectCompilationOperation(*Fixture.Mesh);
	EXPECT_EQ(Fixture.Mesh->GetSource().GetIdentity(), Cold.SourceIdentity);
	std::string Error;
	Fixture.Mesh->PostLoad();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
	const auto Warm = InspectCompilationOperation(*Fixture.Mesh);
	EXPECT_TRUE(CacheLog.empty());
	EXPECT_TRUE(Warm.Error.empty());
	const auto Revision = Fixture.Mesh->GetPackage()->GetEditRevision();
	for (uint32 Index = 0; Index < 10; ++Index)
		EXPECT_EQ(Warm.RequestId, InspectCompilationOperation(*Fixture.Mesh).RequestId);
	EXPECT_EQ(Revision, Fixture.Mesh->GetPackage()->GetEditRevision());
	EXPECT_FALSE(Fixture.Mesh->GetSource().IsGeometryResident());
	EXPECT_FALSE(Fixture.Mesh->IsCompiling());
	const auto CacheFile = Fixture.Root / "BlockedManagerCache";
	ASSERT_TRUE(FFileHelper::SaveArrayToFile(FByteBuffer{std::byte{1}}, CacheFile));
	FPaths::SetDerivedDataCacheDirForTests(CacheFile.generic_string());
	Fixture.Mesh->PostLoad();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Fixture.Mesh);
	const auto FailedCache = InspectCompilationOperation(*Fixture.Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, FailedCache.Status);
	EXPECT_TRUE(HasCacheError(CacheLog, "write"));
	EXPECT_TRUE(FailedCache.Error.empty());
	EXPECT_EQ(FailedCache.RenderBuilderVersion, IMeshBuilderModule::Get()->GetBuildVersion());
	EXPECT_LE(FormatStaticMeshCompilationDiagnostic(FailedCache).size(), 4096u);
	EXPECT_FALSE(Fixture.Mesh->GetPackage()->IsDirty());
	const auto Synchronous = Fixture.Mesh->Build(EStaticMeshBuildMode::Synchronous);
	EXPECT_TRUE(Synchronous);
	ASSERT_TRUE(Synchronous);
	EXPECT_EQ(Fixture.Mesh->GetCompilationDiagnostic().RequestId, FailedCache.RequestId);

}

TEST(FStaticMeshAuthoredCompilationTests, ManagerQualifiesAllChannelsManySectionsAndConvexLimits)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto Geometry = MakeResidencyGeometry();
	const auto Triangle = Geometry.Sections.front();
	Geometry.Sections.clear();
	for (uint32 Index = 0; Index < 128; ++Index)
	{
		auto& Section = Geometry.Sections.emplace_back(Triangle);
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
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()), .bPersistDerivedData = false}));
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	ASSERT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Mesh).Status);
	ASSERT_NE(nullptr, Mesh->GetRenderData());
	const auto* Original = Mesh->GetRenderData();
	EXPECT_EQ(MaxStaticMeshUVChannels, Original->LODResources.front().NumTexCoords);
	EXPECT_EQ(128u, Original->LODResources.front().Sections.size());
	auto* Body = NewObject<DBodySetup>(Mesh, FName("ConvexLimitBody"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::ConvexHullFromLOD0);
	ASSERT_TRUE(Mesh->SetBodySetup(Body));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()), .bPersistDerivedData = false}));
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_EQ(EStaticMeshCompilationStatus::Succeeded, InspectCompilationOperation(*Mesh).Status);
	EXPECT_NE(Original, Mesh->GetRenderData());
	EXPECT_EQ(EPhysicsMeshBuildStatus::Failed, Mesh->GetCollisionBuildStatus());
	EXPECT_EQ(0u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes);
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
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	ASSERT_TRUE(Barrier.Wait(1));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
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
	EXPECT_EQ(1u, StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords);
	EXPECT_GT(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
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
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto& Source = const_cast<FStaticMeshSource&>(Fixture.Mesh->GetSource());
	const auto Resource = AttachResidencyProbe(Source, true);
	const auto Identity = Source.GetIdentity();
	const auto Revision = Fixture.Mesh->GetRenderResourceStatus().Revision;
	const auto* Cpu = Fixture.Mesh->GetRenderData();
	const auto Dirty = Fixture.Mesh->GetPackage()->IsDirty();
	const auto Before = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
	const auto ObjectCount = GDObjectArray.GetNum();
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
	EXPECT_EQ(Fixture.Mesh->GetRenderData(), Cpu);
	EXPECT_EQ(Fixture.Mesh->GetPackage()->IsDirty(), Dirty);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords, Before.OutstandingRecords);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, Before.EstimatedWorkingSetBytes);
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
	ASSERT_NE(Fixture.Mesh->GetCollisionBuildStatus(), Durin::EPhysicsMeshBuildStatus::Failed)
		<< (Fixture.Mesh->GetCollisionBuildError() ? *Fixture.Mesh->GetCollisionBuildError() : std::string{});
	const auto CookRoot = std::filesystem::absolute(Fixture.Root / "Cook");
	FCookContext Context(ECookTargetPlatform::Win64, ECookTargetProfile::Game);
	ASSERT_TRUE(SaveCookPackage((*Fixture.Mesh).GetPackage(), "/Game/InspectionMesh", Context)) << Error;
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
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	ASSERT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	const auto Revision = Mesh->GetRenderResourceStatus().Revision;
	EXPECT_FALSE(Durin::FStaticMeshTestAccess::ReplaceRenderData(Mesh, nullptr, {}));
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_GT(Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_EQ(Mesh->GetRenderDataLoadStatus().CpuPhase, ECookedMeshCpuPhase::Failed);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
	EXPECT_FALSE(Mesh->GetBodySetup()->GetResidentComplexGeometry());
	Mesh->RebuildCollision();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Pending);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_TRUE(Mesh->GetBodySetup()->GetResidentComplexGeometry());

	FStaticMeshSource Source;
	std::string Error;
	auto Geometry = MakeResidencyGeometry();
	Geometry.Sections.front().Positions.front().x = 7;
	ASSERT_TRUE(Source.Initialize(std::move(Geometry)));
	EXPECT_FALSE(Durin::FStaticMeshTestAccess::ReplaceSourceRenderData(Mesh, Source, nullptr, {}, 2.0f));
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), Source.GetIdentity());
	EXPECT_EQ(Mesh->GetNormalizedSize(), 2.0f);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);

	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Product;
	ASSERT_TRUE((Product = BuildRenderForTest({
		.Settings = MakeStaticMeshBuildSettings(Mesh->GetMaterialSlots(), Mesh->GetNormalizedSize()),
		.Source = Source}))) << Error;
	ASSERT_TRUE(Durin::FStaticMeshTestAccess::ReplaceSourceRenderData(Mesh, Source, std::move((*Product)),
		CaptureStaticMeshReconciliation(*Mesh).MaterialSlots, Mesh->GetNormalizedSize()));
	EXPECT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Mesh->GetRenderDataLoadStatus().CpuPhase, ECookedMeshCpuPhase::CpuReady);
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_FALSE(Mesh->GetCollisionBuildError());
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath, EAssetPackageUnloadPolicy::DiscardUnsaved));
}

TEST(FStaticMeshReplacementTests, CollisionConfigurationCanPrecedeCpuData)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, "DeferredCollisionMesh");
	Mesh->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::ComplexOnly);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
	EXPECT_FALSE(Mesh->GetCollisionBuildError());
	EXPECT_EQ(Mesh->GetBodySetup()->GetCollisionQueryPolicy(), EBodySetupCollisionQueryPolicy::ComplexOnly);
	Mesh->RebuildCollision();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Failed);
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_FALSE(Mesh->GetCollisionBuildError());
	MarkObjectHierarchyAsGarbage(Mesh);
}

TEST(FStaticMeshRenderBuildTests, RejectionLogsMeshIndexAndPreservesOutput)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Source = std::make_shared<FMeshDescription>(MakeResidencyGeometry());
	Source->Sections[0].Name = "RejectedMesh";
	Source->Sections[0].Indices[1] = 99;
	const auto Module = IMeshBuilderModule::Get();
	ASSERT_TRUE(Module);
	FCacheLogCapture LogSession;
	const auto Cursor = FLogger::Get().ReadRecords(0, 1).NewestAvailableSequence + 1;
	FStaticMeshRenderData Product;
	ASSERT_TRUE(Module->BuildRender(Product, {.Geometry = std::make_shared<const FMeshDescription>(MakeResidencyGeometry()),
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}}}));
	const auto* Positions = Product.LODResources.front().VertexBuffers.PositionVertexBuffer.GetPositions().data();
	const auto Bounds = Product.LocalBounds;
	const auto Result = Module->BuildRender(Product, {.Geometry = Source,
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}}});
	EXPECT_FALSE(Result);
	Source.reset();
	ASSERT_EQ(Product.LODResources.size(), 1u);
	EXPECT_EQ(Product.LODResources.front().VertexBuffers.PositionVertexBuffer.GetPositions().data(), Positions);
	EXPECT_EQ(Product.LocalBounds.Min, Bounds.Min);
	EXPECT_EQ(Product.LocalBounds.Max, Bounds.Max);
	ASSERT_EQ(Product.MaterialSlots.size(), 1u);
	EXPECT_EQ(Product.MaterialSlots.front().Name, "Material");
	FLogger::Get().Flush();
	const auto Records = FLogger::Get().ReadRecords(Cursor).Records;
	const auto Record = std::ranges::find_if(Records, [](const auto& Value) {
		return Value.Module == "MeshBuilder" && Value.Level == ELogLevel::Error;
	});
	ASSERT_NE(Record, Records.end());
	EXPECT_TRUE(Record->Message.contains("out-of-range index"));
	EXPECT_TRUE(Record->Message.contains("RejectedMesh"));
	EXPECT_TRUE(Record->Message.contains("index 1, actual 99, expected 3"));
}

TEST(FPhysicsCookTests, InvalidInputCancellationAndBudgetFailWithoutInstallingGeometry)
{
	using namespace Durin;
	FCookBodySetupInfo Info{.TriangleMeshDesc = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 99}},
		.Mode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, .bPersistDerivedData = false};
	const auto Failed = FPhysicsCookHelper::Cook(Info);
	ASSERT_FALSE(Failed);
	EXPECT_NE(Failed.error().find("Cook:"), std::string::npos);

	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	const auto Cancelled = FPhysicsCookHelper::Cook(Info, Control);
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());
	const auto Budget = FPhysicsCookHelper::Cook(Info, {.MaximumWorkingSetBytes = 1});
	ASSERT_FALSE(Budget);
	EXPECT_NE(Budget.error().find("Cook:"), std::string::npos);
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
	Invalid.Sections[0].Name = "RejectedSource";
	Invalid.Sections[0].Positions[1].z = std::numeric_limits<float>::infinity();
	const auto NonFinite = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(NonFinite);
	EXPECT_EQ(NonFinite.error().Code, EStaticMeshSourceError::NonFinitePosition);
	EXPECT_EQ(NonFinite.error().MeshName, "RejectedSource");
	EXPECT_EQ(NonFinite.error().Index, 1u);
	EXPECT_TRUE(std::isinf(NonFinite.error().Position.z));
	Invalid = MakeResidencyGeometry();
	Invalid.Sections[0].Indices.clear();
	const auto Empty = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(Empty);
	EXPECT_EQ(Empty.error().Code, EStaticMeshSourceError::EmptyMesh);
	EXPECT_EQ(Empty.error().Field, "Indices");
	EXPECT_EQ(Empty.error().Actual, 0u);
	EXPECT_EQ(Empty.error().Expected, 1u);
	Invalid = MakeResidencyGeometry();
	Invalid.Sections[0].Indices.pop_back();
	const auto Triangles = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(Triangles);
	EXPECT_EQ(Triangles.error().Code, EStaticMeshSourceError::TriangleList);
	EXPECT_EQ(Triangles.error().Actual, 2u);
	EXPECT_EQ(Triangles.error().Expected, 3u);
	Invalid = MakeResidencyGeometry();
	Invalid.PolygonGroups.push_back(Invalid.PolygonGroups.front());
	const auto Duplicate = Source.Initialize(std::move(Invalid));
	EXPECT_FALSE(Duplicate);
	EXPECT_EQ(Duplicate.error().Code, EStaticMeshSourceError::DuplicateMaterial);
	EXPECT_EQ(Duplicate.error().Actual, 0u);
	EXPECT_EQ(Source.GetIdentity(), Identity);
	EXPECT_EQ(Source.AcquireGeometry().value(), Original);
	EXPECT_EQ(Original->Sections[0].Name, "Fixture");
}

TEST(FStaticMeshDerivedDataCacheTests, PayloadRebuildLogsRenderAndCollisionCacheIssues)
{
	FCacheLogCapture CollisionLog;
	using namespace Durin;
	const FScopedDerivedDataCacheRestore CacheRestore;
	FStaticMeshCacheFixture Fixture = ImportCacheFixture("StaticMeshTypedCacheRejection");
	ASSERT_NE(Fixture.Mesh, nullptr);
	FStaticMeshBuildRequest Request{.Settings = MakeStaticMeshBuildSettings(Fixture.Mesh->GetMaterialSlots(), Fixture.Mesh->GetNormalizedSize()),
		.Source = Fixture.Mesh->GetSource()};
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Render;
	FCacheLogCapture RenderCacheLog;
	std::string Error;
	ASSERT_TRUE((Render = BuildRenderForTest(Request, {}, &RenderCacheLog)));
	ASSERT_TRUE(std::filesystem::remove(GetObjectPath(Fixture, BuildStaticMeshDerivedDataKey({.SourceHash = Request.Source.GetIdentity(),
		.ReconciliationHash = BuildStaticMeshReconciliationHash(Request.Settings.MaterialSlots, Request.Settings.NormalizedSize),
		.BuilderVersion = IMeshBuilderModule::Get()->GetBuildVersion(),
		.MaterialSlotCount = uint32(Request.Settings.MaterialSlots.size()),
		.TargetPlatform = EAssetPayloadTargetPlatform::Win64}).value().ToString())));
	const std::array<std::byte, 4> Invalid{};
	const auto SeedKey = BuildStaticMeshDerivedDataKey({.SourceHash = Request.Source.GetIdentity(),
		.ReconciliationHash = BuildStaticMeshReconciliationHash(Request.Settings.MaterialSlots, Request.Settings.NormalizedSize),
		.BuilderVersion = IMeshBuilderModule::Get()->GetBuildVersion(),
		.MaterialSlotCount = uint32(Request.Settings.MaterialSlots.size()),
		.TargetPlatform = EAssetPayloadTargetPlatform::Win64}).value();
	ASSERT_TRUE(DerivedData::GetCacheStorage().Put({*SeedKey.AsCacheKey(), Invalid, MaximumStaticMeshPayloadBytes, DerivedData::ECacheStorageNamespace::Records}));
	ASSERT_TRUE((Render = BuildRenderForTest(Request, {}, &RenderCacheLog))) << Error;
	ASSERT_EQ(RenderCacheLog.size(), 1u);
	const auto RenderRejection = RenderCacheLog.front();
	EXPECT_NE(RenderRejection.Message.find("cache read"), std::string::npos);
	EXPECT_NE(RenderRejection.Message.find("manifest"), std::string::npos);
	ASSERT_TRUE((Render = BuildRenderForTest(Request, {}, &RenderCacheLog)));
	EXPECT_TRUE(RenderCacheLog.empty());

	std::expected<FPhysicsCookResult, std::string> Collision;
	CollisionLog.Reset();
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0))));
	const auto Key = GetCollisionKey(*Fixture.Mesh->GetRenderData());
	const auto Path = Fixture.CacheRoot / "records" / "v4" / "StaticMeshCollision"
		/ Key.ToString().substr(0, 2) / (Key.ToString() + ".bin");
	ASSERT_TRUE(std::filesystem::remove(Path));
	ASSERT_TRUE(DerivedData::GetCacheStorage().Put({Key, Invalid, MaximumPhysicsCollisionPayloadBytes, DerivedData::ECacheStorageNamespace::Records}));
	CollisionLog.Reset();
	ASSERT_TRUE((Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(*Fixture.Mesh->GetRenderData(),
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0))));
	ASSERT_EQ(CollisionLog.size(), 1u);
	const auto CollisionRejection = CollisionLog.front();
	Collision = {};
	EXPECT_NE(CollisionRejection.Message.find("cache read"), std::string::npos);
	EXPECT_NE(CollisionRejection.Message.find("manifest"), std::string::npos);
	EXPECT_NE(RenderRejection.Message.find("cache read"), std::string::npos);
	ASSERT_TRUE(UnloadPackage(Fixture.AssetPath));
}

TEST(FStaticMeshDerivedDataCacheTests, ColdAndWarmAssemblyRepairFiniteIncorrectBounds)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Info = FModuleManager::Get().FindModule("MeshBuilder");
	struct FRestoreImplementation
	{
		FModuleManager::FModuleInfoPtr Info;
		std::unique_ptr<IModuleInterface> Original;
		~FRestoreImplementation()
		{
			FAssetCompilingManager::Get().FinishAllCompilation();
			Info->Module = std::move(Original);
		}
	} Restore{Info, std::move(Info->Module)};
	class FIncorrectBoundsModule final : public IMeshBuilderModule
	{
	public:
		IMeshBuilderModule& Original;
		uint32 BuildCount = 0;
		explicit FIncorrectBoundsModule(IMeshBuilderModule& InOriginal) : Original(InOriginal) {}
		auto GetBuildVersion() const -> uint64 override { return 778; }
		auto BuildRender(FStaticMeshRenderData& Render, const FStaticMeshBuildParameters& Parameters) -> bool override
		{
			++BuildCount;
			if (!Original.BuildRender(Render, Parameters)) return false;
			Render.LocalBounds = FBox(FVector3(100), FVector3(101));
			for (auto& LOD : Render.LODResources)
			{
				LOD.LocalBounds = Render.LocalBounds;
				for (auto& Section : LOD.Sections) Section.LocalBounds = Render.LocalBounds;
			}
			return true;
		}
	};
	auto Implementation = std::make_unique<FIncorrectBoundsModule>(static_cast<IMeshBuilderModule&>(*Restore.Original));
	auto& Module = *Implementation;
	Info->Module = std::move(Implementation);
	const FScopedDerivedDataCacheRestore CacheRestore;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "IncorrectBoundsCache").generic_string());
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	for (uint32 Attempt = 0; Attempt < 2; ++Attempt)
	{
		auto Render = BuildRenderForTest({.Source = Source});
		ASSERT_TRUE(Render) << Render.error();
		EXPECT_EQ((*Render)->LocalBounds.Min, FVector3(-0.75, -0.75, 0));
		EXPECT_EQ((*Render)->LocalBounds.Max, FVector3(0.75, 0.75, 0));
		EXPECT_EQ((*Render)->LODResources.front().LocalBounds.Min, (*Render)->LocalBounds.Min);
		EXPECT_EQ((*Render)->LODResources.front().Sections.front().LocalBounds.Max, (*Render)->LocalBounds.Max);
	}
	EXPECT_EQ(Module.BuildCount, 1u);
}

TEST(FStaticMeshDerivedDataCacheTests, BuildBoundariesTranslateModuleFailureAndCancellation)
{
	using namespace Durin;
	class FInvalidProductModule final : public IMeshBuilderModule
	{
	public:
		bool bFail = false;
		auto GetBuildVersion() const -> uint64 override
		{
			return 777;
		}
		auto BuildRender(FStaticMeshRenderData&,
			const FStaticMeshBuildParameters&) -> bool override
		{
			return !bFail;
		}
	};
	const FScopedDerivedDataCacheRestore CacheRestore;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "InvalidProductCache").generic_string());
	FAssetCompilingManager::Get().FinishAllCompilation();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Info = FModuleManager::Get().FindModule("MeshBuilder");
	struct FRestoreImplementation
	{
		FModuleManager::FModuleInfoPtr Info;
		std::unique_ptr<IModuleInterface> Original;
		~FRestoreImplementation()
		{
			FAssetCompilingManager::Get().FinishAllCompilation();
			Info->Module = std::move(Original);
		}
	} Restore{Info, std::move(Info->Module)};
	auto Implementation = std::make_unique<FInvalidProductModule>();
	auto& Module = *Implementation;
	Info->Module = std::move(Implementation);
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Product;
	std::string Error;
	const auto& Outcome = (Product = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false}));
	EXPECT_FALSE(Outcome);
	ASSERT_FALSE(Outcome);

	EXPECT_FALSE(Outcome.error().empty());
	EXPECT_FALSE(Product);
	const auto Authored = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())),
		.Source = Source, .bPersistDerivedData = false});
	ASSERT_FALSE(Authored);
	EXPECT_EQ(Authored.error(), Outcome.error());
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("TypedWorkerFailure"));
	FStaticMeshTestAccess::SetMaterialSlots(Mesh, FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()), .bPersistDerivedData = false})) << Error;
	FAssetCompilingManager::Get().FinishAllCompilation();
	const auto Diagnostic = Mesh->GetCompilationDiagnostic();
	ASSERT_FALSE(Diagnostic.Error.empty());
	EXPECT_EQ(Diagnostic.Error, Authored.error());
	const auto Synchronous = Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous, {.Source = Source});
	ASSERT_FALSE(Synchronous);
	EXPECT_EQ(Synchronous.error(), Authored.error());
	Module.bFail = true;
	// Use a distinct build key so the session cannot reuse the previous invalid-product completion.
	const auto Failed = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()), 2.0f),
		.Source = Source, .bPersistDerivedData = false});
	ASSERT_FALSE(Failed);

	EXPECT_NE(Failed.error().find("see MeshBuilder logs"), std::string::npos) << Failed.error();
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	const auto Cancelled = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false},
		Control);
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_FALSE(Mesh->IsCompiling());
}

TEST(FStaticMeshAuthoredCompilationTests, TokenReportsCancellationAndErrorsReportGeometryCounts)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	const FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "SourceCancellationCache").generic_string());
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	std::expected<std::unique_ptr<FStaticMeshRenderData>, std::string> Product;
	uint32 Checks = 0;
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [&] { return ++Checks == 3; }};
	const auto& Cancelled = (Product = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false},
		Control));
	EXPECT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());

	EXPECT_FALSE(*Product);
	FStaticMeshRenderData Invalid;
	Invalid.LODResources.resize(1);
	Invalid.LODResources[0].IndexBuffer.Init(std::vector<uint32>{0, 1});
	std::expected<FPhysicsCookResult, std::string> Collision;
	const auto& Rejected = (Collision = FPhysicsCookHelper::Cook(CaptureCollisionCookInfoForTest(Invalid,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, false)));
	EXPECT_FALSE(Rejected);
	Invalid.LODResources.clear();
	EXPECT_NE(Rejected.error().find("Cook:"), std::string::npos);
	EXPECT_NE(Rejected.error().find("0 vertices, 2 indices"), std::string::npos);
	EXPECT_FALSE(Collision);
}

TEST(FStaticMeshAuthoredCompilationTests, BuildErrorsOwnInputAndRejectedReservation)
{
	using namespace Durin;
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshBuildRequest Request{.Source = Source};
	Request.Settings.NormalizedSize = std::numeric_limits<float>::quiet_NaN();
	const auto Invalid = BuildStaticMeshRenderData(Request);
	Request.Settings.NormalizedSize = 1.5f;
	ASSERT_FALSE(Invalid);
	EXPECT_NE(Invalid.error().find("normalization is invalid"), std::string::npos);
	const auto Budget = BuildStaticMeshRenderData(Request, {.MaximumWorkingSetBytes = 1});
	Request.Source = {};
	Source = {};
	ASSERT_FALSE(Budget);
	EXPECT_NE(Budget.error().find("decoded source exceeds its reservation"), std::string::npos);
	EXPECT_LE(Budget.error().size(), MaximumAssetBuildDiagnosticBytes);
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	const auto Cancelled = BuildStaticMeshRenderData({}, Control);
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());
}

TEST(FStaticMeshAuthoredCompilationTests, ApplicationFailurePreservesSourceSlotsAndImportOwnership)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ApplicationCauseMesh"));
	const auto Invalid = CommitStaticMeshBuild(*Mesh, {}, {}, {});
	ASSERT_FALSE(Invalid);
	EXPECT_FALSE(Invalid.error().empty());
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	auto* Other = NewObject<DStaticMesh>(nullptr, FName("ForeignImportOwner"));
	auto* ForeignImport = NewObject<DAssetImportData>(Other, FName("ForeignImport"));
	const auto* OriginalRender = Mesh->GetRenderData();
	const auto OriginalRevision = Mesh->GetRenderResourceStatus().Revision;
	const auto OriginalSource = Mesh->GetSource().GetIdentity();
	const std::vector<FMeshMaterialSlotDefinition> Slots(Mesh->GetMaterialSlots().begin(), Mesh->GetMaterialSlots().end());
	auto Render = BuildStaticMeshRenderData({.Settings = MakeStaticMeshBuildSettings(Slots, Mesh->GetNormalizedSize()), .Source = Mesh->GetSource(), .bPersistDerivedData = false});
	ASSERT_TRUE(Render);
	const auto Rejected = CommitStaticMeshBuild(*Mesh, std::move(*Render), Mesh->GetSource(), CaptureStaticMeshReconciliation(*Mesh), false, ForeignImport);
	ASSERT_FALSE(Rejected);
	EXPECT_NE(Rejected.error().find(ForeignImport->GetObjectPath()), std::string::npos);
	EXPECT_EQ(Mesh->GetRenderData(), OriginalRender);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, OriginalRevision);
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), OriginalSource);
	EXPECT_TRUE(std::ranges::equal(Mesh->GetMaterialSlots(), Slots));
	EXPECT_EQ(Mesh->GetAssetImportData(), nullptr);
}

TEST(FStaticMeshAuthoredCompilationTests, SynchronousFailureExposesOwnedMessage)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("SynchronousTypedFailure"));
	auto Geometry = MakeResidencyGeometry();
	Geometry.Sections.front().Indices.back() = 77;
	const auto SourceFailure = FStaticMeshTestAccess::BuildCandidate(Mesh, std::move(Geometry));
	ASSERT_FALSE(SourceFailure);
	EXPECT_FALSE(SourceFailure.error().empty());
	EXPECT_LE(SourceFailure.error().size(), MaximumAssetBuildDiagnosticBytes);
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	const auto InvalidSourceFailure = Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous, {.Source = FStaticMeshSource{}});
	ASSERT_FALSE(InvalidSourceFailure);
	EXPECT_FALSE(InvalidSourceFailure.error().empty());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	EXPECT_FALSE(Mesh->IsCompiling());
}

TEST(FStaticMeshReplacementTests, ReplacementRepairsBoundsRestoresSlotsAndRejectsInvalidScreenSize)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ReplacementPreparation"));
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	auto Render = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false});
	ASSERT_TRUE(Render) << Render.error();
	(*Render)->LocalBounds = FBox(FVector3(100), FVector3(101));
	(*Render)->LODResources.front().LocalBounds = (*Render)->LocalBounds;
	(*Render)->LODResources.front().Sections.front().LocalBounds = (*Render)->LocalBounds;
	(*Render)->MaterialSlots.front() = {};
	std::vector<FMeshMaterialSlotDefinition> Slots{{.Name = FName("Authored"), .SourceMaterialIndex = 7}};
	const auto Replaced = FStaticMeshTestAccess::ReplaceRenderData(Mesh, std::move(*Render), Slots);
	ASSERT_TRUE(Replaced) << Replaced.error();
	ASSERT_NE(Mesh->GetRenderData(), nullptr);
	EXPECT_EQ(Mesh->GetRenderData()->LocalBounds.Min, FVector3(-0.75, -0.75, 0));
	EXPECT_EQ(Mesh->GetRenderData()->LocalBounds.Max, FVector3(0.75, 0.75, 0));
	EXPECT_EQ(Mesh->GetRenderData()->MaterialSlots.front().Name, "Authored");
	EXPECT_EQ(Mesh->GetRenderData()->MaterialSlots.front().SourceMaterialIndex, 7u);
	Render = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false});
	ASSERT_TRUE(Render) << Render.error();
	(*Render)->LODResources.front().ScreenSize = 0.5f;
	const auto Rejected = FStaticMeshTestAccess::ReplaceRenderData(Mesh, std::move(*Render), Slots);
	ASSERT_FALSE(Rejected);
	EXPECT_FALSE(Rejected.error().empty());
}

TEST(FStaticMeshReplacementTests, ReplacementRejectsInvalidSourceSlotsAndUVChannels)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("TypedReplacementValues"));
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	const auto InvalidSource = Durin::FStaticMeshTestAccess::ReplaceSourceRenderData(Mesh, Source, nullptr, {}, std::numeric_limits<float>::quiet_NaN());
	EXPECT_FALSE(InvalidSource.error().empty());
	auto DuplicateRender = std::make_unique<FStaticMeshRenderData>();
	DuplicateRender->MaterialSlots = {{"Duplicate", 0}, {"Duplicate", 1}};
	std::vector<FMeshMaterialSlotDefinition> Slots{
		{.Name = FName("Duplicate")}, {.Name = FName("Duplicate")}};
	const auto Duplicate = Durin::FStaticMeshTestAccess::ReplaceRenderData(Mesh, std::move(DuplicateRender), std::move(Slots));
	EXPECT_FALSE(Duplicate.error().empty());
	auto UVRender = std::make_unique<FStaticMeshRenderData>();
	UVRender->MaterialSlots = {{"Slot", 0}};
	UVRender->LODResources.emplace_back().NumTexCoords = MaxStaticMeshUVChannels + 1;
	const auto UV = Durin::FStaticMeshTestAccess::ReplaceRenderData(Mesh, std::move(UVRender), {{.Name = FName("Slot")}});
	EXPECT_FALSE(UV.error().empty());
	const auto Missing = Durin::FStaticMeshTestAccess::ReplaceRenderData(Mesh, nullptr, {});
	EXPECT_FALSE(Missing.error().empty());
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

TEST(FStaticMeshAuthoredCompilationTests, ReimportCommitsSourceAndMetadataDespiteDerivedDataFailure)
{
	using namespace Durin;
	using namespace AssetForge::Builtins;
	const FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("StaticMeshAuthoredImportFailure", true);
	ASSERT_NE(Fixture.Mesh, nullptr);
	auto& Mesh = *Fixture.Mesh;
	Mesh.SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	ASSERT_TRUE(Mesh.GetBodySetup()->GetResidentComplexGeometry());
	const auto OldSource = Mesh.GetSource().GetIdentity();
	const auto OldHash = Mesh.GetAssetImportData()->GetSourceData().FindByRole("source")->GetContentHash();
	ASSERT_TRUE(SaveMeshFixtureText("o Replacement\nv 0 0 0\nv 4 0 0\nv 0 2 0\nf 1 2 3\n", Fixture.SourcePath.generic_string()));
	struct FResetHook
	{
		~FResetHook() { FAssetCompilingManager::Get().FinishAllCompilation(); AssetPrivate::SetStaticMeshCompilationPhaseHookForTests({}); }
	} Reset;
	AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([](uint64, EStaticMeshCompilationPhase Phase) {
		if (Phase == EStaticMeshCompilationPhase::Building) throw std::runtime_error("Injected render DDC failure.");
	});
	const auto Imported = ReimportStaticMesh(Mesh);
	ASSERT_EQ(Imported.Status, EReimportResult::Succeeded) << Imported.Message;
	const auto NewSource = Mesh.GetSource().GetIdentity();
	const auto NewHash = Mesh.GetAssetImportData()->GetSourceData().FindByRole("source")->GetContentHash();
	EXPECT_NE(NewSource, OldSource);
	EXPECT_NE(NewHash, OldHash);
	EXPECT_EQ(Mesh.GetRenderData(), nullptr);
	EXPECT_FALSE(Mesh.GetBodySetup()->GetResidentComplexGeometry());
	const auto Geometry = Mesh.GetSource().AcquireGeometry();
	ASSERT_TRUE(Geometry);
	for (const auto& Group : (*Geometry)->PolygonGroups)
		EXPECT_EQ(std::ranges::count_if(Mesh.GetMaterialSlots(), [&](const auto& Slot) { return Slot.SourceMaterialIndex == Group.SourceMaterialIndex; }), 1);
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	EXPECT_EQ(Mesh.GetCompilationDiagnostic().Status, EStaticMeshCompilationStatus::Failed);
	EXPECT_EQ(Mesh.GetSource().GetIdentity(), NewSource);
	EXPECT_EQ(Mesh.GetAssetImportData()->GetSourceData().FindByRole("source")->GetContentHash(), NewHash);
	EXPECT_EQ(Mesh.GetRenderData(), nullptr);
	EXPECT_FALSE(Mesh.GetBodySetup()->GetResidentComplexGeometry());
	EXPECT_TRUE(Mesh.GetPackage()->IsDirty());
	ASSERT_TRUE(SavePackage(Mesh.GetPackage()));
}

TEST(FStaticMeshAuthoredCompilationTests, ReimportSaveFailureRetainsAuthoredCommit)
{
	using namespace Durin;
	const FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("StaticMeshReimportSaveFailure", true);
	ASSERT_NE(Fixture.Mesh, nullptr);
	auto& Mesh = *Fixture.Mesh;
	const auto OldSource = Mesh.GetSource().GetIdentity();
	const auto OldHash = Mesh.GetAssetImportData()->GetSourceData().FindByRole("source")->GetContentHash();
	const auto Destination = Fixture.Root / "Content/Mesh.dasset";
	ASSERT_TRUE(std::filesystem::remove(Destination));
	ASSERT_TRUE(std::filesystem::create_directory(Destination));
	ASSERT_TRUE(SaveMeshFixtureText("o SavedReplacement\nv 0 0 0\nv 3 0 0\nv 0 2 0\nf 1 2 3\n", Fixture.SourcePath.generic_string()));
	std::optional<FReimportResult> Completed;
	FReimportManager::Reimport(Mesh, {}, [&](FReimportResult Result) { Completed = std::move(Result); });
	ASSERT_TRUE(Completed);
	EXPECT_EQ(Completed->Status, EReimportResult::Failed);
	EXPECT_NE(Completed->SaveCause, nullptr);
	EXPECT_EQ(Completed->FactoryCause, nullptr);
	EXPECT_NE(Mesh.GetSource().GetIdentity(), OldSource);
	EXPECT_NE(Mesh.GetAssetImportData()->GetSourceData().FindByRole("source")->GetContentHash(), OldHash);
	EXPECT_TRUE(Mesh.GetPackage()->IsDirty());
	ASSERT_TRUE(std::filesystem::remove(Destination));
	ASSERT_TRUE(std::filesystem::remove(Fixture.SourcePath));
	ASSERT_TRUE(SavePackage(Mesh.GetPackage()));
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	ASSERT_NE(Mesh.GetRenderData(), nullptr);
}

TEST(FStaticMeshAuthoredCompilationTests, ImportedCommitRetiresPriorWorkerAndRejectsReentrantBuild)
{
	using namespace Durin;
	using namespace AssetForge::Builtins;
	const FScopedDerivedDataCacheRestore RestoreCache;
	auto Fixture = ImportCacheFixture("StaticMeshImportedCommitRetirement", true);
	ASSERT_NE(Fixture.Mesh, nullptr);
	auto& Mesh = *Fixture.Mesh;
	Mesh.SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	const auto OldSource = Mesh.GetSource().GetIdentity();
	const auto* OldData = Mesh.GetAssetImportData();
	FStaticMeshSource Source;
	auto Geometry = MakeResidencyGeometry();
	Geometry.Sections.front().Positions[1].x = 3;
	ASSERT_TRUE(Source.Initialize(Geometry));
	auto* Data = NewObject<DStaticMeshImportData>(&Mesh, FName("ReplacementImportData"));
	Data->SetState(static_cast<const DStaticMeshImportData*>(OldData)->GetStaticMeshState());
	EXPECT_FALSE(Mesh.SetImportedSource(Source, {}, *Data));
	auto AmbiguousSlots = FStaticMeshTestAccess::MakeSlots(Geometry);
	AmbiguousSlots.push_back({.Name = FName("DuplicateSourceIndex"), .SourceMaterialIndex = 0});
	EXPECT_FALSE(Mesh.SetImportedSource(Source, std::move(AmbiguousSlots), *Data));
	EXPECT_EQ(Mesh.GetSource().GetIdentity(), OldSource);
	EXPECT_EQ(Mesh.GetAssetImportData(), OldData);
	EXPECT_NE(Mesh.GetRenderData(), nullptr);
	EXPECT_TRUE(Mesh.GetBodySetup()->GetResidentComplexGeometry());
	auto* OtherOwnedBody = NewObject<DBodySetup>(&Mesh, FName("OtherOwnedBodyDuringMutation"));
	OtherOwnedBody->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	uint32 CollisionCompletions = 0;
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(Mesh.Build(EStaticMeshBuildMode::Asynchronous, {}));
	Mesh.GetBodySetup()->CreatePhysicsMeshesAsync(Mesh, false, [&](const auto& Result) {
		++CollisionCompletions;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Superseded);
		EXPECT_EQ(Mesh.GetSource().GetIdentity(), OldSource);
		EXPECT_EQ(Mesh.GetAssetImportData(), OldData);
		EXPECT_TRUE(Mesh.IsCompilationMutationBlocked());
		EXPECT_TRUE(Mesh.GetBodySetup()->IsCompilationMutationBlocked());
		EXPECT_TRUE(OtherOwnedBody->IsCompilationMutationBlocked());
		OtherOwnedBody->CreatePhysicsMeshesAsync(Mesh, false);
		EXPECT_EQ(OtherOwnedBody->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Failed);
		Mesh.GetBodySetup()->CreatePhysicsMeshesAsync(Mesh, false);
		EXPECT_EQ(Mesh.GetBodySetup()->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Failed);
		ASSERT_TRUE(Mesh.GetBodySetup()->GetPhysicsMeshBuildError());
		EXPECT_FALSE(Mesh.Build(EStaticMeshBuildMode::Asynchronous));
		EXPECT_FALSE(Mesh.SetImportedSource(Source, FStaticMeshTestAccess::MakeSlots(Geometry), *Data));
	});
	ASSERT_TRUE(Barrier.Wait(2));
	EXPECT_TRUE(Mesh.IsCompiling());
	std::jthread ReleaseWorker([&] { Barrier.Release(); });
	ASSERT_TRUE(Mesh.SetImportedSource(Source, FStaticMeshTestAccess::MakeSlots(Geometry), *Data));
	EXPECT_EQ(CollisionCompletions, 1u);
	EXPECT_FALSE(Mesh.IsCompilationMutationBlocked());
	EXPECT_FALSE(Mesh.GetBodySetup()->IsCompilationMutationBlocked());
	EXPECT_FALSE(OtherOwnedBody->IsCompilationMutationBlocked());
	EXPECT_FALSE(Mesh.IsCompiling());
	EXPECT_EQ(Mesh.GetSource().GetIdentity(), Source.GetIdentity());
	EXPECT_EQ(Mesh.GetAssetImportData(), Data);
	EXPECT_EQ(Mesh.GetRenderData(), nullptr);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords, 0u);
	uint32 Notifications = 0;
	struct FNotificationScope
	{
		FDelegateHandle Handle;
		~FNotificationScope() { FAssetCompilingManager::Get().OnAssetPostCompile().Remove(Handle); }
	} Notification{FAssetCompilingManager::Get().OnAssetPostCompile().AddLambda([&](const FAssetPostCompileData& Event) {
		if (Event.CompilerName != FName("Durin.StaticMesh")) return;
		for (const auto& Asset : Event.Assets)
			if (Asset.Get() == &Mesh) { ++Notifications; EXPECT_FALSE(Mesh.IsCompiling()); EXPECT_NE(Mesh.GetRenderData(), nullptr); }
	})};
	Mesh.PostEditChange();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	ASSERT_NE(Mesh.GetRenderData(), nullptr);
	EXPECT_EQ(Mesh.GetCompilationDiagnostic().Status, EStaticMeshCompilationStatus::Succeeded);
	EXPECT_EQ(Notifications, 1u);
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	Mesh.GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Notifications, 1u);
	Mesh.PostEditChange();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(Mesh);
	Mesh.GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Notifications, 2u);
	EXPECT_EQ(Mesh.GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_TRUE(Mesh.GetBodySetup()->GetResidentComplexGeometry());
}

TEST(FStaticMeshAuthoredCompilationTests, SynchronousBuildRetiresSelectedWorkerWithoutApplyingUnrelatedWork)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("DirectBuild"));
	auto* Other = NewObject<DStaticMesh>(nullptr, FName("OtherAsyncBuild"));
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	ASSERT_TRUE(Other->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())}));
	ASSERT_TRUE(Barrier.Wait(2));
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
	const auto RequestId = Mesh->GetCompilationDiagnostic().RequestId;
	std::jthread ReleaseWorker([&] { Barrier.Release(); });
	const auto Built = Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous, {.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())});
	ASSERT_TRUE(Built) << Built.error();
	ASSERT_NE(Mesh->GetRenderData(), nullptr);
	const auto* Render = Mesh->GetRenderData();
	const auto Revision = Mesh->GetRenderResourceStatus().Revision;
	EXPECT_EQ(Mesh->GetCompilationDiagnostic().RequestId, RequestId);
	EXPECT_EQ(Other->GetRenderData(), nullptr);
	EXPECT_TRUE(Other->IsCompiling());
	const auto Invalid = Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous, {.Source = FStaticMeshSource{}});
	ASSERT_FALSE(Invalid);
	EXPECT_EQ(Mesh->GetRenderData(), Render);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Revision);
	Barrier.Release();
	FAssetCompilingManager::Get().FinishAllCompilation();
	EXPECT_EQ(Mesh->GetRenderData(), Render);
	EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Revision);
	EXPECT_NE(Other->GetRenderData(), nullptr);
}

TEST(FStaticMeshAuthoredCompilationTests, RunningCookAbortFinalizesAfterRetirementAndBeforeNotificationReturns)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("CancellationRetirementObserver"));
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Mesh->FinishCompilation();
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	auto* Body = Mesh->GetBodySetup();
	ASSERT_NE(Body, nullptr);
	uint32 Notifications = 0;
	uint64 HeldBytes = 0;
	FStaticMeshWorkerBarrier Barrier;
	Body->CreatePhysicsMeshesAsync(*Mesh, false, [&](const FPhysicsCookCompletionResult& Result) {
		++Notifications;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Superseded);
		EXPECT_FALSE(Result.Error);
		const auto Retiring = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests();
		EXPECT_EQ(Retiring.EstimatedWorkingSetBytes, 0u);
		EXPECT_EQ(Retiring.OutstandingRecords, 0u);
		// Finishing or pumping recursively must not deliver this observer twice.
		Body->FinishPhysicsMeshes();
		FAssetCompilingManager::Get().ProcessAsyncTasks();
		EXPECT_EQ(Notifications, 1u);
		EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
	});
	ASSERT_TRUE(Barrier.Wait(1));
	HeldBytes = StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes;
	ASSERT_GT(HeldBytes, 0u);
	Body->AbortPhysicsMeshAsyncCreation();
	EXPECT_TRUE(Body->IsCompiling());
	EXPECT_FALSE(Mesh->IsCompiling());
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_EQ(Notifications, 0u);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, HeldBytes);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	Barrier.Release();
	Body->FinishPhysicsMeshes();
	EXPECT_FALSE(Body->IsCompiling());
	EXPECT_EQ(Notifications, 1u);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords, 0u);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
}

TEST(FStaticMeshAuthoredCompilationTests, PipelineDiagnosticsStayBounded)
{
	using namespace Durin;
	const auto Failure = MakeAssetBuildDiagnostic(std::string(MaximumAssetBuildDiagnosticBytes + 1, 'a'));
	EXPECT_EQ(Failure.size(), MaximumAssetBuildDiagnosticBytes);
}

TEST(FStaticMeshSeparatedBuildTests, CollisionSnapshotOutlivesRenderAndOwnsCancellation)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	const FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "DetachedCollisionSnapshot").generic_string());
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	auto Render = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false});
	ASSERT_TRUE(Render);
	ASSERT_TRUE((*Render)->LocalBounds.bIsValid);
	auto Request = CaptureCollisionCookInfoForTest(**Render,
		EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, false);
	Render->reset();
	Source = {};
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	const auto Cancelled = FPhysicsCookHelper::Cook(Request, Control);
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());
	const auto Budget = FPhysicsCookHelper::Cook(Request, {.MaximumWorkingSetBytes = 1});
	ASSERT_FALSE(Budget);
	EXPECT_NE(Budget.error().find("Cook:"), std::string::npos);
	const auto Collision = FPhysicsCookHelper::Cook(Request);
	ASSERT_TRUE(Collision) << Collision.error();
	EXPECT_TRUE(Collision->Complex);
	EXPECT_FALSE(Collision->Simple);
	auto Retry = Request;
	EXPECT_NE(Retry.TriangleMeshDesc.Positions.data(), Request.TriangleMeshDesc.Positions.data());
	EXPECT_NE(Retry.TriangleMeshDesc.Indices.data(), Request.TriangleMeshDesc.Indices.data());
	Request.TriangleMeshDesc.Indices = {0, 1};
	const auto Invalid = FPhysicsCookHelper::Cook(Request);
	Request = {};
	const auto Retried = FPhysicsCookHelper::Cook(Retry);
	ASSERT_TRUE(Retried);
	ASSERT_FALSE(Invalid);

	EXPECT_TRUE(Collision->Complex);
}

TEST(FPhysicsCookTests, CookAndAsyncInstallationUseIndependentCollisionWork)
{
	FCacheLogCapture CollisionLog;
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	const FScopedDerivedDataCacheRestore RestoreCache;
	FPaths::SetDerivedDataCacheDirForTests((Testing::GetTestWorkDirectory() / "IndependentPhysicsCook").generic_string());
	struct FDataProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{ return FTriMeshCollisionData{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2}}; }
	} DataProvider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("ProviderIndependentCook"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	auto Data = DataProvider.GetPhysicsTriMeshData();
	ASSERT_TRUE(Data);
	const auto Info = Body->GetCookInfo(std::move(*Data));
	const auto Cold = FPhysicsCookHelper::Cook(Info);
	ASSERT_TRUE(Cold) << Cold.error();
	EXPECT_TRUE(Cold->Complex);
	const auto Warm = FPhysicsCookHelper::Cook(Info);
	ASSERT_TRUE(Warm);
	EXPECT_TRUE(CollisionLog.empty());
	Body->CreatePhysicsMeshes(DataProvider);
	ASSERT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_TRUE(Body->GetResidentComplexGeometry());

	{
		FStaticMeshWorkerBarrier Barrier;
		uint32 Completions = 0;
		Body->CreatePhysicsMeshesAsync(DataProvider, false,
			[&](const FPhysicsCookCompletionResult& Result) { EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Succeeded); EXPECT_FALSE(Result.Error); ++Completions; });
		ASSERT_TRUE(Barrier.Wait(1));
		Barrier.Release();
		Body->FinishPhysicsMeshes();
		EXPECT_EQ(Completions, 1u);
		EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
		EXPECT_TRUE(Body->GetResidentComplexGeometry());
	}
		MarkObjectHierarchyAsGarbage(Body);
	}
TEST(FStaticMeshSeparatedBuildTests, CollisionWorkersRejectSupersededGeometrySettingsAndOwners)
{
	using namespace Durin;
	for (uint32 Scenario = 0; Scenario != 7; ++Scenario)
	{
		SCOPED_TRACE(Scenario);
		auto* Mesh = NewObject<DStaticMesh>(nullptr, FName(std::format("CollisionLifecycle{}", Scenario)));
		ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
		Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
		ASSERT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
		const auto* Render = Mesh->GetRenderData();
		const auto Revision = Mesh->GetRenderResourceStatus().Revision;
		const auto OldIdentity = Mesh->GetBodySetup()->GetResidentComplexGeometry().GetIdentity();
		FStaticMeshWorkerBarrier Barrier;
		Mesh->RebuildCollision();
		ASSERT_TRUE(Barrier.Wait(1));
		EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Pending);
		EXPECT_FALSE(Mesh->GetBodySetup()->GetResidentComplexGeometry());
		switch (Scenario)
		{
		case 0: {
			std::jthread ReleaseWorker([&] { Barrier.Release(); });
			auto Geometry = MakeResidencyGeometry();
			Geometry.Sections.front().Positions.front().x += 3;
			ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, std::move(Geometry)));
			break;
		}
		case 1: {
			auto* Replacement = NewObject<DBodySetup>(Mesh, FName("Replacement"));
			Replacement->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
			ASSERT_TRUE(Mesh->SetBodySetup(Replacement));
			break;
		}
		case 2: Mesh->GetBodySetup()->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::ComplexOnly); break;
		case 3: Mesh->GetBodySetup()->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None); break;
		case 4: Mesh->RebuildCollision(); break;
		case 5: Mesh->GetBodySetup()->AbortPhysicsMeshAsyncCreation(); break;
		case 6: MarkObjectHierarchyAsGarbage(Mesh); break;
		}
		if (Scenario != 0) EXPECT_GT(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
		if (Scenario == 5) EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
		Barrier.Release();
		FAssetCompilingManager::Get().FinishAllCompilation();
		EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
		EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().OutstandingRecords, 0u);
		if (Scenario == 6) continue;
		if (Scenario != 0)
		{
			EXPECT_EQ(Mesh->GetRenderData(), Render);
			EXPECT_EQ(Mesh->GetRenderResourceStatus().Revision, Revision);
		}
		if (Scenario == 3 || Scenario == 5) EXPECT_FALSE(Mesh->GetBodySetup()->GetResidentComplexGeometry());
		else
		{
			EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
			EXPECT_NE(Mesh->GetBodySetup()->GetResidentComplexGeometry().GetIdentity(), OldIdentity);
		}
		MarkObjectHierarchyAsGarbage(Mesh);
	}
}

TEST(FStaticMeshSeparatedBuildTests, IndependentCollisionHonorsPersistenceAndPreservesAuthoredPrimitives)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	const FScopedDerivedDataCacheRestore RestoreCache;
	const auto Cache = Testing::GetTestWorkDirectory() / "IndependentCollisionPersistence";
	FPaths::SetDerivedDataCacheDirForTests(Cache.generic_string());
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("IndependentCollisionPersistence"));
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Asynchronous, {.Source = Source,
		.PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry()),
		.bPersistDerivedData = false}));
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	ASSERT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	const auto Key = GetCollisionKey(*Mesh->GetRenderData()).ToString();
	const auto CollisionPath = Cache / "records" / "v4" / "StaticMeshCollision" / Key.substr(0, 2) / (Key + ".bin");
	EXPECT_FALSE(std::filesystem::exists(CollisionPath));
	Mesh->RebuildCollision();
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_TRUE(std::filesystem::exists(CollisionPath));
	Mesh->GetBodySetup()->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	ASSERT_TRUE(Mesh->GetBodySetup()->SetBox({2, 3, 4}, {1, 2, 3}));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous, {.Source = Source}));
	ASSERT_EQ(Mesh->GetBodySetup()->GetSimpleCollision().size(), 1u);
	const auto& Element = Mesh->GetBodySetup()->GetSimpleCollision().front();
	EXPECT_EQ(Element.Type, EBodySetupShapeType::Box);
	EXPECT_EQ(Element.Dimensions, FVector3(2, 3, 4));
	EXPECT_EQ(Element.Transform.Translation, FVector3(1, 2, 3));
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	MarkObjectHierarchyAsGarbage(Mesh);
}

TEST(FPhysicsMeshLifecycleTests, DetachedProviderBuildsWithoutStaticMeshOrRendering)
{
	using namespace Durin;
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}};
		}
	};
	auto* Body = NewObject<DBodySetup>(nullptr, FName("IndependentPhysicsBody"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
	uint32 CompletionCount = 0;
	{
		FProvider Provider;
		Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) { EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Succeeded); EXPECT_FALSE(Result.Error); ++CompletionCount; });
	} // The data provider may die before the worker consumes its capture.
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(CompletionCount, 1u);
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready) << (Body->GetPhysicsMeshBuildError() ? *Body->GetPhysicsMeshBuildError() : std::string{});
	EXPECT_TRUE(Body->GetResidentComplexGeometry());
	const auto Generation = Body->GetPhysicsMeshRequestGeneration();
	const auto Geometry = Body->GetResidentComplexGeometry();
	Body->InvalidatePhysicsData();
	EXPECT_EQ(Body->ApplyPhysicsMeshes(Generation, {}, Geometry), EPhysicsCookCompletionStatus::Superseded);
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	FProvider Provider;
	Body->CreatePhysicsMeshes(Provider, false);
	ASSERT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_TRUE(Body->GetResidentComplexGeometry());
	{
		FStaticMeshWorkerBarrier Barrier;
		Body->CreatePhysicsMeshesAsync(Provider, false);
		ASSERT_TRUE(Barrier.Wait(1));
		const auto PendingGeneration = Body->GetPhysicsMeshRequestGeneration();
		EXPECT_EQ(Body->ApplyPhysicsMeshes(PendingGeneration, {}, {}), EPhysicsCookCompletionStatus::Failed);
		ASSERT_TRUE(Body->GetPhysicsMeshBuildError());
		EXPECT_NE(Body->GetPhysicsMeshBuildError()->find("Installation:"), std::string::npos);
		EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Failed);
		EXPECT_EQ(Body->ApplyPhysicsMeshes(Generation, {}, Geometry), EPhysicsCookCompletionStatus::Superseded);
		EXPECT_NE(Body->GetPhysicsMeshBuildError()->find("Installation:"), std::string::npos);
		Body->InvalidatePhysicsData();
		Barrier.Release();
		Body->FinishPhysicsMeshes();
	}
	{
		FStaticMeshWorkerBarrier Barrier;
		uint32 CancelledCompletions = 0;
		Body->CreatePhysicsMeshesAsync(Provider, false,
			[&](const FPhysicsCookCompletionResult& Result) { EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Superseded); EXPECT_FALSE(Result.Error); ++CancelledCompletions; });
		ASSERT_TRUE(Barrier.Wait(1));
		Body->InvalidatePhysicsData();
		Barrier.Release();
		Body->FinishPhysicsMeshes();
		EXPECT_EQ(CancelledCompletions, 1u);
		EXPECT_FALSE(Body->GetResidentComplexGeometry());
		EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
	}
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, PhysicsCompilationDoesNotDependOnRenderCompiler)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	auto RenderManager = AssetPrivate::CreateStaticMeshCompilingManager();
	struct FRestartRender
	{
		std::shared_ptr<IAssetCompilingManager> Manager;
		~FRestartRender() { EXPECT_TRUE(Manager->Start()); }
	} Restart{RenderManager};
	RenderManager->Shutdown();
	EXPECT_FALSE(GetStaticMeshCompilationManagerDiagnostics().bAcceptingRequests);
	EXPECT_TRUE(GetPhysicsMeshCompilationManagerDiagnostics().bAcceptingRequests);
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{ FTriMeshCollisionData Data; Data.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}; Data.Indices = {0, 1, 2}; return Data; }
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("IndependentPhysicsCompiler"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Body->CreatePhysicsMeshesAsync(Provider, false);
	EXPECT_TRUE(Body->IsCompiling());
	EXPECT_EQ(GetStaticMeshCompilationManagerDiagnostics().OutstandingRecords, 0u);
	Body->FinishPhysicsMeshes();
	EXPECT_FALSE(Body->IsCompiling());
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_TRUE(Body->GetResidentComplexGeometry());
	EXPECT_EQ(GetPhysicsMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, 0u);
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, RenderOnlyBarrierDoesNotWaitForPhysics)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("PhysicsBarrierMesh"));
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	FStaticMeshWorkerBarrier Barrier;
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	ASSERT_TRUE(Barrier.Wait(1));
	EXPECT_FALSE(Mesh->IsCompiling());
	EXPECT_TRUE(Mesh->GetBodySetup()->IsCompiling());
	Mesh->FinishCompilation();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Pending);
	Barrier.Release();
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	MarkObjectHierarchyAsGarbage(Mesh);
}

TEST(FPhysicsMeshLifecycleTests, CollisionProjectionMatchesRenderNormalizationAndOwnsSource)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("PhysicsProjectionParity"));
	auto Geometry = MakeResidencyGeometry();
	Geometry.Sections.front().Positions = {{-7, 2, 4}, {5, 2, 4}, {1, 8, 9}};
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, std::move(Geometry)));
	auto Captured = Mesh->CreatePhysicsMeshInputTask();
	ASSERT_TRUE(Captured);
	const auto PositionsView = Mesh->GetRenderData()->LODResources.front().VertexBuffers.PositionVertexBuffer.GetPositions();
	const std::vector<FVector3f> ExpectedPositions(PositionsView.begin(), PositionsView.end());
	const auto IndicesView = Mesh->GetRenderData()->LODResources.front().IndexBuffer.GetIndices();
	const std::vector<uint32> ExpectedIndices(IndicesView.begin(), IndicesView.end());
	const auto Direct = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Direct);
	EXPECT_EQ(Direct->Positions, ExpectedPositions);
	EXPECT_EQ(Direct->Indices, ExpectedIndices);
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	auto Data = (*Captured)->Execute({.MaximumWorkingSetBytes = (*Captured)->GetWorkingSetBytes()});
	ASSERT_TRUE(Data) << Data.error();
	EXPECT_EQ(Data->Positions, ExpectedPositions);
	EXPECT_EQ(Data->Indices, ExpectedIndices);
	const auto Again = (*Captured)->Execute({.MaximumWorkingSetBytes = (*Captured)->GetWorkingSetBytes()});
	ASSERT_TRUE(Again);
	EXPECT_EQ(Again->Positions, ExpectedPositions);
	const auto Budget = (*Captured)->Execute({.MaximumWorkingSetBytes = 1});
	ASSERT_FALSE(Budget);

	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	auto Cancelled = (*Captured)->Execute(Control);
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());
	MarkObjectHierarchyAsGarbage(Mesh);
}

TEST(FPhysicsMeshLifecycleTests, SameGeometryAssetCommitPreservesPhysicsResources)
{
	using namespace Durin;
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("SameGeometryPhysics"));
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	Mesh->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Mesh->GetBodySetup()->FinishPhysicsMeshes();
	const auto Revision = Mesh->GetBodySetup()->GetRevision();
	const auto Identity = Mesh->GetBodySetup()->GetResidentComplexGeometry().GetIdentity();
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	EXPECT_EQ(Mesh->GetBodySetup()->GetRevision(), Revision);
	EXPECT_EQ(Mesh->GetBodySetup()->GetResidentComplexGeometry().GetIdentity(), Identity);
	EXPECT_EQ(Mesh->GetCollisionBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	MarkObjectHierarchyAsGarbage(Mesh);
}

TEST(FPhysicsMeshLifecycleTests, ResidentInputTaskAndCookInfoOwnRepeatableSnapshots)
{
	using namespace Durin;
	struct FProvider final : IInterface_CollisionDataProvider
	{
		FTriMeshCollisionData Data{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}};
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override { return Data; }
	};
	std::unique_ptr<FPhysicsMeshInputTask> Task;
	{
		FProvider Provider;
		auto Captured = Provider.CreatePhysicsMeshInputTask();
		ASSERT_TRUE(Captured);
		Task = std::move(*Captured);
		Provider.Data = {};
	}
	const FAssetBuildTaskContext Context{.MaximumWorkingSetBytes = Task->GetWorkingSetBytes()};
	auto First = Task->Execute(Context);
	auto Second = Task->Execute(Context);
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	EXPECT_EQ(First->Positions, Second->Positions);
	EXPECT_EQ(First->Indices, Second->Indices);
	EXPECT_EQ(First->Indices.size(), 3u);
	EXPECT_FALSE(Task->Execute({.MaximumWorkingSetBytes = 1}));
	const Durin::FAssetBuildTaskContext Control{.Cancellation = [] { return true; }};
	const auto Cancelled = Task->Execute(Control);
	ASSERT_TRUE(Cancelled);
	EXPECT_TRUE(Control.Cancellation.IsCancelled());

	auto* Body = NewObject<DBodySetup>(nullptr, FName("CookInfoSnapshot"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Body->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::ComplexOnly);
	auto Info = Body->GetCookInfo(std::move(*First), false);
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	EXPECT_EQ(Info.Mode, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	EXPECT_FALSE(Info.bPersistDerivedData);
	const auto Cooked = FPhysicsCookHelper::Cook(std::move(Info), Context);
	ASSERT_TRUE(Cooked) << Cooked.error();
	EXPECT_TRUE(Cooked->Complex);
	EXPECT_FALSE(Cooked->Simple);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsCookTests, InputFailuresNotifyOnceAndPreserveCancellationAndBoundedText)
{
	using namespace Durin;
	struct FFailingProvider final : IInterface_CollisionDataProvider
	{
		struct FCancelInput final : FPhysicsMeshInputTask
		{
			auto GetWorkingSetBytes() const -> uint64 override { return 1024 * 1024; }
			auto Execute(const FAssetBuildTaskContext& Control) const
				-> std::expected<FTriMeshCollisionData, std::string> override
			{ Control.Cancellation.Cancel(); return {}; }
		};
		bool bCancel = false;
		auto CreatePhysicsMeshInputTask() const
			-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string> override
		{
			if (bCancel) return std::unique_ptr<FPhysicsMeshInputTask>(std::make_unique<FCancelInput>());
			return IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask();
		}
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			std::string Message(MaximumAssetBuildDiagnosticBytes + 100, 'x');
			return std::unexpected(MakeAssetBuildDiagnostic(Message, "Input"));
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsFailureOwnership"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	uint32 Completions = 0;
	std::optional<FPhysicsCookCompletionResult> Failed;
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) { Failed = Result; ++Completions; });
	ASSERT_TRUE(Failed);
	EXPECT_EQ(Failed->Status, EPhysicsCookCompletionStatus::Failed);
	ASSERT_TRUE(Failed->Error);
	EXPECT_NE(Failed->Error->find("Input:"), std::string::npos);
	EXPECT_EQ(Failed->Error->size(), MaximumAssetBuildDiagnosticBytes);
	EXPECT_EQ(Completions, 1u);
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Failed);
	ASSERT_TRUE(Body->GetPhysicsMeshBuildError());
	EXPECT_NE(Body->GetPhysicsMeshBuildError()->find("Input:"), std::string::npos);
	Provider.bCancel = true;
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Completion) {
		EXPECT_EQ(Completion.Status, EPhysicsCookCompletionStatus::Cancelled);
		EXPECT_FALSE(Completion.Error);
		++Completions;
	});
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
	EXPECT_EQ(Completions, 2u);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, ImmediateFailuresCanReenterWithANewRequest)
{
	using namespace Durin;
	struct FProvider final : IInterface_CollisionDataProvider
	{
		bool FailInput = true;
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			if (FailInput) return std::unexpected(std::string(MaximumAssetBuildDiagnosticBytes + 100, 'x'));
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}};
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsImmediateFailureReentry"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	std::optional<FPhysicsCookCompletionResult> InputFailure, AdmissionFailure;
	uint32 Notifications = 0;
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) {
		InputFailure = Result;
		++Notifications;
		ASSERT_EQ(Result.Status, EPhysicsCookCompletionStatus::Failed);
		ASSERT_TRUE(Result.Error);
		EXPECT_EQ(Result.Error->size(), MaximumAssetBuildDiagnosticBytes);
		Provider.FailInput = false;
		Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Next) {
			++Notifications;
			EXPECT_EQ(Next.Status, EPhysicsCookCompletionStatus::Succeeded);
			EXPECT_FALSE(Next.Error);
		});
	});
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(Notifications, 2u);
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
	ASSERT_TRUE(InputFailure->Error);
	EXPECT_EQ(InputFailure->Error->size(), MaximumAssetBuildDiagnosticBytes);

	auto Manager = AssetPrivate::CreatePhysicsMeshCompilingManager();
	struct FRestart
	{
		std::shared_ptr<IAssetCompilingManager> Manager;
		~FRestart() { EXPECT_TRUE(Manager->Start()); }
	} Restart{Manager};
	Manager->Shutdown();
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) {
		AdmissionFailure = Result;
		++Notifications;
		ASSERT_EQ(Result.Status, EPhysicsCookCompletionStatus::Failed);
		ASSERT_TRUE(Result.Error);
		EXPECT_NE(Result.Error->find("Scheduling:"), std::string::npos);
		ASSERT_TRUE(Manager->Start());
		Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Next) {
			++Notifications;
			EXPECT_EQ(Next.Status, EPhysicsCookCompletionStatus::Succeeded);
		});
	});
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(Notifications, 4u);
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
	ASSERT_TRUE(AdmissionFailure);
	EXPECT_TRUE(AdmissionFailure->Error);
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::None);
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) {
		++Notifications;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Succeeded);
		EXPECT_FALSE(Result.Error);
	});
	EXPECT_EQ(Notifications, 5u);
	EXPECT_FALSE(Body->IsCompiling());
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, WaitingDDCFreesCpuSlotsAndPhysicsWaitResumesRenderWithoutApplyingIt)
{
	using namespace Durin;
	InitializeDObjectSystem();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FAssetCompilingManager::Get().FinishAllCompilation();
	class FHeldCache final : public DerivedData::ICache
	{
	public:
		mutable std::atomic<uint32> Reads = 0;
		mutable FThreadEvent Release;
		auto Get(const DerivedData::FCacheGetRequest&) const -> DerivedData::FCacheGetResult override
		{ ++Reads; Release.WaitFor(10.0); return std::optional<DerivedData::FCacheRecord>{}; }
		auto Put(const DerivedData::FCachePutRequest&) const -> DerivedData::FCachePutResult override { return {}; }
	};
	auto Cache = std::make_shared<FHeldCache>();
	FThreadEvent CallbackEntered, ReleaseCallback, CpuMailbox;
	std::atomic<bool> Resumed = false;
	std::atomic<uint32> CallbackCount = 0;
	struct FProvider final : IInterface_CollisionDataProvider
	{
		FThreadEvent& Mailbox;
		std::atomic<bool>& Resumed;
		bool WaitForResume = false;
		FProvider(FThreadEvent& InMailbox, std::atomic<bool>& InResumed) : Mailbox(InMailbox), Resumed(InResumed) {}
		struct FInput final : FPhysicsMeshInputTask
		{
			FThreadEvent& Mailbox;
			std::atomic<bool>& Resumed;
			bool Wait;
			FInput(FThreadEvent& InMailbox, std::atomic<bool>& InResumed, bool InWait) : Mailbox(InMailbox), Resumed(InResumed), Wait(InWait) {}
			auto GetWorkingSetBytes() const -> uint64 override { return 1024 * 1024; }
			auto Execute(const FAssetBuildTaskContext&) const -> std::expected<FTriMeshCollisionData, std::string> override
			{
				if (Wait) Resumed = Mailbox.WaitFor(2.0);
				// Fail before physics DDC I/O, so this probes execution capacity rather
				// than competing for the deliberately blocked cache I/O workers.
				return FTriMeshCollisionData{{{0, 0, 0}}, {0, 1}};
			}
		};
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override { return FTriMeshCollisionData{}; }
		auto CreatePhysicsMeshInputTask() const -> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string> override
		{ return std::unique_ptr<FPhysicsMeshInputTask>(std::make_unique<FInput>(Mailbox, Resumed, WaitForResume)); }
	} Provider{CpuMailbox, Resumed};
	struct FDrain
	{
		std::shared_ptr<FHeldCache> Cache;
		FThreadEvent& ReleaseCallback;
		FThreadEvent& CpuMailbox;
		~FDrain()
		{
			Cache->Release.Trigger(); ReleaseCallback.Trigger(); CpuMailbox.Trigger();
			FAssetCompilingManager::Get().FinishAllCompilation();
			AssetPrivate::SetStaticMeshBuildCompletionHookForTests({});
			AssetPrivate::SetStaticMeshCompilationPhaseHookForTests({});
			AssetPrivate::SetStaticMeshAsyncCacheForTests({});
		}
	} Drain{Cache, ReleaseCallback, CpuMailbox};
	AssetPrivate::SetStaticMeshAsyncCacheForTests(Cache);
	AssetPrivate::SetStaticMeshBuildCompletionHookForTests([&] {
		if (++CallbackCount == 2) CallbackEntered.Trigger();
		ReleaseCallback.WaitFor(10.0);
	});
	AssetPrivate::SetStaticMeshCompilationPhaseHookForTests([&](uint64, EStaticMeshCompilationPhase Phase) {
		if (Phase == EStaticMeshCompilationPhase::Mailbox) CpuMailbox.Trigger();
	});
	FStaticMeshSource Source;
	const auto Geometry = MakeResidencyGeometry();
	ASSERT_TRUE(Source.Initialize(Geometry));
	std::array<DStaticMesh*, 2> Meshes;
	for (uint32 Index = 0; Index < Meshes.size(); ++Index)
	{
		Meshes[Index] = NewObject<DStaticMesh>(nullptr, FName(std::format("HeldDdcCpuSlot{}", Index)));
		ASSERT_TRUE(Meshes[Index]->BuildFromSource(EStaticMeshBuildMode::Asynchronous,
			{.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(Geometry), .bPersistDerivedData = false}));
	}
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Cache->Reads.load() != 2 && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	ASSERT_EQ(Cache->Reads.load(), 2u);
	const auto Held = GetStaticMeshCompilationManagerDiagnostics();
	EXPECT_EQ(Held.AdmittedBuildCount, 0u);
	EXPECT_EQ(Held.OutstandingRecords, 2u);
	ASSERT_GT(Held.EstimatedWorkingSetBytes, 0u);
	EXPECT_EQ(AssetPrivate::GetAssetBuildScheduler()->GetScheduledMemory(), 0u);
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsWhileRenderWaitsForDdc"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	FThreadEvent PhysicsFinished;
	std::jthread Watchdog([&] { if (!PhysicsFinished.WaitFor(2.0)) Cache->Release.Trigger(); });
	Body->CreatePhysicsMeshes(Provider, false);
	PhysicsFinished.Trigger();
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Failed);
	EXPECT_FALSE(CallbackEntered.WaitFor(0.01));
	EXPECT_EQ(GetStaticMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, Held.EstimatedWorkingSetBytes);
	Cache->Release.Trigger();
	ASSERT_TRUE(CallbackEntered.WaitFor(5.0));
	Meshes[0]->CancelCompilation();
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_TRUE(Meshes[0]->IsCompiling());
	EXPECT_FALSE(CpuMailbox.WaitFor(0.01));
	EXPECT_EQ(GetStaticMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, Held.EstimatedWorkingSetBytes);
	ReleaseCallback.Trigger();
	Provider.WaitForResume = true;
	Body->CreatePhysicsMeshes(Provider, false);
	EXPECT_TRUE(Resumed.load());
	EXPECT_EQ(Meshes[1]->GetRenderData(), nullptr);
	EXPECT_TRUE(Meshes[1]->IsCompiling());
	EXPECT_EQ(GetStaticMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, Held.EstimatedWorkingSetBytes);
	Meshes[0]->FinishCompilation();
	EXPECT_EQ(Meshes[0]->GetCompilationDiagnostic().Status, EStaticMeshCompilationStatus::Cancelled);
	Meshes[1]->FinishCompilation();
	EXPECT_EQ(Meshes[1]->GetCompilationDiagnostic().Status, EStaticMeshCompilationStatus::Succeeded);
	EXPECT_NE(Meshes[1]->GetRenderData(), nullptr);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
	for (auto* Mesh : Meshes) MarkObjectHierarchyAsGarbage(Mesh);
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FAssetBuildSchedulerTests, MemoryPressureQueuesAndOversizedWorkRunsAlone)
{
	using namespace Durin::AssetPrivate;
	FAssetBuildScheduler Scheduler(2, 100);
	std::vector<std::shared_ptr<FAssetBuildExecutionSlot>> Running;
	uint32 Launched = 0;
	const auto Launch = [&](auto Slot) { ++Launched; Running.push_back(std::move(Slot)); return true; };
	Scheduler.Enqueue(false, 60, Launch);
	Scheduler.Enqueue(false, 60, Launch);
	EXPECT_EQ(Launched, 1u); // A CPU slot is free, but the memory estimate does not fit.
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 60u);
	Running.clear();
	Scheduler.Pump();
	EXPECT_EQ(Launched, 2u);
	Scheduler.Enqueue(false, 150, Launch);
	EXPECT_EQ(Launched, 2u);
	Running.clear();
	Scheduler.Pump();
	EXPECT_EQ(Launched, 3u); // Oversized work must not starve at zero concurrency.
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 150u);
	Scheduler.Enqueue(false, 0, Launch);
	EXPECT_EQ(Launched, 3u);
	Running.clear();
	Scheduler.Pump();
	EXPECT_EQ(Launched, 4u);
	Running.clear();
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 0u);
}

TEST(FAssetBuildSchedulerTests, NestedDependencyRetainsRetirementButReleasesExecutionMemory)
{
	using namespace Durin;
	using namespace Durin::AssetPrivate;
	FAssetBuildScheduler Scheduler(2, 100);
	Tasks::FTaskEvent Dependency("AssetBudget.DdcRetirement", false);
	FThreadEvent BodyExited;
	FTaskHandle Task;
	struct FRelease
	{
		Tasks::FTaskEvent& Event;
		FTaskHandle& Task;
		bool Triggered = false;
		auto Trigger() -> void { if (!std::exchange(Triggered, true)) Event.Trigger(); }
		~FRelease() { Trigger(); if (Task.IsValid()) (void)WaitTask(Task); }
	} Release{Dependency, Task};
	Scheduler.Enqueue(false, 100, [&](auto Slot) {
		Task = Tasks::LaunchTask("AssetBudget.Dispatch", [Slot = std::move(Slot), &Dependency, &BodyExited] {
			Tasks::AddNested(Dependency);
			BodyExited.Trigger();
		});
		return true;
	});
	ASSERT_TRUE(BodyExited.WaitFor(5.0));
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Scheduler.GetScheduledMemory() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 0u);
	EXPECT_FALSE(Task.IsComplete());
	std::shared_ptr<FAssetBuildExecutionSlot> BlockingCpu;
	Scheduler.Enqueue(false, 100, [&](auto Slot) { BlockingCpu = std::move(Slot); return true; });
	uint32 Resumed = 0;
	Scheduler.Enqueue(false, 100, [&](auto) { ++Resumed; return false; }, Task);
	Scheduler.Pump();
	EXPECT_EQ(Resumed, 0u);
	Release.Trigger();
	ASSERT_EQ(WaitTask(Task).WaitStatus, ETaskWaitStatus::Completed);
	Scheduler.Pump();
	EXPECT_EQ(Resumed, 0u); // Retirement is ready, but CPU consumption must reacquire memory.
	BlockingCpu.reset();
	Scheduler.Pump();
	EXPECT_EQ(Resumed, 1u);
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 0u);
}

TEST(FAssetBuildSchedulerTests, NativeCancellationBeforeEntryReleasesMemoryWithoutExecutorProgress)
{
	using namespace Durin;
	using namespace Durin::AssetPrivate;
	FAssetBuildScheduler Scheduler(2, 100);
	Tasks::FTaskEvent Prerequisite("AssetBudget.BlockedEntry", false);
	struct FRelease { Tasks::FTaskEvent& Event; ~FRelease() { Event.Trigger(); } } Release{Prerequisite};
	FTaskHandle Task;
	bool Entered = false;
	Scheduler.Enqueue(false, 100, [&](auto Slot) {
		Task = Tasks::LaunchTask("AssetBudget.Canceled", [Slot = std::move(Slot), &Entered] { Entered = true; }, Prerequisite);
		return true;
	});
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 100u);
	ASSERT_TRUE(CancelTask(Task));
	EXPECT_EQ(WaitTask(Task).TaskState, ETaskState::Canceled);
	EXPECT_FALSE(Entered);
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 0u);
	uint32 Launched = 0;
	Scheduler.Enqueue(false, 100, [&](auto) { ++Launched; return false; });
	EXPECT_EQ(Launched, 1u);
	EXPECT_EQ(Scheduler.GetScheduledMemory(), 0u);
}

TEST(FPhysicsMeshLifecycleTests, QueuedAbortWithdrawsInputWithoutStartingCook)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	struct FProvider final : IInterface_CollisionDataProvider
	{
		mutable std::atomic<uint32> Executions = 0;
		struct FInput final : FPhysicsMeshInputTask
		{
			std::atomic<uint32>& Executions;
			explicit FInput(std::atomic<uint32>& Executions) : Executions(Executions) {}
			auto GetWorkingSetBytes() const -> uint64 override { return 1024 * 1024; }
			auto Execute(const FAssetBuildTaskContext&) const -> std::expected<FTriMeshCollisionData, std::string> override
			{ ++Executions; return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}}; }
		};
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override { return {}; }
		auto CreatePhysicsMeshInputTask() const -> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, std::string> override
		{ return std::unique_ptr<FPhysicsMeshInputTask>(std::make_unique<FInput>(Executions)); }
	} Provider;
	uint32 Notifications = 0;
	std::vector<std::shared_ptr<AssetPrivate::FAssetBuildExecutionSlot>> Slots;
	struct FDrain
	{
		decltype(Slots)& Held;
		~FDrain() { Held.clear(); FAssetCompilingManager::Get().FinishAllCompilation(); }
	} Drain{Slots};
	const auto Scheduler = AssetPrivate::GetAssetBuildScheduler();
	for (uint32 Index = 0; Index != 2; ++Index)
		Scheduler->Enqueue(false, 1, [&](auto Slot) { Slots.push_back(std::move(Slot)); return true; });
	ASSERT_EQ(Slots.size(), 2u);
	auto* Body = NewObject<DBodySetup>(nullptr, FName("QueuedPhysicsAbort"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const auto& Result) {
		++Notifications;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Cancelled);
		EXPECT_FALSE(Result.Error);
	});
	Body->AbortPhysicsMeshAsyncCreation();
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(Notifications, 1u);
	Slots.clear();
	Scheduler->Pump();
	EXPECT_EQ(Provider.Executions.load(), 0u);
	EXPECT_FALSE(Body->IsCompiling());
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Unavailable);
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, ShutdownWaitsForStartedCookAndDiscardsItsResult)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{ return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}}; }
	} Provider;
	uint32 Notifications = 0;
	FStaticMeshWorkerBarrier Barrier;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("RunningPhysicsShutdown"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const auto& Result) {
		++Notifications;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Superseded);
		EXPECT_FALSE(Result.Error);
	});
	ASSERT_TRUE(Barrier.Wait(1));
	const auto Manager = AssetPrivate::CreatePhysicsMeshCompilingManager();
	std::jthread ReleaseWorker([&] { Barrier.Release(); });
	Manager->Shutdown();
	EXPECT_EQ(Notifications, 1u);
	EXPECT_EQ(Manager->GetNumRemainingAssets(), 0u);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	EXPECT_TRUE(Manager->Start());
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, DdcWaitReleasesMemoryAndAbortLetsCookRetireBeforeNotification)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	class FHeldCache final : public DerivedData::ICache
	{
	public:
		mutable FThreadEvent Entered, Release;
		mutable std::atomic<uint32> Writes = 0;
		auto Get(const DerivedData::FCacheGetRequest&) const -> DerivedData::FCacheGetResult override
		{ Entered.Trigger(); Release.WaitFor(10.0); return std::optional<DerivedData::FCacheRecord>{}; }
		auto Put(const DerivedData::FCachePutRequest&) const -> DerivedData::FCachePutResult override { ++Writes; return {}; }
	};
	auto Cache = std::make_shared<FHeldCache>();
	struct FDrain
	{
		std::shared_ptr<FHeldCache> Cache;
		~FDrain() { Cache->Release.Trigger(); FAssetCompilingManager::Get().FinishAllCompilation(); PhysicsPrivate::SetAsyncCacheForTests({}); }
	};
	PhysicsPrivate::SetAsyncCacheForTests(Cache);
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{ return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}}; }
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsDdcMemoryRetirement"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	uint32 Notifications = 0;
	FDrain Drain{Cache};
	Body->CreatePhysicsMeshesAsync(Provider, true, [&](const auto& Result) {
		++Notifications;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Superseded);
		EXPECT_FALSE(Result.Error);
		EXPECT_FALSE(Body->IsCompiling());
		Body->FinishPhysicsMeshes();
		EXPECT_EQ(Notifications, 1u);
	});
	ASSERT_TRUE(Cache->Entered.WaitFor(5.0));
	const auto Scheduler = AssetPrivate::GetAssetBuildScheduler();
	const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (Scheduler->GetScheduledMemory() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
	EXPECT_EQ(Scheduler->GetScheduledMemory(), 0u);
	EXPECT_EQ(GetPhysicsMeshCompilationManagerDiagnostics().AdmittedBuildCount, 0u);
	Body->AbortPhysicsMeshAsyncCreation();
	FAssetCompilingManager::Get().ProcessAsyncTasks();
	EXPECT_TRUE(Body->IsCompiling());
	EXPECT_EQ(Notifications, 0u);
	Cache->Release.Trigger();
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(Notifications, 1u);
	EXPECT_GT(Cache->Writes.load(), 0u); // Abort did not cancel the running DDC cook.
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	EXPECT_EQ(Scheduler->GetScheduledMemory(), 0u);
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, NotificationExceptionPreservesInstalledProductAndRetiresRecord)
{
	using namespace Durin;
	FAssetCompilingManager::Get().FinishAllCompilation();
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}};
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsNotificationException"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	uint32 Notifications = 0;
	Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) {
		++Notifications;
		EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Succeeded);
		EXPECT_TRUE(Body->GetResidentComplexGeometry());
		const auto During = GetPhysicsMeshCompilationManagerDiagnostics();
		EXPECT_EQ(During.OutstandingRecords, 0u);
		EXPECT_EQ(During.EstimatedWorkingSetBytes, 0u);
		Body->FinishPhysicsMeshes();
		EXPECT_EQ(GetPhysicsMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, During.EstimatedWorkingSetBytes);
		throw std::runtime_error("Injected physics notification failure.");
	});
	EXPECT_THROW(Body->FinishPhysicsMeshes(), std::runtime_error);
	EXPECT_EQ(Notifications, 1u);
	EXPECT_FALSE(Body->IsCompiling());
	EXPECT_EQ(GetPhysicsMeshCompilationManagerDiagnostics().OutstandingRecords, 0u);
	EXPECT_EQ(GetPhysicsMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, 0u);
	EXPECT_EQ(StaticMeshBuildTestSupport::GetMeshBuildDiagnosticsForTests().EstimatedWorkingSetBytes, 0u);
	EXPECT_TRUE(Body->GetResidentComplexGeometry());
	Body->CreatePhysicsMeshes(Provider, false);
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_EQ(GetPhysicsMeshCompilationManagerDiagnostics().EstimatedWorkingSetBytes, 0u);
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, CompletionPreservesCookFailureWithoutOwnerWrapper)
{
	using namespace Durin;
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			return FTriMeshCollisionData{{{0, 0, 0}}, {0, 0}};
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsCookFailureCompletion"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	std::optional<FPhysicsCookCompletionResult> Completed;
	uint32 Completions = 0;
	Body->CreatePhysicsMeshesAsync(Provider, false,
		[&](const FPhysicsCookCompletionResult& Result) { Completed = Result; ++Completions; });
	Body->FinishPhysicsMeshes();
	ASSERT_TRUE(Completed);
	EXPECT_EQ(Completions, 1u);
	EXPECT_EQ(Completed->Status, EPhysicsCookCompletionStatus::Failed);
	ASSERT_TRUE(Completed->Error);
	ASSERT_TRUE(Body->GetPhysicsMeshBuildError());
	EXPECT_NE(Completed->Error->find("Cook:"), std::string::npos);
	EXPECT_EQ(*Completed->Error, *Body->GetPhysicsMeshBuildError());
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Failed);
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	Body->InvalidatePhysicsData();
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
	EXPECT_FALSE(Completed->Error->empty());
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FPhysicsMeshLifecycleTests, SupersededCompletionPreservesInstalledGeometryWithoutError)
{
	using namespace Durin;
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}};
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, FName("PhysicsSupersededCompletion"));
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Body->CreatePhysicsMeshes(Provider, false);
	ASSERT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	const auto Geometry = Body->GetResidentComplexGeometry();
	std::optional<FPhysicsCookCompletionResult> Completed;
	{
		FStaticMeshWorkerBarrier Barrier;
		Body->CreatePhysicsMeshesAsync(Provider, false,
			[&](const FPhysicsCookCompletionResult& Result) { Completed = Result; });
		ASSERT_TRUE(Barrier.Wait(1));
		ASSERT_TRUE(Body->InstallPhysicsMeshes({}, Geometry));
		Barrier.Release();
		Body->FinishPhysicsMeshes();
	}
	ASSERT_TRUE(Completed);
	EXPECT_EQ(Completed->Status, EPhysicsCookCompletionStatus::Superseded);
	EXPECT_FALSE(Completed->Error);
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError());
	EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_EQ(Body->GetResidentComplexGeometry().GetIdentity(), Geometry.GetIdentity());
	MarkObjectHierarchyAsGarbage(Body);
}

TEST(FStaticMeshSourceTests, VertexInstancesRoundTripAndRejectInvalidTopology)
{
	using namespace Durin;
	auto Description = MakeResidencyGeometry();
	auto& Section = Description.Sections.front();
	Section.VertexInstanceVertices = {0, 1, 2, 0};
	Section.Indices = {0, 1, 2, 3, 2, 1};
	FStaticMeshAttributes Attributes(Description);
	Attributes.GetVertexInstanceUVs(0, 0) = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
	Attributes.GetVertexInstanceNormals(0) = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, -1}};
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(Description));
	const auto Identity = Source.GetIdentity();
	Source.ReleaseGeometry();
	const auto Read = Source.AcquireGeometry();
	ASSERT_TRUE(Read);
	const auto& Restored = (*Read)->Sections.front();
	EXPECT_EQ(Restored.Positions.size(), 3u);
	EXPECT_EQ(Restored.VertexInstanceVertices, Section.VertexInstanceVertices);
	EXPECT_EQ(Restored.Indices, Section.Indices);
	FStaticMeshConstAttributes RestoredAttributes(**Read);
	EXPECT_EQ(RestoredAttributes.GetVertexInstanceUVs(0, 0)[3], FVector2f(1, 1));
	EXPECT_EQ(RestoredAttributes.GetVertexInstanceNormals(0)[3], FVector3f(0, 0, -1));
	Section.VertexInstanceVertices[3] = 99;
	const auto InvalidVertex = Source.Initialize(Description);
	ASSERT_FALSE(InvalidVertex);
	EXPECT_EQ(InvalidVertex.error().Code, EStaticMeshSourceError::IndexRange);
	EXPECT_EQ(Source.GetIdentity(), Identity);
	Section.VertexInstanceVertices[3] = 0;
	Section.Indices.back() = 4;
	EXPECT_FALSE(Source.Initialize(Description));
	Section.Indices.back() = 1;
	Section.Normals.pop_back();
	const auto InvalidAttributes = Source.Initialize(Description);
	ASSERT_FALSE(InvalidAttributes);
	EXPECT_EQ(InvalidAttributes.error().Code, EStaticMeshSourceError::ChannelLength);
	EXPECT_EQ(Source.GetIdentity(), Identity);
}

TEST(FStaticMeshBuildModuleTests, SharedVerticesPreserveInstanceSeamsAndCollisionCoordinates)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Description = MakeResidencyGeometry();
	auto& Section = Description.Sections.front();
	// An unused geometric vertex must affect both projections' normalization identically.
	Section.Positions.push_back({3, 3, 0});
	Section.VertexInstanceVertices = {0, 1, 2, 0};
	Section.Indices = {0, 1, 2, 3, 2, 1};
	FStaticMeshAttributes Attributes(Description);
	Attributes.GetVertexInstanceNormals(0) = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, -1}};
	Attributes.GetVertexInstanceUVs(0, 0) = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(Description));
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("InstanceSeamMesh"));
	ASSERT_TRUE(Mesh->BuildFromSource(EStaticMeshBuildMode::Synchronous,
		{.Source = Source, .PreparedMaterialSlots = FStaticMeshTestAccess::MakeSlots(Description), .bPersistDerivedData = false}));
	const auto& LOD = Mesh->GetRenderData()->LODResources.front();
	const auto Positions = LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
	const auto Normals = LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetNormals();
	const auto UVs = LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords()[0];
	ASSERT_EQ(Positions.size(), 4u);
	EXPECT_EQ(Positions[0], Positions[3]);
	EXPECT_NE(Normals[0], Normals[3]);
	EXPECT_NE(UVs[0], UVs[3]);
	const auto Collision = Mesh->GetPhysicsTriMeshData();
	ASSERT_TRUE(Collision);
	ASSERT_EQ(Collision->Positions.size(), 4u);
	EXPECT_EQ(Collision->Indices, (std::vector<uint32>{0, 1, 2, 0, 2, 1}));
	EXPECT_EQ(Collision->Positions[0], Positions[0]);
	EXPECT_EQ(Collision->Positions[1], Positions[1]);
	EXPECT_EQ(Collision->Positions[2], Positions[2]);
}

TEST(FStaticMeshAuthoredCompilationTests, CurrentSourceBuildUsesExplicitModeAndObjectCompletion)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("ExplicitBuildMode"));
	EXPECT_FALSE(Mesh->Build(EStaticMeshBuildMode::Synchronous));
	ASSERT_TRUE(FStaticMeshTestAccess::Build(Mesh, MakeResidencyGeometry()));
	const auto Identity = Mesh->GetSource().GetIdentity();
	ASSERT_TRUE(Mesh->Build(EStaticMeshBuildMode::Synchronous, {.bPersistDerivedData = false}));
	EXPECT_FALSE(Mesh->IsCompiling());
	ASSERT_TRUE(Mesh->Build(EStaticMeshBuildMode::Asynchronous, {.bPersistDerivedData = false}));
	StaticMeshBuildTestSupport::FinishMeshBuildForTests(*Mesh);
	EXPECT_EQ(Mesh->GetCompilationDiagnostic().Status, EStaticMeshCompilationStatus::Succeeded);
	EXPECT_FALSE(Mesh->IsCompiling());
	EXPECT_EQ(Mesh->GetSource().GetIdentity(), Identity);
}

TEST(FStaticMeshBuildModuleTests, PositionExpansionRejectsInvalidMappingAndHonorsCancellation)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FCacheLogCapture LogSession;
	for (uint32 Scenario = 0; Scenario < 2; ++Scenario)
	{
		auto Geometry = MakeResidencyGeometry();
		auto& Mapping = Geometry.Sections.front().VertexInstanceVertices;
		if (Scenario == 0) Mapping = {0, 1, 99};
		else Mapping.assign(1024, 0);
		FStaticMeshRenderData Product;
		Product.MaterialSlots.push_back({"Previous", 7});
		uint32 Checks = 0;
		FAssetBuildTaskContext Control;
		if (Scenario == 1) Control.Cancellation = [&] { return ++Checks == 2; };
		EXPECT_FALSE(IMeshBuilderModule::Get()->BuildRender(Product, {
			.Geometry = std::make_shared<const FMeshDescription>(std::move(Geometry)),
			.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}},
			.Control = Control}));
		if (Scenario == 1) EXPECT_EQ(Checks, 2u);
		EXPECT_TRUE(Product.LODResources.empty());
		ASSERT_EQ(Product.MaterialSlots.size(), 1u);
		EXPECT_EQ(Product.MaterialSlots.front().Name, "Previous");
	}
}

TEST(FStaticMeshBuildModuleTests, SectionUVStreamsKeepLocalTangentCoordinatesAndFallbacks)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Geometry = MakeResidencyGeometry();
	Geometry.Sections.front().UVChannels[0] = {{0, 0}, {1, 0}, {0, 1}};
	Geometry.Sections.front().Normals.assign(3, FVector3f(0, 0, 2));
	auto Second = Geometry.Sections.front();
	Second.Name = "Second";
	Second.Normals.clear();
	Second.UVChannels[0] = {{0, 0}, {0, 1}, {1, 0}};
	Second.UVChannels[1] = {{2, 2}, {3, 2}, {2, 3}};
	auto Third = Second;
	Third.Name = "Fallback";
	Third.UVChannels[0][0].x = std::numeric_limits<float>::quiet_NaN();
	Third.UVChannels[1].clear();
	Geometry.Sections.push_back(std::move(Second));
	Geometry.Sections.push_back(std::move(Third));
	FStaticMeshRenderData Product;
	ASSERT_TRUE(IMeshBuilderModule::Get()->BuildRender(Product, {
		.Geometry = std::make_shared<const FMeshDescription>(std::move(Geometry)),
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}}}));
	const auto& LOD = Product.LODResources.front();
	const auto& Buffers = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const auto UVs = Buffers.TexCoordVertexBuffer.GetTexCoords();
	const auto Normals = Buffers.TangentsVertexBuffer.GetNormals();
	const auto Tangents = Buffers.TangentsVertexBuffer.GetTangents();
	ASSERT_EQ(Tangents.size(), 9u);
	EXPECT_EQ(LOD.NumTexCoords, 2u);
	EXPECT_EQ(UVs[0][1], FVector2f(1, 0));
	EXPECT_EQ(UVs[0][4], FVector2f(0, 1));
	EXPECT_EQ(UVs[1][3], FVector2f(2, 2));
	for (size_t Index = 0; Index < 9; ++Index)
	{
		EXPECT_EQ(Normals[Index], FVector3f(0, 0, 1));
		EXPECT_TRUE(Math::IsFinite(Tangents[Index]));
		if (Index < 3) EXPECT_EQ(FVector3f(Tangents[Index]), FVector3f(1, 0, 0));
		else if (Index < 6) EXPECT_EQ(FVector3f(Tangents[Index]), FVector3f(0, 1, 0));
		else EXPECT_EQ(UVs[0][Index], FVector2f(0));
		if (Index < 3 || Index >= 6) EXPECT_EQ(UVs[1][Index], FVector2f(0));
	}
}

TEST(FStaticMeshBuildModuleTests, GeneratedNormalsRemainSmoothAcrossUVInstances)
{
	using namespace Durin;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	auto Description = MakeResidencyGeometry();
	auto& Section = Description.Sections.front();
	Section.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	Section.VertexInstanceVertices = {0, 1, 2, 0, 3, 1};
	Section.Indices = {0, 1, 2, 3, 4, 5};
	FStaticMeshAttributes(Description).GetVertexInstanceUVs(0, 0) = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 1}, {0, 0}};
	FStaticMeshRenderData Product;
	ASSERT_TRUE(IMeshBuilderModule::Get()->BuildRender(Product, {
		.Geometry = std::make_shared<const FMeshDescription>(Description),
		.MaterialSlots = std::array{FStaticMeshBuildMaterialSlot{FName("Material"), "Material", 0}}}));
	const auto Normals = Product.LODResources.front().VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.GetNormals();
	ASSERT_EQ(Normals.size(), 6u);
	EXPECT_EQ(Normals[0], Normals[3]);
	EXPECT_EQ(Normals[1], Normals[5]);
	EXPECT_GT(Normals[0].y, 0.0f);
	EXPECT_GT(Normals[0].z, 0.0f);
}

TEST(FStaticMeshDerivedDataCacheTests, BuildFunctionRejectsMismatchedBuilderIdentity)
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	const auto* Module = IMeshBuilderModule::Get();
	ASSERT_NE(Module, nullptr);
	const auto Version = Module->GetBuildVersion();
	EXPECT_NE(Version, 0u);
	EXPECT_EQ(Version, Module->GetBuildVersion());
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	FStaticMeshBuildRequest Request{
		.Settings = MakeStaticMeshBuildSettings(FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry())),
		.Source = Source};
	auto Definition = MakeStaticMeshSessionDefinition(uint32(Request.Settings.MaterialSlots.size()), Version ^ 1ull);
	ASSERT_TRUE(Definition);
	auto Service = CreateBuild();
	ASSERT_TRUE(Service->Register(StaticMeshPrivate::MakeRenderBuildFunction()));
	auto Session = Service->CreateSession();
	ASSERT_TRUE(Session);
	std::optional<FBuildCompleteParams> Result;
	FBuildRequestOptions Options{.Policy = {.QueryCache = false, .StoreOnBuild = false}};
	Options.InputResolver = StaticMeshPrivate::MakeRenderInputResolver(Request);
	ASSERT_TRUE((*Session)->Build(*Definition, [&](auto Value) { Result = std::move(Value); }, {}, Options));
	ASSERT_TRUE(Result);
	EXPECT_EQ(Result->GetStatus(), EStatus::Error);
	EXPECT_EQ(Result->GetOutput(), nullptr);
	EXPECT_NE(Result->GetDiagnostic().find("identity does not match"), std::string_view::npos);
}

TEST(FStaticMeshDerivedDataCacheTests, WorkerSettingsCaptureOwnedValuesAndPreserveIdentity)
{
	using namespace Durin;
	auto Slots = FStaticMeshTestAccess::MakeSlots(MakeResidencyGeometry());
	const auto Expected = BuildStaticMeshReconciliationHash(Slots, 2.0f);
	const auto Settings = MakeStaticMeshBuildSettings(Slots, 2.0f);
	Slots.front().Name = FName("ChangedOwnerSlot");
	Slots.front().SourceName = "ChangedOwnerSource";
	Slots.front().SourceMaterialIndex = 99;
	EXPECT_EQ(Settings.NormalizedSize, 2.0f);
	EXPECT_EQ(BuildStaticMeshReconciliationHash(Settings.MaterialSlots, Settings.NormalizedSize), Expected);
	EXPECT_NE(BuildStaticMeshReconciliationHash(Slots, Settings.NormalizedSize), Expected);
}

TEST(FPhysicsCookTests, BuildFunctionRejectsMismatchedIdentityBeforeRecipeDecoding)
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	const uint64 Version = FCollisionCookedData::GetBuildVersion();
	EXPECT_NE(Version, 0u);
	EXPECT_EQ(Version, FCollisionCookedData::GetBuildVersion());
	FCookBodySetupInfo Info{
		.TriangleMeshDesc = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2}},
		.Mode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0};
	const auto Input = FPhysicsCookHelper::Capture(std::move(Info));
	ASSERT_TRUE(Input);
	// None is invalid for an executable cook; identity must be checked first.
	auto Definition = MakePhysicsCookSessionDefinition(EBodySetupCollisionSourceMode::None, Version ^ (uint64{1} << 63));
	ASSERT_TRUE(Definition);
	auto Service = CreateBuild();
	ASSERT_TRUE(Service->Register(PhysicsPrivate::MakeCollisionBuildFunction()));
	auto Session = Service->CreateSession();
	ASSERT_TRUE(Session);
	std::optional<FBuildCompleteParams> Result;
	FBuildRequestOptions Options{.Policy = {.QueryCache = false, .StoreOnBuild = false}};
	Options.InputResolver = PhysicsPrivate::MakeCollisionInputResolver(*Input);
	ASSERT_TRUE((*Session)->Build(*Definition, [&](auto Value) { Result = std::move(Value); }, {}, Options));
	ASSERT_TRUE(Result);
	EXPECT_EQ(Result->GetStatus(), EStatus::Error);
	EXPECT_EQ(Result->GetOutput(), nullptr);
	EXPECT_NE(Result->GetDiagnostic().find("identity does not match"), std::string_view::npos);
}

TEST(FPhysicsCookTests, CapturedTargetRejectsUnsupportedRepresentation)
{
	using namespace Durin;
	FCookBodySetupInfo Info{
		.TriangleMeshDesc = {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2}},
		.Mode = EBodySetupCollisionSourceMode::TriangleMeshFromLOD0};
	const auto Supported = FPhysicsCookHelper::Capture(Info, {}, EAssetPayloadTargetPlatform::Win64);
	ASSERT_TRUE(Supported);
	EXPECT_EQ(Supported->GetTargetPlatform(), EAssetPayloadTargetPlatform::Win64);
	const auto Unsupported = FPhysicsCookHelper::Capture(Info, {}, EAssetPayloadTargetPlatform::MacOS);
	ASSERT_FALSE(Unsupported);
	EXPECT_NE(Unsupported.error().find("unsupported"), std::string::npos);
	EXPECT_FALSE(MakePhysicsCookSessionDefinition(Info.Mode, FCollisionCookedData::GetBuildVersion(),
		0, EAssetPayloadTargetPlatform::MacOS));
}

TEST(FPhysicsCookTests, QuerySettingsReuseGeometryCacheAndPreservePublicationPolicy)
{
	using namespace Durin;
	using namespace Durin::DerivedData;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	FAssetCompilingManager::Get().FinishAllCompilation();
	FModuleManager::Get().LoadModuleChecked("MeshBuilder");
	FStaticMeshSource Source;
	ASSERT_TRUE(Source.Initialize(MakeResidencyGeometry()));
	auto Render = BuildRenderForTest({.Source = Source, .bPersistDerivedData = false});
	ASSERT_TRUE(Render);
	FStaticMeshPayloadData RenderPayload;
	ASSERT_TRUE(MakeStaticMeshPayloadData(**Render, RenderPayload));
	FByteBuffer RenderBytes;
	FCanonicalMemoryWriter RenderWriter(RenderBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
	RenderPayload.Serialize(RenderWriter);
	ASSERT_FALSE(RenderWriter.IsError());
	const auto Slots = FStaticMeshTestAccess::MakeSlots(Source);
	class FRecordingCache final : public ICache
	{
	public:
		mutable std::mutex Mutex;
		mutable std::vector<FCacheRecord> Records;
		mutable uint32 Hits = 0, Writes = 0;
		auto Get(const FCacheGetRequest& Request) const -> FCacheGetResult override
		{
			const std::lock_guard Lock(Mutex);
			for (const auto& Record : Records) if (Record.GetKey() == Request.Key)
			{
				++Hits;
				return std::optional{Record};
			}
			return std::optional<FCacheRecord>{};
		}
		auto Put(const FCachePutRequest& Request) const -> FCachePutResult override
		{
			const std::lock_guard Lock(Mutex);
			++Writes;
			Records.push_back(Request.Record);
			return {};
		}
	};
	struct FRestoreCache
	{
		~FRestoreCache()
		{
			FAssetCompilingManager::Get().FinishAllCompilation();
			PhysicsPrivate::SetAsyncCacheForTests({});
		}
	} Restore;
	auto Cache = std::make_shared<FRecordingCache>();
	PhysicsPrivate::SetAsyncCacheForTests(Cache);
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 0, 2}},
				{0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3}};
		}
	} Provider;
	for (auto Mode : {EBodySetupCollisionSourceMode::TriangleMeshFromLOD0, EBodySetupCollisionSourceMode::ConvexHullFromLOD0})
	for (auto Policy : {EBodySetupCollisionQueryPolicy::SimpleOnly, EBodySetupCollisionQueryPolicy::ComplexOnly,
		EBodySetupCollisionQueryPolicy::SimpleAndComplex})
	for (auto TraceMode : {EBodySetupCollisionTraceMode::SimpleAndComplex, EBodySetupCollisionTraceMode::SimpleAsComplex,
		EBodySetupCollisionTraceMode::ComplexAsSimple})
	{
		SCOPED_TRACE(std::format("mode={} policy={} trace={}", int(Mode), int(Policy), int(TraceMode)));
		auto* Body = NewObject<DBodySetup>(nullptr, "GeometryOnlyCook");
		Body->SetCollisionSourceMode(Mode);
		Body->SetCollisionQueryPolicy(Policy);
		Body->SetCollisionTraceMode(TraceMode);
		std::optional<FPhysicsCookCompletionResult> Completion;
		Body->CreatePhysicsMeshesAsync(Provider, true, [&](const auto& Result) { Completion = Result; });
		Body->FinishPhysicsMeshes();
		ASSERT_TRUE(Completion);
		ASSERT_EQ(Completion->Status, EPhysicsCookCompletionStatus::Succeeded);
		const auto& Geometry = Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0
			? Body->GetResidentComplexGeometry() : Body->GetResidentSimpleGeometry();
		ASSERT_TRUE(Geometry);
		FPhysicsCollisionPayloadData Payload;
		ASSERT_TRUE(MakePhysicsCollisionPayloadData(Geometry, Payload));
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
		Payload.Serialize(Writer);
		ASSERT_FALSE(Writer.IsError());
		FStaticMeshCookedProduct Product;
		const auto Decoded = DecodeStaticMeshCookedProduct(RenderBytes, Bytes, Slots, Mode, Product);
		ASSERT_TRUE(Decoded) << FormatCookedMeshProductError(Decoded.Error);
		ASSERT_TRUE(Body->InstallPhysicsMeshes(Product.SimpleCollision, Product.ComplexCollision));
		const auto Identity = Geometry.GetIdentity();
		const auto Generation = Body->GetPhysicsMeshRequestGeneration();
		FPhysicsBodyDesc Desc;
		const bool HasSimple = Mode == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			&& Policy != EBodySetupCollisionQueryPolicy::ComplexOnly;
		const bool HasComplex = Mode == EBodySetupCollisionSourceMode::TriangleMeshFromLOD0
			&& Policy != EBodySetupCollisionQueryPolicy::SimpleOnly;
		EXPECT_EQ(Body->PopulateCollisionGeometry(Desc, FTransform{}), HasSimple || HasComplex);
		EXPECT_EQ(bool(Desc.SimpleGeometry), HasSimple);
		EXPECT_EQ(bool(Desc.ComplexGeometry), HasComplex);
		EXPECT_EQ(Desc.TracePolicy, static_cast<EPhysicsCollisionTracePolicy>(TraceMode));
		Body->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::SimpleAndComplex);
		Body->SetCollisionTraceMode(EBodySetupCollisionTraceMode::SimpleAndComplex);
		EXPECT_EQ(Geometry.GetIdentity(), Identity);
		EXPECT_EQ(Body->GetPhysicsMeshRequestGeneration(), Generation);
		MarkObjectHierarchyAsGarbage(Body);
	}
	EXPECT_EQ(Cache->Records.size(), 2u);
	EXPECT_EQ(Cache->Writes, 2u);
	EXPECT_EQ(Cache->Hits, 16u);
}

TEST(FPhysicsMeshLifecycleTests, QueryAndSimpleEditsPreservePendingCookAndIndependentCaches)
{
	using namespace Durin;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	struct FProvider final : IInterface_CollisionDataProvider
	{
		mutable uint32 Captures = 0;
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			++Captures;
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}};
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, "IndependentCollisionDomains");
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	std::optional<FPhysicsCookCompletionResult> Completion;
	uint32 Completions = 0;
	{
		FStaticMeshWorkerBarrier Barrier;
		Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) { Completion = Result; ++Completions; });
		ASSERT_TRUE(Barrier.Wait(1));
		const auto Generation = Body->GetPhysicsMeshRequestGeneration();
		const auto Revision = Body->GetRevision();
		{
			const FScopedBodySetupUpdate Update(*Body);
			ASSERT_TRUE(Body->SetSphere(0.5, {0, 3, 0}));
			Body->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::ComplexOnly);
			{
				const FScopedBodySetupUpdate Nested(*Body);
				Body->SetCollisionTraceMode(EBodySetupCollisionTraceMode::ComplexAsSimple);
			}
			EXPECT_EQ(Body->GetRevision(), Revision);
		}
		EXPECT_EQ(Body->GetRevision(), Revision + 1);
		EXPECT_EQ(Body->GetPhysicsMeshRequestGeneration(), Generation);
		EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Pending);
		Barrier.Release();
		Body->FinishPhysicsMeshes();
	}
	ASSERT_TRUE(Completion);
	EXPECT_EQ(Completion->Status, EPhysicsCookCompletionStatus::Succeeded);
	EXPECT_EQ(Completions, 1u); EXPECT_EQ(Provider.Captures, 1u);
	ASSERT_TRUE(Body->GetResidentComplexGeometry());
	const auto Complex = Body->GetResidentComplexGeometry();
	const auto Generation = Body->GetPhysicsMeshRequestGeneration();
	FSimpleCollisionElement Hull; Hull.Type = EBodySetupShapeType::Convex;
	Hull.Vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
	ASSERT_TRUE(Body->SetSimpleCollision({Hull}));
	FCollisionGeometryRef Simple; ASSERT_TRUE(Body->BuildSimpleGeometry(Simple));
	EXPECT_EQ(Body->GetResidentComplexGeometry().GetIdentity(), Complex.GetIdentity());
	Body->SetCollisionQueryPolicy(EBodySetupCollisionQueryPolicy::SimpleAndComplex);
	Body->SetCollisionTraceMode(EBodySetupCollisionTraceMode::SimpleAndComplex);
	ASSERT_TRUE(Body->InstallPhysicsMeshes({}, Complex));
	for (uint32 Index = 0; Index < 3; ++Index)
	{
		EXPECT_TRUE(Body->IsValid());
		FCollisionGeometryRef Current; ASSERT_TRUE(Body->BuildSimpleGeometry(Current));
		EXPECT_EQ(Current.GetIdentity(), Simple.GetIdentity());
	}
	Body->InvalidatePhysicsData();
	const auto Invalidated = Body->GetPhysicsMeshRequestGeneration();
	Body->InvalidatePhysicsData();
	EXPECT_EQ(Body->GetPhysicsMeshRequestGeneration(), Invalidated);
	EXPECT_GT(Invalidated, Generation);
	FCollisionGeometryRef Current; ASSERT_TRUE(Body->BuildSimpleGeometry(Current));
	EXPECT_EQ(Current.GetIdentity(), Simple.GetIdentity());
	EXPECT_FALSE(Body->GetResidentComplexGeometry());
	MarkObjectHierarchyAsGarbage(Body); CollectGarbage();
}

TEST(FPhysicsMeshLifecycleTests, ExternalPublicationSupersedesAnOlderFailedCook)
{
	using namespace Durin;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	struct FProvider final : IInterface_CollisionDataProvider
	{
		bool bDegenerate = false;
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{
			return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, bDegenerate ? std::vector<uint32>{0, 0, 0} : std::vector<uint32>{0, 1, 2}};
		}
	} Provider;
	auto* Body = NewObject<DBodySetup>(nullptr, "FailedCookSupersededByPublication");
	Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
	Body->CreatePhysicsMeshes(Provider, false);
	const auto Published = Body->GetResidentComplexGeometry(); ASSERT_TRUE(Published);
	Provider.bDegenerate = true;
	std::optional<FPhysicsCookCompletionResult> Completion;
	{
		FStaticMeshWorkerBarrier Barrier;
		Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) { Completion = Result; });
		ASSERT_TRUE(Barrier.Wait(1));
		const auto Superseded = Body->GetPhysicsMeshRequestGeneration();
		ASSERT_TRUE(Body->InstallPhysicsMeshes({}, Published));
		EXPECT_GT(Body->GetPhysicsMeshRequestGeneration(), Superseded);
		Barrier.Release(); Body->FinishPhysicsMeshes();
	}
	ASSERT_TRUE(Completion); EXPECT_EQ(Completion->Status, EPhysicsCookCompletionStatus::Superseded);
	EXPECT_FALSE(Body->GetPhysicsMeshBuildError()); EXPECT_EQ(Body->GetPhysicsMeshBuildStatus(), EPhysicsMeshBuildStatus::Ready);
	EXPECT_EQ(Body->GetResidentComplexGeometry().GetIdentity(), Published.GetIdentity());
	MarkObjectHierarchyAsGarbage(Body); CollectGarbage();
}

TEST(FPhysicsMeshLifecycleTests, BatchedRecipeAndExplicitRequestDoNotScheduleASecondCook)
{
	using namespace Durin;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	struct FProvider final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return true; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{ return FTriMeshCollisionData{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}, {0, 1, 2}}; }
	} Provider;
	auto* Mesh = DStaticMesh::CreateDebugTriangle();
	auto* Body = NewObject<DBodySetup>(Mesh, "ExplicitBatchBody"); ASSERT_TRUE(Mesh->SetBodySetup(Body));
	const auto Revision = Body->GetRevision();
	uint64 Request = 0;
	uint32 Completions = 0;
	{
		const FScopedBodySetupUpdate Update(*Body);
		Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) {
			EXPECT_EQ(Result.Status, EPhysicsCookCompletionStatus::Succeeded); ++Completions;
		});
		Request = Body->GetPhysicsMeshRequestGeneration();
	}
	EXPECT_EQ(Body->GetRevision(), Revision + 1);
	EXPECT_EQ(Body->GetPhysicsMeshRequestGeneration(), Request);
	Body->FinishPhysicsMeshes();
	EXPECT_EQ(Completions, 1u); EXPECT_EQ(Body->GetPhysicsMeshRequestGeneration(), Request);
	EXPECT_EQ(Body->GetCollisionBuildRevision(), 1u);
	EXPECT_TRUE(Body->GetResidentComplexGeometry());
	MarkObjectHierarchyAsGarbage(Mesh); CollectGarbage();
}

TEST(FPhysicsMeshLifecycleTests, ScopedUpdateToleratesOwnerDestructionFromInlineCompletion)
{
	using namespace Durin;
	InitializeDObjectSystem();
	if (!FAssetCompilingManager::Get().IsAcceptingRequests()) ASSERT_TRUE(InitializeAssetCompilingManager());
	FAssetCompilingManager::Get().FinishAllCompilation();
	struct FUnavailable final : IInterface_CollisionDataProvider
	{
		auto ContainsPhysicsTriMeshData() const -> bool override { return false; }
		auto GetPhysicsTriMeshData() const -> std::expected<FTriMeshCollisionData, std::string> override
		{ return std::unexpected("No collision input."); }
	} Provider;
	for (const auto Mode : {EBodySetupCollisionSourceMode::None, EBodySetupCollisionSourceMode::TriangleMeshFromLOD0})
	{
		auto* Body = NewObject<DBodySetup>(nullptr, "DestroyedScopedCollisionOwner");
		const FObjectKey Key(Body);
		uint32 Completions = 0;
		{
			const FScopedBodySetupUpdate Update(*Body);
			Body->SetCollisionSourceMode(Mode);
			ASSERT_TRUE(Body->SetSphere(0.5));
			Body->CreatePhysicsMeshesAsync(Provider, false, [&](const FPhysicsCookCompletionResult& Result) {
				EXPECT_EQ(Result.Status, Mode == EBodySetupCollisionSourceMode::None
					? EPhysicsCookCompletionStatus::Succeeded : EPhysicsCookCompletionStatus::Failed);
				++Completions;
				MarkObjectHierarchyAsGarbage(Body); CollectGarbage();
				EXPECT_EQ(ResolveObjectKey(Key), nullptr);
			});
		}
		EXPECT_EQ(Completions, 1u); EXPECT_EQ(ResolveObjectKey(Key), nullptr);
	}
}
