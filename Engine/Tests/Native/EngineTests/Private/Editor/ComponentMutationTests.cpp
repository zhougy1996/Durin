#include "Editor/EditorActorSubsystemTestSupport.h"
#include "Transactions/Transactor.h"
#include "Actors/ActorChange.h"

#include "Editor/EditorTransactionTestSupport.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/CameraActor.h"
#include "Actors/SplineMeshActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "DObject/PackageLinker.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/PackageCapture.h"
#include "DObject/WeakObjectPtr.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "NativeDObjectTestSupport.h"
#include "NativeTestSupport.h"
#include "Misc/MountPathTestSupport.h"
#include "Rendering/LightSceneProxy.h"
#include "Rendering/SceneInterface.h"
#include "Rendering/SplineMeshSceneProxy.h"
#include "Transactions/Transaction.h"
#include <gtest/gtest.h>

namespace Durin
{
	// Observes creation, registration and teardown on the actual retained component.
	class FStructuralTestLight : public DLightComponent
	{
		static auto GetPrivateStaticClass() -> DClass*;
		DECLARE_CLASS(FStructuralTestLight, DLightComponent, FStructuralTestLight::GetPrivateStaticClass)
		DEFINE_DEFAULT_OBJECT_INITIALIZER_CONSTRUCTOR_CALL(FStructuralTestLight)
	public:
		explicit FStructuralTestLight(const FObjectInitializer& Initializer = FObjectInitializer::Get()) : Super(Initializer)
		{ SetRelativeLocation({3.0, 0.0, 0.0}); }
		auto OnComponentCreated() -> void override { Super::OnComponentCreated(); ++Created; CreatedLocation = GetWorldLocation(); if (CreationProbe) CreationProbe(*this); }
		auto OnComponentDestroyed() -> void override { Super::OnComponentDestroyed(); ++Destroyed; }
		auto OnRegister() -> void override { Super::OnRegister(); ++Registered; }
		auto OnUnregister() -> void override { Super::OnUnregister(); ++Unregistered; if (UnregisterProbe) UnregisterProbe(*this); }
		auto InitializeComponent() -> void override { Super::InitializeComponent(); ++Initialized; }
		auto UninitializeComponent() -> void override { Super::UninitializeComponent(); ++Uninitialized; }
		auto PostLoad() -> void override { Super::PostLoad(); if (PostLoadProbe) PostLoadProbe(*this); }
		inline static std::function<void(FStructuralTestLight&)> PostLoadProbe;
		inline static std::function<void(FStructuralTestLight&)> CreationProbe;
		inline static std::function<void(FStructuralTestLight&)> UnregisterProbe;
		uint32 Created = 0, Destroyed = 0, Registered = 0, Unregistered = 0, Initialized = 0, Uninitialized = 0;
		FVector3 CreatedLocation{0.0};
	protected:
		auto CreateSceneProxy(FLightSceneProxyDesc) const -> std::unique_ptr<FLightSceneProxy> override { return {}; }
	};
	IMPLEMENT_CLASS_NO_AUTO_REGISTRATION(FStructuralTestLight)
}

namespace
{
	using namespace Durin;
	using namespace Durin::Editor;


	class FComponentTestScene final : public FSceneInterface
	{
	public:
		auto Release() -> void override {}
		auto AddPrimitive(DPrimitiveComponent* Value) -> void override { Primitives.insert(Value); }
		auto RemovePrimitive(DPrimitiveComponent* Value) -> void override { Primitives.erase(Value); }
		auto AddLight(DLightComponent* Value) -> void override { Lights[Value] = Value->GetWorldLocation(); }
		auto RemoveLight(DLightComponent* Value) -> void override { Lights.erase(Value); }
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
		std::unordered_set<DPrimitiveComponent*> Primitives;
		std::unordered_map<DLightComponent*, FVector3> Lights;
	};

	struct FComponentFixture
	{
		FComponentFixture() : Transactions(Editor.GetTransactions())
		{
			World = Editor.GetWorld();
			World->SetRenderScene(&Scene);
			Actor = World->SpawnActor<AStaticMeshActor>("Owner");
		}
		~FComponentFixture()
		{
			World->SetRenderScene(nullptr);
			Editor.Shutdown();
		}
		Tests::FActorEditorFixture Editor;
		FComponentTestScene Scene;
		DWorld* World = nullptr;
		AActor* Actor = nullptr;
		TStrongObjectPtr<DTransBuffer> Transactions;
	};
}

TEST(FActorMutationTests, EveryConstructibleActorClassRetainsIdentityAcrossUndoRedoAndCollection)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	for (DClass* Class : GetDerivedClasses(AActor::StaticClass(), true))
	{
		if (!CanConstructObjectOfClass(Class, AActor::StaticClass())) continue;
		SCOPED_TRACE(Class->GetName());
		const auto Added = F.Editor.GetActorEditor()->SpawnActor(*Level, Class, FName(), FTransform{}, {false});
		ASSERT_TRUE(Added);
		auto* Actor = Added;
		const auto Components = Actor->GetComponentsSnapshot();
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_FALSE(Level->ContainsActor(Actor));
		EXPECT_TRUE(IsPackageCaptureObjectOmitted(Actor));
		for (const auto& Component : Components) EXPECT_FALSE(Component->IsRegistered());
		CollectGarbage();
		ASSERT_TRUE(IsValid(Actor));
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_TRUE(Level->ContainsActor(Actor));
		EXPECT_EQ(Actor->GetComponentsSnapshot(), Components);
		EXPECT_TRUE(Actor->GetPrimaryActorTick().IsTickFunctionRegistered());
		for (const auto& Component : Components) EXPECT_TRUE(Component->IsRegistered());
		AActor* Source = Actor;
		const auto Copies = F.Editor.GetActorEditor()->DuplicateActors(*Level, {&Source, 1}, {false});
		ASSERT_FALSE(Copies.empty());
		ASSERT_EQ(Copies.size(), 1u);
		EXPECT_EQ(Copies.front()->GetClass(), Class);
		EXPECT_NE(Copies.front().Get(), Actor);
		ASSERT_TRUE(F.Transactions->Undo());
		ASSERT_TRUE(F.Transactions->Undo());
	}
}

TEST(FActorMutationTests, DeletionRestoresEarlierPropertyHistoryExternalAttachmentsAndRenderState)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Actor = Level->SpawnActor<AStaticMeshActor>("Child");
	ASSERT_TRUE(Actor->AttachToActor(F.Actor, EAttachmentTransformRule::KeepRelative));
	auto* Light = static_cast<FStructuralTestLight*>(Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light"));
	auto* Grandchild = Level->SpawnActor<AStaticMeshActor>("Grandchild");
	ASSERT_TRUE(Grandchild->GetRootComponent()->AttachToComponent(Light, EAttachmentTransformRule::KeepRelative));
	{
		Editor::FScopedTransaction Transaction(F.Transactions.Get(), {"test", "Edit actor before delete"});
		(void)Actor->GetRootComponent()->Modify();
		Actor->GetRootComponent()->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Before = Grandchild->GetActorTransform().Translation;
	AActor* Target = Actor;
	ASSERT_TRUE(F.Editor.GetActorEditor()->DestroyActors(*Level, {&Target, 1}, {false}));
	EXPECT_EQ(Actor->GetAttachParentActor(), nullptr);
	EXPECT_EQ(Grandchild->GetRootComponent()->GetAttachParent(), nullptr);
	EXPECT_EQ(Grandchild->GetActorTransform().Translation, Before);
	EXPECT_FALSE(F.Scene.Lights.contains(Light));
	EXPECT_FALSE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Level->FindActorByName("Child"), Actor);
	EXPECT_EQ(Actor->GetAttachParentActor(), F.Actor);
	EXPECT_EQ(Grandchild->GetRootComponent()->GetAttachParent(), Light);
	EXPECT_EQ(Grandchild->GetActorTransform().Translation, Before);
	EXPECT_EQ(F.Scene.Lights.at(Light), Light->GetWorldLocation());
	EXPECT_EQ(Light->Initialized, 1u);
	EXPECT_EQ(Light->Uninitialized, 0u);
	EXPECT_EQ(Light->Created, 1u);
	EXPECT_EQ(Light->Destroyed, 0u);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Actor->GetActorTransform().Translation, FVector3(0.0));
	ASSERT_TRUE(F.Transactions->Redo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(Level->ContainsActor(Actor));
}

