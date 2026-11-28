#include "Editor/ActorHierarchyChangeTestSupport.h"
#include "Editor/EditorActorSubsystemTestSupport.h"
#include "Transactions/Transactor.h"
#include "Editor/EditorTransactionTestSupport.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/CameraActor.h"
#include "Actors/SplineMeshActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "StaticMesh/StaticMesh.h"
#include "Asset/AssetImportData.h"
#include "Materials/Material.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
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
		auto OnRegister() -> void override { Super::OnRegister(); ++Registered; if (RegisterProbe) RegisterProbe(*this); }
		auto OnUnregister() -> void override { Super::OnUnregister(); ++Unregistered; if (UnregisterProbe) UnregisterProbe(*this); }
		auto InitializeComponent() -> void override { Super::InitializeComponent(); ++Initialized; }
		auto UninitializeComponent() -> void override { Super::UninitializeComponent(); ++Uninitialized; }
		auto PostLoad() -> void override { Super::PostLoad(); if (PostLoadProbe) PostLoadProbe(*this); }
		auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override
		{
			Super::PostEditChangeProperty(Event);
			if (ObserveParentLoad && Event.Origin != EPropertyChangeOrigin::Edit && Event.MemberProperty
				&& Event.MemberProperty->NamePrivate == FName("AttachParent")) ObserveParentLoad();
		}
		std::function<void()> ObserveParentLoad;
		inline static std::function<void(FStructuralTestLight&)> PostLoadProbe;
		inline static std::function<void(FStructuralTestLight&)> CreationProbe;
		inline static std::function<void(FStructuralTestLight&)> RegisterProbe;
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
	using namespace Durin;


	class FComponentTestScene final : public FSceneInterface
	{
	public:
		auto Release() -> void override {}
		auto AddPrimitive(DPrimitiveComponent* Value) -> void override { Primitives.insert(Value); }
		auto RemovePrimitive(DPrimitiveComponent* Value) -> void override { Primitives.erase(Value); }
		auto AddLight(DLightComponent* Value) -> void override { Lights[Value] = Value->GetWorldLocation(); }
		auto RemoveLight(DLightComponent* Value) -> void override { Lights.erase(Value); }
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
		CollectGarbage(Durin::EObjectFlags::Standalone);
		ASSERT_TRUE(IsValid(Actor));
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_TRUE(Level->ContainsActor(Actor));
		EXPECT_EQ(Actor->GetComponentsSnapshot(), Components);
		EXPECT_TRUE(Actor->PrimaryActorTick.IsTickFunctionRegistered());
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
		Durin::FScopedTransaction Transaction("test", "Edit actor before delete", nullptr);
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
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

TEST(FActorMutationTests, MixedBatchDeletionRestoresActorOrderPrimaryCameraAndRegistration)
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Level->GetActors(), Order);
	EXPECT_EQ(Level->GetPrimaryCameraActor(), First);
	EXPECT_EQ(Second->GetAttachParentActor(), First);
	EXPECT_TRUE(First->GetRootComponent()->IsRegistered());
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Level->FindActorByName(Copy->GetFName()), Copy);
	EXPECT_EQ(Copy->GetAttachParentActor(), ParentCopy);
	EXPECT_TRUE(F.Scene.Lights.contains(LightCopy));
}

TEST(FActorMutationTests, DuplicatePostLoadObservesRemappedForwardReferenceOnce)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Source = Level->SpawnActor<AStaticMeshActor>("MeshUser");
	auto* Provider = Level->SpawnActor<AActor>("MeshOwner");
	auto* Mesh = NewObject<DStaticMesh>(Provider, "SharedMesh");
	ASSERT_TRUE(Mesh);
	FMeshDescription Geometry;
	Geometry.PolygonGroups.push_back({.Name = "Default", .SourceMaterialIndex = 0, .SourceName = "Default"});
	auto& Section = Geometry.Sections.emplace_back();
	Section.Positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
	Section.Indices = {0, 1, 2};
	Section.SourceMaterialIndex = 0;
	FStaticMeshSource Authored;
	ASSERT_TRUE(Authored.Initialize(std::move(Geometry)));
	auto* Import = NewObject<DAssetImportData>(Mesh, "SourceData");
	const auto Initialized = Mesh->SetImportedSource(std::move(Authored), {{.Name = "Default", .SourceName = "Default"}}, *Import);
	ASSERT_TRUE(Initialized) << Initialized.error();
	Source->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	TStrongObjectPtr<DMaterial> Material(NewObject<DMaterial>(nullptr, "SharedMaterial"));
	ASSERT_TRUE(Source->GetStaticMeshComponent()->SetMaterial(Material.Get()));
	ASSERT_TRUE(Source->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Observer"));
	int Calls = 0;
	TObjectPtr<DStaticMesh> ObservedMesh;
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::PostLoadProbe = {}; } } Reset;
	FStructuralTestLight::PostLoadProbe = [&](FStructuralTestLight& Component) {
		auto* Copy = Cast<AStaticMeshActor>(Component.GetOuter());
		ASSERT_TRUE(Copy && Copy != Source);
		++Calls;
		ObservedMesh = Copy->GetStaticMeshComponent()->GetStaticMesh();
		ASSERT_TRUE(ObservedMesh);
		EXPECT_NE(ObservedMesh.Get(), Mesh);
		EXPECT_EQ(Copy->GetStaticMeshComponent()->GetMaterial(), Material.Get());
	};
	AActor* Sources[] = {Source, Provider};
	const auto Copies = F.Editor.GetActorEditor()->DuplicateActors(*Level, Sources, {false});
	FStructuralTestLight::PostLoadProbe = {};
	ASSERT_EQ(Copies.size(), 2u);
	ASSERT_EQ(Calls, 1);
	ASSERT_TRUE(ObservedMesh);
	EXPECT_EQ(ObservedMesh->GetOuter(), Copies.back().Get());
	auto* MeshCopy = static_cast<AStaticMeshActor*>(Copies.front().Get())->GetStaticMeshComponent();
	EXPECT_EQ(MeshCopy->GetStaticMesh(), ObservedMesh.Get());
	EXPECT_EQ(Source->GetStaticMeshComponent()->GetStaticMesh(), Mesh);
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(MeshCopy->GetStaticMesh(), ObservedMesh.Get());
	EXPECT_EQ(MeshCopy->GetMaterial(), Material.Get());
}

TEST(FActorMutationTests, DuplicationKeepsInternalChildrenAndDetachesNonRootExternalChildrenInWorldSpace)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Source = Level->SpawnActor<AStaticMeshActor>("Source");
	auto* External = Cast<DSceneComponent>(Source->AddInstanceComponent(DSceneComponent::StaticClass(), "External"));
	auto* Inner = Cast<DSceneComponent>(Source->AddInstanceComponent(DSceneComponent::StaticClass(), "Inner"));
	ASSERT_TRUE(External && Inner);
	ASSERT_TRUE(External->AttachToComponent(F.Actor->GetRootComponent(), EAttachmentTransformRule::KeepRelative));
	ASSERT_TRUE(Inner->AttachToComponent(External, EAttachmentTransformRule::KeepRelative));
	External->SetWorldLocation({17, 4, 9});
	Inner->SetRelativeLocation({3, 2, 1});
	const auto Location = Inner->GetWorldLocation();
	AActor* Sources[] = {Source, F.Actor};
	const auto Copies = F.Editor.GetActorEditor()->DuplicateActors(*Level, Sources, {false});
	ASSERT_EQ(Copies.size(), 2u);
	const auto& Components = Copies.front()->GetInstanceComponents();
	ASSERT_EQ(Components.size(), 2u);
	auto* ExternalCopy = Cast<DSceneComponent>(Components[0].Get());
	auto* InnerCopy = Cast<DSceneComponent>(Components[1].Get());
	ASSERT_TRUE(ExternalCopy && InnerCopy);
	auto Check = [&] {
		EXPECT_EQ(External->GetAttachParent(), F.Actor->GetRootComponent());
		EXPECT_EQ(ExternalCopy->GetAttachParent(), nullptr);
		EXPECT_EQ(ExternalCopy->GetWorldLocation(), External->GetWorldLocation());
		EXPECT_EQ(InnerCopy->GetAttachParent(), ExternalCopy);
		EXPECT_EQ(InnerCopy->GetWorldLocation(), Location);
	};
	Check();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Level->ContainsActor(Copies.front().Get()));
	ASSERT_TRUE(F.Transactions->Redo());
	Check();
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Copy->GetAttachParentActor(), F.Actor);
	const auto Rootless = F.Editor.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), "Rootless", FTransform{}, {false});
	ASSERT_TRUE(Rootless);
	EXPECT_EQ(Rootless->GetRootComponent(), nullptr);
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Level->ContainsActor(Rootless));
}

