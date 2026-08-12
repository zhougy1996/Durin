#include "WorldTestSupport.h"
#include "Actors/SplineMeshActor.h"
#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"

TEST(FWorldTests, StartsWithoutALevelAndActorOperationsAreSafe)
{
	Durin::DWorld* World = CreateEmptyWorld();
	EXPECT_EQ(World->GetCurrentLevel(), nullptr);
	EXPECT_EQ(World->SpawnActor<Durin::ACameraActor>("Camera"), nullptr);
	EXPECT_TRUE(World->GetActors().empty());
	EXPECT_FALSE(World->ContainsActor(nullptr));
	EXPECT_EQ(World->FindActorByName("Camera"), nullptr);
	EXPECT_FALSE(World->DestroyActor(nullptr));
	World->DestroyAllActors();
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTests, DuplicatesLevelForPlayWithoutDuplicatingExternalAssets)
{
	Durin::DWorld* EditorWorld = CreateWorld();
	Durin::AStaticMeshActor* SourceActor = EditorWorld->SpawnActor<Durin::AStaticMeshActor>("Mesh");
	ASSERT_NE(SourceActor, nullptr);
	Durin::DStaticMesh* SharedMesh = Durin::NewObject<Durin::DStaticMesh>(nullptr, "SharedMesh");
	SourceActor->GetStaticMeshComponent()->SetStaticMesh(SharedMesh);
	Durin::FTransform SourceTransform;
	SourceTransform.Translation = {1.0, 2.0, 3.0};
	SourceActor->SetActorTransform(SourceTransform);

	Durin::DWorld* PlayWorld = CreateEmptyWorld();
	std::string Error;
	auto* PlayLevel = Durin::Cast<Durin::DLevel>(Durin::DuplicateObjectGraph(EditorWorld->GetCurrentLevel(), PlayWorld, "PlayLevel", &Error));
	ASSERT_NE(PlayLevel, nullptr) << Error;
	ASSERT_TRUE(PlayWorld->SetCurrentLevel(PlayLevel));
	auto* PlayActor = Durin::Cast<Durin::AStaticMeshActor>(PlayLevel->FindActorByName("Mesh"));
	ASSERT_NE(PlayActor, nullptr);
	Durin::DStaticMeshComponent* PlayComponent = PlayActor->GetStaticMeshComponent();
	ASSERT_NE(PlayComponent, nullptr);
	EXPECT_NE(PlayActor, SourceActor);
	EXPECT_NE(PlayComponent, SourceActor->GetStaticMeshComponent());
	EXPECT_EQ(PlayComponent->GetStaticMesh(), SharedMesh);
	EXPECT_EQ(PlayActor->GetOuter(), PlayLevel);
	EXPECT_EQ(PlayLevel->GetOuter(), PlayWorld);
	EXPECT_EQ(PlayLevel->GetPackage(), nullptr);
	ExpectVectorNear(PlayActor->GetActorTransform().Translation, SourceTransform.Translation);

	PlayActor->GetRootComponent()->SetWorldLocation({9.0, 8.0, 7.0});
	ExpectVectorNear(SourceActor->GetActorTransform().Translation, SourceTransform.Translation);
	Durin::MarkObjectHierarchyAsGarbage(PlayWorld);
	EXPECT_FALSE(Durin::IsValid(PlayLevel));
	EXPECT_FALSE(Durin::IsValid(PlayActor));
	EXPECT_FALSE(Durin::IsValid(PlayComponent));
	EXPECT_TRUE(Durin::IsValid(SharedMesh));
	Durin::MarkObjectHierarchyAsGarbage(EditorWorld);
	Durin::MarkAsGarbage(SharedMesh);
	Durin::CollectGarbage();
}