TEST(FActorMutationTests, MixedBatchDeletionRestoresActorOrderPrimaryCameraAndUnregisteredComponents)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* First = Level->SpawnActor<ACameraActor>("CameraA");
	auto* Second = Level->SpawnActor<ACameraActor>("CameraB");
	ASSERT_EQ(Level->GetPrimaryCameraActor(), First);
	First->GetRootComponent()->UnregisterComponent();
	ASSERT_TRUE(Second->AttachToActor(First, EAttachmentTransformRule::KeepRelative));
	const auto Order = Level->GetActors();
	std::array<AActor*, 2> Targets{First, F.Actor};
	ASSERT_TRUE(F.Editor.GetActorEditor()->DestroyActors(*Level, Targets, {false}));
	EXPECT_EQ(Level->GetPrimaryCameraActor(), Second);
	EXPECT_EQ(Second->GetAttachParentActor(), nullptr);
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Level->GetActors(), Order);
	EXPECT_EQ(Level->GetPrimaryCameraActor(), First);
	EXPECT_EQ(Second->GetAttachParentActor(), First);
	EXPECT_FALSE(First->GetRootComponent()->IsRegistered());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Level->GetPrimaryCameraActor(), Second);
}

TEST(FActorMutationTests, DuplicationRemapsSelectedHierarchyOwnedComponentsAndSplineCache)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Source = Level->SpawnActor<ASplineMeshActor>("Spline");
	FSplinePoint Start, End;
	End.Position = {100.0, 0.0, 0.0};
	Source->GetSplineComponent()->SetSplinePoints({Start, End});
	ASSERT_TRUE(Source->AttachToActor(F.Actor, EAttachmentTransformRule::KeepRelative));
	Source->GetRootComponent()->SetRelativeLocation({7.0, 0.0, 0.0});
	auto* Light = Source->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light");
	Source->SetActorHiddenInGame(true);
	std::array<AActor*, 2> Targets{F.Actor, Source};
	const auto Result = F.Editor.GetActorEditor()->DuplicateActors(*Level, Targets, {false});
	ASSERT_FALSE(Result.empty());
	ASSERT_EQ(Result.size(), 2u);
	auto* ParentCopy = Result[0].Get();
	auto* Copy = static_cast<ASplineMeshActor*>(Result[1].Get());
	EXPECT_NE(Copy->GetSplineComponent(), Source->GetSplineComponent());
	EXPECT_EQ(Copy->GetAttachParentActor(), ParentCopy);
	EXPECT_EQ(Copy->GetActorTransform().Translation, Source->GetActorTransform().Translation);
	EXPECT_NEAR(Copy->GetSplineComponent()->GetLocalSplineLength(), 100.0, 1.e-6);
	EXPECT_TRUE(Copy->IsHidden());
	auto* LightCopy = static_cast<FStructuralTestLight*>(Copy->GetInstanceComponents().front().Get());
	EXPECT_NE(LightCopy, Light);
	EXPECT_EQ(LightCopy->GetOwner(), Copy);
	EXPECT_EQ(LightCopy->Registered, 1u);
	ASSERT_TRUE(F.Transactions->Undo());
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Level->FindActorByName(Copy->GetFName()), Copy);
	EXPECT_EQ(Copy->GetAttachParentActor(), ParentCopy);
	EXPECT_TRUE(F.Scene.Lights.contains(LightCopy));
}

TEST(FActorMutationTests, SingleActorDuplicationRestoresItsExternalParentAndRootlessActorsAreAdmitted)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Source = Level->SpawnActor<AStaticMeshActor>("Attached");
	ASSERT_TRUE(Source->AttachToActor(F.Actor, EAttachmentTransformRule::KeepRelative));
	AActor* Target = Source;
	const auto Duplicated = F.Editor.GetActorEditor()->DuplicateActors(*Level, {&Target, 1}, {false});
	ASSERT_FALSE(Duplicated.empty());
	auto* Copy = Duplicated.front().Get();
	EXPECT_EQ(Copy->GetAttachParentActor(), F.Actor);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Copy->GetAttachParentActor(), nullptr);
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Copy->GetAttachParentActor(), F.Actor);
	const auto Rootless = F.Editor.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), "Rootless", FTransform{}, {false});
	ASSERT_TRUE(Rootless);
	EXPECT_EQ(Rootless->GetRootComponent(), nullptr);
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Level->ContainsActor(Rootless));
}

TEST(FActorMutationTests, BatchConflictRejectsEveryWriteAndRetainsHistoryForRetry)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Other = Level->SpawnActor<AActor>("Other");
	std::array<AActor*, 2> Targets{F.Actor, Other};
	ASSERT_TRUE(F.Editor.GetActorEditor()->DestroyActors(*Level, Targets, {false}));
	auto* Conflict = Level->SpawnActor<AActor>(Other->GetFName());
	ASSERT_NE(Conflict, nullptr);
	EXPECT_NE(Conflict->GetFName(), Other->GetFName());
	// Bypass the naming service to exercise defensive replay against external corruption.
	Conflict->Rename(Other->GetFName());
	const auto Head = F.Transactions->GetUndoId();
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_FALSE(Level->ContainsActor(F.Actor));
	EXPECT_FALSE(F.Actor->GetRootComponent()->IsRegistered());
	EXPECT_TRUE(IsPackageCaptureObjectOmitted(F.Actor));
	ASSERT_TRUE(Level->DestroyActor(Conflict));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Level->ContainsActor(F.Actor));
	EXPECT_TRUE(Level->ContainsActor(Other));
}

TEST(FActorMutationTests, CombinedAttachmentCyclesRejectRestorationBeforeAnyMembershipWrite)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Other = Level->SpawnActor<AStaticMeshActor>("Other");
	auto* ExternalChild = static_cast<DSceneComponent*>(Other->AddInstanceComponent(DSceneComponent::StaticClass(), "ExternalChild"));
	auto* Internal = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Internal"));
	ASSERT_TRUE(F.Actor->AttachToActor(Other, EAttachmentTransformRule::KeepRelative));
	ASSERT_TRUE(ExternalChild->AttachToComponent(Internal, EAttachmentTransformRule::KeepRelative));
	AActor* Target = F.Actor;
	ASSERT_TRUE(F.Editor.GetActorEditor()->DestroyActors(*Level, {&Target, 1}, {false}));
	ASSERT_TRUE(Other->GetRootComponent()->AttachToComponent(ExternalChild, EAttachmentTransformRule::KeepWorld));
	const auto Head = F.Transactions->GetUndoId();
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_FALSE(Level->ContainsActor(F.Actor));
	EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachParent(), nullptr);
	EXPECT_EQ(ExternalChild->GetAttachParent(), nullptr);
	ASSERT_TRUE(Other->GetRootComponent()->DetachFromComponent());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(ExternalChild->GetAttachParent(), Internal);
	EXPECT_EQ(F.Actor->GetAttachParentActor(), Other);
}

TEST(FActorMutationTests, RenamePreservesIdentityAndRejectsConflictingReplayWithoutMovingHistory)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	const auto Before = F.Actor->GetFName();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameActor(*Level, F.Actor, "Renamed", {false}));
	const auto Head = F.Transactions->GetUndoId();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameActor(*Level, F.Actor, "Renamed", {false}));
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	auto* Conflict = Level->SpawnActor<AActor>(Before);
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_EQ(F.Actor->GetFName(), FName("Renamed"));
	ASSERT_TRUE(Level->DestroyActor(Conflict));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(F.Actor->GetFName(), Before);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Level->FindActorByName("Renamed"), F.Actor);
}

