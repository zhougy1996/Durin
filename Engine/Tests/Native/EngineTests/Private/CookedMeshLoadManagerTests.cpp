#include <gtest/gtest.h>
#include <semaphore>

#include "Asset/CookedMeshLoadManagerPrivate.h"
#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "HAL/PlatformLTS.h"
#include "NativeTestSupport.h"
#include "Physics/BodySetup.h"
#include "Physics/PhysicsDerivedData.h"
#include "Rendering/SceneInterface.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshLoadUpdate.h"
#include "StaticMeshTestAccess.h"
#include "Threading/RunnableThread.h"

namespace
{
	using namespace Durin;

	class FControlledPackageResource final : public FPackageResource
	{
	public:
		explicit FControlledPackageResource(FByteBuffer InBytes, bool bInFail = false)
			: FPackageResource(InBytes.size()), Bytes(std::move(InBytes)), bFail(bInFail) {}
		auto Release() -> void { if (!bReleased.exchange(true)) Gate.release(); }
		auto HasStarted() const -> bool { return bStarted.load(std::memory_order_acquire); }

	private:
		auto ReadRangeImpl(uint64, uint64 Size, const std::atomic_bool& bCancelled)
			-> FPackageResourceReadResult override
		{
			bStarted.store(true, std::memory_order_release);
			if (!bReleased.load(std::memory_order_acquire)) Gate.acquire();
			if (bCancelled.load(std::memory_order_acquire))
				return std::unexpected(FPackageResourceReadError{
					.Status = EPackageResourceReadStatus::Cancelled, .Message = "Fixture read was cancelled."});
			if (bFail)
				return std::unexpected(FPackageResourceReadError{
					.Status = EPackageResourceReadStatus::IoError, .Message = "Fixture storage is unavailable."});
			EXPECT_EQ(Size, Bytes.size());
			return FSharedByteBuffer::Take(FByteBuffer(Bytes));
		}
		FByteBuffer Bytes;
		bool bFail;
		std::binary_semaphore Gate{0};
		std::atomic_bool bStarted = false;
		std::atomic_bool bReleased = false;
	};

	// Release blocked I/O before a local scheduler is destroyed, including assertion exits.
	struct FReadGates
	{
		std::vector<std::shared_ptr<FControlledPackageResource>> Resources;
		~FReadGates() { for (const auto& Resource : Resources) Resource->Release(); }
		auto Add(const FByteBuffer& Bytes, bool bFail = false) -> std::shared_ptr<FControlledPackageResource>
		{
			auto Resource = std::make_shared<FControlledPackageResource>(Bytes, bFail);
			Resources.push_back(Resource);
			return Resource;
		}
		auto Release() -> void { for (const auto& Resource : Resources) Resource->Release(); }
	};

