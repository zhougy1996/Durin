#include "Operations/ComponentMutations.h"
#include "Operations/ActorMutations.h"
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
		auto OnComponentCreated() -> void override { Super::OnComponentCreated(); ++Created; CreatedLocation = GetWorldLocation(); }
		auto OnComponentDestroyed() -> void override { Super::OnComponentDestroyed(); ++Destroyed; }
		auto OnRegister() -> void override { Super::OnRegister(); ++Registered; }
		auto OnUnregister() -> void override { Super::OnUnregister(); ++Unregistered; }
		auto InitializeComponent() -> void override { Super::InitializeComponent(); ++Initialized; }
		auto UninitializeComponent() -> void override { Super::UninitializeComponent(); ++Uninitialized; }
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
	using Durin::Editor::Level::FComponentMutations;
	using Durin::Editor::Level::FActorMutations;

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
		static auto Initialize() -> std::string_view { Testing::InitializeDObjectSystemForTests(); return "ComponentTransactions"; }
		FComponentFixture() : Transactions(Initialize())
		{
			World = NewObject<DWorld>(nullptr, "ComponentTestWorld");
			Root = std::make_unique<TStrongObjectPtr<DWorld>>(World);
			EXPECT_TRUE(World->InitializeSubsystems());
			EXPECT_TRUE(World->SetCurrentLevel(NewObject<DLevel>(World, "Level")));
			World->SetRenderScene(&Scene);
			Actor = World->SpawnActor<AStaticMeshActor>("Owner");
		}
		~FComponentFixture()
		{
			EXPECT_TRUE(Transactions->Reset());
			World->SetRenderScene(nullptr);
			MarkObjectHierarchyAsGarbage(World);
			Root.reset();
			CollectGarbage();
		}
		FComponentTestScene Scene;
		DWorld* World = nullptr;
		AActor* Actor = nullptr;
		std::unique_ptr<TStrongObjectPtr<DWorld>> Root;
		Tests::FTestTransactorOwner Transactions;
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
		const auto Added = FActorMutations::Add(*Level, Class, FName(), FTransform{}, F.Transactions.Get());
		ASSERT_TRUE(Added) << Added.Message;
		auto* Actor = Added.Actors.front().Get();
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
		const auto Copies = FActorMutations::Duplicate(*Level, {&Source, 1}, F.Transactions.Get());
		ASSERT_TRUE(Copies) << Copies.Message;
		ASSERT_EQ(Copies.Actors.size(), 1u);
		EXPECT_EQ(Copies.Actors.front()->GetClass(), Class);
		EXPECT_NE(Copies.Actors.front().Get(), Actor);
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
		Transaction.Modify(Actor->GetRootComponent());
		Actor->GetRootComponent()->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Before = Grandchild->GetActorTransform().Translation;
	AActor* Target = Actor;
	ASSERT_TRUE(FActorMutations::Remove(*Level, {&Target, 1}, F.Transactions.Get()));
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
	EXPECT_EQ(Light->Initialized, 2u);
	EXPECT_EQ(Light->Uninitialized, 1u);
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
	ASSERT_TRUE(FActorMutations::Remove(*Level, Targets, F.Transactions.Get()));
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
	const auto Result = FActorMutations::Duplicate(*Level, Targets, F.Transactions.Get());
	ASSERT_TRUE(Result) << Result.Message;
	ASSERT_EQ(Result.Actors.size(), 2u);
	auto* ParentCopy = Result.Actors[0].Get();
	auto* Copy = static_cast<ASplineMeshActor*>(Result.Actors[1].Get());
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
	const auto Duplicated = FActorMutations::Duplicate(*Level, {&Target, 1}, F.Transactions.Get());
	ASSERT_TRUE(Duplicated) << Duplicated.Message;
	auto* Copy = Duplicated.Actors.front().Get();
	EXPECT_EQ(Copy->GetAttachParentActor(), F.Actor);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Copy->GetAttachParentActor(), nullptr);
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Copy->GetAttachParentActor(), F.Actor);
	const auto Rootless = FActorMutations::Add(*Level, AActor::StaticClass(), "Rootless", FTransform{}, F.Transactions.Get());
	ASSERT_TRUE(Rootless);
	EXPECT_EQ(Rootless.Actors.front()->GetRootComponent(), nullptr);
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Level->ContainsActor(Rootless.Actors.front().Get()));
}