TEST(FActorMutationTests, RejectionIsAtomicAndHistoryDiscardCollectsInactiveActorGraphs)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	FTransBufferTestAccess::SetState(*F.Transactions.Get(), ETransactorState::Undoing);
	const auto Count = Level->GetActors().size();
	EXPECT_FALSE(F.Editor.GetActorEditor()->SpawnActor(*Level, AStaticMeshActor::StaticClass(), "Blocked", FTransform{}, {false}));
	EXPECT_EQ(Level->GetActors().size(), Count);
	FTransBufferTestAccess::SetState(*F.Transactions.Get(), ETransactorState::Idle);
	std::array<AActor*, 2> Invalid{F.Actor, F.Actor};
	EXPECT_FALSE(F.Editor.GetActorEditor()->DestroyActors(*Level, Invalid, {false}));
	EXPECT_TRUE(F.Editor.GetActorEditor()->DuplicateActors(*Level, Invalid, {false}).empty());
	EXPECT_FALSE(F.Editor.GetActorEditor()->RenameActor(*Level, F.Actor, "ReadOnly", {true}));
	const auto Added = F.Editor.GetActorEditor()->SpawnActor(*Level, AStaticMeshActor::StaticClass(), "Disposable", FTransform{}, {false});
	ASSERT_TRUE(Added);
	TWeakObjectPtr<AActor> Weak(Added);
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Reset());
	CollectGarbage();
	EXPECT_EQ(Weak.Get(), nullptr);
}

TEST(FActorMutationTests, MixedSynchronousEditsJoinOuterTransactionAndReplayInOrder)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	const auto Outer = F.Transactions->Begin("test", "Build actor hierarchy");
	ASSERT_GE(Outer, 0);
	const auto Spawned = Editor->SpawnActor(*Level, ACameraActor::StaticClass(), "Grouped", {}, {false});
	ASSERT_TRUE(Spawned);
	auto* Actor = Spawned;
	ASSERT_TRUE(Editor->RenameActor(*Level, Actor, "GroupedRenamed", {false}));
	std::array<AActor*, 1> Actors{Actor};
	ASSERT_TRUE(Editor->AttachActors(*Level, Actors, F.Actor, EAttachmentTransformRule::KeepWorld, {false}));
	const auto Added = Editor->AddComponent(*Actor, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	ASSERT_TRUE(Editor->RenameComponent(*Actor, Added, "GroupedComponent", {false}));
	const auto ComponentCopy = Editor->DuplicateComponent(*Actor, Added, {false});
	ASSERT_TRUE(ComponentCopy);
	ASSERT_TRUE(Editor->ReparentComponent(*Actor, static_cast<DSceneComponent*>(ComponentCopy),
		static_cast<DSceneComponent*>(Added), {false}));
	ASSERT_TRUE(Editor->DeleteComponent(*Actor, ComponentCopy, {false}));
	ASSERT_TRUE(Editor->DetachActors(*Level, Actors, EDetachmentTransformRule::KeepWorld, {false}));
	ASSERT_TRUE(Editor->AttachActors(*Level, Actors, F.Actor, EAttachmentTransformRule::KeepWorld, {false}));
	const auto Duplicated = Editor->DuplicateActors(*Level, Actors, {false});
	ASSERT_FALSE(Duplicated.empty());
	auto* Duplicate = Duplicated.front().Get();
	std::array<AActor*, 1> Duplicates{Duplicate};
	ASSERT_TRUE(Editor->DestroyActors(*Level, Duplicates, {false}));
	EXPECT_EQ(F.Transactions->GetQueueLength(), 0u);
	ASSERT_TRUE(F.Transactions->End());
	EXPECT_EQ(F.Transactions->GetQueueLength(), 1u);
	EXPECT_EQ(F.Transactions->GetUndoContext(false).Description, "Build actor hierarchy");
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Level->ContainsActor(Actor));
	EXPECT_FALSE(Level->ContainsActor(Duplicate));
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Level->ContainsActor(Actor));
	EXPECT_FALSE(Level->ContainsActor(Duplicate));
	EXPECT_EQ(Actor->GetFName(), FName("GroupedRenamed"));
	EXPECT_EQ(Actor->GetAttachParentActor(), F.Actor);
	EXPECT_TRUE(Actor->OwnsComponent(Added));
	EXPECT_EQ(Added->GetFName(), FName("GroupedComponent"));
}

TEST(FActorMutationTests, JoinedChangesKeepOnePackageRevisionAndRestoreCleanCheckpoint)
{
	FComponentFixture F;
	const auto Directory = Testing::GetTestWorkDirectory() / "GroupedActorMutation";
	std::filesystem::create_directories(Directory);
	Testing::RegisterMountPointForTests("/GroupedActorMutation/", Directory.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/GroupedActorMutation/Level", Path));
	auto* Package = NewObject<DPackage>(nullptr, Path.GetPackageName());
	Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DPackage> PackageRoot(Package);
	auto* Level = NewObject<DLevel>(Package, "Level");
	ASSERT_TRUE(F.World->SetCurrentLevel(Level));
	Package->ClearDirty();
	auto* Editor = F.Editor.GetActorEditor();
	const auto Outer = F.Transactions->Begin("test", "Populate level");
	ASSERT_GE(Outer, 0);
	const auto First = Editor->SpawnActor(*Level, ACameraActor::StaticClass(), "First", {}, {false});
	ASSERT_TRUE(First);
	ASSERT_TRUE(Editor->SpawnActor(*Level, ACameraActor::StaticClass(), "Second", {}, {false}));
	ASSERT_TRUE(F.Transactions->End());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_TRUE(Level->GetActors().empty());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Level->GetActors().size(), 2u);
}

TEST(FActorMutationTests, TransitionStatesRejectAllActorEditingBeforePreparation)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	const auto Count = Level->GetActors().size();
	for (auto State : {ETransactorState::Undoing,
		ETransactorState::Redoing, ETransactorState::RecoveryRequired, ETransactorState::Destroying})
	{
		FTransBufferTestAccess::SetState(*F.Transactions.Get(), State);
		bool bConfigured = false;
		const auto Result = Editor->SpawnActor(*Level, ACameraActor::StaticClass(), "Rejected", {},
			{false}, [&](AActor&) { bConfigured = true; });
		EXPECT_FALSE(Result);
		EXPECT_FALSE(Result);
		EXPECT_FALSE(bConfigured);
		EXPECT_EQ(Level->GetActors().size(), Count);
	}
	FTransBufferTestAccess::SetState(*F.Transactions.Get(), ETransactorState::Idle);
}

TEST(FComponentMutationTests, AdditionUndoRedoBalancesLifecycleAndSceneMembership)
{
	FComponentFixture F;
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), F.Actor->GetRootComponent(), {false});
	ASSERT_TRUE(Added);
	auto* Light = static_cast<FStructuralTestLight*>(Added);
	EXPECT_EQ(Light->Created, 1u);
	EXPECT_EQ(Light->Registered, 1u);
	EXPECT_EQ(Light->Initialized, 1u);
	EXPECT_TRUE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
	ASSERT_TRUE(F.Scene.Lights.contains(Light));
	const auto Generation = Light->GetRegistrationGeneration();
	for (uint32 Repeat = 0; Repeat < 2; ++Repeat)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_TRUE(IsValid(Light));
		EXPECT_FALSE(F.Actor->OwnsComponent(Light));
		EXPECT_FALSE(Light->IsRegistered());
		EXPECT_FALSE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
		EXPECT_FALSE(F.Scene.Lights.contains(Light));
		EXPECT_EQ(Light->Destroyed, Repeat + 1);
		EXPECT_EQ(Light->Unregistered, Repeat + 1);
		EXPECT_EQ(Light->Uninitialized, Repeat + 1);
		EXPECT_TRUE(IsPackageCaptureObjectOmitted(Light));
		CollectGarbage();
		ASSERT_TRUE(IsValid(Light));
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_TRUE(F.Actor->IsInstanceComponent(Light));
		EXPECT_EQ(Light->Created, Repeat + 2);
		EXPECT_EQ(Light->Registered, Repeat + 2);
		EXPECT_EQ(Light->Initialized, Repeat + 2);
		EXPECT_TRUE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
		EXPECT_TRUE(F.Scene.Lights.contains(Light));
		EXPECT_FALSE(IsPackageCaptureObjectOmitted(Light));
	}
	EXPECT_GT(Light->GetRegistrationGeneration(), Generation);
}

