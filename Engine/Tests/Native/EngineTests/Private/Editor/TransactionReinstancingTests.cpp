#include "Editor/TransactionRecordTestSupport.h"
#include "Actors/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"

using namespace Durin::Tests::ObjectState;

TEST(FTransactionTemplateTests, PublishedClassDefaultUsesModifyAndSurvivesRecreationAndGC)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Class = DTransactionRecordParticipant::StaticClass();
	const std::array Classes{Class};
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	auto* Default = const_cast<DTransactionRecordParticipant*>(static_cast<const DTransactionRecordParticipant*>(Class->GetDefaultObject()));
	ASSERT_NE(Default, nullptr);
	Default->ClearFlags(EObjectFlags::Transactional);
	EXPECT_FALSE(CanTransactObject(*Default));
	EXPECT_FALSE(CaptureSerializedObject(*Default));
	Default->SetFlags(EObjectFlags::Transactional);
	const FPersistentObjectRef Logical(Default);
	const auto Exact = FPersistentObjectRef::FromKey(FObjectKey(Default));
	EXPECT_EQ(Logical.GetReferenceType(), FPersistentObjectRef::EReferenceType::SubObject);
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "TemplateHistory"));
	TStrongObjectPtr<DTransactionRecordParticipant> Owner(NewObject<DTransactionRecordParticipant>(nullptr, "TemplateReferenceOwner"));
	Owner->Hard = Default;
	const auto Original = Default->Value;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Edit class default", Default});
		ASSERT_TRUE(Default->Modify(false));
		ASSERT_TRUE(Owner->Modify(false));
		Default->Value = 37;
		Owner->Value = 5;
	}
	Default->Rename("RejectedTemplateRename");
	EXPECT_EQ(Logical.Resolve(), Default);
	Private::ReleaseClassDefaultObjectForTests(Class);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Logical.Resolve(), nullptr);
	EXPECT_EQ(Exact.Resolve(), nullptr);
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	auto* Replacement = const_cast<DTransactionRecordParticipant*>(static_cast<const DTransactionRecordParticipant*>(Class->GetDefaultObject()));
	ASSERT_NE(Replacement, nullptr);
	EXPECT_EQ(Logical.Resolve(), Replacement);
	EXPECT_EQ(Logical, FPersistentObjectRef(Replacement));
	EXPECT_EQ(Logical.GetHash(), FPersistentObjectRef(Replacement).GetHash());
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Replacement);
	Replacement->Value = 81;
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Replacement->Value, Original);
	EXPECT_EQ(Owner->Hard.Get(), Replacement);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Replacement->Value, 81);
	EXPECT_EQ(Owner->Value, 5);
	Buffer->Reset();
	Replacement->Value = Original;
}

TEST(FTransactionTemplateTests, DefaultSubobjectHistoryResolvesRecreatedGraphWithoutActivation)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Class = AStaticMeshActor::StaticClass();
	const std::array Classes{Class};
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	auto* Default = const_cast<AStaticMeshActor*>(static_cast<const AStaticMeshActor*>(Class->GetDefaultObject()));
	ASSERT_NE(Default, nullptr);
	auto* Component = Default->GetStaticMeshComponent();
	ASSERT_NE(Component, nullptr);
	EXPECT_TRUE(Component->HasAnyObjectFlags(EObjectFlags::DefaultSubobject));
	EXPECT_FALSE(CanTransactObject(*Component));
	Component->SetFlags(EObjectFlags::Transactional);
	const FPersistentObjectRef Logical(Component);
	const auto Before = Component->GetRelativeTransform();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "SubobjectHistory"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Edit default subobject", Component});
		ASSERT_TRUE(Component->Modify(false));
		Component->SetRelativeLocation({12.0, 0.0, 0.0});
	}
	Private::ReleaseClassDefaultObjectForTests(Class);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	Default = const_cast<AStaticMeshActor*>(static_cast<const AStaticMeshActor*>(Class->GetDefaultObject()));
	Component = Default->GetStaticMeshComponent();
	Component->SetFlags(EObjectFlags::Transactional);
	EXPECT_EQ(Logical.Resolve(), Component);
	Component->SetRelativeLocation({29.0, 0.0, 0.0});
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Component->GetRelativeLocation(), Before.Translation);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Component->GetRelativeLocation().x, 29.0);
	EXPECT_FALSE(Component->IsRegistered());
	EXPECT_FALSE(Component->HasBegunPlay());
	Buffer->Reset();
	Component->SetRelativeTransform(Before);
	Component->ClearFlags(EObjectFlags::Transactional);
}

