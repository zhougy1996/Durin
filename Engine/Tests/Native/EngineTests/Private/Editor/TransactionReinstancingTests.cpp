#include "Editor/TransactionRecordTestSupport.h"

using namespace Durin::Tests::ObjectState;

namespace
{
	class FReinstancedValueCommand final : public Durin::FCommandChange
	{
	public:
		inline static std::function<void(Durin::DObject*)> DestructionObserver;
		~FReinstancedValueCommand() override { if (DestructionObserver) DestructionObserver(Target.Resolve()); }
		FReinstancedValueCommand(DTransactionRecordParticipant* Object, int32 InBefore, int32 InAfter, bool InSupport = true)
			: Target(Object), Before(InBefore), After(InAfter), bSupport(InSupport), Packages{Object->GetPackage()} {}
		auto Apply() -> Durin::FCommandChangeResult override { static_cast<DTransactionRecordParticipant*>(Target.Resolve())->Value = After; return {}; }
		auto Revert() -> Durin::FCommandChangeResult override { static_cast<DTransactionRecordParticipant*>(Target.Resolve())->Value = Before; return {}; }
		auto GetDescription() const -> std::string_view override { return "Reinstanced native value"; }
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override { return {Target.Resolve()}; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override { return Packages; }
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override { Target.AddReferencedObjects(Collector); }
		auto GetAllocatedSize() const -> size_t override { return sizeof(*this); }
		auto CreateReplacement(const Durin::FObjectReplacementMap& Map) const -> std::expected<std::unique_ptr<Durin::FCommandChange>, std::string> override
		{
			if (!bSupport) return Durin::FCommandChange::CreateReplacement(Map);
			auto Key = Map.RemapObjectKey(Target.GetKey());
			if (!Key) return std::unexpected(Durin::ToString(Key.error()));
			return std::make_unique<FReinstancedValueCommand>(static_cast<DTransactionRecordParticipant*>(Durin::ResolveObjectKey(*Key)), Before, After);
		}
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
			Buffer->EstablishSavedState(*Current);
			Pair = {Current, Candidate};
		}
		auto TearDown() -> void override
		{
			Operation.Abort();
			(void)Operation.Retire();
			Buffer.Reset();
			Durin::MarkObjectHierarchyAsGarbage(Current);
			Durin::MarkObjectHierarchyAsGarbage(Candidate);
			Durin::CollectGarbage();
		}
		auto Record(int32 Value) -> void
		{
			Durin::Editor::FScopedTransaction Scope(Buffer.Get(), {"test", "Reinstanced value", Old});
			ASSERT_TRUE(Old->Modify());
			Old->Value = Value;
			ASSERT_TRUE(Scope.End());
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

TEST_F(FTransactionReinstancingTests, RetainsUndoRedoPrimaryIdentityAndNewPackageCheckpoint)
{
	using namespace Durin;
	Record(42);
	const auto Id = Buffer->GetUndoId();
	New->Value = 77;
	const FObjectKey OldKey(Old);
	auto Ready = Prepare(); ASSERT_TRUE(Ready) << ToString(Ready.error());
	EXPECT_EQ(New->SerializationLoads, 0u);
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoId(), Id);
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, New);
	EXPECT_FALSE(Buffer->GetPackageRevisionState(*Current).has_value());
	ASSERT_TRUE(Operation.Retire());
	CollectGarbage();
	EXPECT_EQ(ResolveObjectKey(OldKey), nullptr);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0);
	EXPECT_TRUE(Candidate->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 77);
	EXPECT_FALSE(Candidate->IsDirty());
	EXPECT_TRUE(Buffer->GetPackageRevisionState(*Candidate)->bCheckpointValid);
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

TEST_F(FTransactionReinstancingTests, AbortAndPersistenceFailureLeaveOriginalHistoryUntouched)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	EXPECT_FALSE(Operation.TryCommit([]() -> std::expected<void, Durin::FObjectReplacementError> {
		return std::unexpected(Durin::FObjectReplacementError{.Code = Durin::EObjectReplacementError::ParticipantRejected});
	}));
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
	Operation.Abort();
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Old->Value, 0);
	EXPECT_EQ(New->SerializationLoads, 0u);
}

TEST_F(FTransactionReinstancingTests, ChangedHistoryAfterPrepareRejectsPublication)
{
	Record(12);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_FALSE(Operation.TryCommit());
	EXPECT_EQ(Durin::FindPackage(Current->GetPackagePath()), Current);
	EXPECT_EQ(Buffer->GetRedoContext().PrimaryObject, Old);
}

TEST_F(FTransactionReinstancingTests, UnsupportedAnnotationRetiresWholeTransactionWithoutPartialRemap)
{
	Old->bAnnotationEnabled = true; Old->AnnotationReference = Old;
	Record(12);
	ASSERT_TRUE(Prepare());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_FALSE(Buffer->CanUndo());
	ASSERT_TRUE(Operation.Retire());
}

TEST_F(FTransactionReinstancingTests, ActorAnnotationRebindsTypedSceneCacheWithoutChangingOriginalSnapshot)
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
	auto Replaced = PrepareTransactionAnnotationReplacement(Source, Map);
	ASSERT_TRUE(Replaced) << Replaced.error();
	EXPECT_NE(Replaced->get(), Source.get());
	EXPECT_EQ(AfterRoot->GetRelativeLocation().x, 0.0);
	const auto* Annotation = dynamic_cast<const FActorTransactionAnnotation*>(Replaced->get());
	ASSERT_NE(Annotation, nullptr);
	ASSERT_TRUE(Annotation->GetComponentInstanceDataCache().ApplyToActor(*AfterActor, ECacheApplyPhase::PostUserConstructionScript));
	EXPECT_EQ(AfterRoot->GetRelativeLocation().x, 17.0);
	EXPECT_TRUE(Source->Validate());
	EXPECT_FALSE(Annotation->Restore(*BeforeActor));
}