	auto MakePayload() -> FByteBuffer
	{
		FStaticMeshPayloadData Payload;
		Payload.LocalBounds = FBox(FVector3(0), FVector3(1, 1, 0));
		Payload.MaterialSlotCount = 1;
		auto& LOD = Payload.LODs.emplace_back();
		LOD.LocalBounds = Payload.LocalBounds;
		LOD.Positions = {FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 1, 0)};
		LOD.Normals.assign(3, FVector3f(0, 0, 1));
		LOD.Tangents.assign(3, FVector4f(1, 0, 0, 1));
		LOD.TexCoords[0] = {FVector2f(0, 0), FVector2f(1, 0), FVector2f(0, 1)};
		LOD.NumTexCoords = 1;
		LOD.Indices = {0, 1, 2};
		LOD.Sections = {{.FirstIndex = 0, .IndexCount = 3, .MinVertexIndex = 0,
			.MaxVertexIndex = 2, .MaterialSlotIndex = 0, .LocalBounds = Payload.LocalBounds}};
		FByteBuffer Bytes;
		FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
		Payload.Serialize(Writer);
		EXPECT_FALSE(Writer.IsError());
		return Bytes;
	}

	class FPublicationTestScene final : public FSceneInterface
	{
	public:
		auto Release() -> void override {}
		auto AddPrimitive(DPrimitiveComponent*) -> void override
		{
			if (!ObservedMesh) return;
			++Publications;
			EXPECT_NE(ObservedMesh->GetRenderData(), nullptr);
			EXPECT_TRUE(ObservedMesh->GetBodySetup()->GetResidentComplexGeometry());
		}
		auto RemovePrimitive(DPrimitiveComponent*) -> void override {}
		auto AddLight(DLightComponent*) -> void override {}
		auto RemoveLight(DLightComponent*) -> void override {}
		auto AddSkyBox(DSkyBoxComponent*) -> void override {}
		auto RemoveSkyBox(DSkyBoxComponent*) -> void override {}
		auto AddProceduralSky(DProceduralSkyComponent*) -> void override {}
		auto RemoveProceduralSky(DProceduralSkyComponent*) -> void override {}
		auto AddSkyLight(DSkyLightComponent*) -> void override {}
		auto RemoveSkyLight(DSkyLightComponent*) -> void override {}
		auto AddVolumetricCloud(DVolumetricCloudComponent*) -> void override {}
		auto RemoveVolumetricCloud(DVolumetricCloudComponent*) -> void override {}
		auto UpdateSkyLighting() -> void override {}
		auto UpdatePrimitiveTransform(FPrimitiveComponentId, const FMatrix&) -> void override {}
		auto UpdatePrimitiveVisibility(FPrimitiveComponentId, bool) -> void override {}
		auto UpdatePrimitiveMaterialBinding(FPrimitiveComponentId, const FMaterialRenderProxyBindingUpdate&) -> void override {}
		auto UpdateMaterialParameterCollection(std::shared_ptr<const FMaterialParameterCollectionSnapshot>) -> void override {}
		auto RemoveMaterialParameterCollection(FGuid) -> void override {}
		auto UpdatePrimitiveCustomData(FPrimitiveComponentId, FPrimitiveCustomData) -> void override {}
		auto UpdateStaticMeshRenderInstances(FPrimitiveComponentId, FStaticMeshRenderInstances) -> void override {}
		auto UpdateSplineMeshDynamicData(FPrimitiveComponentId, FSplineMeshRenderDynamicData) -> void override {}
		DStaticMesh* ObservedMesh = nullptr;
		uint32 Publications = 0;
	};

	class FCookedMeshLoadManagerTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			InitializeDObjectSystem();
			if (!GIsGameThreadIdInitialized)
			{
				GGameThreadId = FPlatformLTS::GetCurrentThreadId();
				GIsGameThreadIdInitialized = true;
			}
		}
		auto TearDown() -> void override
		{
			for (auto& Mesh : Meshes)
			{
				auto* Retired = Mesh.Get();
				Mesh.Reset();
				MarkObjectHierarchyAsGarbage(Retired);
			}
			CollectGarbage(EObjectFlags::Standalone);
		}
		auto MakeMesh(FBulkData Bulk) -> DStaticMesh*
		{
			auto* Mesh = NewObject<DStaticMesh>(nullptr, FName("CookedMeshUpdate" + std::to_string(Meshes.size())));
			Meshes.emplace_back(Mesh);
			FStaticMeshTestAccess::SetMaterialSlots(Mesh, {{.Name = FName("Slot0")}});
			const_cast<FBulkData&>(Mesh->GetCookedRenderData()) = std::move(Bulk);
			return Mesh;
		}
		auto MakeMesh(const std::shared_ptr<FControlledPackageResource>& Resource, uint64 Size) -> DStaticMesh*
		{
			auto Bulk = FBulkData::TryAttach({.LogicalSize = Size,
				.Range = {.Resource = Resource, .StoredSize = Size, .Alignment = 1}});
			EXPECT_TRUE(Bulk);
			return MakeMesh(Bulk ? std::move(*Bulk) : FBulkData{});
		}
		auto Submit(FCookedMeshLoadManager& Manager, DStaticMesh& Mesh) -> std::shared_ptr<FStaticMeshLoadUpdate>
		{
			auto Update = std::make_shared<FStaticMeshLoadUpdate>(Mesh, false);
			FStaticMeshTestAccess::SetPendingUpdate(&Mesh, Update);
			EXPECT_TRUE(Manager.Submit(Update));
			return Update;
		}
		template<typename Predicate>
		auto PumpUntil(FCookedMeshLoadManager& Manager, Predicate&& Done) -> bool
		{
			const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!Done() && std::chrono::steady_clock::now() < Deadline)
			{
				Manager.Pump();
				std::this_thread::yield();
			}
			return Done();
		}
		std::vector<TStrongObjectPtr<DStaticMesh>> Meshes;
	};
}