TEST(FTransactionTemplateTests, ResurrectionUsesTheOwningClassDefaultSubobject)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Class = AStaticMeshActor::StaticClass();
	const std::array Classes{Class};
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	auto* Default = static_cast<const AStaticMeshActor*>(Class->GetDefaultObject());
	ASSERT_NE(Default, nullptr);
	auto* Template = Default->GetStaticMeshComponent();
	ASSERT_NE(Template, nullptr);
	TStrongObjectPtr<AStaticMeshActor> Actor(NewObject<AStaticMeshActor>(nullptr, "ResurrectionMeshActor"));
	auto* Component = Actor->GetStaticMeshComponent();
	ASSERT_NE(Component, nullptr);
	const auto Original = Template->GetRelativeTransform();
	struct FRestoreTemplate
	{
		DStaticMeshComponent* Component;
		FTransform Transform;
		~FRestoreTemplate() { Component->SetRelativeTransform(Transform); }
	} Restore{Template, Original};
	Template->SetRelativeLocation({37.0, 0.0, 0.0});
	Component->SetRelativeTransform(Template->GetRelativeTransform());
	auto ChangesFromTemplate = [](DObject& Target) -> std::expected<FTransactionObjectChange, std::string> {
		auto Live = DiffUtil::GetDiffableObject(Target);
		if (!Live) return std::unexpected(Live.error());
		FDiffableObject Dead{.ObjectInfo = Live->ObjectInfo};
		Dead.ObjectInfo.bIsPendingKill = true;
		return DiffUtil::GenerateObjectDiff(Dead, *Live);
	};
	auto Owner = ChangesFromTemplate(*Actor); ASSERT_TRUE(Owner) << Owner.error();
	EXPECT_FALSE(std::ranges::contains(Owner->ChangedProperties, FName("RootComponent")));
	auto Same = ChangesFromTemplate(*Component); ASSERT_TRUE(Same) << Same.error();
	EXPECT_FALSE(std::ranges::contains(Same->ChangedProperties, FName("RelativeTransform")));
	Component->SetRelativeTransform(Original);
	auto Changed = ChangesFromTemplate(*Component); ASSERT_TRUE(Changed) << Changed.error();
	EXPECT_TRUE(std::ranges::contains(Changed->ChangedProperties, FName("RelativeTransform")));
}

namespace
{
	class FReinstancedValueCommand final : public Durin::FCommandChange
	{
	public:
		inline static std::function<void(Durin::DObject*)> DestructionObserver;
		~FReinstancedValueCommand() override { if (DestructionObserver) DestructionObserver(Target.Resolve()); }
		FReinstancedValueCommand(DTransactionRecordParticipant* Object, int32 InBefore, int32 InAfter, bool InSupport = true)
			: Target(Object), Before(InBefore), After(InAfter), bSupport(InSupport), Packages{Object->GetPackage()} {}
		auto HasExpired(Durin::DObject* Owner) const noexcept -> bool override { return !Owner; }
		auto Apply(Durin::DObject*) -> void override { static_cast<DTransactionRecordParticipant*>(Target.Resolve())->Value = After; }
		auto Revert(Durin::DObject*) -> void override { static_cast<DTransactionRecordParticipant*>(Target.Resolve())->Value = Before; }
		auto GetDescription() const -> std::string_view override { return "Reinstanced native value"; }
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override { return {Target.Resolve()}; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override { return Packages; }
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) -> void override
		{
			if (!bSupport) { std::as_const(Target).AddReferencedObjects(Collector); return; }
			Target.AddReferencedObjects(Collector);
			Durin::DObject* Package = Packages.front();
			Collector.AddReferencedObject(Package);
			Packages.front() = Durin::Cast<Durin::DPackage>(Package);
		}
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
	private:
		Durin::FPersistentObjectRef Target;
		int32 Before, After;
		bool bSupport;
		std::array<Durin::DPackage*, 1> Packages;
	};