TEST(FNativeConstructionPIETests, SplineMeshActorRegeneratesTransientSegmentsAndMutatesDuringPlay)
{
	Durin::DWorld* EditorWorld = CreateWorld();
	auto* SourceActor = EditorWorld->SpawnActor<Durin::ASplineMeshActor>("SplinePath");
	ASSERT_NE(SourceActor, nullptr);
	auto* SharedMesh = Durin::DStaticMesh::CreateDebugTriangle();
	SourceActor->SetPathMesh(SharedMesh);
	SourceActor->GetSplineComponent()->SetSplinePoints({
		Durin::FSplinePoint({0.0, 0.0, 0.0}),
		Durin::FSplinePoint({100.0, 0.0, 0.0}),
		Durin::FSplinePoint({200.0, 0.0, 0.0})});
	SourceActor->SetPathCollisionEnabled(true);
	const auto SourceSegments = SourceActor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	ASSERT_EQ(SourceSegments.size(), 2u);

	Durin::DWorld* PlayWorld = CreateEmptyWorld();
	PlayWorld->SetWorldType(Durin::EWorldType::PlayInEditor);
	std::unordered_map<Durin::DObject*, Durin::DObject*> EditorToPlay;
	std::string Error;
	auto* PlayLevel = Durin::Cast<Durin::DLevel>(Durin::DuplicateObjectGraph(
		EditorWorld->GetCurrentLevel(), PlayWorld, "PlayLevel", &Error, &EditorToPlay));
	ASSERT_NE(PlayLevel, nullptr) << Error;
	ASSERT_TRUE(PlayWorld->SetCurrentLevel(PlayLevel));
	auto* PlayActor = Durin::Cast<Durin::ASplineMeshActor>(PlayLevel->FindActorByName("SplinePath"));
	ASSERT_NE(PlayActor, nullptr);
	const auto PlaySegments = PlayActor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	ASSERT_EQ(PlaySegments.size(), 2u);
	EXPECT_EQ(PlayWorld->GetPhysicsScene().GetBodyCount(), 2u);
	for (Durin::DActorComponent* SourceSegment : SourceSegments)
		EXPECT_FALSE(EditorToPlay.contains(SourceSegment));
	for (Durin::DActorComponent* PlaySegment : PlaySegments)
	{
		EXPECT_EQ(PlaySegment->GetCreationMethod(), Durin::EComponentCreationMethod::Generated);
		EXPECT_TRUE(PlaySegment->IsRegistered());
	}
	ASSERT_TRUE(PlayWorld->BeginPlay({}));
	for (Durin::DActorComponent* PlaySegment : PlaySegments) EXPECT_TRUE(PlaySegment->HasBegunPlay());
	Durin::FSplinePoint Edited = *PlayActor->GetSplineComponent()->GetSplinePoint(1);
	Edited.Position.y = 35.0;
	ASSERT_TRUE(PlayActor->GetSplineComponent()->UpdateSplinePoint(1, Edited));
	const auto MutatedSegments = PlayActor->FindComponentsByClass<Durin::DSplineMeshComponent>();
	ASSERT_EQ(MutatedSegments.size(), 2u);
	EXPECT_EQ(PlayWorld->GetPhysicsScene().GetBodyCount(), 2u);
	for (Durin::DActorComponent* Segment : PlaySegments)
		EXPECT_NE(std::ranges::find(MutatedSegments, Segment), MutatedSegments.end());
	PlayWorld->EndPlay();
	for (Durin::DActorComponent* PlaySegment : PlaySegments) EXPECT_FALSE(PlaySegment->HasBegunPlay());
	ASSERT_TRUE(PlayWorld->SetCurrentLevel(nullptr, false));
	EXPECT_EQ(PlayWorld->GetPhysicsScene().GetBodyCount(), 0u);
	Durin::MarkObjectHierarchyAsGarbage(PlayWorld);
	Durin::MarkObjectHierarchyAsGarbage(EditorWorld);
	Durin::MarkAsGarbage(SharedMesh);
	Durin::CollectGarbage();
}