TEST_F(FCookedMeshLoadManagerTests, QueuesConcreteUpdatesWithinConcurrencyLimit)
{
	const auto Bytes = MakePayload();
	FCookedMeshLoadManager Manager({.MaxConcurrentRequests = 2});
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	std::vector<std::shared_ptr<FStaticMeshLoadUpdate>> Updates;
	for (int Index = 0; Index < 6; ++Index)
		Updates.push_back(Submit(Manager, *MakeMesh(Gates.Add(Bytes), Bytes.size())));
	Manager.Pump();
	EXPECT_EQ(Manager.GetDiagnostics().InFlightCount, 2u);
	EXPECT_EQ(Manager.GetDiagnostics().PendingRequestCount, 4u);
	Gates.Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] { return Manager.GetDiagnostics().SucceededCount == 6; }));
	for (const auto& Update : Updates) EXPECT_TRUE(Update->IsSuccessfullyFinished());
	for (const auto& Mesh : Meshes)
	{
		EXPECT_NE(Mesh->GetRenderData(), nullptr);
		EXPECT_FALSE(Mesh->HasPendingRenderDataLoad());
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Readiness, EStaticMeshRenderResourceReadiness::Unavailable);
	}
	EXPECT_EQ(Manager.GetDiagnostics().PeakInFlightCount, 2u);
}

TEST_F(FCookedMeshLoadManagerTests, CancelsQueuedAndReadingUpdatesWithoutPublishing)
{
	const auto Bytes = MakePayload();
	FCookedMeshLoadManager Manager({.MaxConcurrentRequests = 1});
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	auto FirstResource = Gates.Add(Bytes);
	auto PendingResource = Gates.Add(Bytes);
	auto* First = MakeMesh(FirstResource, Bytes.size());
	auto* Second = MakeMesh(PendingResource, Bytes.size());
	auto Active = Submit(Manager, *First);
	auto Queued = Submit(Manager, *Second);
	Manager.Pump();
	EXPECT_TRUE(First->HasPendingRenderDataLoad());
	Second->CancelPendingRenderDataLoad();
	EXPECT_TRUE(Queued->IsCompleted());
	EXPECT_TRUE(Queued->IsCancelled());
	EXPECT_EQ(PendingResource->GetReadStats().RequestCount, 0u);
	EXPECT_EQ(Second->GetRenderDataLoadError().Code, ECookedMeshLoadError::Cancelled);
	First->CancelPendingRenderDataLoad();
	Gates.Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] { return Active->IsCompleted(); }));
	EXPECT_TRUE(Active->IsCancelled());
	EXPECT_EQ(First->GetRenderData(), nullptr);
	EXPECT_EQ(Second->GetRenderData(), nullptr);
	EXPECT_FALSE(Second->RequestRenderDataAndResources());
	Manager.Pump();
	EXPECT_EQ(Manager.GetDiagnostics().CancelledCount, 2u);
}