	class FTransactionReinstancingTests : public testing::Test
	{
	protected:
		auto SetUp() -> void override
		{
			using namespace Durin;
			InitializeDObjectSystem();
			Testing::RegisterMountPointForTests("/TransactionReinstance/", Testing::GetTestWorkDirectory().generic_string() + "/");
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate("/TransactionReinstance/History", Path));
			Current = CreatePackage(Path);
			Candidate = NewObject<DPackage>(nullptr, "ReinstancingCandidate");
			ASSERT_TRUE(Candidate->InitializePreparedAssetPackage(Path));
			Old = NewObject<DTransactionRecordParticipant>(Current, "Target", EObjectFlags::Public);
			New = NewObject<DTransactionRecordParticipant>(Candidate, "Target", EObjectFlags::Public);
			Buffer = TStrongObjectPtr<DTransBuffer>(NewObject<DTransBuffer>(nullptr, "ReinstancingHistory"));
			Current->SetFlags(EObjectFlags::Transactional);
			Current->ClearDirty();
			Pair = {Current, Candidate};
		}
		auto TearDown() -> void override
		{
			Operation.Abort();
			(void)Operation.Retire();
			Buffer.Reset();
			Durin::MarkObjectHierarchyAsGarbage(Current);
			Durin::MarkObjectHierarchyAsGarbage(Candidate);
			Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
		}
		auto Record(int32 Value) -> void
		{
			Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Reinstanced value", Old});
			ASSERT_TRUE(Old->Modify());
			Old->Value = Value;
		}
		auto Prepare() -> std::expected<void, Durin::FObjectReplacementError>
		{
			const std::shared_ptr<Durin::IObjectReplacementParticipant> Participants[]{
				Durin::CreateTransactorReloadParticipant(*Buffer, std::span(&Current, 1))};
			return Operation.Prepare(std::span(&Pair, 1), Participants);
		}
		Durin::Testing::FScopedMountRegistryFixture Mounts;
		Durin::DPackage* Current = nullptr;
		Durin::DPackage* Candidate = nullptr;
		DTransactionRecordParticipant* Old = nullptr;
		DTransactionRecordParticipant* New = nullptr;
		Durin::TStrongObjectPtr<Durin::DTransBuffer> Buffer;
		Durin::FObjectReplacementPackagePair Pair;
		Durin::FObjectGraphReplacement Operation;
	};
}

TEST(FTransactionTemplateTests, OpaqueCustomParticipantsExpireBeforeRecreatedDefaultsCanBeMutated)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Class = DTransactionRecordParticipant::StaticClass();
	const std::array Classes{Class};
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	auto* Default = const_cast<DTransactionRecordParticipant*>(static_cast<const DTransactionRecordParticipant*>(Class->GetDefaultObject()));
	const auto Original = Default->Value;
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "OpaqueTemplateHistory"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Native default command", Default});
		ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FReinstancedValueCommand>(Default, Original, 8, false)));
	}
	Private::ReleaseClassDefaultObjectForTests(Class);
	ASSERT_TRUE(Private::CreateClassDefaultObjectsForBatch(Classes));
	Default = const_cast<DTransactionRecordParticipant*>(static_cast<const DTransactionRecordParticipant*>(Class->GetDefaultObject()));
	Default->Value = 93;
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(Default->Value, 93);
	Buffer->Reset();
	Default->Value = Original;
	CollectGarbage(Durin::EObjectFlags::Standalone);
}