TEST(FActorMutationTests, UnpublishedActorReservesItsNameThroughBatchUndo)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Other = Level->SpawnActor<AActor>("Other");
	std::array<AActor*, 2> Targets{F.Actor, Other};
	ASSERT_TRUE(F.Editor.GetActorEditor()->DestroyActors(*Level, Targets, {false}));
	auto* Conflict = Level->SpawnActor<AActor>(Other->GetFName());
	ASSERT_NE(Conflict, nullptr);
	EXPECT_NE(Conflict->GetFName(), Other->GetFName());
	const auto PreviousName = Conflict->GetFName();
	Conflict->Rename(Other->GetFName());
	EXPECT_EQ(Conflict->GetFName(), PreviousName);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Level->ContainsActor(F.Actor));
	EXPECT_TRUE(Level->ContainsActor(Other));
}

TEST(FActorMutationTests, CancellationDuringFirstDetachLeavesSecondChildAndActorPublished)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* First = Level->SpawnActor<AStaticMeshActor>("FirstChild");
	auto* Second = Level->SpawnActor<AStaticMeshActor>("SecondChild");
	ASSERT_TRUE(First->AttachToActor(F.Actor, EAttachmentTransformRule::KeepWorld));
	ASSERT_TRUE(Second->AttachToActor(F.Actor, EAttachmentTransformRule::KeepWorld));
	FScopedTransaction Outer("Cancel during removal");
	int Calls = 0;
	auto Handle = FEditorSupportDelegates::ActorHierarchyChanged.AddLambda([&](DLevel& Changed) {
		if (&Changed == Level && ++Calls == 1) Outer.Cancel();
	});
	AActor* Target = F.Actor;
	const bool Removed = F.Editor.GetActorEditor()->DestroyActors(*Level, {&Target, 1}, {false});
	FEditorSupportDelegates::ActorHierarchyChanged.Remove(Handle);
	EXPECT_FALSE(Removed);
	EXPECT_EQ(Calls, 1);
	EXPECT_EQ(First->GetAttachParentActor(), nullptr);
	EXPECT_EQ(Second->GetAttachParentActor(), Target);
	EXPECT_TRUE(Level->ContainsActor(Target));
	EXPECT_FALSE(F.Transactions->CanUndo());
}

TEST(FActorMutationTests, UndoRestoresActorDespiteConflictingExternalAttachment)
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
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Level->ContainsActor(F.Actor));
	EXPECT_EQ(Other->GetRootComponent()->GetAttachParent(), ExternalChild);
	EXPECT_FALSE(ExternalChild->IsAttachedTo(ExternalChild));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(Level->ContainsActor(F.Actor));
}

TEST(FActorMutationTests, ReversingAnAttachmentRestoresBothDirectionsAcrossRecordOrder)
{
	FComponentFixture F;
	auto* First = F.Actor->GetRootComponent();
	auto* Second = F.World->GetCurrentLevel()->SpawnActor<AStaticMeshActor>("Second")->GetRootComponent();
	ASSERT_TRUE(Second->AttachToComponent(First, EAttachmentTransformRule::KeepRelative));
	{
		FScopedTransaction Scope("Reverse attachment");
		ASSERT_TRUE(First->Modify());
		ASSERT_TRUE(Second->Modify());
		ASSERT_TRUE(Second->DetachFromComponent());
		ASSERT_TRUE(First->AttachToComponent(Second, EAttachmentTransformRule::KeepRelative));
	}
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(First->GetAttachParent(), nullptr);
	EXPECT_EQ(Second->GetAttachParent(), First);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(First->GetAttachParent(), Second);
	EXPECT_EQ(Second->GetAttachParent(), nullptr);
}

TEST(FActorMutationTests, UndoRenameDisplacesTheActorUsingTheHistoricalName)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	const auto Before = F.Actor->GetFName();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameActor(*Level, F.Actor, "Renamed", {false}));
	const auto Head = F.Transactions->GetUndoContext().TransactionId;
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameActor(*Level, F.Actor, "Renamed", {false}));
	EXPECT_EQ(F.Transactions->GetUndoContext().TransactionId, Head);
	auto* Conflict = Level->SpawnActor<AActor>(Before);
	ASSERT_TRUE(F.Transactions->UndoExpected(Head));
	const auto DisplacedName = Conflict->GetFName();
	EXPECT_NE(DisplacedName, Before);
	EXPECT_EQ(Level->FindActorByName(DisplacedName), Conflict);
	EXPECT_EQ(F.Actor->GetFName(), Before);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Level->FindActorByName("Renamed"), F.Actor);
}

TEST(FActorMutationTests, RejectionIsAtomicAndHistoryDiscardCollectsInactiveActorGraphs)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), true);
	const auto Count = Level->GetActors().size();
	EXPECT_FALSE(F.Editor.GetActorEditor()->SpawnActor(*Level, AStaticMeshActor::StaticClass(), "Blocked", FTransform{}, {false}));
	EXPECT_EQ(Level->GetActors().size(), Count);
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), false);
	std::array<AActor*, 2> Invalid{F.Actor, F.Actor};
	EXPECT_FALSE(F.Editor.GetActorEditor()->DestroyActors(*Level, Invalid, {false}));
	EXPECT_TRUE(F.Editor.GetActorEditor()->DuplicateActors(*Level, Invalid, {false}).empty());
	EXPECT_FALSE(F.Editor.GetActorEditor()->RenameActor(*Level, F.Actor, "ReadOnly", {true}));
	const auto Added = F.Editor.GetActorEditor()->SpawnActor(*Level, AStaticMeshActor::StaticClass(), "Disposable", FTransform{}, {false});
	ASSERT_TRUE(Added);
	TWeakObjectPtr<AActor> Weak(Added);
	ASSERT_TRUE(F.Transactions->Undo());
	F.Transactions->Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
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
	ASSERT_TRUE(F.Transactions->End());
	EXPECT_EQ(F.Transactions->GetQueueLength(), 1u);
	EXPECT_EQ(F.Transactions->GetUndoContext(false).Description, "Build actor hierarchy");
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Level->ContainsActor(Actor));
	EXPECT_FALSE(Level->ContainsActor(Duplicate));
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Level->ContainsActor(Actor));
	EXPECT_FALSE(Level->ContainsActor(Duplicate));
	EXPECT_EQ(Actor->GetFName(), FName("GroupedRenamed"));
	EXPECT_EQ(Actor->GetAttachParentActor(), F.Actor);
	EXPECT_TRUE(Actor->OwnsComponent(Added));
	EXPECT_EQ(Added->GetFName(), FName("GroupedComponent"));
}

TEST(FActorMutationTests, JoinedActorSpawnsRestoreCleanPackageOnUndo)
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

TEST(FActorMutationTests, BusyHistoryRejectsActorEditingBeforePreparation)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	const auto Count = Level->GetActors().size();
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions, true);
	bool bConfigured = false;
	const auto Result = Editor->SpawnActor(*Level, ACameraActor::StaticClass(), "Rejected", {},
		{false}, [&](AActor&) { bConfigured = true; });
	EXPECT_FALSE(Result);
	EXPECT_FALSE(bConfigured);
	EXPECT_EQ(Level->GetActors().size(), Count);
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), false);
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
		CollectGarbage(Durin::EObjectFlags::Standalone);
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
	ASSERT_TRUE(Light->SetComponentTickGroup(ETickingGroup::PostPhysics));
	Light->SetComponentTickEnabled(true);
	Light->SetRelativeLocation({4.0, 0.0, 0.0});
	auto* Child = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Child"));
	Child->SetRelativeLocation({2.0, 0.0, 0.0});
	ASSERT_TRUE(Child->AttachToComponent(Light, EAttachmentTransformRule::KeepRelative));
	const auto Instances = F.Actor->GetInstanceComponents();
	{
		Durin::FScopedTransaction Transaction("test", "Edit light before deletion", nullptr);
		(void)Light->Modify();
		Light->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Before = Child->GetWorldLocation();
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, Light, {false}));
	EXPECT_FALSE(Light->IsRegistered());
	EXPECT_FALSE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
	EXPECT_TRUE(Light->IsComponentTickEnabled());
	EXPECT_EQ(Light->GetPrimaryComponentTick().GetTickGroup(), ETickingGroup::PostPhysics);
	EXPECT_EQ(Child->GetAttachParent(), nullptr);
	EXPECT_EQ(Child->GetWorldLocation(), Before);
	EXPECT_FALSE(F.Scene.Lights.contains(Light));
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(F.Actor->GetInstanceComponents(), Instances);
	EXPECT_TRUE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
	EXPECT_TRUE(Light->IsComponentTickEnabled());
	EXPECT_EQ(Light->GetPrimaryComponentTick().GetTickGroup(), ETickingGroup::PostPhysics);
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
	EXPECT_FALSE(Light->GetPrimaryComponentTick().IsTickFunctionRegistered());
	EXPECT_TRUE(Light->IsComponentTickEnabled());
	EXPECT_EQ(Light->GetPrimaryComponentTick().GetTickGroup(), ETickingGroup::PostPhysics);
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
	TWeakObjectPtr<DObject> CopiedPayload(Inners.front());
	EXPECT_EQ(Copy->GetAttachParent(), nullptr);
	EXPECT_EQ(Copy->GetWorldLocation(), Source->GetWorldLocation());
	EXPECT_NEAR(Copy->GetLocalSplineLength(), Source->GetLocalSplineLength(), 1.e-6);
	ASSERT_TRUE(F.Transactions->Undo());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(CopiedPayload.IsValid());
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