TEST(FActorMutationTests, BatchConflictRejectsEveryWriteAndRetainsHistoryForRetry)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Other = Level->SpawnActor<AActor>("Other");
	std::array<AActor*, 2> Targets{F.Actor, Other};
	ASSERT_TRUE(FActorMutations::Remove(*Level, Targets, F.Transactions.Get()));
	auto* Conflict = Level->SpawnActor<AActor>(Other->GetFName());
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
	ASSERT_TRUE(FActorMutations::Remove(*Level, {&Target, 1}, F.Transactions.Get()));
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
	ASSERT_TRUE(FActorMutations::Rename(*Level, F.Actor, "Renamed", F.Transactions.Get()));
	const auto Head = F.Transactions->GetUndoId();
	ASSERT_TRUE(FActorMutations::Rename(*Level, F.Actor, "Renamed", F.Transactions.Get()));
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
	const auto Scope = F.Transactions->Begin({.Description = "Block actor editing"});
	const auto Count = Level->GetActors().size();
	EXPECT_FALSE(FActorMutations::Add(*Level, AStaticMeshActor::StaticClass(), "Blocked", FTransform{}, F.Transactions.Get()));
	EXPECT_EQ(Level->GetActors().size(), Count);
	(void)F.Transactions->Cancel(Scope.ScopeId);
	std::array<AActor*, 2> Invalid{F.Actor, F.Actor};
	EXPECT_FALSE(FActorMutations::Remove(*Level, Invalid, F.Transactions.Get()));
	EXPECT_FALSE(FActorMutations::Duplicate(*Level, Invalid, F.Transactions.Get()));
	EXPECT_FALSE(FActorMutations::Rename(*Level, F.Actor, "ReadOnly", F.Transactions.Get(), true));
	const auto Added = FActorMutations::Add(*Level, AStaticMeshActor::StaticClass(), "Disposable", FTransform{}, F.Transactions.Get());
	ASSERT_TRUE(Added);
	TWeakObjectPtr<AActor> Weak(Added.Actors.front().Get());
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Reset());
	CollectGarbage();
	EXPECT_EQ(Weak.Get(), nullptr);
}

TEST(FComponentMutationTests, AdditionUndoRedoBalancesLifecycleAndSceneMembership)
{
	FComponentFixture F;
	const auto Added = FComponentMutations::Add(*F.Actor, FStructuralTestLight::StaticClass(), F.Actor->GetRootComponent(), F.Transactions.Get());
	ASSERT_TRUE(Added) << Added.Message;
	auto* Light = static_cast<FStructuralTestLight*>(Added.Component);
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
	const auto Added = FComponentMutations::Add(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Added) << Added.Message;
	auto* Light = static_cast<FStructuralTestLight*>(Added.Component);
	EXPECT_EQ(Light->GetWorldLocation(), FVector3(3.0, 0.0, 0.0));
	EXPECT_EQ(Light->CreatedLocation, Light->GetWorldLocation());
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Light->GetWorldLocation(), FVector3(3.0, 0.0, 0.0));
	const auto Child = FComponentMutations::Add(*F.Actor, FStructuralTestLight::StaticClass(), Light, F.Transactions.Get());
	ASSERT_TRUE(Child) << Child.Message;
	EXPECT_EQ(static_cast<DSceneComponent*>(Child.Component)->GetRelativeLocation(), FVector3(0.0));
	EXPECT_EQ(static_cast<FStructuralTestLight*>(Child.Component)->CreatedLocation, Light->GetWorldLocation());
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
		Transaction.Modify(Light);
		Light->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Before = Child->GetWorldLocation();
	ASSERT_TRUE(FComponentMutations::Remove(*F.Actor, Light, F.Transactions.Get()));
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
	ASSERT_TRUE(F.Transactions->Undo());
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
	const auto Result = FComponentMutations::Duplicate(*F.Actor, Source, F.Transactions.Get());
	ASSERT_TRUE(Result) << Result.Message;
	auto* Copy = static_cast<DSplineComponent*>(Result.Component);
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
	const auto Result = FComponentMutations::Duplicate(*F.Actor, Source, F.Transactions.Get());
	ASSERT_TRUE(Result) << Result.Message;
	auto* Copy = static_cast<FStructuralTestLight*>(Result.Component);
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
	const auto Result = FComponentMutations::Add(*Empty, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Result) << Result.Message;
	EXPECT_EQ(Empty->GetRootComponent(), Result.Component);
	Result.Component->UnregisterComponent();
	ASSERT_TRUE(FComponentMutations::Remove(*Empty, Result.Component, F.Transactions.Get()));
	EXPECT_EQ(Empty->GetRootComponent(), nullptr);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Empty->GetRootComponent(), Result.Component);
	EXPECT_FALSE(Result.Component->IsRegistered());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Empty->GetRootComponent(), nullptr);
}