TEST(FComponentMutationTests, AdditionPreservesDefaultsAndExplicitChildSnapsBeforeCreation)
{
	FComponentFixture F;
	F.Actor->GetRootComponent()->SetWorldLocation({10.0, 0.0, 0.0});
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	auto* Light = static_cast<FStructuralTestLight*>(Added);
	EXPECT_EQ(Light->GetWorldLocation(), FVector3(3.0, 0.0, 0.0));
	EXPECT_EQ(Light->CreatedLocation, Light->GetWorldLocation());
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Light->GetWorldLocation(), FVector3(3.0, 0.0, 0.0));
	const auto Child = F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), Light, {false});
	ASSERT_TRUE(Child);
	EXPECT_EQ(static_cast<DSceneComponent*>(Child)->GetRelativeLocation(), FVector3(0.0));
	EXPECT_EQ(static_cast<FStructuralTestLight*>(Child)->CreatedLocation, Light->GetWorldLocation());
}

TEST(FComponentMutationTests, DeletionPreservesIdentityForEarlierPropertyHistoryAndChildren)
{
	FComponentFixture F;
	auto* Light = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light"));
	Light->SetRelativeLocation({4.0, 0.0, 0.0});
	auto* Child = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Child"));
	Child->SetRelativeLocation({2.0, 0.0, 0.0});
	ASSERT_TRUE(Child->AttachToComponent(Light, EAttachmentTransformRule::KeepRelative));
	const auto Instances = F.Actor->GetInstanceComponents();
	{
		Editor::FScopedTransaction Transaction(F.Transactions.Get(), {"test", "Edit light before deletion"});
		(void)Light->Modify();
		Light->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Before = Child->GetWorldLocation();
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, Light, {false}));
	EXPECT_FALSE(Light->IsRegistered());
	EXPECT_EQ(Child->GetAttachParent(), nullptr);
	EXPECT_EQ(Child->GetWorldLocation(), Before);
	EXPECT_FALSE(F.Scene.Lights.contains(Light));
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(F.Actor->GetInstanceComponents(), Instances);
	EXPECT_EQ(Child->GetAttachParent(), Light);
	EXPECT_EQ(Child->GetWorldLocation(), Before);
	ASSERT_TRUE(F.Scene.Lights.contains(Light));
	EXPECT_EQ(F.Scene.Lights.at(Light), Light->GetWorldLocation());
	ASSERT_TRUE(F.Transactions->Undo()) << Editor::FormatTransactorResult(F.Transactions->GetLastResult());
	EXPECT_EQ(Light->GetRelativeLocation(), FVector3(4.0, 0.0, 0.0));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Light->GetRelativeLocation(), FVector3(9.0, 0.0, 0.0));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(F.Scene.Lights.contains(Light));
	EXPECT_FALSE(F.Actor->OwnsComponent(Light));
}

TEST(FComponentMutationTests, DuplicationRebuildsSplineCacheAndPreservesDetachedTransform)
{
	FComponentFixture F;
	F.Actor->GetRootComponent()->SetWorldLocation({10.0, 0.0, 0.0});
	auto* Source = static_cast<DSplineComponent*>(F.Actor->AddInstanceComponent(DSplineComponent::StaticClass(), "Spline"));
	FSplinePoint Start, End;
	End.Position = {100.0, 0.0, 0.0};
	Source->SetSplinePoints({Start, End});
	ASSERT_TRUE(Source->DetachFromComponent());
	Source->SetWorldLocation({7.0, 0.0, 0.0});
	auto* Payload = NewObject<DObject>(Source, "Payload");
	const auto Result = F.Editor.GetActorEditor()->DuplicateComponent(*F.Actor, Source, {false});
	ASSERT_TRUE(Result);
	auto* Copy = static_cast<DSplineComponent*>(Result);
	EXPECT_NE(Copy, Source);
	EXPECT_NE(Copy->GetFName(), Source->GetFName());
	const auto Inners = GDObjectArray.GetObjectsWithOuter(Copy, EObjectQueryScope::LiveOnly);
	ASSERT_EQ(Inners.size(), 1u);
	EXPECT_NE(Inners.front(), Payload);
	EXPECT_EQ(Inners.front()->GetOuter(), Copy);
	EXPECT_EQ(Copy->GetAttachParent(), nullptr);
	EXPECT_EQ(Copy->GetWorldLocation(), Source->GetWorldLocation());
	EXPECT_NEAR(Copy->GetLocalSplineLength(), Source->GetLocalSplineLength(), 1.e-6);
	ASSERT_TRUE(F.Transactions->Undo());
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(IsValid(Inners.front()));
	EXPECT_EQ(Copy->GetAttachParent(), nullptr);
	EXPECT_EQ(Copy->GetWorldLocation(), Source->GetWorldLocation());
	EXPECT_NEAR(Copy->GetLocalSplineLength(), 100.0, 1.e-6);
}

TEST(FComponentMutationTests, DuplicatePublishesCopiedFieldsBeforeFirstLifecycleAndRegister)
{
	FComponentFixture F;
	auto* Source = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Source"));
	Source->SetRelativeLocation({8.0, 0.0, 0.0});
	const auto Result = F.Editor.GetActorEditor()->DuplicateComponent(*F.Actor, Source, {false});
	ASSERT_TRUE(Result);
	auto* Copy = static_cast<FStructuralTestLight*>(Result);
	EXPECT_EQ(Copy->Created, 1u);
	EXPECT_EQ(Copy->Registered, 1u);
	EXPECT_EQ(Copy->CreatedLocation, Source->GetWorldLocation());
	EXPECT_EQ(Copy->GetAttachParent(), Source->GetAttachParent());
	EXPECT_EQ(F.Scene.Lights.at(Copy), Source->GetWorldLocation());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(F.Scene.Lights.contains(Copy));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(F.Scene.Lights.at(Copy), Source->GetWorldLocation());
}

TEST(FComponentMutationTests, RootDeletionAndUnregisteredStateRoundTrip)
{
	FComponentFixture F;
	auto* Empty = F.World->SpawnActor<AActor>("Empty");
	const auto Result = F.Editor.GetActorEditor()->AddComponent(*Empty, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Result);
	EXPECT_EQ(Empty->GetRootComponent(), Result);
	Result->UnregisterComponent();
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*Empty, Result, {false}));
	EXPECT_EQ(Empty->GetRootComponent(), nullptr);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Empty->GetRootComponent(), Result);
	EXPECT_FALSE(Result->IsRegistered());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Empty->GetRootComponent(), nullptr);
}

TEST(FComponentMutationTests, RejectionDoesNotPublishAndDiscardedInactiveComponentsAreCollected)
{
	FComponentFixture F;
	const auto Count = F.Actor->GetComponents().size();
	FTransBufferTestAccess::SetState(*F.Transactions.Get(), ETransactorState::Undoing);
	ASSERT_FALSE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false}));
	EXPECT_EQ(F.Actor->GetComponents().size(), Count);
	FTransBufferTestAccess::SetState(*F.Transactions.Get(), ETransactorState::Idle);
	ASSERT_FALSE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, F.Actor->GetRootComponent(), {false}));
	ASSERT_FALSE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {true}));
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*F.Actor, DStaticMeshComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	EXPECT_TRUE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Added)));
	TWeakObjectPtr<DActorComponent> Weak(Added);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Added)));
	ASSERT_TRUE(F.Transactions->Reset());
	CollectGarbage();
	EXPECT_EQ(Weak.Get(), nullptr);
}

TEST(FComponentMutationTests, ConflictingRootRejectsUndoWithoutPartialRestoration)
{
	FComponentFixture F;
	auto* Owner = F.World->SpawnActor<AActor>("RootOwner");
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*Owner, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*Owner, Added, {false}));
	auto* Replacement = static_cast<DSceneComponent*>(Owner->AddInstanceComponent(DSceneComponent::StaticClass(), "Replacement"));
	const auto Head = F.Transactions->GetUndoId();
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_EQ(Owner->GetRootComponent(), Replacement);
	EXPECT_FALSE(Owner->OwnsComponent(Added));
	EXPECT_FALSE(Added->IsRegistered());
	EXPECT_TRUE(IsPackageCaptureObjectOmitted(Added));
	ASSERT_TRUE(Owner->DestroyInstanceComponent(Replacement));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Owner->GetRootComponent(), Added);
}