TEST(FComponentMutationTests, UndoRootDeletionRegistersRestoredComponent)
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
	EXPECT_TRUE(Result->IsRegistered());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Empty->GetRootComponent(), nullptr);
}

TEST(FComponentMutationTests, RejectionDoesNotPublishAndDiscardedInactiveComponentsAreCollected)
{
	FComponentFixture F;
	const auto Count = F.Actor->GetComponents().size();
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), true);
	ASSERT_FALSE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false}));
	EXPECT_EQ(F.Actor->GetComponents().size(), Count);
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), false);
	ASSERT_FALSE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, F.Actor->GetRootComponent(), {false}));
	ASSERT_FALSE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {true}));
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*F.Actor, DStaticMeshComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	EXPECT_TRUE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Added)));
	TWeakObjectPtr<DActorComponent> Weak(Added);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(F.Scene.Primitives.contains(static_cast<DPrimitiveComponent*>(Added)));
	F.Transactions->Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Weak.Get(), nullptr);
}

TEST(FComponentMutationTests, RootUndoRestoresRecordedInstanceMembership)
{
	FComponentFixture F;
	auto* Owner = F.World->SpawnActor<AActor>("RootOwner");
	const auto Added = F.Editor.GetActorEditor()->AddComponent(*Owner, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*Owner, Added, {false}));
	auto* Replacement = static_cast<DSceneComponent*>(Owner->AddInstanceComponent(DSceneComponent::StaticClass(), "Replacement"));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Owner->GetRootComponent(), Added);
	EXPECT_TRUE(Owner->OwnsComponent(Added));
	EXPECT_FALSE(Owner->OwnsComponent(Replacement));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(Owner->OwnsComponent(Added));
}

TEST(FComponentMutationTests, RenamePreservesIdentityAndEarlierPropertyHistory)
{
	FComponentFixture F;
	auto* Light = static_cast<FStructuralTestLight*>(F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light"));
	const FName Before = Light->GetFName();
	const auto Path = Light->GetObjectPath();
	{
		Durin::FScopedTransaction Transaction("test", "Edit before rename", nullptr);
		(void)Light->Modify();
		Light->SetRelativeLocation({9.0, 0.0, 0.0});
	}
	const auto Instances = F.Actor->GetInstanceComponents();
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Light, "Renamed", {false}));
	EXPECT_EQ(Light->GetFName(), FName("Renamed"));
	EXPECT_NE(Light->GetObjectPath(), Path);
	const auto Head = F.Transactions->GetUndoContext().TransactionId;
	ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Light, "Renamed", {false}));
	EXPECT_EQ(F.Transactions->GetUndoContext().TransactionId, Head);
	CollectGarbage(Durin::EObjectFlags::Standalone);
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

TEST(FComponentMutationTests, RenameReservesInactiveNamesAndUndoDisplacesLiveConflicts)
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
	const auto Head = F.Transactions->GetUndoContext().TransactionId;
	ASSERT_TRUE(F.Transactions->UndoExpected(Head));
	EXPECT_NE(Conflict->GetFName(), FName("Original"));
	EXPECT_EQ(Conflict->GetOuter(), F.Actor);
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
	int ParentLoads = 0;
	Light->ObserveParentLoad = [&] {
		++ParentLoads;
		auto* RestoredParent = Light->GetAttachParent();
		ASSERT_NE(RestoredParent, nullptr);
		EXPECT_FALSE(std::ranges::contains(RestoredParent->GetAttachChildren(), TObjectPtr<DSceneComponent>(Light)));
	};
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
	EXPECT_EQ(ParentLoads, 4);
	Light->ObserveParentLoad = {};
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
	const auto Head = F.Transactions->GetUndoContext().TransactionId;
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Component, nullptr, {false}));
	EXPECT_EQ(F.Transactions->GetUndoContext().TransactionId, Head);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Component->GetAttachParent(), F.Actor->GetRootComponent());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Component->GetAttachParent(), nullptr);
}

TEST(FComponentMutationTests, ReparentRestoresSnapshotAndCompletesRedoWhenParentWouldCycle)
{
	FComponentFixture F;
	auto* Moving = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Moving"));
	auto* Parent = static_cast<DSceneComponent*>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Moving, Parent, {false}));
	ASSERT_TRUE(Moving->DetachFromComponent());
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_EQ(Moving->GetAttachParent(), F.Actor->GetRootComponent());
	// Undo captured the external detached state as its inverse.
	ASSERT_TRUE(F.Transactions->Redo());
	ASSERT_EQ(Moving->GetAttachParent(), nullptr);
	ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Moving, Parent, {false}));
	ASSERT_TRUE(F.Transactions->Undo());
	ASSERT_TRUE(Parent->AttachToComponent(Moving, EAttachmentTransformRule::KeepWorld));
	const auto Redo = F.Transactions->GetRedoContext().TransactionId;
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(F.Transactions->GetUndoContext().TransactionId, Redo);
	EXPECT_EQ(Moving->GetAttachParent(), nullptr);
	EXPECT_EQ(Parent->GetAttachParent(), Moving);
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
	for (int Step = 0; Step < 6; ++Step) { ASSERT_TRUE(F.Transactions->Undo()) << Step; CollectGarbage(Durin::EObjectFlags::Standalone); }
	EXPECT_FALSE(F.Actor->OwnsComponent(Added));
	EXPECT_FALSE(F.Actor->OwnsComponent(Copy));
	EXPECT_FALSE(F.Actor->OwnsComponent(Parent));
	for (int Step = 0; Step < 6; ++Step) { ASSERT_TRUE(F.Transactions->Redo()); CollectGarbage(Durin::EObjectFlags::Standalone); }
	EXPECT_FALSE(F.Actor->OwnsComponent(Added));
	EXPECT_TRUE(F.Actor->OwnsComponent(Copy));
	EXPECT_EQ(static_cast<DSceneComponent*>(Copy)->GetAttachParent(), Parent);
	EXPECT_TRUE(F.Scene.Lights.contains(static_cast<DLightComponent*>(Copy)));
}