TEST_F(FTransactionReinstancingTests, UndoDeletionRestoresTopLevelRegistrationAndRenamesTheOccupant)
{
	using namespace Durin;
	ASSERT_TRUE(std::ranges::contains(Current->GetTopLevelAssets(), Old));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Delete top-level object"});
		ASSERT_TRUE(Old->Modify());
		Old->MarkAsGarbage();
	}
	EXPECT_FALSE(std::ranges::contains(Current->GetTopLevelAssets(), Old));
	auto* Conflict = NewObject<DTransactionRecordParticipant>(Current, "Target", EObjectFlags::Public);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_FALSE(Old->IsGarbage());
	EXPECT_EQ(Old->GetFName(), FName("Target"));
	EXPECT_NE(Conflict->GetFName(), Old->GetFName());
	EXPECT_TRUE(std::ranges::contains(Current->GetTopLevelAssets(), Conflict));
	EXPECT_TRUE(std::ranges::contains(Current->GetTopLevelAssets(), Old));
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_FALSE(std::ranges::contains(Current->GetTopLevelAssets(), Old));
}

TEST_F(FTransactionReinstancingTests, RetainsUndoRedoPrimaryIdentityAndNewPackageCheckpoint)
{
	using namespace Durin;
	Record(42);
	const auto Id = Buffer->GetUndoContext().TransactionId;
	New->Value = 77;
	const FObjectKey OldKey(Old);
	auto Ready = Prepare(); ASSERT_TRUE(Ready) << ToString(Ready.error());
	EXPECT_EQ(New->SerializationLoads, 0u);
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, New);
	ASSERT_TRUE(Operation.Retire());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0);
	EXPECT_TRUE(Candidate->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 77);
	EXPECT_FALSE(Candidate->IsDirty());
}

TEST_F(FTransactionReinstancingTests, ExplicitReinstancingHookUsesTheSharedCheckedHistoryPublication)
{
	using namespace Durin;
	Record(42);
	TStrongObjectPtr<DPackage> KeepCandidate(Candidate);
	auto* Reinstanced = NewObject<DTransactionRecordParticipant>(Current, "Replacement", EObjectFlags::Public);
	Old->Rename("TRASH_Target");
	Reinstanced->Rename("Target");
	const FObjectKey OldKey(Old);
	const FObjectReplacementMap::FEntry Entries[]{{Old, Reinstanced}};
	FObjectReplacementMap Map;
	ASSERT_TRUE(Map.BuildFromObjects(Entries));
	Reinstanced->Value = 77;
	ASSERT_TRUE(Buffer->OnObjectsReinstanced(Map));
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Reinstanced);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Reinstanced->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Reinstanced->Value, 77);
	Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Busy reinstancing", Reinstanced});
	EXPECT_FALSE(Buffer->OnObjectsReinstanced(Map));
	Scope.Cancel();
	MarkAsGarbage(Old);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Reinstanced->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Reinstanced->Value, 77);
}

TEST_F(FTransactionReinstancingTests, ExplicitMapRejectsOwnerMismatchAndPartialMapRetiresTheWholeEntry)
{
	using namespace Durin;
	Record(42);
	FObjectReplacementMap Map;
	const FObjectReplacementMap::FEntry MissingOwner{Old, New};
	EXPECT_FALSE(Map.BuildFromObjects(std::span(&MissingOwner, 1)));
	EXPECT_TRUE(Map.GetEntries().empty());
	const FObjectReplacementMap::FEntry PackageOnly{Current, Candidate};
	ASSERT_TRUE(Map.BuildFromObjects(std::span(&PackageOnly, 1)));
	ASSERT_TRUE(Buffer->OnObjectsReinstanced(Map));
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_FALSE(Buffer->CanUndo());
	EXPECT_EQ(Old->Value, 42);
	EXPECT_EQ(New->Value, 0);
}