TEST_F(FCookedMeshLoadManagerTests, ShutdownDrainsActiveAndQueuedUpdates)
{
	const auto Bytes = MakePayload();
	FCookedMeshLoadManager Manager({.MaxConcurrentRequests = 1});
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	auto First = Submit(Manager, *MakeMesh(Gates.Add(Bytes), Bytes.size()));
	auto PendingResource = Gates.Add(Bytes);
	auto Second = Submit(Manager, *MakeMesh(PendingResource, Bytes.size()));
	Manager.Pump();
	Gates.Release();
	Manager.Shutdown();
	EXPECT_TRUE(First->IsCompleted());
	EXPECT_TRUE(Second->IsCompleted());
	EXPECT_TRUE(First->IsCancelled());
	EXPECT_TRUE(Second->IsCancelled());
	EXPECT_EQ(PendingResource->GetReadStats().RequestCount, 0u);
	EXPECT_EQ(Manager.GetDiagnostics().CancelledCount, 2u);
	EXPECT_EQ(Manager.GetDiagnostics().InFlightCount, 0u);
	EXPECT_EQ(Manager.GetDiagnostics().PendingRequestCount, 0u);
}

TEST_F(FCookedMeshLoadManagerTests, DestroyWaitsForAcceptedReadsBeforeReleasingMesh)
{
	const auto Bytes = MakePayload();
	FCookedMeshLoadManager Manager;
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	auto Resource = Gates.Add(Bytes);
	auto* Mesh = MakeMesh(Resource, Bytes.size());
	auto Update = Submit(Manager, *Mesh);
	ASSERT_TRUE(PumpUntil(Manager, [&] { return Resource->HasStarted(); }));
	Mesh->BeginDestroy();
	EXPECT_FALSE(Mesh->IsReadyForFinishDestroy());
	Resource->Release();
	Update->Finish();
	EXPECT_TRUE(Update->IsCompleted());
	EXPECT_TRUE(Update->IsCancelled());
	EXPECT_TRUE(Mesh->IsReadyForFinishDestroy());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
}

TEST_F(FCookedMeshLoadManagerTests, BlockingFinishOnlyPublishesSelectedUpdate)
{
	const auto Bytes = MakePayload();
	FCookedMeshLoadManager Manager;
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	auto* Selected = MakeMesh(Gates.Add(Bytes), Bytes.size());
	auto* Unrelated = MakeMesh(Gates.Add(Bytes), Bytes.size());
	auto First = Submit(Manager, *Selected);
	auto Second = Submit(Manager, *Unrelated);
	Manager.Pump();
	Gates.Release();
	First->Finish();
	EXPECT_TRUE(First->IsSuccessfullyFinished());
	EXPECT_NE(Selected->GetRenderData(), nullptr);
	EXPECT_FALSE(Second->IsCompleted());
	EXPECT_EQ(Unrelated->GetRenderData(), nullptr);
	Second->Abort();
	Second->Finish();
}

TEST_F(FCookedMeshLoadManagerTests, InvalidFieldsAndDecodeAndReadFailuresStayOnAsset)
{
	FCookedMeshLoadManager Manager;
	ASSERT_TRUE(Manager.Initialize());
	auto* Invalid = MakeMesh(FBulkData{});
	auto InvalidUpdate = std::make_shared<FStaticMeshLoadUpdate>(*Invalid, false);
	FStaticMeshTestAccess::SetPendingUpdate(Invalid, InvalidUpdate);
	EXPECT_FALSE(Manager.Submit(InvalidUpdate));
	EXPECT_TRUE(InvalidUpdate->IsCompleted());
	EXPECT_EQ(Invalid->GetRenderDataLoadError().Code, ECookedMeshLoadError::Read);
	EXPECT_FALSE(Invalid->HasPendingRenderDataLoad());
	auto Bulk = FBulkData::TryCreateDetached(FByteBuffer{std::byte{0xff}});
	ASSERT_TRUE(Bulk);
	auto* Malformed = MakeMesh(std::move(*Bulk));
	auto Decode = Submit(Manager, *Malformed);
	FReadGates Gates;
	const auto Bytes = MakePayload();
	auto* Unreadable = MakeMesh(Gates.Add(Bytes, true), Bytes.size());
	auto Read = Submit(Manager, *Unreadable);
	Gates.Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] { return Decode->IsCompleted() && Read->IsCompleted(); }));
	EXPECT_EQ(Malformed->GetRenderDataLoadError().Code, ECookedMeshLoadError::Product);
	EXPECT_FALSE(Malformed->GetRenderDataLoadError().Diagnostic.empty());
	EXPECT_EQ(Unreadable->GetRenderDataLoadError().Code, ECookedMeshLoadError::Read);
	EXPECT_NE(Unreadable->GetRenderDataLoadError().Diagnostic.find("Fixture storage"), std::string::npos);
	EXPECT_EQ(Malformed->GetRenderData(), nullptr);
	EXPECT_EQ(Unreadable->GetRenderData(), nullptr);
}