TEST(FActorMutationTests, NameAndLabelEditsMarkOnceBeforeTransactionCommit)
{
	FComponentFixture F;
	const auto Directory = Testing::CreateTestFixtureDirectory("NameEditRevision");
	Testing::RegisterMountPointForTests("/NameEditRevision/", Directory.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/NameEditRevision/Level", Path));
	TStrongObjectPtr<DPackage> Package(CreatePackage(Path));
	ASSERT_TRUE(Package);
	auto* Level = NewObject<DLevel>(Package.Get(), "Level");
	ASSERT_TRUE(F.World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<AActor>("Original");
	ASSERT_TRUE(Actor);
	auto* Child = Actor->AddInstanceComponent(DActorComponent::StaticClass(), "Child");
	ASSERT_TRUE(Child);
	auto* Editor = F.Editor.GetActorEditor();
	FScopedTransaction Scope("Name and label edits");
	auto Revision = Package->GetEditRevision();
	ASSERT_TRUE(Editor->SetActorLabel(*Level, Actor, "Display", {false}));
	EXPECT_EQ(Package->GetEditRevision(), ++Revision);
	ASSERT_TRUE(Editor->RenameActor(*Level, Actor, "Internal", {false}));
	EXPECT_EQ(Package->GetEditRevision(), ++Revision);
	ASSERT_TRUE(Editor->RenameComponent(*Actor, Child, "RenamedChild", {false}));
	EXPECT_EQ(Package->GetEditRevision(), ++Revision);
	ASSERT_TRUE(Level->SetPrimaryCameraActor(nullptr));
	EXPECT_EQ(Package->GetEditRevision(), Revision);
}

TEST(FComponentMutationTests, DirtyNotificationCancellationAllowsLabelRenameAndReparentWithoutUndoHistory)
{
	FComponentFixture F;
	const auto Directory = Testing::CreateTestFixtureDirectory("CancelOnDirty");
	Testing::RegisterMountPointForTests("/CancelOnDirty/", Directory.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CancelOnDirty/Level", Path));
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, Path.GetPackageName()));
	Package->InitializeAssetPackage(Path);
	auto* Level = NewObject<DLevel>(Package.Get(), "Level");
	ASSERT_TRUE(F.World->SetCurrentLevel(Level));
	auto* Actor = Level->SpawnActor<AStaticMeshActor>("Original");
	ASSERT_TRUE(Actor);
	auto* Child = static_cast<DSceneComponent*>(Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Child"));
	auto* Parent = static_cast<DSceneComponent*>(Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
	ASSERT_TRUE(Child && Parent);
	ASSERT_NE(Child->GetAttachParent(), Parent);
	auto* Editor = F.Editor.GetActorEditor();
	int Cancellations = 0;
	const auto Handle = OnAssetPackagesChanged().AddLambda([&] {
		if (F.Transactions->IsActive())
		{
			++Cancellations;
			F.Transactions->Cancel();
		}
	});
	EXPECT_TRUE(Editor->SetActorLabel(*Level, Actor, "New label", {false}));
	EXPECT_EQ(Cancellations, 1);
	EXPECT_EQ(Actor->GetActorLabel(), "New label");
	EXPECT_EQ(F.Transactions->GetQueueLength(), 0u);
	EXPECT_TRUE(Editor->RenameActor(*Level, Actor, "NewName", {false}));
	EXPECT_TRUE(Editor->RenameComponent(*Actor, Child, "NewChild", {false}));
	EXPECT_TRUE(Editor->ReparentComponent(*Actor, Child, Parent, {false}));
	OnAssetPackagesChanged().Remove(Handle);
	EXPECT_EQ(Cancellations, 4);
	EXPECT_EQ(Actor->GetFName(), FName("NewName"));
	EXPECT_EQ(Child->GetFName(), FName("NewChild"));
	EXPECT_EQ(Child->GetAttachParent(), Parent);
	EXPECT_EQ(F.Transactions->GetQueueLength(), 0u);
}

TEST(FComponentMutationTests, RemovalCallbacksCanDirtyOtherPackagesOnlyOutsideBufferReplay)
{
	FComponentFixture F;
	const auto Directory = Testing::CreateTestFixtureDirectory("CallbackDirty");
	Testing::RegisterMountPointForTests("/CallbackDirty/", Directory.generic_string() + "/");
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/CallbackDirty/Witness", Path));
	TStrongObjectPtr<DPackage> Other(NewObject<DPackage>(nullptr, Path.GetPackageName()));
	Other->InitializeAssetPackage(Path);
	auto* Witness = NewObject<DLevel>(Other.Get(), "Witness");
	auto* Light = F.Actor->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Light");
	ASSERT_NE(Light, nullptr);
	struct FResetProbes
	{
		~FResetProbes() { FStructuralTestLight::UnregisterProbe = {}; FStructuralTestLight::RegisterProbe = {}; }
	} Reset;
	int Calls = 0;
	auto MarkWitness = [&](FStructuralTestLight&) { ++Calls; Witness->MarkPackageDirty(); };
	FStructuralTestLight::UnregisterProbe = MarkWitness;
	FStructuralTestLight::RegisterProbe = MarkWitness;
	Other->ClearDirty();
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, Light, {false}));
	EXPECT_TRUE(Other->IsDirty());
	Other->ClearDirty();
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Other->IsDirty());
	int Completions = 0;
	const auto Completion = F.Transactions->OnRedo().AddLambda([&](const auto&, bool) { ++Completions; Witness->MarkPackageDirty(); });
	EXPECT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(Other->IsDirty());
	EXPECT_EQ(Calls, 3);

	EXPECT_FALSE(F.Transactions->Redo());
	F.Transactions->OnRedo().Remove(Completion);
	EXPECT_EQ(Completions, 2);
	EXPECT_TRUE(Other->IsDirty());
}

TEST(FComponentMutationTests, StructuralEditsRestorePackageContentsAndDirtyState)
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
	F.Transactions->Reset();
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
	std::string Reason;
	const auto Rejected = Editor->SpawnActor(*Level, AActor::StaticClass(), "Unpublished", {}, {false}, [&](AActor& Actor) {
		Prepared = &Actor;
		FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), true);
	}, ESpawnActorNameMode::Requested, {}, &Reason);
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions.Get(), false);
	EXPECT_FALSE(Rejected);
	EXPECT_EQ(Reason, "Editor history is unavailable for recording.");
	EXPECT_FALSE(IsValid(Prepared));
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
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::PostLoadProbe = {}; FStructuralTestLight::RegisterProbe = {}; } } Reset;
	int Observed = 0;
	FStructuralTestLight::PostLoadProbe = [&](FStructuralTestLight&) {
		++Observed;
		EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachChildren().size(), ChildCount);
	};
	int Registrations = 0;
	FStructuralTestLight::RegisterProbe = [&](FStructuralTestLight& Component) {
		++Registrations;
		EXPECT_EQ(Component.GetOwner()->GetAttachParentActor(), nullptr);
		EXPECT_TRUE(Level->ContainsActor(Component.GetOwner()));
	};
	AActor* Target = Source;
	const auto Copied = F.Editor.GetActorEditor()->DuplicateActors(*Level, {&Target, 1}, {false});
	FStructuralTestLight::RegisterProbe = {};
	ASSERT_FALSE(Copied.empty());
	EXPECT_EQ(Registrations, 1);
	EXPECT_EQ(Observed, 1);
	EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachChildren().size(), ChildCount + 1);
	EXPECT_EQ(Copied.front()->GetAttachParentActor(), F.Actor);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(F.Actor->GetRootComponent()->GetAttachChildren().size(), ChildCount);
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Copied.front()->GetAttachParentActor(), F.Actor);
}

TEST(FActorMutationTests, DuplicateReturnsPublishedPrefixAndOneUndoRestoresTheLevel)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* First = Level->SpawnActor<AStaticMeshActor>("BatchFirst");
	auto* Second = Level->SpawnActor<AStaticMeshActor>("BatchSecond");
	ASSERT_TRUE(First->AttachToActor(F.Actor, EAttachmentTransformRule::KeepRelative));
	ASSERT_TRUE(First->AddInstanceComponent(FStructuralTestLight::StaticClass(), "Probe"));
	const auto Before = Level->GetActors().size();
	bool RejectedSecond = false;
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::RegisterProbe = {}; } } Reset;
	FStructuralTestLight::RegisterProbe = [&](FStructuralTestLight& Component) {
		EXPECT_TRUE(Level->ContainsActor(Component.GetOwner()));
		EXPECT_EQ(Component.GetOwner()->GetAttachParentActor(), nullptr);
		for (auto* Inner : GDObjectArray.GetObjectsWithOuter(Level, EObjectQueryScope::LiveOnly))
			if (auto* Candidate = Cast<AActor>(Inner); Candidate && !Level->ContainsActor(Candidate))
			{
				MarkObjectHierarchyAsGarbage(Candidate);
				RejectedSecond = true;
			}
	};
	AActor* Sources[] = {First, Second};
	const auto Copies = F.Editor.GetActorEditor()->DuplicateActors(*Level, Sources, {false});
	FStructuralTestLight::RegisterProbe = {};
	ASSERT_TRUE(RejectedSecond);
	ASSERT_EQ(Copies.size(), 1u);
	EXPECT_NE(Copies.front().Get(), First);
	EXPECT_EQ(Copies.front()->GetAttachParentActor(), F.Actor);
	EXPECT_EQ(Level->GetActors().size(), Before + 1);
	EXPECT_TRUE(Level->ContainsActor(Copies.front().Get()));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Level->GetActors().size(), Before);
	EXPECT_FALSE(Level->ContainsActor(Copies.front().Get()));
	EXPECT_TRUE(Level->ContainsActor(First));
	EXPECT_TRUE(Level->ContainsActor(Second));
	EXPECT_FALSE(F.Transactions->CanUndo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Level->ContainsActor(Copies.front().Get()));
	EXPECT_EQ(Level->GetActors().size(), Before + 1);
}