TEST(FComponentMutationTests, RejectionDoesNotPublishAndDiscardedInactiveComponentsAreCollected)
{
	FComponentFixture F;
	const auto Count = F.Actor->GetComponents().size();
	const auto Scope = F.Transactions->Begin({.Description = "Block structural editing"});
	ASSERT_FALSE(FComponentMutations::Add(*F.Actor, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get()));
	EXPECT_EQ(F.Actor->GetComponents().size(), Count);
	(void)F.Transactions->Cancel(Scope.ScopeId);
	ASSERT_FALSE(FComponentMutations::Remove(*F.Actor, F.Actor->GetRootComponent(), F.Transactions.Get()));
	ASSERT_FALSE(FComponentMutations::Add(*F.Actor, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get(), true));
	const auto Added = FComponentMutations::Add(*F.Actor, DStaticMeshComponent::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Added);
	EXPECT_TRUE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Added.Component)));
	TWeakObjectPtr<DActorComponent> Weak(Added.Component);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Added.Component)));
	ASSERT_TRUE(F.Transactions->Reset());
	CollectGarbage();
	EXPECT_EQ(Weak.Get(), nullptr);
}

TEST(FComponentMutationTests, ConflictingRootRejectsUndoWithoutPartialRestoration)
{
	FComponentFixture F;
	auto* Owner = F.World->SpawnActor<AActor>("RootOwner");
	const auto Added = FComponentMutations::Add(*Owner, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Added);
	ASSERT_TRUE(FComponentMutations::Remove(*Owner, Added.Component, F.Transactions.Get()));
	auto* Replacement = static_cast<DSceneComponent*>(Owner->AddInstanceComponent(DSceneComponent::StaticClass(), "Replacement"));
	const auto Head = F.Transactions->GetUndoId();
	EXPECT_FALSE(F.Transactions->Undo());
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	EXPECT_EQ(Owner->GetRootComponent(), Replacement);
	EXPECT_FALSE(Owner->OwnsComponent(Added.Component));
	EXPECT_FALSE(Added.Component->IsRegistered());
	EXPECT_TRUE(IsPackageCaptureObjectOmitted(Added.Component));
	ASSERT_TRUE(Owner->DestroyInstanceComponent(Replacement));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Owner->GetRootComponent(), Added.Component);
}