TEST_F(FTransactionReinstancingTests, RedoBranchSurvivesReinstancingAtUndonePosition)
{
	Record(42);
	ASSERT_TRUE(Buffer->Undo());
	New->Value = 9;
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_TRUE(Buffer->CanRedo()); EXPECT_FALSE(Buffer->CanUndo());
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 42);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 9);
	EXPECT_FALSE(Candidate->IsDirty());
}

TEST_F(FTransactionReinstancingTests, AbortingPreparedReplacementLeavesOriginalHistoryUntouched)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
	Operation.Abort();
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Old->Value, 0);
	EXPECT_EQ(New->SerializationLoads, 0u);
}

TEST_F(FTransactionReinstancingTests, UndoAfterPreparationRemapsTheCurrentRedoEntry)
{
	Current->MarkDirty();
	Record(12);
	ASSERT_TRUE(Prepare());
	Old->Value = 37;
	ASSERT_TRUE(Buffer->Undo());
	bool ObservedReplacement = false;
	const auto Handle = Durin::OnAssetPackagesChanged().AddLambda([&] {
		if (Durin::FindPackage(Candidate->GetPackagePath()) == Candidate)
		{
			ObservedReplacement = true;
			EXPECT_EQ(Buffer->GetRedoContext().PrimaryObject, New);
		}
	});
	auto Published = Operation.TryCommit();
	Durin::OnAssetPackagesChanged().Remove(Handle);
	ASSERT_TRUE(Published) << Durin::ToString(Published.error());
	EXPECT_TRUE(ObservedReplacement);
	EXPECT_EQ(Buffer->GetRedoContext().PrimaryObject, New);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(New->Value, 37);
}

TEST_F(FTransactionReinstancingTests, WritableAnnotationPreservesHistoryAcrossReinstancing)
{
	Old->bAnnotationEnabled = true; Old->AnnotationReference = Old;
	Record(12);
	ASSERT_TRUE(Prepare());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(New->Value, 0);
}

TEST_F(FTransactionReinstancingTests, ActorAnnotationRebindsTypedSceneCacheInPlace)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	auto* BeforeRoot = static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	auto* AfterRoot = static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	BeforeRoot->SetRelativeLocation({17.0, 0.0, 0.0});
	std::shared_ptr<ITransactionObjectAnnotation> Source = std::make_shared<FActorTransactionAnnotation>(*BeforeActor);
	FObjectReplacementMap Map;
	ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	auto Replaced = ReinstanceTransactionAnnotation(Source, Map);
	ASSERT_TRUE(Replaced) << Replaced.error();
	EXPECT_EQ(AfterRoot->GetRelativeLocation().x, 0.0);
	const auto* Annotation = dynamic_cast<const FActorTransactionAnnotation*>(Source.get());
	ASSERT_NE(Annotation, nullptr);
	const auto& Cache = Annotation->GetComponentInstanceDataCache();
	ASSERT_TRUE(Cache.Validate(AfterActor));
	Cache.ApplyToActor(*AfterActor, ECacheApplyPhase::PostUserConstructionScript);
	EXPECT_EQ(AfterRoot->GetRelativeLocation().x, 17.0);
	AfterRoot->SetRelativeLocation({19.0, 0.0, 0.0});
	FActorTransactionAnnotation CurrentAnnotation(*AfterActor);
	const auto Changes = CurrentAnnotation.ComputeAdditionalObjectChanges(Annotation);
	ASSERT_EQ(Changes.size(), 1u); EXPECT_EQ(Changes[0].first, AfterRoot);
	EXPECT_EQ(Changes[0].second.ChangedProperties, std::vector<FName>{"RelativeTransform"});
	EXPECT_TRUE(Source->Validate());
	EXPECT_THROW(Annotation->CreateSnapshot(*BeforeActor), std::runtime_error);
}