TEST(FComponentMutationTests, RenamePreservesIdentityAndEarlierPropertyHistory)
{
	FComponentFixture F;
	auto* Light = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light"));
	const FName Before = Light->GetFName();
	const auto Path = Light->GetObjectPath();
	{
		Editor::FScopedTransaction Transaction(F.Transactions.Get(), {"test", "Edit before rename"});
		(void)Light->Modify();
		Light->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Instances = F.Actor->GetInstanceComponents();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Light, "Renamed", {false}));
	EXPECT_EQ(Light->GetFName(), FName("Renamed"));
	EXPECT_NE(Light->GetObjectPath(), Path);
	const auto Head = F.Transactions->GetUndoId();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Light, "Renamed", {false}));
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Light->GetFName(), Before);
	EXPECT_EQ(Light->GetObjectPath(), Path);
	ASSERT_TRUE(F.Transactions->Undo()) << Editor::FormatTransactorResult(F.Transactions->GetLastResult());
	EXPECT_EQ(Light->GetRelativeLocation(), FVector3(3.0, 0.0, 0.0));
	ASSERT_TRUE(F.Transactions->Redo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Light->GetFName(), FName("Renamed"));
	EXPECT_EQ(F.Actor->GetInstanceComponents(), Instances);
	EXPECT_EQ(Light->Registered, 1u);
	EXPECT_EQ(Light->Created, 1u);
	EXPECT_TRUE(F.Scene.Lights.contains(Light));
}

TEST(FComponentMutationTests, RenameNativeRootRestoresNameWithoutChangingRootOrRenderMembership)
{
	FComponentFixture F;
	auto* Root = F.Actor->GetRootComponent();
	const auto Before = Root->GetFName();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Root, "RenamedRoot", {false}));
	EXPECT_EQ(Root->GetFName(), FName("RenamedRoot"));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Root->GetFName(), Before);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Root->GetFName(), FName("RenamedRoot"));
	EXPECT_EQ(F.Actor->GetRootComponent(), Root);
	EXPECT_TRUE(Root->IsRegistered());
	EXPECT_TRUE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Root)));
}

TEST(FComponentMutationTests, RenameReservesInactiveNamesAndConflictsRejectBeforeWrites)
{
	FComponentFixture F;
	const auto Inactive = F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Inactive);
	const auto Reserved = Inactive->GetFName();
	ASSERT_TRUE(F.Transactions->Undo());
	auto* Component = F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Original");
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Component, Reserved, {false}));
	EXPECT_NE(Component->GetFName(), Reserved);
	const auto After = Component->GetFName();
	auto* Conflict = F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Original");
	const auto Head = F.Transactions->GetUndoId();
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_EQ(Component->GetFName(), After);
	ASSERT_TRUE(F.Actor->DestroyInstanceComponent(Conflict));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Component->GetFName(), FName("Original"));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Component->GetFName(), After);
}

TEST(FComponentMutationTests, ReparentRestoresBothSidesDescendantsAndRenderTransforms)
{
	FComponentFixture F;
	auto* Root = F.Actor->GetRootComponent();
	Root->SetWorldLocation({10.0, 0.0, 0.0});
	auto* Parent = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
	ASSERT_TRUE(Parent->DetachFromComponent());
	Parent->SetWorldLocation({30.0, 0.0, 0.0});
	auto* Light = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light"));
	Light->SetRelativeLocation({4.0, 0.0, 0.0});
	auto* Child = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Child"));
	ASSERT_TRUE(Child->AttachToComponent(Light, EAttachmentTransformRule::KeepRelative));
	Child->SetRelativeLocation({2.0, 0.0, 0.0});
	const auto Instances = F.Actor->GetInstanceComponents();
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Light, Parent, {false}));
	EXPECT_EQ(Light->GetWorldLocation(), FVector3(14.0, 0.0, 0.0));
	EXPECT_EQ(Light->GetRelativeLocation(), FVector3(-16.0, 0.0, 0.0));
	for (int Repeat = 0; Repeat < 2; ++Repeat)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_EQ(Light->GetAttachParent(), Root);
		EXPECT_FALSE(std::ranges::contains(Parent->GetAttachChildren(), Light, [](const auto& Entry) { return Entry.Get(); }));
		EXPECT_TRUE(std::ranges::contains(Root->GetAttachChildren(), Light, [](const auto& Entry) { return Entry.Get(); }));
		Root->SetWorldLocation({15.0, 0.0, 0.0});
		EXPECT_EQ(Light->GetWorldLocation(), FVector3(19.0, 0.0, 0.0));
		EXPECT_EQ(Child->GetWorldLocation(), FVector3(21.0, 0.0, 0.0));
		EXPECT_EQ(F.Scene.Lights.at(Child), Child->GetWorldLocation());
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_EQ(Light->GetAttachParent(), Parent);
		EXPECT_EQ(Light->GetRelativeLocation(), FVector3(-16.0, 0.0, 0.0));
		EXPECT_FALSE(std::ranges::contains(Root->GetAttachChildren(), Light, [](const auto& Entry) { return Entry.Get(); }));
		Parent->SetWorldLocation({40.0, 0.0, 0.0});
		EXPECT_EQ(Light->GetWorldLocation(), FVector3(24.0, 0.0, 0.0));
		EXPECT_EQ(Child->GetWorldLocation(), FVector3(26.0, 0.0, 0.0));
		EXPECT_EQ(F.Scene.Lights.at(Light), Light->GetWorldLocation());
		EXPECT_EQ(F.Scene.Lights.at(Child), Child->GetWorldLocation());
		Parent->SetWorldLocation({30.0, 0.0, 0.0});
		Root->SetWorldLocation({10.0, 0.0, 0.0});
	}
	EXPECT_EQ(F.Actor->GetInstanceComponents(), Instances);
	EXPECT_EQ(Light->Created, 1u);
	EXPECT_EQ(Light->Registered, 1u);
}

TEST(FComponentMutationTests, ReparentSupportsDetachedStateAndRejectsInvalidEdits)
{
	FComponentFixture F;
	auto* Component = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Moving"));
	auto* Child = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Child"));
	ASSERT_TRUE(Child->AttachToComponent(Component, EAttachmentTransformRule::KeepRelative));
	auto* Foreign = F.World->SpawnActor<AStaticMeshActor>("Foreign");
	const auto Before = Component->GetWorldLocation();
	EXPECT_FALSE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Component, Child, {false}));
	EXPECT_FALSE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Component, Foreign->GetRootComponent(), {false}));
	EXPECT_FALSE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, F.Actor->GetRootComponent(), Component, {false}));
	EXPECT_FALSE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Component, nullptr, {true}));
	EXPECT_FALSE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Component, "New", {true}));
	EXPECT_FALSE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Foreign->GetRootComponent(), "New", {false}));
	EXPECT_FALSE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Component, FName(), {false}));
	EXPECT_FALSE(F.Transactions->CanUndo());
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Component, nullptr, {false}));
	EXPECT_EQ(Component->GetAttachParent(), nullptr);
	EXPECT_EQ(Component->GetWorldLocation(), Before);
	const auto Head = F.Transactions->GetUndoId();
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Component, nullptr, {false}));
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Component->GetAttachParent(), F.Actor->GetRootComponent());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Component->GetAttachParent(), nullptr);
}

TEST(FComponentMutationTests, ReparentRejectsChangedParentAndCyclesWithoutMovingHistory)
{
	FComponentFixture F;
	auto* Moving = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Moving"));
	auto* Parent = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Moving, Parent, {false}));
	ASSERT_TRUE(Moving->DetachFromComponent());
	const auto Head = F.Transactions->GetUndoId();
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_EQ(Moving->GetAttachParent(), nullptr);
	ASSERT_TRUE(Moving->AttachToComponent(Parent, EAttachmentTransformRule::KeepWorld));
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(Parent->AttachToComponent(Moving, EAttachmentTransformRule::KeepWorld));
	const auto Redo = F.Transactions->GetRedoId();
	EXPECT_FALSE(F.Transactions->Redo());
	EXPECT_EQ(F.Transactions->GetRedoId(), Redo);
	EXPECT_EQ(Moving->GetAttachParent(), F.Actor->GetRootComponent());
	ASSERT_TRUE(Parent->AttachToComponent(F.Actor->GetRootComponent(), EAttachmentTransformRule::KeepWorld));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Moving->GetAttachParent(), Parent);
}