TEST_F(FTransactionReinstancingTests, CustomCommandOptInRebindsFrozenParticipantsAndPackages)
{
	using namespace Durin;
	ASSERT_EQ(Buffer->Begin("test", "Native command"), 0);
	Old->Value = 12;
	ASSERT_TRUE(GUndo->StoreUndo(Old, std::make_unique<FReinstancedValueCommand>(Old, 0, 12)));
	ASSERT_EQ(Buffer->End(), 1);
	ASSERT_TRUE(Prepare());
	ASSERT_TRUE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Operation.Retire());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(New->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(New->Value, 12);
}

TEST_F(FTransactionReinstancingTests, UnsupportedCommandRetiresItsGenericPeerAsOneTransaction)
{
	using namespace Durin;
	FPackagePath OtherPath; ASSERT_TRUE(FPackagePath::TryCreate("/TransactionReinstance/Other", OtherPath));
	auto* OtherPackage = CreatePackage(OtherPath);
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(OtherPackage, "UnmappedCommandPeer"));
	Buffer->EstablishSavedState(*OtherPackage);
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
	const auto State = Buffer->GetPackageRevisionState(*OtherPackage);
	ASSERT_TRUE(State); EXPECT_FALSE(State->bCheckpointValid);
	EXPECT_TRUE(OtherPackage->IsDirty());
	ASSERT_TRUE(Operation.Retire());
	MarkObjectHierarchyAsGarbage(OtherPackage);
}

TEST_F(FTransactionReinstancingTests, CanceledRecordingAfterPreparationInvalidatesPreparedHistory)
{
	using namespace Durin;
	Record(12);
	(void)Tests::CollectTransactionEvents(*Buffer);
	ASSERT_TRUE(Prepare());
	ASSERT_EQ(Buffer->Begin("test", "Temporary recording"), 0);
	Buffer->Cancel();
	(void)Tests::CollectTransactionEvents(*Buffer);
	EXPECT_FALSE(Operation.TryCommit());
	EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Old);
}

TEST_F(FTransactionReinstancingTests, ComponentAnnotationRebindsExactTargetWithoutLifecycleCallbacks)
{
	using namespace Durin;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor");
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor");
	auto* BeforeRoot = static_cast<DTransactionLifecycleScene*>(BeforeActor->GetRootComponent());
	auto* AfterRoot = static_cast<DTransactionLifecycleScene*>(AfterActor->GetRootComponent());
	std::shared_ptr<ITransactionObjectAnnotation> Source = std::make_shared<FComponentTransactionAnnotation>(*BeforeRoot);
	FObjectReplacementMap Map; ASSERT_TRUE(Map.Build(std::span(&Pair, 1)));
	auto Replaced = PrepareTransactionAnnotationReplacement(Source, Map);
	ASSERT_TRUE(Replaced) << Replaced.error();
	const auto* Annotation = dynamic_cast<const FComponentTransactionAnnotation*>(Replaced->get());
	ASSERT_NE(Annotation, nullptr);
	EXPECT_EQ(Annotation->GetComponent(), AfterRoot);
	EXPECT_EQ(static_cast<const FComponentTransactionAnnotation*>(Source.get())->GetComponent(), BeforeRoot);
	EXPECT_EQ(AfterRoot->Created, 0u); EXPECT_EQ(AfterRoot->Registered, 0u);
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
	using namespace Durin; using namespace Durin::Editor;
	auto* BeforeActor = NewObject<DTransactionConstructionActor>(Current, "Actor", EObjectFlags::Transactional);
	auto* AfterActor = NewObject<DTransactionConstructionActor>(Candidate, "Actor", EObjectFlags::Transactional);
	ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
	static_cast<DSceneComponent*>(BeforeActor->FindGeneratedComponent(DTransactionConstructionActor::Key(0)))->SetRelativeLocation({17.0, 0.0, 0.0});
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Generated history", BeforeActor});
		ASSERT_TRUE(BeforeActor->Modify()); BeforeActor->GeneratedCount = 2;
		ASSERT_TRUE(BeforeActor->RerunConstructionScripts());
		ASSERT_TRUE(Scope.End());
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
	using namespace Durin; using namespace Durin::Editor;
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "UnaffectedNativePeer"));
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Mixed affected and unaffected", Old});
		ASSERT_TRUE(Old->Modify()); Old->Value = 12;
		Peer->Value = 9;
		ASSERT_TRUE(GUndo->StoreUndo(Peer.Get(), std::make_unique<FReinstancedValueCommand>(Peer.Get(), 0, 9, false)));
		ASSERT_TRUE(Scope.End());
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
	using namespace Durin; using namespace Durin::Editor;
	TStrongObjectPtr<DTransactionRecordParticipant> Peer(NewObject<DTransactionRecordParticipant>(nullptr, "WeakOnlyReinstancingPeer"));
	TStrongObjectPtr<DTransactionRecordParticipant> KeepNew(New);
	Peer->Weak = Old;
	{
		FScopedTransaction Scope(Buffer.Get(), {"test", "Weak-only dependency", Peer.Get()});
		ASSERT_TRUE(Peer->Modify()); Peer->Value = 12; ASSERT_TRUE(Scope.End());
	}
	ASSERT_TRUE(Prepare()); ASSERT_TRUE(Operation.TryCommit());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	Peer->Weak = nullptr;
	ASSERT_TRUE(Operation.Retire()); CollectGarbage();
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Peer->Weak.Get(), New);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_FALSE(Peer->Weak.IsValid());
}