TEST_F(FCookedMeshLoadManagerTests, CompletionLimitDefersResultsWithoutFailingLoads)
{
	FCookedMeshLoadManager Manager({.MaxCompletionsPerPump = 1});
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	const auto Bytes = MakePayload();
	for (int Index = 0; Index < 4; ++Index)
		Submit(Manager, *MakeMesh(Gates.Add(Bytes), Bytes.size()));
	Manager.Pump();
	Gates.Release();
	uint64 PreviousSucceeded = 0;
	ASSERT_TRUE(PumpUntil(Manager, [&] {
		const auto Succeeded = Manager.GetDiagnostics().SucceededCount;
		EXPECT_LE(Succeeded - PreviousSucceeded, 1u);
		PreviousSucceeded = Succeeded;
		return Succeeded == 4;
	}));
	for (const auto& Mesh : Meshes)
	{
		EXPECT_NE(Mesh->GetRenderData(), nullptr);
		EXPECT_EQ(Mesh->GetRenderDataLoadError().Code, ECookedMeshLoadError::None);
	}
}

TEST_F(FCookedMeshLoadManagerTests, BlockingRetryRetiresCancelledQueuedAndReadingUpdates)
{
	Testing::FScopedAssetRuntimeForTests Runtime;
	const auto Root = std::filesystem::absolute(Testing::GetTestWorkDirectory() / "CancelledMeshRetry");
	std::filesystem::create_directories(Root);
	ASSERT_TRUE(Runtime.RestartCooked(Root));
	FCookedMeshLoadManager Manager;
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	const auto Bytes = MakePayload();
	for (bool bStartRead : {false, true})
	{
		auto Resource = Gates.Add(Bytes);
		auto* Mesh = MakeMesh(Resource, Bytes.size());
		auto Update = Submit(Manager, *Mesh);
		if (bStartRead)
			ASSERT_TRUE(PumpUntil(Manager, [&] { return Resource->HasStarted(); }));
		Mesh->CancelPendingRenderDataLoad();
		EXPECT_FALSE(Mesh->RequestRenderDataAndResources());
		EXPECT_EQ(Mesh->HasPendingRenderDataLoad(), bStartRead);
		Resource->Release();
		ASSERT_TRUE(Mesh->EnsureRenderDataLoadedBlocking());
		EXPECT_TRUE(Update->IsCompleted());
		EXPECT_TRUE(Update->IsCancelled());
		EXPECT_NE(Mesh->GetRenderData(), nullptr);
		EXPECT_FALSE(Mesh->HasPendingRenderDataLoad());
		EXPECT_EQ(Mesh->GetRenderDataLoadError().Code, ECookedMeshLoadError::None);
		EXPECT_EQ(Mesh->GetRenderResourceStatus().Readiness, EStaticMeshRenderResourceReadiness::Unavailable);
		Manager.Pump();
	}
	EXPECT_EQ(Manager.GetDiagnostics().CancelledCount, 2u);
}