TEST(FComponentMutationTests, AllComponentStructureCommandsShareOneHistoryAcrossCollection)
{
	FComponentFixture F;
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false});
	const auto Parent = F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	ASSERT_TRUE(Parent);
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Added, "Renamed", {false}));
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, static_cast<DSceneComponent*>(Added), static_cast<DSceneComponent*>(Parent), {false}));
	const auto Copy = F.Editor.GetActorEditor()->DuplicateComponent(*F.Actor, Added, {false});
	ASSERT_TRUE(Copy);
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, Added, {false}));
	for (int Step = 0; Step < 6; ++Step) { ASSERT_TRUE(F.Transactions->Undo()); CollectGarbage(); }
	EXPECT_FALSE(F.Actor->OwnsComponent(Added));
	EXPECT_FALSE(F.Actor->OwnsComponent(Copy));
	EXPECT_FALSE(F.Actor->OwnsComponent(Parent));
	for (int Step = 0; Step < 6; ++Step) { ASSERT_TRUE(F.Transactions->Redo()); CollectGarbage(); }
	EXPECT_FALSE(F.Actor->OwnsComponent(Added));
	EXPECT_TRUE(F.Actor->OwnsComponent(Copy));
	EXPECT_EQ(static_cast<DSceneComponent*>(Copy)->GetAttachParent(), Parent);
	EXPECT_TRUE(F.Scene.Lights.contains(static_cast<DLightComponent*>(Copy)));
}

TEST(FComponentMutationTests, SuspendedComponentsStayOutOfPackageCaptureAndPreserveSavedRevision)
{
	FComponentFixture F;
	static uint32 NextPackage = 1;
	static const bool bMounted = [] {
		const auto Directory = Testing::GetTestWorkDirectory() / "ComponentMutation";
		std::filesystem::create_directories(Directory);
		Testing::RegisterMountPointForTests("/ComponentMutation/", Directory.generic_string() + "/");
		return true;
	}();
	(void)bMounted;
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate(std::format("/ComponentMutation/Level_{}", NextPackage++), Path));
	auto* Package = NewObject<DPackage>(nullptr, Path.GetPackageName());
	Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DPackage> PackageRoot(Package);
	auto* Level = NewObject<DLevel>(Package, Path.GetPackageName());
	ASSERT_TRUE(F.World->SetCurrentLevel(Level));
	auto* Owner = Level->SpawnActor<AActor>("Owner");
	Package->ClearDirty();
	F.Transactions->EstablishSavedState(*Package);
	ObjectPackage::FLinkerTables Baseline, Active, Absent;
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, Baseline));
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*Owner, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, Active));
	EXPECT_EQ(Active.Exports.size(), Baseline.Exports.size() + 1);
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*Owner, Added, {false}));
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, Absent));
	EXPECT_EQ(Absent.Exports.size(), Baseline.Exports.size());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Package->IsDirty());
	Absent = {};
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, Absent));
	EXPECT_EQ(Absent.Exports.size(), Baseline.Exports.size());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Package->IsDirty());
	auto* Parent = static_cast<DSceneComponent*>(Owner->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
	auto* Child = static_cast<DSceneComponent*>(Owner->AddInstanceComponent(DSceneComponent::StaticClass(), "Child"));
	Package->ClearDirty();
	F.Transactions->EstablishSavedState(*Package);
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*Owner, Child, "Renamed", {false}));
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*Owner, Child, Parent, {false}));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(F.Transactions->Redo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_EQ(Child->GetAttachParent(), Parent);
	EXPECT_EQ(Child->GetFName(), FName("Renamed"));
	auto* Actor = Level->SpawnActor<AStaticMeshActor>("ActorHistory");
	Package->ClearDirty();
	F.Transactions->EstablishSavedState(*Package);
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*Owner, Child, "SecondName", {false}));
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameActor(*Level, Actor, "RenamedActor", {false}));
	AActor* Target = Actor;
	const auto Copy = F.Editor.GetActorEditor()->DuplicateActors(*Level, {&Target, 1}, {false});
	ASSERT_FALSE(Copy.empty());
	ASSERT_TRUE(F.Editor.GetActorEditor()->DestroyActors(*Level, {&Target, 1}, {false}));
	EXPECT_TRUE(IsPackageCaptureObjectOmitted(Actor));
	ObjectPackage::FLinkerTables ActorCapture;
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, ActorCapture));
	for (int Step = 0; Step < 4; ++Step) ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Level->FindActorByName("ActorHistory"), Actor);
	for (int Step = 0; Step < 4; ++Step) ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(F.Transactions->Reset());
	MarkObjectHierarchyAsGarbage(Package);
}

TEST(FActorMutationTests, PreparationRejectsRecursionAndCleansThrowingAndRejectedObjects)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	AActor* Prepared = nullptr;
	const auto Failed = Editor->SpawnActor(*Level, AStaticMeshActor::StaticClass(), "Unpublished", {}, {false}, [&](AActor& Actor) {
		Prepared = &Actor;
		const auto Recursive = Editor->SpawnActor(*Level, AActor::StaticClass(), "Recursive", {}, {false});
		EXPECT_FALSE(Recursive);
		throw std::runtime_error("Preparation fixture");
	});
	EXPECT_FALSE(Failed);
	EXPECT_FALSE(IsValid(Prepared));
	EXPECT_EQ(Level->FindActorByName("Unpublished"), nullptr);
	EXPECT_FALSE(F.Transactions->CanUndo());
	const auto Succeeded = Editor->SpawnActor(*Level, AActor::StaticClass(), "Unpublished", {}, {false});
	ASSERT_TRUE(Succeeded);
	EXPECT_EQ(Succeeded->GetFName(), FName("Unpublished"));
}

TEST(FActorMutationTests, DetachedOwnerAndDuplicateAttachmentParticipantsAreRejectedBeforeWrites)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	AActor* Targets[] = {F.Actor, F.Actor};
	EXPECT_FALSE(Editor->AttachActors(*Level, Targets, F.Actor, EAttachmentTransformRule::KeepWorld, {false}));
	AActor* Actor = F.Actor;
	ASSERT_TRUE(Editor->DestroyActors(*Level, {&Actor, 1}, {false}));
	EXPECT_FALSE(Editor->AddComponent(*Actor, DSceneComponent::StaticClass(), nullptr, {false}));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Level->ContainsActor(Actor));
}

TEST(FActorMutationTests, WrongThreadCannotEnterActorOrComponentPreparation)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	AActor* WorkerResult = nullptr;
	DActorComponent* WorkerComponentResult = nullptr;
	auto* Service = Editor;
	std::thread Worker([&] {
		WorkerResult = Service->SpawnActor(*Level, AActor::StaticClass(), "Worker", {}, {false});
		WorkerComponentResult = Service->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
	});
	Worker.join();
	EXPECT_FALSE(WorkerResult);
	EXPECT_FALSE(WorkerComponentResult);
	EXPECT_EQ(Level->FindActorByName("Worker"), nullptr);
	EXPECT_FALSE(F.Transactions->CanUndo());
}

TEST(FActorMutationTests, DuplicationDoesNotPublishBoundaryAttachmentsDuringDerivedStatePreparation)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Source = Level->SpawnActor<AStaticMeshActor>("BoundarySource");
	ASSERT_TRUE(Source->AttachToActor(F.Actor, EAttachmentTransformRule::KeepRelative));
	ASSERT_NE(Source->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Observer"), nullptr);
	const auto ChildCount = F.Actor->GetRootComponent()->GetAttachChildren().size();
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::PostLoadProbe = {}; } } Reset;
	int Observed = 0;
	FStructuralTestLight::PostLoadProbe = [&](FStructuralTestLight&) {
		++Observed;
		EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachChildren().size(), ChildCount);
	};
	AActor* Target = Source;
	const auto Copied = F.Editor.GetActorEditor()->DuplicateActors(*Level, {&Target, 1}, {false});
	ASSERT_FALSE(Copied.empty());
	EXPECT_GT(Observed, 0);
	EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachChildren().size(), ChildCount + 1);
	EXPECT_EQ(Copied.front()->GetAttachParentActor(), F.Actor);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachChildren().size(), ChildCount);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Copied.front()->GetAttachParentActor(), F.Actor);
}