TEST(FWorldTests, AppliesOnlyEditableRuntimePropertiesBackToTheirEditorObjects)
{
	Durin::DWorld* EditorWorld = CreateWorld();
	Durin::ACameraActor* EditorActor = EditorWorld->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(EditorActor, nullptr);
	Durin::DActorComponent* OriginalOwnedComponent = EditorActor->GetOwnedComponents().front().Get();

	Durin::DWorld* PlayWorld = CreateEmptyWorld();
	std::unordered_map<Durin::DObject*, Durin::DObject*> EditorToPlay;
	std::string Error;
	auto* PlayLevel = Durin::Cast<Durin::DLevel>(Durin::DuplicateObjectGraph(EditorWorld->GetCurrentLevel(), PlayWorld, "PlayLevel", &Error, &EditorToPlay));
	ASSERT_NE(PlayLevel, nullptr) << Error;
	ASSERT_TRUE(PlayWorld->SetCurrentLevel(PlayLevel));
	auto* PlayActor = Durin::Cast<Durin::ACameraActor>(EditorToPlay.at(EditorActor));
	auto* PlayCamera = Durin::Cast<Durin::DCameraComponent>(EditorToPlay.at(EditorActor->GetCameraComponent()));
	ASSERT_NE(PlayActor, nullptr);
	ASSERT_NE(PlayCamera, nullptr);
	PlayActor->GetRootComponent()->SetWorldLocation({4.0, 5.0, 6.0});
	PlayCamera->SetFieldOfViewDegrees(92.0f);

	std::unordered_map<Durin::DObject*, Durin::DObject*> PlayToEditor;
	for (const auto& [EditorObject, PlayObject] : EditorToPlay) PlayToEditor.emplace(PlayObject, EditorObject);
	ASSERT_TRUE(Durin::CopyEditableObjectProperties(PlayCamera, EditorActor->GetCameraComponent(), PlayToEditor, &Error)) << Error;
	EditorActor->GetRootComponent()->UpdateComponentToWorld();
	ExpectVectorNear(EditorActor->GetActorTransform().Translation, {4.0, 5.0, 6.0});
	EXPECT_NEAR(EditorActor->GetCameraComponent()->GetFieldOfViewDegrees(), 92.0f, 1.e-6f);
	ASSERT_EQ(EditorActor->GetOwnedComponents().size(), 1u);
	EXPECT_EQ(EditorActor->GetOwnedComponents().front().Get(), OriginalOwnedComponent);

	Durin::MarkObjectHierarchyAsGarbage(EditorWorld);
	Durin::MarkObjectHierarchyAsGarbage(PlayWorld);
	Durin::CollectGarbage();
}

TEST(FWorldTests, TransientLevelMustBeReparentedBeforeCrossingWorldLifetime)
{
	Durin::DWorld* FirstWorld = CreateEmptyWorld();
	Durin::DWorld* SecondWorld = CreateEmptyWorld();
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(FirstWorld, "TransferredLevel");
	ASSERT_TRUE(FirstWorld->SetCurrentLevel(Level, false));
	ASSERT_TRUE(FirstWorld->SetCurrentLevel(nullptr, false));

	EXPECT_FALSE(SecondWorld->SetCurrentLevel(Level, false));
	Level->SetOuterPrivate(SecondWorld);
	EXPECT_TRUE(SecondWorld->SetCurrentLevel(Level, false));

	Durin::MarkObjectHierarchyAsGarbage(FirstWorld);
	EXPECT_TRUE(Durin::IsValid(Level));
	Durin::MarkObjectHierarchyAsGarbage(SecondWorld);
	EXPECT_FALSE(Durin::IsValid(Level));
	Durin::CollectGarbage();
}