TEST(FComponentMutationTests, RenamePreservesIdentityAndEarlierPropertyHistory)
{
	FComponentFixture F;
	auto* Light = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light"));
	const FName Before = Light->GetFName();
	const auto Path = Light->GetObjectPath();
	{
		Editor::FScopedTransaction Transaction(F.Transactions.Get(), {"test", "Edit before rename"});
		Transaction.Modify(Light);
		Light->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Instances = F.Actor->GetInstanceComponents();
	ASSERT_TRUE(FComponentMutations::Rename(*F.Actor, Light, "Renamed", F.Transactions.Get()));
	EXPECT_EQ(Light->GetFName(), FName("Renamed"));
	EXPECT_NE(Light->GetObjectPath(), Path);
	const auto Head = F.Transactions->GetUndoId();
	ASSERT_TRUE(FComponentMutations::Rename(*F.Actor, Light, "Renamed", F.Transactions.Get()));
	EXPECT_EQ(F.Transactions->GetUndoId(), Head);
	CollectGarbage();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Light->GetFName(), Before);
	EXPECT_EQ(Light->GetObjectPath(), Path);
	ASSERT_TRUE(F.Transactions->Undo());
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
	ASSERT_TRUE(FComponentMutations::Rename(*F.Actor, Root, "RenamedRoot", F.Transactions.Get()));
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
	const auto Inactive = FComponentMutations::Add(*F.Actor, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Inactive);
	const auto Reserved = Inactive.Component->GetFName();
	ASSERT_TRUE(F.Transactions->Undo());
	auto* Component = F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Original");
	ASSERT_TRUE(FComponentMutations::Rename(*F.Actor, Component, Reserved, F.Transactions.Get()));
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
	ASSERT_TRUE(FComponentMutations::Reparent(*F.Actor, Light, Parent, F.Transactions.Get()));
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
	EXPECT_FALSE(FComponentMutations::Reparent(*F.Actor, Component, Child, F.Transactions.Get()));
	EXPECT_FALSE(FComponentMutations::Reparent(*F.Actor, Component, Foreign->GetRootComponent(), F.Transactions.Get()));
	EXPECT_FALSE(FComponentMutations::Reparent(*F.Actor, F.Actor->GetRootComponent(), Component, F.Transactions.Get()));
	EXPECT_FALSE(FComponentMutations::Reparent(*F.Actor, Component, nullptr, F.Transactions.Get(), true));
	EXPECT_FALSE(FComponentMutations::Rename(*F.Actor, Component, "New", F.Transactions.Get(), true));
	EXPECT_FALSE(FComponentMutations::Rename(*F.Actor, Foreign->GetRootComponent(), "New", F.Transactions.Get()));
	EXPECT_FALSE(FComponentMutations::Rename(*F.Actor, Component, FName(), F.Transactions.Get()));
	EXPECT_FALSE(F.Transactions->CanUndo());
	ASSERT_TRUE(FComponentMutations::Reparent(*F.Actor, Component, nullptr, F.Transactions.Get()));
	EXPECT_EQ(Component->GetAttachParent(), nullptr);
	EXPECT_EQ(Component->GetWorldLocation(), Before);
	const auto Head = F.Transactions->GetUndoId();
	ASSERT_TRUE(FComponentMutations::Reparent(*F.Actor, Component, nullptr, F.Transactions.Get()));
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
	ASSERT_TRUE(FComponentMutations::Reparent(*F.Actor, Moving, Parent, F.Transactions.Get()));
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
	const auto Added = FComponentMutations::Add(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, F.Transactions.Get());
	const auto Parent = FComponentMutations::Add(*F.Actor, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Added);
	ASSERT_TRUE(Parent);
	ASSERT_TRUE(FComponentMutations::Rename(*F.Actor, Added.Component, "Renamed", F.Transactions.Get()));
	ASSERT_TRUE(FComponentMutations::Reparent(*F.Actor, static_cast<DSceneComponent*>(Added.Component), static_cast<DSceneComponent*>(Parent.Component), F.Transactions.Get()));
	const auto Copy = FComponentMutations::Duplicate(*F.Actor, Added.Component, F.Transactions.Get());
	ASSERT_TRUE(Copy);
	ASSERT_TRUE(FComponentMutations::Remove(*F.Actor, Added.Component, F.Transactions.Get()));
	for (int Step = 0; Step < 6; ++Step) { ASSERT_TRUE(F.Transactions->Undo()); CollectGarbage(); }
	EXPECT_FALSE(F.Actor->OwnsComponent(Added.Component));
	EXPECT_FALSE(F.Actor->OwnsComponent(Copy.Component));
	EXPECT_FALSE(F.Actor->OwnsComponent(Parent.Component));
	for (int Step = 0; Step < 6; ++Step) { ASSERT_TRUE(F.Transactions->Redo()); CollectGarbage(); }
	EXPECT_FALSE(F.Actor->OwnsComponent(Added.Component));
	EXPECT_TRUE(F.Actor->OwnsComponent(Copy.Component));
	EXPECT_EQ(static_cast<DSceneComponent*>(Copy.Component)->GetAttachParent(), Parent.Component);
	EXPECT_TRUE(F.Scene.Lights.contains(static_cast<DLightComponent*>(Copy.Component)));
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
	auto* Owner = Level->SpawnActor<AActor>("Owner");
	Package->ClearDirty();
	F.Transactions->EstablishSavedState(*Package);
	ObjectPackage::FLinkerTables Baseline, Active, Absent;
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, Baseline));
	const auto Added = FComponentMutations::Add(*Owner, DSceneComponent::StaticClass(), nullptr, F.Transactions.Get());
	ASSERT_TRUE(Added) << Added.Message;
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(CapturePackageLinker(Package, EDefaultDeltaMode::NoDelta, {}, Active));
	EXPECT_EQ(Active.Exports.size(), Baseline.Exports.size() + 1);
	ASSERT_TRUE(FComponentMutations::Remove(*Owner, Added.Component, F.Transactions.Get()));
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
	ASSERT_TRUE(FComponentMutations::Rename(*Owner, Child, "Renamed", F.Transactions.Get()));
	ASSERT_TRUE(FComponentMutations::Reparent(*Owner, Child, Parent, F.Transactions.Get()));
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
	ASSERT_TRUE(FComponentMutations::Rename(*Owner, Child, "SecondName", F.Transactions.Get()));
	ASSERT_TRUE(FActorMutations::Rename(*Level, Actor, "RenamedActor", F.Transactions.Get()));
	AActor* Target = Actor;
	const auto Copy = FActorMutations::Duplicate(*Level, {&Target, 1}, F.Transactions.Get());
	ASSERT_TRUE(Copy) << Copy.Message;
	ASSERT_TRUE(FActorMutations::Remove(*Level, {&Target, 1}, F.Transactions.Get()));
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