TEST(FActorMutationTests, UnavailableServiceAndRecoveringHistoryRejectBeforePreparation)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	TStrongObjectPtr<DEditorActorSubsystem> Unavailable(NewObject<DEditorActorSubsystem>(nullptr, "UninitializedActorService"));
	EXPECT_FALSE(Unavailable->SpawnActor(*Level, AActor::StaticClass(), "Unavailable", {}, {false}));
	class FIncompleteChange final : public FActorCommandChange
	{
	public:
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return {}; }

		auto GetDescription() const -> std::string_view override { return "Incomplete compensation fixture"; }
		auto Replay(ETransactionOperation) -> FActorChangeResult override
		{ return {{.Code = EActorChangeError::RollbackIncomplete}}; }
	};
	auto* Recovering = F.Transactions.Get();
	EXPECT_EQ(Durin::Editor::ApplyEditorChange(*Recovering, std::make_unique<FIncompleteChange>()).Code, ETransactorResultCode::RecoveryRequired);
	auto* Editor = F.Editor.GetActorEditor();
	EXPECT_FALSE(Editor->SpawnActor(*Level, AActor::StaticClass(), "RecoveryBlocked", {}, {false}));
	EXPECT_EQ(Level->FindActorByName("RecoveryBlocked"), nullptr);
}

TEST(FActorMutationTests, GeneralHistoryRetiresWithDurinEdRatherThanLevelEditor)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	ASSERT_TRUE(Editor->RenameActor(*Level, F.Actor, "ModuleOwnedActor", {false}));
	ASSERT_TRUE(Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false}));
	const auto UndoId = F.Transactions->GetUndoId();
	EXPECT_EQ(F.Transactions->DiscardCustomChangesByModule("LevelEditor").Code, ETransactorResultCode::NoOp);
	EXPECT_EQ(F.Transactions->GetUndoId(), UndoId);
	ASSERT_TRUE(F.Transactions->DiscardCustomChangesByModule("DurinEd"));
	EXPECT_FALSE(F.Transactions->CanUndo());
}

TEST(FComponentMutationTests, ReplayRejectsDetachedOwnersBeforeChangingMembershipOrProperties)
{
	for (int Operation = 0; Operation < 4; ++Operation)
	{
		SCOPED_TRACE(Operation);
		FComponentFixture F;
		auto* Level = F.World->GetCurrentLevel();
		auto* Child = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Child"));
		auto* Parent = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
		auto* Editor = F.Editor.GetActorEditor();
		bool Result = false;
		switch (Operation)
		{
		case 0: Result = Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false}); break;
		case 1: Result = Editor->DeleteComponent(*F.Actor, Child, {false}); break;
		case 2: Result = Editor->RenameComponent(*F.Actor, Child, "Renamed", {false}); break;
		case 3: Result = Editor->ReparentComponent(*F.Actor, Child, Parent, {false}); break;
		}
		ASSERT_TRUE(Result);
		const auto Head = F.Transactions->GetUndoId();
		const auto Name = Child->GetFName();
		const auto ComponentCount = F.Actor->GetComponents().size();
		ASSERT_TRUE(Level->SetActorPresence(F.Actor, false));
		EXPECT_FALSE(F.Transactions->Undo());
		EXPECT_EQ(F.Transactions->GetUndoId(), Head);
		EXPECT_EQ(Child->GetFName(), Name);
		EXPECT_EQ(F.Actor->GetComponents().size(), ComponentCount);
		ASSERT_TRUE(Level->SetActorPresence(F.Actor, true));
		ASSERT_TRUE(F.Transactions->Undo());
	}
}

TEST(FComponentMutationTests, AdditionHistoryRetainsSubobjectsCreatedAfterPublication)
{
	FComponentFixture F;
	auto* Editor = F.Editor.GetActorEditor();
	const auto Added = Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	auto* Payload = NewObject<DObject>(Added, "LatePayload");
	TWeakObjectPtr<DObject> WeakPayload(Payload);
	ASSERT_TRUE(F.Transactions->Undo());
	CollectGarbage();
	EXPECT_EQ(WeakPayload.Get(), Payload);
	ASSERT_TRUE(F.Transactions->Redo());
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Reset());
	CollectGarbage();
	EXPECT_FALSE(WeakPayload.IsValid());
}

TEST(FActorMutationTests, UnrecordableStructuralEditRemainsAppliedAndInvalidatesCheckpoint)
{
	FComponentFixture F;
	const auto Directory = Testing::CreateTestFixtureDirectory("RejectedStructuralFinalization");
	Testing::RegisterMountPointForTests("/RejectedStructural/", Directory.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/RejectedStructural/Level", Path));
	auto* Package = NewObject<DPackage>(nullptr, Path.GetPackageName());
	Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DPackage> PackageRoot(Package);
	auto* Level = NewObject<DLevel>(Package, "RejectedLevel");
	auto* Actor = NewObject<AStaticMeshActor>(Level, "PreparedActor");
	ASSERT_TRUE(Actor->RerunConstructionScripts());
	AActor* Raw = Actor;
	class FOverflowingPresence final : public FActorCommandChange
	{
	public:
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override
		{ return Change->GetModifiedObjects(); }

		explicit FOverflowingPresence(std::unique_ptr<FActorCommandChange> InChange) : Change(std::move(InChange)) {}
		auto GetDescription() const -> std::string_view override { return "Overflowing presence fixture"; }
		auto GetAffectedPackages() const -> std::span<DPackage* const> override { return Change->GetAffectedPackages(); }
		auto AddReferencedObjects(FReferenceCollector& Collector) const -> void override { Change->AddReferencedObjects(Collector); }
		auto GetAllocatedSize() const -> size_t override { return bApplied ? std::numeric_limits<size_t>::max() : Change->GetAllocatedSize(); }
		auto Replay(ETransactionOperation Operation) -> FActorChangeResult override
		{
			auto Result = Change->Replay(Operation);
			if (Result) bApplied = Operation != ETransactionOperation::Undo;
			return Result;
		}
		std::unique_ptr<FActorCommandChange> Change;
		bool bApplied = false;
	};
	auto Change = std::make_unique<FOverflowingPresence>(MakeActorPresenceChange(*Level, {&Raw, 1}, false, "Prepared actor"));
	Package->ClearDirty();
	F.Transactions->EstablishSavedState(*Package);
	const auto BeforeRevision = Package->GetEditRevision();
	const auto Result = Durin::Editor::ApplyEditorChange(*F.Transactions, std::move(Change));
	EXPECT_EQ(Result.Code, ETransactorResultCode::Failed);
	EXPECT_TRUE(Level->ContainsActor(Actor));
	EXPECT_TRUE(Package->IsDirty());
	EXPECT_GE(Package->GetEditRevision(), BeforeRevision);
	ASSERT_TRUE(F.Transactions->Reset());
	MarkObjectHierarchyAsGarbage(Package);
}

TEST(FComponentMutationTests, ComponentRemovingItselfDuringCreationCannotReportSuccess)
{
	FComponentFixture F;
	auto* RecoveryHistory = F.Transactions.Get();
	auto* Editor = F.Editor.GetActorEditor();
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::CreationProbe = {}; } } Reset;
	FStructuralTestLight::CreationProbe = [](FStructuralTestLight& Component) {
		EXPECT_TRUE(Component.GetOwner()->DestroyInstanceComponent(&Component));
	};
	const auto Result = Editor->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false});
	EXPECT_FALSE(Result);
	EXPECT_EQ(RecoveryHistory->GetLastResult().Code, ETransactorResultCode::RecoveryRequired);
	EXPECT_EQ(RecoveryHistory->GetState(), ETransactorState::RecoveryRequired);
}

TEST(FActorMutationTests, OversizedActorEditsStillPublishUndoableIdentity)
{
	FComponentFixture F;
	Durin::FTransBufferTestAccess::InitializeMemoryBudget(*F.Transactions.Get(), 1);
	auto* Editor = F.Editor.GetActorEditor();
	const auto Added = Editor->SpawnActor(*F.World->GetCurrentLevel(), AStaticMeshActor::StaticClass(), "Oversized", {}, {false});
	ASSERT_TRUE(Added);
	auto* Actor = Added;
	EXPECT_TRUE(F.World->GetCurrentLevel()->ContainsActor(Actor));
	ASSERT_TRUE(F.Transactions->Undo());
	CollectGarbage();
	EXPECT_TRUE(IsValid(Actor));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(F.World->GetCurrentLevel()->FindActorByName("Oversized"), Actor);
}