TEST(FWorldTests, SimulatesPhysicsComponentsAndHonorsTheWorldToggle)
{
	Durin::DWorld* World = CreateWorld();
	Durin::AStaticMeshActor* Actor = World->SpawnActor<Durin::AStaticMeshActor>("FallingMesh");
	ASSERT_NE(Actor, nullptr);
	auto* Physics = Durin::Cast<Durin::DPhysicsComponent>(Actor->AddInstanceComponent(Durin::DPhysicsComponent::StaticClass(), "Physics"));
	ASSERT_NE(Physics, nullptr);
	Actor->GetRootComponent()->SetWorldLocation({0.0, 0.0, 10.0});
	World->BeginPlay({});
	World->Tick({.DeltaSeconds = 0.5f});
	EXPECT_LT(Actor->GetActorTransform().Translation.z, 10.0);
	EXPECT_LT(Physics->GetLinearVelocity().z, 0.0);

	World->SetPhysicsSimulationEnabled(false);
	const Durin::FVector3 PausedLocation = Actor->GetActorTransform().Translation;
	World->Tick({.DeltaSeconds = 0.5f});
	ExpectVectorNear(Actor->GetActorTransform().Translation, PausedLocation);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTests, RoutesPlayLifecycleThroughActorsAndComponents)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Actor = World->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(Actor, nullptr);
	ASSERT_FALSE(Actor->HasBegunPlay());
	ASSERT_FALSE(Actor->GetCameraComponent()->HasBegunPlay());

	World->BeginPlay({});
	EXPECT_TRUE(World->HasBegunPlay());
	EXPECT_TRUE(Actor->HasBegunPlay());
	EXPECT_TRUE(Actor->GetCameraComponent()->HasBegunPlay());
	World->SetPaused(true);
	World->RequestSingleStep();
	World->Tick({.DeltaSeconds = 1.0f / 60.0f});

	World->EndPlay();
	EXPECT_FALSE(World->HasBegunPlay());
	EXPECT_FALSE(Actor->HasBegunPlay());
	EXPECT_FALSE(Actor->GetCameraComponent()->HasBegunPlay());
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FWorldTests, SpawnsEnumeratesAndFindsActors)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* Camera = World->SpawnActor<Durin::ACameraActor>("Camera");
	Durin::AStaticMeshActor* Mesh = World->SpawnActor<Durin::AStaticMeshActor>("Mesh");

	ASSERT_NE(Camera, nullptr);
	ASSERT_NE(Mesh, nullptr);
	ASSERT_EQ(Camera->GetClass(), Durin::ACameraActor::StaticClass());
	ASSERT_EQ(Mesh->GetClass(), Durin::AStaticMeshActor::StaticClass());
	ASSERT_FALSE(Camera->GetClass()->GetFName().IsNone());
	ASSERT_FALSE(Mesh->GetClass()->GetFName().IsNone());
	EXPECT_EQ(Camera->GetClass()->GetName(), "Durin::ACameraActor");
	EXPECT_EQ(Mesh->GetClass()->GetName(), "Durin::AStaticMeshActor");
	EXPECT_EQ(World->GetActors().size(), 2u);
	EXPECT_TRUE(World->ContainsActor(Camera));
	EXPECT_EQ(World->FindActorByName("Camera"), Camera);
	EXPECT_EQ(Camera->GetOuter(), World->GetCurrentLevel());
	EXPECT_EQ(Mesh->GetOuter(), World->GetCurrentLevel());
	EXPECT_EQ(Camera->GetCameraComponent()->GetOuter(), Camera);

	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FCameraComponentTests, ProjectionParametersAreUpdatedAtomicallyAndClamped)
{
	Durin::DWorld* World = CreateWorld();
	Durin::ACameraActor* CameraActor = World->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	Durin::DCameraComponent* Camera = CameraActor->GetCameraComponent();
	ASSERT_NE(Camera, nullptr);
	Camera->SetProjectionParameters(200.0f, -5.0f, -1.0f);
	EXPECT_FLOAT_EQ(Camera->GetFieldOfViewDegrees(), 170.0f);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.001f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 1.001f);
	Camera->SetNearClip(25.0f);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 25.0f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 26.0f);
	Camera->SetFarClip(10.0f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 26.0f);
	EXPECT_EQ(Camera->GetAspectRatioMode(), Durin::ECameraAspectRatioMode::Viewport);
	EXPECT_FLOAT_EQ(Camera->ResolveAspectRatio(4.0f / 3.0f), 4.0f / 3.0f);
	Camera->SetAspectRatio(Durin::ECameraAspectRatioMode::Ratio16By9, 2.0f);
	EXPECT_FLOAT_EQ(Camera->ResolveAspectRatio(4.0f / 3.0f), 16.0f / 9.0f);
	Camera->SetAspectRatio(Durin::ECameraAspectRatioMode::Custom, 20.0f);
	EXPECT_FLOAT_EQ(Camera->GetCustomAspectRatio(), 10.0f);
	EXPECT_FLOAT_EQ(Camera->ResolveAspectRatio(4.0f / 3.0f), 10.0f);
	Durin::MarkObjectHierarchyAsGarbage(World);
	Durin::CollectGarbage();
}