TEST_F(FCookedMeshLoadManagerTests, PublicationRefreshesConsumersOnceAfterRenderAndCollisionAreInstalled)
{
	FPublicationTestScene Scene;
	TStrongObjectPtr<DWorld> World(NewObject<DWorld>(nullptr, "CookedPublicationWorld"));
	struct FWorldCleanup
	{
		TStrongObjectPtr<DWorld>& World;
		~FWorldCleanup()
		{
			World->CleanupWorld();
			World->SetRenderScene(nullptr);
			auto* Retired = World.Get();
			World.Reset();
			MarkObjectHierarchyAsGarbage(Retired);
		}
	} Cleanup{World};
	World->InitializeSubsystems();
	ASSERT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World.Get(), "CookedPublicationLevel")));
	World->SetRenderScene(&Scene);
	const std::vector<FVector3> Positions{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	const std::vector<uint32> Indices{0, 1, 2};
	FPhysicsCollisionPayloadData Collision;
	ASSERT_TRUE(MakePhysicsCollisionPayloadData(FCollisionGeometryRef::BuildTriangleMesh(Positions, Indices), Collision));
	FByteBuffer CollisionBytes;
	FCanonicalMemoryWriter Writer(CollisionBytes, EArchivePurpose::CookedPayload, {.Target = {"Win64", "Game"}});
	Collision.Serialize(Writer);
	ASSERT_FALSE(Writer.IsError()) << Writer.GetError();
	for (bool bAsync : {false, true})
	{
		auto Bulk = FBulkData::TryCreateDetached(MakePayload());
		ASSERT_TRUE(Bulk);
		auto* Mesh = MakeMesh(std::move(*Bulk));
		auto* Body = NewObject<DBodySetup>(Mesh, "CookedPublicationBody");
		Body->SetCollisionSourceMode(EBodySetupCollisionSourceMode::TriangleMeshFromLOD0);
		ASSERT_TRUE(Mesh->SetBodySetup(Body));
		auto CollisionBulk = FBulkData::TryCreateDetached(CollisionBytes);
		ASSERT_TRUE(CollisionBulk);
		const_cast<FBulkData&>(Mesh->GetCookedCollisionData()) = std::move(*CollisionBulk);
		auto* Actor = World->SpawnActor<AStaticMeshActor>();
		ASSERT_NE(Actor, nullptr);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Scene.ObservedMesh = Mesh;
		Scene.Publications = 0;
		FCookedMeshLoadManager Manager;
		ASSERT_TRUE(Manager.Initialize());
		auto Update = Submit(Manager, *Mesh);
		if (bAsync) ASSERT_TRUE(PumpUntil(Manager, [&] { return Update->IsCompleted(); }));
		else Update->Finish();
		EXPECT_TRUE(Update->IsSuccessfullyFinished());
		EXPECT_EQ(Scene.Publications, 1u);
		Scene.ObservedMesh = nullptr;
	}
}

TEST_F(FCookedMeshLoadManagerTests, MetadataChangeDiscardsCandidate)
{
	FCookedMeshLoadManager Manager;
	ASSERT_TRUE(Manager.Initialize());
	FReadGates Gates;
	const auto Bytes = MakePayload();
	auto* Mesh = MakeMesh(Gates.Add(Bytes), Bytes.size());
	auto Update = Submit(Manager, *Mesh);
	Manager.Pump();
	EXPECT_TRUE(Mesh->RenameMaterialSlot(0, FName("ChangedSlot")));
	Gates.Release();
	ASSERT_TRUE(PumpUntil(Manager, [&] { return Update->IsCompleted(); }));
	EXPECT_TRUE(Update->IsCancelled());
	EXPECT_EQ(Mesh->GetRenderData(), nullptr);
}

TEST_F(FCookedMeshLoadManagerTests, StoppedSchedulerTerminatesUpdateWithoutRetry)
{
	FCookedMeshLoadManager Manager;
	auto Bulk = FBulkData::TryCreateDetached(MakePayload());
	ASSERT_TRUE(Bulk);
	auto* Mesh = MakeMesh(std::move(*Bulk));
	auto Update = std::make_shared<FStaticMeshLoadUpdate>(*Mesh, false);
	FStaticMeshTestAccess::SetPendingUpdate(Mesh, Update);
	EXPECT_FALSE(Manager.Submit(Update));
	EXPECT_TRUE(Update->IsCompleted());
	EXPECT_EQ(Mesh->GetRenderDataLoadError().Code, ECookedMeshLoadError::Unavailable);
	EXPECT_FALSE(Mesh->HasPendingRenderDataLoad());
	EXPECT_FALSE(Mesh->RequestRenderDataAndResources());
}