TEST(FActorMutationTests, ParentCopyRetiredDuringPreparationStillAllowsChildDuplication)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Parent = Level->SpawnActor<AStaticMeshActor>("SourceParent");
	auto* Child = Level->SpawnActor<AStaticMeshActor>("SourceChild");
	ASSERT_TRUE(Child->AttachToActor(Parent));
	ASSERT_TRUE(Parent->AddInstanceComponent(FStructuralTestLight::StaticClass(), "RetireCopy"));
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::PostLoadProbe = {}; } } Reset;
	int Calls = 0;
	FStructuralTestLight::PostLoadProbe = [&](FStructuralTestLight& Component) {
		auto* Copy = Cast<AActor>(Component.GetOuter());
		++Calls;
		if (Copy && Copy != Parent) MarkObjectHierarchyAsGarbage(Copy);
	};
	const auto Immediate = DuplicateObject(Parent, Level, "RetiredDuringPostLoad");
	ASSERT_FALSE(Immediate);
	EXPECT_EQ(Immediate.error().Code, EObjectGraphError::InvalidRoot);
	ASSERT_EQ(Calls, 1);
	Calls = 0;
	AActor* Sources[] = {Child, Parent};
	const auto Copies = F.Editor.GetActorEditor()->DuplicateActors(*Level, Sources, {false});
	FStructuralTestLight::PostLoadProbe = {};
	ASSERT_EQ(Calls, 1);
	ASSERT_EQ(Copies.size(), 1u);
	EXPECT_EQ(Copies.front()->GetAttachParentActor(), Parent);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(Level->ContainsActor(Copies.front().Get()));
	EXPECT_FALSE(F.Transactions->CanUndo());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(Copies.front()->GetAttachParentActor(), Parent);
}

TEST(FActorMutationTests, ChildFirstDuplicationResolvesPublishedParentAfterRegistration)
{
	enum class EParent { Copied, CopiedWithoutMount, Existing, Missing };
	for (auto Scenario : {EParent::Copied, EParent::CopiedWithoutMount, EParent::Existing, EParent::Missing})
	{
		SCOPED_TRACE(static_cast<int>(Scenario));
		const bool bRejectParent = Scenario == EParent::Existing || Scenario == EParent::Missing;
		FComponentFixture F;
		auto* Level = F.World->GetCurrentLevel();
		auto* Parent = Level->SpawnActor<AStaticMeshActor>("Parent");
		auto* Child = Level->SpawnActor<AStaticMeshActor>("Child");
		auto* Driver = Level->SpawnActor<AStaticMeshActor>("Driver");
		auto* Mount = Cast<DSceneComponent>(Parent->AddInstanceComponent(DSceneComponent::StaticClass(), "Mount"));
		ASSERT_TRUE(Mount);
		Mount->SetWorldLocation({12, 20, 30});
		ASSERT_TRUE(Child->GetRootComponent()->AttachToComponent(Mount, EAttachmentTransformRule::KeepRelative));
		const auto ChildWorld = Child->GetRootComponent()->GetWorldLocation();
		ASSERT_TRUE(Child->AddInstanceComponent(FStructuralTestLight::StaticClass(), "ChildProbe"));
		ASSERT_TRUE(Driver->AddInstanceComponent(FStructuralTestLight::StaticClass(), "DriverProbe"));
		int ChildRegistrations = 0;
		struct FResetProbe { ~FResetProbe() { FStructuralTestLight::RegisterProbe = {}; } } Reset;
		FStructuralTestLight::RegisterProbe = [&](FStructuralTestLight& Probe) {
			if (Probe.GetFName() == FName("ChildProbe"))
			{
				++ChildRegistrations;
				EXPECT_EQ(Probe.GetOwner()->GetAttachParentActor(), nullptr);
				EXPECT_EQ(Probe.GetOwner()->GetRootComponent()->GetWorldLocation(), ChildWorld);
			}
			else
			{
				if (Scenario == EParent::Missing) Parent->Rename("RenamedParent");
				for (auto* Inner : GDObjectArray.GetObjectsWithOuter(Level, EObjectQueryScope::LiveOnly))
				{
					auto* Candidate = Cast<AActor>(Inner);
					if (!Candidate || Level->ContainsActor(Candidate)) continue;
					if (bRejectParent) MarkObjectHierarchyAsGarbage(Candidate);
					else if (Scenario == EParent::CopiedWithoutMount)
						for (const auto& Component : Candidate->GetComponentsSnapshot())
							if (Component->GetFName() == Mount->GetFName()) Component->DestroyComponent();
				}
			}
		};
		AActor* Sources[] = {Child, Driver, Parent};
		const auto Copies = F.Editor.GetActorEditor()->DuplicateActors(*Level, Sources, {false});
		FStructuralTestLight::RegisterProbe = {};
		ASSERT_EQ(Copies.size(), bRejectParent ? 2u : 3u);
		EXPECT_EQ(ChildRegistrations, 1);
		auto* ExpectedParent = Scenario == EParent::Missing ? nullptr : bRejectParent ? Parent : Copies.back().Get();
		auto CheckAttachment = [&] {
			EXPECT_EQ(Copies.front()->GetAttachParentActor(), ExpectedParent);
			EXPECT_EQ(Copies.front()->GetRootComponent()->GetWorldLocation(), ChildWorld);
			EXPECT_EQ(Copies.front()->GetRootComponent()->GetAttachParent(), ExpectedParent ? ExpectedParent->GetRootComponent() : nullptr);
		};
		CheckAttachment();
		ASSERT_TRUE(F.Transactions->Undo());
		for (const auto& Copy : Copies) EXPECT_FALSE(Level->ContainsActor(Copy.Get()));
		ASSERT_TRUE(F.Transactions->Redo());
		CheckAttachment();
	}
}

TEST(FActorMutationTests, UnavailableServiceAndBusyHistoryRejectBeforePreparation)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	TStrongObjectPtr<DEditorActorSubsystem> Unavailable(NewObject<DEditorActorSubsystem>(nullptr, "UninitializedActorService"));
	EXPECT_FALSE(Unavailable->SpawnActor(*Level, AActor::StaticClass(), "Unavailable", {}, {false}));
	FTransBufferTestAccess::SetHistoryBusy(*F.Transactions, true);
	auto* Editor = F.Editor.GetActorEditor();
	EXPECT_FALSE(Editor->SpawnActor(*Level, AActor::StaticClass(), "FinalizationBlocked", {}, {false}));
	EXPECT_EQ(Level->FindActorByName("FinalizationBlocked"), nullptr);
}

TEST(FActorMutationTests, OrdinaryRenameSurvivesDurinEdCustomCommandRetirement)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Editor = F.Editor.GetActorEditor();
	ASSERT_TRUE(Editor->RenameActor(*Level, F.Actor, "ModuleOwnedActor", {false}));
	auto* Added = Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_NE(Added, nullptr);
	const auto UndoId = F.Transactions->GetUndoContext().TransactionId;
	EXPECT_TRUE(F.Transactions->DiscardCustomChangesByModule("LevelEditor"));
	EXPECT_EQ(F.Transactions->GetUndoContext().TransactionId, UndoId);
	ASSERT_TRUE(F.Transactions->DiscardCustomChangesByModule("DurinEd"));
	EXPECT_EQ(F.Transactions->GetUndoContext().TransactionId, UndoId);
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_FALSE(F.Actor->OwnsComponent(Added));
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(F.Actor->GetName(), "Owner");
}

TEST(FComponentMutationTests, DetachedOwnerRestoresComponentOwnershipWithoutActivation)
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
		const auto Name = Child->GetFName();
		const auto Count = F.Actor->GetComponents().size();
		ASSERT_TRUE(Level->RemoveActor(F.Actor));
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_EQ(Child->GetFName(), FName("Child"));
		EXPECT_EQ(F.Actor->GetComponents().size(), Operation == 0 ? Count - 1 : Operation == 1 ? Count + 1 : Count);
		EXPECT_TRUE(F.Actor->OwnsComponent(Child));
		EXPECT_FALSE(Child->IsRegistered());
		EXPECT_FALSE(Level->ContainsActor(F.Actor));
		if (Operation == 3) EXPECT_EQ(Child->GetAttachParent(), F.Actor->GetRootComponent());
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_EQ(F.Actor->GetComponents().size(), Count);
		EXPECT_EQ(F.Actor->OwnsComponent(Child), Operation != 1);
		EXPECT_EQ(Child->GetFName(), Name);
		EXPECT_FALSE(Child->IsRegistered());
		EXPECT_FALSE(Level->ContainsActor(F.Actor));
	}
}