TEST_F(FTransactionReinstancingTests, ComponentObservationsRebindAbsentGenerationsAndTheirHistoricalReferencePaths)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	TStrongObjectPtr<DTransactionConstructionActor> BeforeRoot(BeforeActor), AfterRoot(AfterActor);
	BeforeActor->GeneratedCount = 2; ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	auto Scene = [](auto* Actor, uint32 Index) { return static_cast<DSceneComponent*>(Actor->FindGeneratedComponent(DTransactionConstructionActor::Key(Index))); };
	ASSERT_TRUE(Scene(BeforeActor, 1)->AttachToComponent(Scene(BeforeActor, 0), EAttachmentTransformRule::KeepRelative));
	std::shared_ptr<ITransactionObjectAnnotation> Source = std::make_shared<FActorTransactionAnnotation>(*BeforeActor);
	BeforeActor->GeneratedCount = 0; ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	AfterActor->GeneratedCount = 0; ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	FObjectReplacementMap Map; auto Built = Map.Build(std::span(&Pair, 1)); ASSERT_TRUE(Built) << ToString(Built.error());
	auto Replaced = ReinstanceTransactionAnnotation(Source, Map); ASSERT_TRUE(Replaced) << Replaced.error();
	AfterActor->GeneratedCount = 2; ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	ASSERT_TRUE(Scene(AfterActor, 1)->AttachToComponent(Scene(AfterActor, 0), EAttachmentTransformRule::KeepRelative));
	// Prepared packages suppress admission until publication; compare equivalent
	// published component flags so this test isolates historical reference paths.
	Scene(AfterActor, 0)->SetFlags(EObjectFlags::Transactional); Scene(AfterActor, 1)->SetFlags(EObjectFlags::Transactional);
	FActorTransactionAnnotation CurrentAnnotation(*AfterActor);
	EXPECT_TRUE(CurrentAnnotation.ComputeAdditionalObjectChanges(Source.get()).empty());
}

TEST_F(FTransactionReinstancingTests, WritableCommandKeepsItsInstanceAndRebindsParticipantsAndPackages)
{
	using namespace Durin;
	ASSERT_EQ(Buffer->Begin("test", "Native command"), 0);
	Old->Value = 12;
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12)));
	ASSERT_EQ(Buffer->End(), 1);
	int Destructions = 0;
	FReinstancedValueCommand::DestructionObserver = [&](DObject*) { ++Destructions; };
	struct FReset { ~FReset() { FReinstancedValueCommand::DestructionObserver = {}; } } Reset;
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	EXPECT_EQ(Destructions, 0);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 12);
}

TEST_F(FTransactionReinstancingTests, UnsupportedCommandRetiresItsGenericPeerAsOneTransaction)
{
	using namespace Durin;
	FPackagePath OtherPath; ASSERT_TRUE(FPackagePath::TryCreate("/TransactionReinstance/Other", OtherPath));
	auto* OtherPackage = CreatePackage(OtherPath);
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(OtherPackage, "UnmappedCommandPeer"));
	OtherPackage->ClearDirty();
	ASSERT_EQ(Buffer->Begin("test", "Mixed native command"), 0);
	ASSERT_TRUE(Peer->Modify()); Peer->Value = 9;
	Old->Value = 12;
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12, false)));
	ASSERT_EQ(Buffer->End(), 1);
	ASSERT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 2u);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Peer->Value, 9);

	EXPECT_TRUE(OtherPackage->IsDirty());
	ASSERT_TRUE(Operation.Retire());
	MarkObjectHierarchyAsGarbage(OtherPackage);
}

TEST_F(FTransactionReinstancingTests, PublicationPreservesCurrentBarrierAndCanceledRecording)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	ASSERT_EQ(Buffer->Begin("test", "Temporary recording"), 0);
	Buffer->Cancel();
	Buffer->SetUndoBarrier();
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 1u);
	EXPECT_FALSE(Buffer->Undo());
	Buffer->RemoveUndoBarrier();
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(New->Value, 0);
}