TEST(FActorMutationTests, WorldClosingDuringActorRemovalCannotReportSuccess)
{
	FComponentFixture F;
	auto* Setup = F.Editor.GetActorEditor();
	ASSERT_TRUE(Setup->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false}));
	ASSERT_TRUE(F.Transactions->Reset());
	auto* RecoveryHistory = F.Transactions.Get();
	auto* Editor = F.Editor.GetActorEditor();
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::UnregisterProbe = {}; } } Reset;
	FStructuralTestLight::UnregisterProbe = [&](FStructuralTestLight&) { F.World->RequestShutdown(); };
	auto* Actor = F.Actor;
	const auto Result = Editor->DestroyActors(*F.World->GetCurrentLevel(), {&Actor, 1}, {false});
	EXPECT_FALSE(Result);
	EXPECT_EQ(RecoveryHistory->GetLastResult().Code, ETransactorResultCode::RecoveryRequired);
	EXPECT_EQ(RecoveryHistory->GetState(), ETransactorState::RecoveryRequired);
}

TEST(FActorMutationTests, LabelsReuseVisibleNamesAfterUndoWithoutReusingObjectIdentity)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    auto* Service = F.GetActorEditor();
    auto* First = Service->SpawnActor(*Level, AActor::StaticClass(), "Example", {}, {false});
    ASSERT_NE(First, nullptr);
    EXPECT_EQ(First->GetActorLabel(), "Example");
    ASSERT_TRUE(F.GetTransactions()->Undo());
    CollectGarbage();
    EXPECT_FALSE(Level->ContainsActor(First));
    EXPECT_FALSE(Level->IsActorNameAvailable("Example"));
    EXPECT_EQ(Level->SpawnActor<AActor>("Example", ESpawnActorNameMode::RequiredReturnNull), nullptr);
    EXPECT_EQ(Service->SpawnActor(*Level, AActor::StaticClass(), "Example", {}, {false}, {}, ESpawnActorNameMode::RequiredReturnNull), nullptr);
    EXPECT_TRUE(F.GetTransactions()->CanRedo());
    auto* Second = Service->SpawnActor(*Level, AActor::StaticClass(), "Example", {}, {false});
    ASSERT_NE(Second, nullptr);
    EXPECT_NE(Second, First);
    EXPECT_EQ(Second->GetFName(), FName("Example_2"));
    EXPECT_EQ(Second->GetActorLabel(), "Example");
    ASSERT_TRUE(F.GetTransactions()->Undo());
    ASSERT_TRUE(F.GetTransactions()->Redo());
    EXPECT_TRUE(Level->ContainsActor(Second));
    EXPECT_EQ(Second->GetActorLabel(), "Example");
    auto* Runtime = Level->SpawnActor<AActor>("Example");
    ASSERT_NE(Runtime, nullptr);
    EXPECT_NE(Runtime->GetFName(), First->GetFName());
    EXPECT_NE(Runtime->GetFName(), Second->GetFName());
    EXPECT_EQ(Runtime->GetActorLabel(), "Example_2");
}

TEST(FActorMutationTests, LabelEditsAndDuplicationPreserveNamesAndReplayExactLabels)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    auto* Service = F.GetActorEditor();
    auto* First = Level->SpawnActor<AActor>("Internal");
    auto* Other = Level->SpawnActor<AActor>("Other");
    ASSERT_NE(First, nullptr); ASSERT_NE(Other, nullptr);
    const std::string Path = First->GetObjectPath();
    ASSERT_TRUE(Service->SetActorLabel(*Level, First, "Visible", {false}));
    EXPECT_EQ(First->GetObjectPath(), Path);
    EXPECT_EQ(First->GetActorLabel(), "Visible");
    const auto Head = F.GetTransactions()->GetUndoId();
    ASSERT_TRUE(Service->SetActorLabel(*Level, First, "Visible", {false}));
    EXPECT_EQ(F.GetTransactions()->GetUndoId(), Head);
    EXPECT_FALSE(Service->SetActorLabel(*Level, First, "Denied", {true}));
    EXPECT_FALSE(Service->SetActorLabel(*Level, First, std::string("bad\0label", 9), {false}));
    ASSERT_TRUE(F.GetTransactions()->Undo()); EXPECT_EQ(First->GetActorLabel(), "Internal");
    ASSERT_TRUE(F.GetTransactions()->Redo()); EXPECT_EQ(First->GetActorLabel(), "Visible");
    ASSERT_TRUE(Service->SetActorLabel(*Level, Other, "Visible", {false}));
    EXPECT_EQ(Other->GetActorLabel(), "Visible_2");
    ASSERT_TRUE(Service->SetActorLabel(*Level, Other, "Visible", {false}, false));
    EXPECT_EQ(Other->GetActorLabel(), "Visible");
    AActor* Sources[] = {First, Other};
    const auto Copies = Service->DuplicateActors(*Level, Sources, {false});
    ASSERT_EQ(Copies.size(), 2u);
    EXPECT_EQ(Copies[0]->GetActorLabel(), "Visible_2");
    EXPECT_EQ(Copies[1]->GetActorLabel(), "Visible_3");
    ASSERT_TRUE(F.GetTransactions()->Undo());
    ASSERT_TRUE(F.GetTransactions()->Redo());
    EXPECT_EQ(Copies[1]->GetActorLabel(), "Visible_3");
    ASSERT_TRUE(Service->RenameActor(*Level, First, "RenamedInternal", {false}));
    EXPECT_EQ(First->GetActorLabel(), "Visible");
    EXPECT_NE(First->GetObjectPath(), Path);
    ASSERT_TRUE(F.GetTransactions()->Undo());
    EXPECT_EQ(First->GetObjectPath(), Path);
    EXPECT_EQ(First->GetActorLabel(), "Visible");
}

TEST(FActorMutationTests, UnspecifiedNamesIgnoreRequiredModeAndLegacyLabelsSurviveInternalRename)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    auto* First = Level->SpawnActor<AActor>();
    auto* Second = Level->SpawnActor<AActor>({}, ESpawnActorNameMode::RequiredReturnNull);
    ASSERT_NE(First, nullptr); ASSERT_NE(Second, nullptr);
    EXPECT_NE(First->GetFName(), Second->GetFName());
    auto* Third = F.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), {}, {}, {false}, {}, ESpawnActorNameMode::RequiredReturnNull);
    ASSERT_NE(Third, nullptr);
    auto* Legacy = NewObject<AActor>(Level, "Legacy");
    EXPECT_EQ(Legacy->GetActorLabel(), "Legacy");
    ASSERT_TRUE(Level->SetActorPresence(Legacy, true));
    const uint64 Revision = Level->GetEditorActorHierarchyRevision();
    ASSERT_TRUE(F.GetActorEditor()->SetActorLabel(*Level, Legacy, "Legacy label", {false}));
    EXPECT_GT(Level->GetEditorActorHierarchyRevision(), Revision);
    ASSERT_TRUE(F.GetTransactions()->Undo());
    ASSERT_TRUE(F.GetActorEditor()->RenameActor(*Level, Legacy, "NewInternal", {false}));
    EXPECT_EQ(Legacy->GetActorLabel(), "Legacy");
    ASSERT_TRUE(F.GetTransactions()->Undo());
    EXPECT_EQ(Legacy->GetName(), "Legacy");
    EXPECT_EQ(Legacy->GetActorLabel(), "Legacy");
}

TEST(FActorMutationTests, LabelCommandComposesWithWholeObjectModifyInTheSameTransaction)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    auto* Actor = Level->SpawnActor<AActor>("Original");
    ASSERT_NE(Actor, nullptr);
    {
        Editor::FScopedTransaction Scope(F.GetTransactions(), {"test", "Modify and label"});
        ASSERT_TRUE(Actor->Modify());
        Actor->SetHidden(true);
        ASSERT_TRUE(F.GetActorEditor()->SetActorLabel(*Level, Actor, "Display", {false}));
    }
    ASSERT_TRUE(F.GetTransactions()->Undo());
    EXPECT_FALSE(Actor->IsHidden());
    EXPECT_EQ(Actor->GetActorLabel(), "Original");
    ASSERT_TRUE(F.GetTransactions()->Redo());
    EXPECT_TRUE(Actor->IsHidden());
    EXPECT_EQ(Actor->GetActorLabel(), "Display");
    EXPECT_EQ(Actor->GetName(), "Original");
}