TEST(FComponentMutationTests, CapturedLevelAndDetachedOwnerRestoreTheirOwnSnapshots)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Camera = Level->SpawnActor<ACameraActor>("Camera");
	ASSERT_NE(Camera, nullptr);
	ASSERT_TRUE(Level->SetPrimaryCameraActor(Camera));
	{
		FScopedTransaction Scope("Edit component and detached level");
		ASSERT_TRUE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false}));
		ASSERT_TRUE(Level->RemoveActor(F.Actor));
		Level->SetFlags(EObjectFlags::Transactional);
		ASSERT_TRUE(Level->Modify(false));
		ASSERT_TRUE(Level->SetPrimaryCameraActor(nullptr));
	}
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Level->GetPrimaryCameraActor(), Camera);
	EXPECT_FALSE(Level->ContainsActor(F.Actor));
}

TEST(FComponentMutationTests, AdditionHistoryRetainsRecordedComponentAndReleasesOuterOnlyChild)
{
	FComponentFixture F;
	auto* Editor = F.Editor.GetActorEditor();
	const auto Added = Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
	ASSERT_TRUE(Added);
	EXPECT_EQ(Added->GetCreationMethod(), EComponentCreationMethod::Instance);
	auto* Payload = NewObject<DObject>(Added, "LatePayload");
	TWeakObjectPtr<DObject> WeakPayload(Payload);
	ASSERT_TRUE(F.Transactions->Undo());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(WeakPayload.IsValid());
	EXPECT_EQ(Added->GetCreationMethod(), EComponentCreationMethod::Instance);
	EXPECT_FALSE(F.Actor->OwnsComponent(Added));
	ASSERT_TRUE(F.Transactions->Redo());
	ASSERT_TRUE(F.Transactions->Undo());
	F.Transactions->Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(WeakPayload.IsValid());
}

TEST(FComponentMutationTests, SnapshotRetainsClearedAssetReferenceButNotItsOuterOnlySibling)
{
	FComponentFixture F;
	auto* Component = Cast<DStaticMeshComponent>(F.Actor->AddInstanceComponent(DStaticMeshComponent::StaticClass(), "MeshComponent"));
	ASSERT_NE(Component, nullptr);
	TWeakObjectPtr<DStaticMesh> Mesh(NewObject<DStaticMesh>(Component, "RecordedMesh"));
	TWeakObjectPtr<DObject> Sibling(NewObject<DObject>(Component, "OuterOnly"));
	Component->SetStaticMesh(Mesh.Get());
	Component->SetFlags(EObjectFlags::Transactional);
	{
		FScopedTransaction Scope("Clear mesh");
		ASSERT_TRUE(Component->Modify(false));
		Component->SetStaticMesh(nullptr);
	}
	CollectGarbage(EObjectFlags::Standalone);
	ASSERT_TRUE(Mesh.IsValid());
	EXPECT_FALSE(Sibling.IsValid());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_EQ(Component->GetStaticMesh(), Mesh.Get());
	ASSERT_TRUE(F.Transactions->Redo());
	F.Transactions->Reset();
	CollectGarbage(EObjectFlags::Standalone);
	EXPECT_FALSE(Mesh.IsValid());
}

TEST(FComponentMutationTests, DeletionSnapshotRetainsInstanceUntilHistoryIsDiscarded)
{
	FComponentFixture F;
	auto* Component = F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "TransientInstance");
	ASSERT_NE(Component, nullptr);
	Component->SetTransient(true);
	TWeakObjectPtr<DActorComponent> Recorded(Component);
	TWeakObjectPtr<DObject> Payload(NewObject<DObject>(Component, "Payload"));
	ASSERT_TRUE(F.Editor.GetActorEditor()->DeleteComponent(*F.Actor, Component, {false}));
	CollectGarbage(EObjectFlags::Standalone);
	ASSERT_FALSE(Payload.IsValid());
	ASSERT_TRUE(Recorded.IsValid());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(F.Actor->IsInstanceComponent(Component));
	EXPECT_TRUE(Component->HasAnyObjectFlags(EObjectFlags::Transient));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_FALSE(F.Actor->OwnsComponent(Component));
	F.Transactions->Reset();
	CollectGarbage(EObjectFlags::Standalone);
	EXPECT_FALSE(Recorded.IsValid());
}

TEST(FComponentMutationTests, CreationCallbackSharesTheMembershipSnapshotTransaction)
{
	FComponentFixture F;
	auto* Witness = F.World->GetCurrentLevel()->SpawnActor<AStaticMeshActor>("NestedCapture");
	ASSERT_TRUE(Witness);
	Witness->SetFlags(EObjectFlags::Transactional);
	struct FResetProbes
	{
		~FResetProbes() { FStructuralTestLight::CreationProbe = {}; FStructuralTestLight::UnregisterProbe = {}; }
	} Reset;
	FStructuralTestLight::CreationProbe = [&](FStructuralTestLight&) {
		ASSERT_TRUE(Witness->Modify(false));
		Witness->SetHidden(true);
	};
	ASSERT_TRUE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false}));
	bool SawRemoval = false, SawCreation = false;
	FStructuralTestLight::UnregisterProbe = [&](FStructuralTestLight&) { SawRemoval = true; EXPECT_FALSE(Witness->IsHidden()); };
	FStructuralTestLight::CreationProbe = [&](FStructuralTestLight&) { SawCreation = true; EXPECT_TRUE(Witness->IsHidden()); };
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(SawRemoval);
	EXPECT_FALSE(Witness->IsHidden());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(SawCreation);
	EXPECT_TRUE(Witness->IsHidden());
}

TEST(FComponentMutationTests, CanceledCreationUnwindsBeforeReplacementRecording)
{
	FComponentFixture F;
	auto* Buffer = F.Transactions.Get();
	auto* Witness = F.World->GetCurrentLevel()->SpawnActor<AStaticMeshActor>("ReplacementWitness");
	ASSERT_TRUE(Witness);
	Witness->SetFlags(EObjectFlags::Transactional);
	{
		Durin::FScopedTransaction Before("Prior edit");
		ASSERT_TRUE(Witness->Modify(false));
		Witness->SetHidden(true);
	}
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Buffer->CanRedo());
	Durin::FScopedTransaction Outer("Canceled");
	const auto Original = Durin::GUndo->GetContext().TransactionId;
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::CreationProbe = {}; } } Reset;
	FStructuralTestLight::CreationProbe = [&](FStructuralTestLight&) { Outer.Cancel(); };
	auto Count = F.Actor->GetComponents().size();
	EXPECT_TRUE(F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false}));
	EXPECT_EQ(F.Actor->GetComponents().size(), Count + 1);
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_TRUE(Buffer->CanRedo());
	ASSERT_EQ(Buffer->Begin("test", "Replacement"), 0);
	const auto Replacement = Durin::GUndo->GetContext().TransactionId;
	EXPECT_NE(Replacement, Original);
	ASSERT_TRUE(Witness->Modify(false));
	Witness->SetHidden(true);
	Buffer->End();
	ASSERT_EQ(Buffer->GetUndoContext().TransactionId, Replacement);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_FALSE(Witness->IsHidden());
	EXPECT_EQ(F.Actor->GetComponents().size(), Count + 1);
}

TEST(FComponentMutationTests, ComponentDestroyedDuringCreationReturnsAbsent)
{
	FComponentFixture F;
	const auto Count = F.Actor->GetComponents().size();
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::CreationProbe = {}; } } Reset;
	bool Removed = false;
	FStructuralTestLight::CreationProbe = [&](FStructuralTestLight& Component) {
		Removed = Component.GetOwner()->DestroyInstanceComponent(&Component);
	};
	EXPECT_EQ(F.Editor.GetActorEditor()->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false}), nullptr);
	EXPECT_TRUE(Removed);
	EXPECT_EQ(F.Actor->GetComponents().size(), Count);
	EXPECT_FALSE(F.Transactions->IsActive());
}