TEST_F(FTransactionReinstancingTests, ResetAfterPreparationPublishesEmptyHistory)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	Buffer->Reset();
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST_F(FTransactionReinstancingTests, GeneratedSourceRejectsSameNamedAuthoredComponentAndPreservesCache)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	auto* BeforeScene = static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	BeforeScene->SetRelativeLocation({17.0, 0.0, 0.0});
	FComponentInstanceDataCache Cache(*BeforeActor);
	auto* Impostor = AfterActor->AddInstanceComponent(DTransactionLifecycleScene::StaticClass(), BeforeScene->GetFName());
	ASSERT_NE(Impostor, nullptr);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	EXPECT_FALSE(Cache.FindAndReplaceInstances(Map));
	EXPECT_TRUE(Cache.Validate(BeforeActor));
	EXPECT_FALSE(Cache.Validate(AfterActor));
}

TEST_F(FTransactionReinstancingTests, ActorHistoryReinstancingRestoresGeneratedInstanceDataAfterReconstruction)
{
	using namespace Durin; using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor", EObjectFlags::Transactional);
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor", EObjectFlags::Transactional);
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->SetRelativeLocation({17.0, 0.0, 0.0});
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Generated history", BeforeActor});
		ASSERT_TRUE(BeforeActor->Modify()); BeforeActor->GeneratedCount = 2;
		ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	}
	AfterActor->GeneratedCount = 2; ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->SetRelativeLocation({77.0, 0.0, 0.0});
	const auto ConstructionCalls = AfterActor->ConstructionCalls;
	auto Ready = Prepare(); ASSERT_TRUE(Ready) << ToString(Ready.error());
	EXPECT_EQ(AfterActor->ConstructionCalls, ConstructionCalls);
	ASSERT_TRUE(Operation.TryCommit()); ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(AfterActor->GeneratedCount, 1);
	EXPECT_EQ(static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->GetRelativeLocation().x, 17.0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(AfterActor->GeneratedCount, 2);
	EXPECT_EQ(static_cast<DSceneComponent*>(AfterActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->GetRelativeLocation().x, 77.0);
}

TEST_F(FTransactionReinstancingTests, DerivedInstanceDataWithoutTypedCopyRejectsWithoutSlicingOrChangingSource)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	ASSERT_TRUE(AfterActor->RerunConstructionScripts());
	auto* Component = static_cast<DTransactionLifecycleScene*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)));
	Component->bUnsupportedInstanceData = true;
	FComponentInstanceDataCache Cache(*BeforeActor);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	EXPECT_FALSE(Cache.FindAndReplaceInstances(Map));
	EXPECT_TRUE(Cache.Validate(BeforeActor));
	EXPECT_FALSE(Cache.Validate(AfterActor));
}

TEST_F(FTransactionReinstancingTests, ComponentCacheRejectsAssignableButDifferentExactClass)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<AActor>(Current, "Actor");
	auto* AfterActor = NewObject<AActor>(Candidate, "Actor");
	const auto Key = DTransactionConstructionActor::Key(0);
	auto* Scene = static_cast<DSceneComponent*>(BeforeActor->AddComponentByClass(DSceneComponent::StaticClass(), false, FTransform(), false, Key, "Generated"));
	Scene->SetRelativeLocation({17.0, 0.0, 0.0});
	FComponentInstanceDataCache Cache(*BeforeActor);
	ASSERT_NE(AfterActor->AddComponentByClass(DTransactionLifecycleScene::StaticClass(), false, FTransform(), false, Key, "Generated"), nullptr);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	EXPECT_FALSE(Cache.FindAndReplaceInstances(Map));
	EXPECT_TRUE(Cache.Validate(BeforeActor));
}