TEST(FCameraEditingTests, SharedTransactionsPreserveAtomicProjectionSemanticsAndStablePaths)
{
	InitializeDObjectSystem();
	auto* Camera = Durin::NewObject<Durin::DCameraComponent>(nullptr, "TransactionalCamera");
	auto* Projection = static_cast<Durin::FStructProperty*>(Camera->GetClass()->FindPropertyByName("ProjectionSettings"));
	ASSERT_NE(Projection, nullptr);
	ASSERT_EQ(Projection->GetKind(), Durin::DurinCodeGen::EPropertyGenFlags::Struct);
	Durin::DStruct* ProjectionStruct = Projection->GetStruct();
	ASSERT_NE(ProjectionStruct, nullptr);
	Durin::FProperty* FieldOfView = ProjectionStruct->FindPropertyByName(Durin::FName("FieldOfViewDegrees"));
	Durin::FProperty* NearClip = ProjectionStruct->FindPropertyByName(Durin::FName("NearClip"));
	Durin::FProperty* AspectRatioMode = ProjectionStruct->FindPropertyByName(Durin::FName("AspectRatioMode"));
	Durin::FProperty* CustomAspectRatio = ProjectionStruct->FindPropertyByName(Durin::FName("CustomAspectRatio"));
	ASSERT_NE(FieldOfView, nullptr);
	ASSERT_NE(NearClip, nullptr);
	ASSERT_NE(AspectRatioMode, nullptr);
	ASSERT_NE(CustomAspectRatio, nullptr);

	auto GetSettings = [&] {
		return Projection->ContainerPtrToValuePtr<Durin::FCameraProjectionSettings>(Camera);
	};
	auto MakeTarget = [&](Durin::FProperty* Field) {
		return Durin::Editor::FPropertyEditTarget::ForMember(Camera, Projection).ForStructMember(Field);
	};
	const Durin::Editor::FPropertyEditTarget NearTarget = MakeTarget(NearClip);
	ASSERT_EQ(NearTarget.Path.size(), 2u);
	EXPECT_EQ(NearTarget.MemberProperty, Projection);
	EXPECT_EQ(NearTarget.LeafProperty, NearClip);
	EXPECT_EQ(NearTarget.Path[0].Property, Projection);
	EXPECT_EQ(NearTarget.Path[1].Property, NearClip);

	Durin::Editor::FTransactionManager Transactions;
	Durin::Editor::FPropertyView View;
	std::string Error;
	const Durin::Editor::FPropertyViewContext Context{
		.Transactions = &Transactions,
		.ReportError = [&Error](std::string Message) { Error = std::move(Message); },
	};
	auto SubmitFloat = [&](Durin::FProperty* Field, float Value, bool bContinuous) {
		return View.SubmitPropertyValueEdit(Context, MakeTarget(Field),
			[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
				*ScratchProperty->ContainerPtrToValuePtr<float>(ScratchContainer, ScratchArrayIndex) = Value;
		}, bContinuous);
	};

	ASSERT_TRUE(SubmitFloat(FieldOfView, 80.0f, true));
	ASSERT_TRUE(SubmitFloat(FieldOfView, 90.0f, true));
	View.FinishActiveEdit(&Context, false);
	EXPECT_TRUE(Error.empty());
	EXPECT_FLOAT_EQ(Camera->GetFieldOfViewDegrees(), 90.0f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FLOAT_EQ(Camera->GetFieldOfViewDegrees(), 60.0f);
	ASSERT_TRUE(Transactions.Redo());
	EXPECT_FLOAT_EQ(Camera->GetFieldOfViewDegrees(), 90.0f);

	Transactions.Clear();
	ASSERT_TRUE(SubmitFloat(NearClip, 2000.0f, true));
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 2000.0f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 2001.0f);
	View.FinishActiveEdit(&Context, true);
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.1f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 1000.0f);
	EXPECT_FALSE(Transactions.CanUndo());

	ASSERT_TRUE(SubmitFloat(NearClip, -5.0f, false));
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.001f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 1000.0f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FLOAT_EQ(Camera->GetNearClip(), 0.1f);
	EXPECT_FLOAT_EQ(Camera->GetFarClip(), 1000.0f);

	ASSERT_TRUE(View.SubmitPropertyValueEdit(Context, MakeTarget(AspectRatioMode),
		[&](Durin::FProperty* ScratchProperty, void* ScratchContainer, Durin::uint32 ScratchArrayIndex) {
			*ScratchProperty->ContainerPtrToValuePtr<Durin::ECameraAspectRatioMode>(ScratchContainer, ScratchArrayIndex) = Durin::ECameraAspectRatioMode::Custom;
	}, false));
	ASSERT_TRUE(SubmitFloat(CustomAspectRatio, 20.0f, false));
	EXPECT_EQ(Camera->GetAspectRatioMode(), Durin::ECameraAspectRatioMode::Custom);
	EXPECT_FLOAT_EQ(Camera->GetCustomAspectRatio(), 10.0f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_FLOAT_EQ(Camera->GetCustomAspectRatio(), 16.0f / 9.0f);
	ASSERT_TRUE(Transactions.Undo());
	EXPECT_EQ(Camera->GetAspectRatioMode(), Durin::ECameraAspectRatioMode::Viewport);

	Transactions.Clear();
	Durin::MarkAsGarbage(Camera);
	Durin::CollectGarbage();
}