TEST(FActorMutationTests, RootlessActorSnapshotNotifiesOutlinerAndReleasesOuterOnlyPayload)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	Tests::FActorHierarchyChangeObserver Hierarchy(*Level);
	auto* Added = F.Editor.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), "Rootless", {}, {false});
	ASSERT_NE(Added, nullptr);
	ASSERT_EQ(Added->GetRootComponent(), nullptr);
	TWeakObjectPtr<DObject> Payload(NewObject<DObject>(Added, "LateActorPayload"));
	EXPECT_TRUE(Hierarchy.Consume());
	ASSERT_TRUE(F.Transactions->Undo());
	EXPECT_TRUE(Hierarchy.Consume());
	EXPECT_FALSE(Level->ContainsActor(Added));
	CollectGarbage(EObjectFlags::Standalone);
	ASSERT_FALSE(Payload.IsValid());
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_TRUE(Hierarchy.Consume());
	EXPECT_EQ(Level->FindActorByName("Rootless"), Added);
	ASSERT_TRUE(F.Transactions->Undo());
	F.Transactions->Reset();
	CollectGarbage(EObjectFlags::Standalone);
	EXPECT_FALSE(Payload.IsValid());
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
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_TRUE(IsValid(Actor));
	ASSERT_TRUE(F.Transactions->Redo());
	EXPECT_EQ(F.World->GetCurrentLevel()->FindActorByName("Oversized"), Actor);
}

TEST(FActorMutationTests, WorldClosingDuringActorRemovalCannotReportSuccess)
{
	FComponentFixture F;
	auto* Setup = F.Editor.GetActorEditor();
	ASSERT_TRUE(Setup->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false}));
	F.Transactions->Reset();
	auto* RecoveryHistory = F.Transactions.Get();
	auto* Editor = F.Editor.GetActorEditor();
	struct FResetProbe { ~FResetProbe() { FStructuralTestLight::UnregisterProbe = {}; } } Reset;
	FStructuralTestLight::UnregisterProbe = [&](FStructuralTestLight&) { F.World->BeginTearingDown(); };
	auto* Actor = F.Actor;
	const auto Result = Editor->DestroyActors(*F.World->GetCurrentLevel(), {&Actor, 1}, {false});
	EXPECT_FALSE(Result);
	EXPECT_TRUE(RecoveryHistory->CanTransact());
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
    CollectGarbage(Durin::EObjectFlags::Standalone);
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
    const auto Head = F.GetTransactions()->GetUndoContext().TransactionId;
    ASSERT_TRUE(Service->SetActorLabel(*Level, First, "Visible", {false}));
    EXPECT_EQ(F.GetTransactions()->GetUndoContext().TransactionId, Head);
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
    ASSERT_TRUE(Level->AddActor(Legacy));
    Durin::Tests::FActorHierarchyChangeObserver Changes(*Level);
    ASSERT_TRUE(F.GetActorEditor()->SetActorLabel(*Level, Legacy, "Legacy label", {false}));
    EXPECT_TRUE(Changes.Consume());
    ASSERT_TRUE(F.GetTransactions()->Undo());
    ASSERT_TRUE(F.GetActorEditor()->RenameActor(*Level, Legacy, "NewInternal", {false}));
    EXPECT_EQ(Legacy->GetActorLabel(), "Legacy");
    ASSERT_TRUE(F.GetTransactions()->Undo());
    EXPECT_EQ(Legacy->GetName(), "Legacy");
    EXPECT_EQ(Legacy->GetActorLabel(), "Legacy");
}

TEST(FActorMutationTests, LabelAndWholeObjectModifyComposeInEitherOrderWithoutCustomHistory)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    auto* Actor = Level->SpawnActor<AActor>("Original");
    ASSERT_NE(Actor, nullptr);
    {
        Durin::FScopedTransaction Scope("test", "Modify and label", nullptr);
        ASSERT_TRUE(F.GetActorEditor()->SetActorLabel(*Level, Actor, "Display", {false}));
        ASSERT_TRUE(Actor->Modify());
        Actor->SetHidden(true);
    }
    ASSERT_TRUE(F.GetTransactions()->Undo());
    EXPECT_FALSE(Actor->IsHidden());
    EXPECT_EQ(Actor->GetActorLabel(), "Original");
    ASSERT_TRUE(F.GetTransactions()->Redo());
    EXPECT_TRUE(Actor->IsHidden());
    EXPECT_EQ(Actor->GetActorLabel(), "Display");
    EXPECT_EQ(Actor->GetName(), "Original");
    ASSERT_EQ(F.GetTransactions()->GetQueueLength(), 1u);
    EXPECT_EQ(F.GetTransactions()->GetTransaction(0)->GetRecordCount(), 1u);
    EXPECT_FALSE(F.GetTransactions()->GetTransaction(0)->IsOwnedByModule("DurinEd"));
    {
        Durin::FScopedTransaction Scope("test", "Modify then label", nullptr);
        ASSERT_TRUE(Actor->Modify()); Actor->SetHidden(false);
        ASSERT_TRUE(F.GetActorEditor()->SetActorLabel(*Level, Actor, "Next", {false}));
    }
    ASSERT_TRUE(F.GetTransactions()->Undo()); EXPECT_TRUE(Actor->IsHidden()); EXPECT_EQ(Actor->GetActorLabel(), "Display");
    ASSERT_TRUE(F.GetTransactions()->Redo()); EXPECT_FALSE(Actor->IsHidden()); EXPECT_EQ(Actor->GetActorLabel(), "Next");
}

TEST(FActorMutationTests, OrdinaryNameLabelAndTagsShareOneRecordAfterStructuralCreation)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    AActor* Actor = nullptr;
    {
        Durin::FScopedTransaction Scope("test", "Create and edit", nullptr);
        Actor = F.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), "Created", {}, {false}); ASSERT_NE(Actor, nullptr);
        ASSERT_TRUE(F.GetActorEditor()->SetActorLabel(*Level, Actor, "Display", {false}));
        ASSERT_TRUE(F.GetActorEditor()->RenameActor(*Level, Actor, "Internal", {false}));
        auto* Tags = Actor->GetClass()->FindPropertyByName("Tags"); ASSERT_NE(Tags, nullptr);
        static_cast<std::vector<FName>*>(Tags->GetValuePtr(Actor))->push_back("PersistentMarker");
    }
    EXPECT_EQ(F.GetTransactions()->GetTransaction(0)->GetRecordCount(), 2u);
    ASSERT_TRUE(F.GetTransactions()->Undo()); EXPECT_FALSE(Level->ContainsActor(Actor));
    ASSERT_TRUE(Actor->SetActorLabel("Outside history"));
    for (int32 Index = 0; Index < 3; ++Index)
    {
        ASSERT_TRUE(F.GetTransactions()->Redo()); EXPECT_TRUE(Level->ContainsActor(Actor));
        EXPECT_EQ(Actor->GetName(), "Internal"); EXPECT_EQ(Actor->GetActorLabel(), "Display");
        auto* Tags = Actor->GetClass()->FindPropertyByName("Tags");
        EXPECT_EQ(*static_cast<std::vector<FName>*>(Tags->GetValuePtr(Actor)), std::vector<FName>{"PersistentMarker"});
        ASSERT_TRUE(F.GetTransactions()->Undo()); EXPECT_FALSE(Level->ContainsActor(Actor));
    }
}

TEST(FActorMutationTests, LabelAndModifyComposeWithLaterComponentPublication)
{
	FComponentFixture F;
	auto* Editor = F.Editor.GetActorEditor();
	auto* Level = F.World->GetCurrentLevel();
	DActorComponent* Added = nullptr;
	{
		Durin::FScopedTransaction Scope("test", "Label then component", nullptr);
		ASSERT_TRUE(Editor->SetActorLabel(*Level, F.Actor, "Display", {false}));
		ASSERT_TRUE(F.Actor->Modify());
		F.Actor->SetHidden(true);
		Added = Editor->AddComponent(*F.Actor, FStructuralTestLight::StaticClass(), nullptr, {false});
		ASSERT_NE(Added, nullptr);
		EXPECT_FALSE(Added->GetFName().IsNone());
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_EQ(F.Actor->GetActorLabel(), "Owner");
		EXPECT_FALSE(F.Actor->IsHidden());
		EXPECT_FALSE(F.Actor->OwnsComponent(Added));
		EXPECT_FALSE(Added->IsRegistered());
		CollectGarbage(Durin::EObjectFlags::Standalone);
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_EQ(F.Actor->GetActorLabel(), "Display");
		EXPECT_TRUE(F.Actor->IsHidden());
		EXPECT_TRUE(F.Actor->OwnsComponent(Added));
		EXPECT_TRUE(Added->IsRegistered());
		EXPECT_EQ(std::ranges::count(F.Actor->GetInstanceComponents(), TObjectPtr<DActorComponent>(Added)), 1);
	}
}