TEST_F(FTransactionReinstancingTests, RetiredCommandDestructionWaitsForRetirementAndSeesPinnedOldObjects)
{
	using namespace Durin;
	ASSERT_EQ(Buffer->Begin("test", "Retired destructor"), 0);
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12, false)));
	Old->Value = 12; ASSERT_EQ(Buffer->End(), 1);
	int32 Destructions = 0;
	bool bSawLiveOld = false;
	FReinstancedValueCommand::DestructionObserver = [&](DObject* Object) { ++Destructions; bSawLiveOld = Object == Old && IsValid(Object); };
	struct FResetObserver { ~FResetObserver() { FReinstancedValueCommand::DestructionObserver = {}; } } ResetObserver;
	ASSERT_TRUE(Prepare()); EXPECT_EQ(Destructions, 0);
	ASSERT_TRUE(Operation.TryCommit()); EXPECT_EQ(Destructions, 0);
	ASSERT_TRUE(Operation.Retire());
	EXPECT_EQ(Destructions, 1); EXPECT_TRUE(bSawLiveOld);
	FReinstancedValueCommand::DestructionObserver = {};
}

TEST_F(FTransactionReinstancingTests, SerializedGeneratedSourceRejectsAmbiguousOwnerIdentity)
{
	using namespace Durin;
	auto* Actor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* OtherOwner = NewObject<AActor>(nullptr, "AmbiguousSourceOwner");
	FByteBuffer Bytes;
	FObjectMemoryWriter Writer(Bytes, EArchivePurpose::Transaction);
	FObjectKey References[]{FObjectKey{}, FObjectKey(Actor), FObjectKey(Actor->GetRootComponent()->GetClass()), FObjectKey(OtherOwner)};
	for (auto& Reference : References) Reference.SerializeForSnapshot(Writer);
	auto Key = DTransactionConstructionActor::Key(0);
	Writer << Key.Namespace << Key.Id.A << Key.Id.B << Key.Id.C << Key.Id.D;
	FObjectMemoryReader Reader(Bytes, EArchivePurpose::Transaction);
	FActorComponentInstanceSourceInfo Source;
	Source.Serialize(Reader);
	EXPECT_TRUE(Reader.IsError());
	EXPECT_FALSE(Source.Validate());
	FObjectReplacementMap Map;
	EXPECT_FALSE(Source.FindAndReplaceInstances(Map));
}

TEST_F(FTransactionReinstancingTests, UnaffectedNativeCommandMovesWithoutRequiringAReplacementHook)
{
	using namespace Durin; using namespace Durin;
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "UnaffectedNativePeer"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Mixed affected and unaffected", Old});
		ASSERT_TRUE(Old->Modify()); Old->Value = 12;
		Peer->Value = 9;
		ASSERT_TRUE(GUndo->StoreUndo(Peer.Get(), std::make_unique<FReinstancedValueCommand>(Peer.Get(), 0, 9, false)));
	}
	New->Value = 77;
	ASSERT_TRUE(Prepare()); ASSERT_TRUE(Operation.TryCommit());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0); EXPECT_EQ(Peer->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 77); EXPECT_EQ(Peer->Value, 9);
}

TEST_F(FTransactionReinstancingTests, WeakOnlySerializedDependencyRebindsWithoutAcquiringCollectorOwnership)
{
	using namespace Durin; using namespace Durin;
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "WeakOnlyReinstancingPeer"));
	TStrongObjectPtr<DTransactionRecordParticipant> KeepNew(New);
	Peer->Weak = Old;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Weak-only dependency", Peer.Get()});
		ASSERT_TRUE(Peer->Modify()); Peer->Value = 12;
	}
	ASSERT_TRUE(Prepare()); ASSERT_TRUE(Operation.TryCommit());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	Peer->Weak = nullptr;
	ASSERT_TRUE(Operation.Retire()); CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Peer->Weak.Get(), New);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_FALSE(Peer->Weak.IsValid());
}