TEST(FActorMutationTests, OrdinaryFieldsAndMultipleMembershipTransitionsShareOneScope)
{
	FComponentFixture F;
	auto* Editor = F.Editor.GetActorEditor();
	DActorComponent* First = nullptr;
	DActorComponent* Last = nullptr;
	{
		Durin::FScopedTransaction Scope("test", "Fields and membership", nullptr);
		ASSERT_TRUE(Editor->SetActorLabel(*F.World->GetCurrentLevel(), F.Actor, "Display", {false}));
		First = Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
		Last = Editor->AddComponent(*F.Actor, DSceneComponent::StaticClass(), nullptr, {false});
		ASSERT_NE(First, nullptr);
		ASSERT_NE(Last, nullptr);
		ASSERT_TRUE(Editor->DeleteComponent(*F.Actor, First, {false}));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_TRUE(F.Actor->GetInstanceComponents().empty());
		EXPECT_FALSE(F.Actor->OwnsComponent(First));
		EXPECT_FALSE(F.Actor->OwnsComponent(Last));
		CollectGarbage(Durin::EObjectFlags::Standalone);
		ASSERT_TRUE(F.Transactions->Redo());
		ASSERT_EQ(F.Actor->GetInstanceComponents().size(), 1u);
		EXPECT_EQ(F.Actor->GetInstanceComponents().front().Get(), Last);
		EXPECT_FALSE(F.Actor->OwnsComponent(First));
		EXPECT_TRUE(F.Actor->OwnsComponent(Last));
		EXPECT_TRUE(Last->IsRegistered());
	}
}

TEST(FComponentMutationTests, OrdinaryRenameAndLaterReparentShareOneScope)
{
	FComponentFixture F;
	auto* Moving = Cast<DSceneComponent>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Moving"));
	auto* Parent = Cast<DSceneComponent>(F.Actor->AddInstanceComponent(DSceneComponent::StaticClass(), "Parent"));
	auto* Before = Moving->GetAttachParent();
	{
		Durin::FScopedTransaction Scope("test", "Rename then reparent", nullptr);
		ASSERT_TRUE(F.Editor.GetActorEditor()->RenameComponent(*F.Actor, Moving, "Renamed", {false}));
		ASSERT_TRUE(F.Editor.GetActorEditor()->ReparentComponent(*F.Actor, Moving, Parent, {false}));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_EQ(Moving->GetName(), "Moving");
		EXPECT_EQ(Moving->GetAttachParent(), Before);
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_EQ(Moving->GetName(), "Renamed");
		EXPECT_EQ(Moving->GetAttachParent(), Parent);
		EXPECT_EQ(std::ranges::count(Parent->GetAttachChildren(), TObjectPtr<DSceneComponent>(Moving)), 1);
	}
}

TEST(FActorMutationTests, LabelRootModifyAndLaterAttachmentShareOneScope)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Parent = Level->SpawnActor<AStaticMeshActor>("Parent");
	const auto Before = F.Actor->GetActorTransform();
	{
		Durin::FScopedTransaction Scope("test", "Label then attachment", nullptr);
		ASSERT_TRUE(F.Editor.GetActorEditor()->SetActorLabel(*Level, F.Actor, "Display", {false}));
		std::array<AActor*, 1> Actors{F.Actor};
		ASSERT_TRUE(F.Editor.GetActorEditor()->AttachActors(*Level, Actors, Parent, EAttachmentTransformRule::KeepWorld, {false}));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_EQ(F.Actor->GetActorLabel(), "Owner");
		EXPECT_EQ(F.Actor->GetAttachParentActor(), nullptr);
		EXPECT_EQ(F.Actor->GetActorTransform().Translation, Before.Translation);
		EXPECT_EQ(F.Actor->GetActorTransform().Rotation, Before.Rotation);
		EXPECT_EQ(F.Actor->GetActorTransform().Scale3D, Before.Scale3D);
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_EQ(F.Actor->GetActorLabel(), "Display");
		EXPECT_EQ(F.Actor->GetAttachParentActor(), Parent);
		EXPECT_EQ(std::ranges::count(Parent->GetRootComponent()->GetAttachChildren(),
			TObjectPtr<DSceneComponent>(F.Actor->GetRootComponent())), 1);
	}
}

TEST(FActorMutationTests, LevelModifyPrimaryCameraAndActorPublicationShareOneScope)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* First = Level->SpawnActor<ACameraActor>("FirstCamera");
	auto* Second = Level->SpawnActor<ACameraActor>("SecondCamera");
	ASSERT_EQ(Level->GetPrimaryCameraActor(), First);
	AActor* Added = nullptr;
	{
		Durin::FScopedTransaction Scope("test", "Camera and publication", nullptr);
		Level->SetFlags(EObjectFlags::Transactional);
		ASSERT_TRUE(Level->Modify(false));
		Added = F.Editor.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), "Added", {}, {false});
		ASSERT_NE(Added, nullptr);
		ASSERT_TRUE(Level->SetPrimaryCameraActor(Second));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_FALSE(Level->ContainsActor(Added));
		EXPECT_EQ(Level->GetPrimaryCameraActor(), First);
		EXPECT_FALSE(Added->PrimaryActorTick.IsTickFunctionRegistered());
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_TRUE(Level->ContainsActor(Added));
		EXPECT_EQ(Level->GetPrimaryCameraActor(), Second);
		EXPECT_TRUE(Added->PrimaryActorTick.IsTickFunctionRegistered());
		EXPECT_EQ(std::ranges::count(Level->GetActors(), TObjectPtr<AActor>(Added)), 1);
	}
}

TEST(FActorMutationTests, LevelHistoryPreservesActorsAddedOutsideTheRecording)
{
	FComponentFixture F;
	auto* Level = F.World->GetCurrentLevel();
	auto* Added = F.Editor.GetActorEditor()->SpawnActor(*Level, AActor::StaticClass(), "Managed", {}, {false});
	ASSERT_NE(Added, nullptr);
	auto* External = Level->SpawnActor<AActor>("External");
	ASSERT_NE(External, nullptr);
	External->ClearFlags(EObjectFlags::Transactional);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		ASSERT_TRUE(F.Transactions->Undo());
		EXPECT_FALSE(Level->ContainsActor(Added));
		EXPECT_TRUE(Level->ContainsActor(External));
		EXPECT_TRUE(External->PrimaryActorTick.IsTickFunctionRegistered());
		ASSERT_TRUE(F.Transactions->Redo());
		EXPECT_TRUE(Level->ContainsActor(Added));
		EXPECT_TRUE(Level->ContainsActor(External));
		EXPECT_EQ(std::ranges::count(Level->GetActors(), TObjectPtr<AActor>(External)), 1);
	}
}

TEST(FActorMutationTests, InitialLabelIsPreparationOnlyAndRenameComposesWithEarlierModify)
{
    Durin::Tests::FActorEditorFixture F;
    auto* Level = F.GetWorld()->GetCurrentLevel();
    auto* Actor = Level->SpawnActor<AActor>("Original"); ASSERT_NE(Actor, nullptr);
    EXPECT_FALSE(Actor->SetInitialActorLabel("Rejected")); EXPECT_EQ(Actor->GetActorLabel(), "Original");
    Durin::Tests::FActorHierarchyChangeObserver Changes(*Level);
    {
        Durin::FScopedTransaction Scope("test", "Modify and rename", nullptr);
        ASSERT_TRUE(Actor->Modify()); Actor->SetHidden(true);
        ASSERT_TRUE(F.GetActorEditor()->RenameActor(*Level, Actor, "NewInternal", {false}));
    }
    EXPECT_EQ(F.GetTransactions()->GetTransaction(0)->GetRecordCount(), 1u);
    ASSERT_TRUE(F.GetTransactions()->Undo()); EXPECT_EQ(Actor->GetName(), "Original"); EXPECT_FALSE(Actor->IsHidden());
    EXPECT_TRUE(Changes.Consume());
    ASSERT_TRUE(F.GetTransactions()->Redo()); EXPECT_EQ(Actor->GetName(), "NewInternal"); EXPECT_TRUE(Actor->IsHidden());
    EXPECT_EQ(Actor->GetActorLabel(), "Original");
}
