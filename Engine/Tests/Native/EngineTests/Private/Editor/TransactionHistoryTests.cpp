#include "Editor/TransactionRecordTestSupport.h"
#include "PropertyEditor/PropertyHandle.h"

using namespace Durin::Tests::ObjectState;

namespace
{
	// The synthetic transaction participant has fields unsupported by package serialization.
	auto SimulatePackageSave(Durin::DPackage& Package) -> void
	{
		Package.ClearDirty();
		Durin::OnPackageSaved().Broadcast(Package);
	}

	class FManagementProbeChange final : public Durin::FCommandChange
	{
	public:
		~FManagementProbeChange() override { if (OnDestroy) OnDestroy(); }
		auto Apply() -> Durin::FCommandChangeResult override { return {}; }
		auto Revert() -> Durin::FCommandChangeResult override { return {}; }
		auto GetDescription() const -> std::string_view override { return "Management probe"; }
		auto GetDetails(Durin::ETransactionOperation) const -> std::string override { if (OnDetails) OnDetails(); return "Management probe details"; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override { if (OnPackages) OnPackages(); return {}; }
		auto GetModifiedObjects() const -> std::vector<Durin::DObject*> override { return Participants; }
		std::vector<Durin::DObject*> Participants;
		auto AddReferencedObjects(Durin::FReferenceCollector& Collector) const -> void override
		{
			if (OnInspect) OnInspect();
			Durin::DObject* Object = Reference.Get();
			if (Object) Collector.AddReferencedObject(Object);
		}
		Durin::TObjectPtr<Durin::DObject> Reference;
		auto GetAllocatedSize() const -> size_t override { if (OnMeasure) OnMeasure(); return AllocatedBytes; }
		size_t AllocatedBytes = 0;
		std::function<void()> OnInspect;
		std::function<void()> OnDestroy;
		std::function<void()> OnMeasure;
		std::function<void()> OnDetails, OnPackages;
	};
}

TEST(FTransBufferTests, CancelTerminatesAfterFailedWholeObjectCapture)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem();
		Tests::FTestTransactorOwner Buffer;
		TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "Participant"));
		Tests::FBufferRecording Recording(Buffer.Get(), {.Description = "Capture participant"});
		auto Change = std::make_unique<FManagementProbeChange>();
		Change->Participants = {Target.Get()};
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
		ASSERT_TRUE(GUndo->ContainsObject(Target.Get()));
		Target->bFailSave = true;
		EXPECT_TRUE(Target->Modify(false));
		Target->Value = 5;
		Recording.Cancel();
	};
	EXPECT_DEATH(Run(), "during Cancel");
}

TEST(FTransBufferTests, OuterEndTerminatesOnFailedNestedCapture)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem();
		Tests::FTestTransactorOwner Buffer;
		int Value = 0;
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
		ASSERT_TRUE(Buffer->Undo());
		(void)MakeSoftPath();
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/NestedCapture", Path));
		TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, Path.GetPackageName()));
		Package->InitializeAssetPackage(Path);
		auto* Target = NewObject<DTransactionRecordParticipant>(Package.Get(), "Failing");
		Package->ClearDirty();
		Tests::FBufferRecording Outer(Buffer.Get(), {.Description = "Outer"});
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 2)));
		{
			Tests::FBufferRecording Inner(Buffer.Get(), {.Description = "Inner"});
			Target->bFailSave = true;
			ASSERT_TRUE(Target->Modify(false));
			Target->Value = 17;
		}
		ASSERT_TRUE(Buffer->IsActive());
		Outer.End();
	};
	EXPECT_DEATH(Run(), "Injected capture failure");
}

TEST(FTransBufferTests, AdmissionFailurePreservesHistoryAndOwningScope)
{
	using namespace Durin;
	InitializeDObjectSystem();
	for (bool bJoined : {false, true})
	{
		Tests::FTestEditorTransactorOwner Buffer;
		int Value = 0;
		ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
		const auto Id = Buffer->GetUndoContext().TransactionId;
		ASSERT_TRUE(Buffer->Undo());
		std::optional<FScopedTransaction> Outer(std::in_place, "Outer", bJoined);
		auto* Original = GUndo;
		auto Change = std::make_unique<FManagementProbeChange>();
		Change->OnPackages = [&] {
			auto* Admitting = GUndo;
			EXPECT_NE(Admitting, nullptr);
			Buffer->Cancel();
			Outer->Cancel();
			EXPECT_EQ(Outer->IsOutstanding(), bJoined);
			EXPECT_EQ(Buffer->End(), 0);
			Buffer->Reset(); EXPECT_FALSE(Buffer->CanTransact());
			EXPECT_LT(Buffer->Begin("Test", "Reentry"), 0);
			EXPECT_EQ(GUndo, Admitting);
			throw std::runtime_error("Admission failed");
		};
		std::string Message;
		ASSERT_FALSE(Buffer->StoreUndo(std::move(Change), &Message));
		if (bJoined)
		{
			EXPECT_EQ(Message, "Admission failed");
		}
		else
		{
			EXPECT_NE(Message.find("active recording"), std::string::npos);
		}
		EXPECT_EQ(GUndo, Original);
		EXPECT_EQ(Outer->IsOutstanding(), bJoined);
		EXPECT_EQ(Value, 0);
		EXPECT_EQ(Buffer->IsActive(), bJoined);
		EXPECT_TRUE(Buffer->CanTransact());
		Outer.reset();
		EXPECT_EQ(Buffer->GetUndoContext().TransactionId, 0u);
		EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Id);
		ASSERT_TRUE(Buffer->Redo());
		EXPECT_EQ(Value, 1);
	}
}

TEST(FTransBufferTests, ReplayDetailsFailurePreservesModelAndCursor)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Replay details"});
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	auto Change = std::make_unique<FManagementProbeChange>();
	auto* Probe = Change.get();
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	ASSERT_TRUE(Scope.End());
	const auto Id = Buffer->GetUndoContext().TransactionId;
	int Queries = 0;
	Probe->OnDetails = [&] { ++Queries; throw std::runtime_error("Replay detail failure"); };
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Id);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, 0u);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Queries, 2);
	EXPECT_EQ(Value, 1);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, 0u);
	EXPECT_TRUE(Buffer->CanTransact());
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(GUndo, nullptr);
}

TEST(FTransBufferTests, SynchronousNotificationsExposeTheRecordingAndRestoredRedoBranch)
{
	using namespace Durin;
	using EState = ETransactionStateEventType;
	InitializeDObjectSystem();
	for (const bool Cancel : {false, true})
	{
		Tests::FTestTransactorOwner Buffer;
		int Value = 0;
		for (int Before = 0; Before < 3; ++Before)
			ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, Before, Before + 1)));
		const auto* First = Buffer->GetTransaction(0);
		const auto* Second = Buffer->GetTransaction(1);
		const auto* Third = Buffer->GetTransaction(2);
		ASSERT_TRUE(Buffer->Undo());
		ASSERT_TRUE(Buffer->Undo());
		const auto PreviousBytes = Buffer->GetUndoSize();
		std::vector<std::optional<EState>> Seen;
		FTransactionId Id = 0;
		auto CheckCandidate = [&] {
			ASSERT_NE(GUndo, nullptr);
			EXPECT_EQ(Buffer->GetQueueLength(), 2u);
			EXPECT_EQ(Buffer->GetUndoCount(), 0u);
			EXPECT_EQ(Buffer->GetTransaction(0), First);
			EXPECT_EQ(Buffer->GetTransaction(1), GUndo);
			EXPECT_EQ(Buffer->GetUndoContext(false).TransactionId, Id);
			EXPECT_FALSE(Buffer->CanRedo());
		};
		auto CheckRestored = [&] {
			EXPECT_EQ(Buffer->GetQueueLength(), 3u);
			EXPECT_EQ(Buffer->GetUndoCount(), 2u);
			EXPECT_EQ(Buffer->GetTransaction(1), Second);
			EXPECT_EQ(Buffer->GetTransaction(2), Third);
			EXPECT_EQ(Buffer->GetUndoSize(), PreviousBytes);
		};
		const auto State = Buffer->OnTransactionStateChanged().AddLambda([&](const FTransactionContext& Context, EState Event) {
			Seen.push_back(Event);
			if (Event == EState::TransactionStarted) Id = Context.TransactionId;
			EXPECT_EQ(Context.TransactionId, Id);
			if (Event == EState::PostTransactionFinalized)
			{
				EXPECT_EQ(GUndo, nullptr);
				EXPECT_EQ(Context.OperationId, 0u);
				CheckRestored();
			}
			else
			{
				EXPECT_NE(Context.OperationId, 0u);
				CheckCandidate();
			}
		});
		const auto Changed = Buffer->OnUndoBufferChanged().AddLambda([&] {
			Seen.push_back(std::nullopt);
			if (Seen.size() == 2) CheckCandidate();
			else
			{
				CheckRestored();
				EXPECT_EQ(Buffer->IsActive(), Cancel);
				EXPECT_EQ(GUndo == nullptr, Cancel);
				EXPECT_EQ(Buffer->CanRedo(), !Cancel);
			}
		});
		ASSERT_EQ(Buffer->Begin("Edit", "Candidate"), 0);
		EXPECT_FALSE(Buffer->CanUndo());
		EXPECT_EQ(Buffer->GetUndoContext().TransactionId, 0u);
		ASSERT_EQ(Buffer->Begin("Edit", "Nested"), 1);
		ASSERT_EQ(Buffer->End(), 2);
		if (Cancel) Buffer->Cancel();
		else ASSERT_EQ(Buffer->End(), 1);
		const std::vector<std::optional<EState>> Expected = Cancel
			? std::vector<std::optional<EState>>{EState::TransactionStarted, std::nullopt, EState::TransactionCanceled, std::nullopt}
			: std::vector<std::optional<EState>>{EState::TransactionStarted, std::nullopt, EState::PreTransactionFinalized,
				EState::TransactionFinalized, std::nullopt, EState::PostTransactionFinalized};
		EXPECT_EQ(Seen, Expected);
		Buffer->OnTransactionStateChanged().Remove(State);
		Buffer->OnUndoBufferChanged().Remove(Changed);
		ASSERT_TRUE(Buffer->Redo());
		EXPECT_EQ(Value, 2);
	}
}

TEST(FTransBufferTests, BudgetPrefixReturnsBeforeTheRetainedSuffixInOriginalOrder)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	std::vector<const FTransaction*> Original;
	for (int Before = 0; Before < 4; ++Before)
	{
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, Before, Before + 1)));
		Original.push_back(Buffer->GetTransaction(Before));
	}
	const auto Bytes = Buffer->GetUndoSize();
	ASSERT_TRUE(Buffer->SetMemoryBudget(Bytes / 2 + 1));
	for (const bool Cancel : {true, false})
	{
		ASSERT_EQ(Buffer->Begin("Edit", "Under budget"), 0);
		ASSERT_EQ(Buffer->GetQueueLength(), 3u);
		EXPECT_EQ(Buffer->GetTransaction(0), Original[2]);
		EXPECT_EQ(Buffer->GetTransaction(1), Original[3]);
		EXPECT_EQ(Buffer->GetTransaction(2), GUndo);
		if (Cancel) Buffer->Cancel();
		else ASSERT_EQ(Buffer->End(), 1);
		ASSERT_EQ(Buffer->GetQueueLength(), Original.size());
		for (size_t Index = 0; Index < Original.size(); ++Index)
			EXPECT_EQ(Buffer->GetTransaction(Index), Original[Index]);
		EXPECT_EQ(Buffer->GetUndoSize(), Bytes);
		EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	}
}

TEST(FTransBufferTests, EffectiveEndRetiresHiddenReferencesWithoutAnotherBufferNotification)
{
	using namespace Durin;
	InitializeDObjectSystem();
	for (const bool RedoBranch : {false, true})
	{
		Tests::FTestTransactorOwner Buffer;
		auto* Dependency = NewObject<DObject>(nullptr, "RetainedByHiddenTransaction");
		const FObjectKey Key(Dependency);
		auto Old = std::make_unique<FManagementProbeChange>();
		Old->Reference = Dependency;
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::move(Old)));
		if (RedoBranch) ASSERT_TRUE(Buffer->Undo());
		else ASSERT_TRUE(Buffer->SetMemoryBudget(0));
		int Changes = 0;
		const auto Listener = Buffer->OnUndoBufferChanged().AddLambda([&] { ++Changes; });
		ASSERT_EQ(Buffer->Begin("Edit", "New branch"), 0);
		EXPECT_EQ(Buffer->GetQueueLength(), 1u);
		const auto EmptyBytes = Buffer->GetUndoSize();
		auto New = std::make_unique<FManagementProbeChange>();
		New->AllocatedBytes = 8192;
		auto* Probe = New.get();
		ASSERT_TRUE(Buffer->StoreUndo(std::move(New)));
		Probe->OnMeasure = [&] {
			EXPECT_FALSE(Buffer->CanTransact());
			EXPECT_FALSE(GUndo->EndOperation());
			EXPECT_EQ(Buffer->GetUndoSize(), std::numeric_limits<size_t>::max());
		};
		EXPECT_GE(Buffer->GetUndoSize(), EmptyBytes + 8192);
		Probe->OnMeasure = {};
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_NE(ResolveObjectKey(Key), nullptr);
		EXPECT_EQ(Buffer->End(), 1);
		EXPECT_EQ(Changes, 1);
		Buffer->OnUndoBufferChanged().Remove(Listener);
		CollectGarbage(EObjectFlags::Standalone);
		EXPECT_EQ(ResolveObjectKey(Key), nullptr);
		EXPECT_TRUE(Buffer->CanUndo());
	}
}

TEST(FTransBufferTests, PreTransactionFinalizedNestedEditJoinsTheOuterUndoEntry)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "EditedInBody"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "EditedBeforeFinalize"));
	FTransactionId Id = 0;
	int Calls = 0;
	const auto Handle = Buffer->OnPreTransactionFinalized().AddLambda([&](const FTransactionContext& Context) {
		++Calls;
		ASSERT_NE(GUndo, nullptr);
		EXPECT_EQ(Context.TransactionId, Id);
		EXPECT_EQ(GUndo->GetContext().OperationId, Context.OperationId);
		EXPECT_EQ(Buffer->GetQueueLength(), 1u);
		EXPECT_EQ(Buffer->GetTransaction(0), GUndo);
		EXPECT_EQ(Buffer->End(), 0);
		FScopedTransaction Inner("Nested edit");
		ASSERT_TRUE(Inner.IsRecordingCurrent());
		EXPECT_EQ(GUndo->GetContext().TransactionId, Id);
		ASSERT_TRUE(Second->Modify());
		Second->Value = 22;
	});
	{
		FScopedTransaction Outer("Outer edit");
		ASSERT_TRUE(Outer.IsRecordingCurrent());
		Id = GUndo->GetContext().TransactionId;
		ASSERT_TRUE(First->Modify());
		First->Value = 11;
	}
	Buffer->OnPreTransactionFinalized().Remove(Handle);
	EXPECT_EQ(Calls, 1);
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_EQ(Buffer->GetUndoContext().Description, "Outer edit");
	EXPECT_EQ(First->Value, 11);
	EXPECT_EQ(Second->Value, 22);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(First->Value, 0);
	EXPECT_EQ(Second->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(First->Value, 11);
	EXPECT_EQ(Second->Value, 22);
}

TEST(FTransBufferTests, PreFinalizeCancellationLeavesReplacementRecordingOpen)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	ASSERT_EQ(Buffer->Begin("test", "Old edit"), 0);
	const auto Handle = Buffer->OnPreTransactionFinalized().AddLambda([&](const FTransactionContext&) {
		Buffer->Cancel();
		ASSERT_EQ(Buffer->Begin("test", "Replacement"), 0);
	});
	EXPECT_EQ(Buffer->End(), 1);
	Buffer->OnPreTransactionFinalized().Remove(Handle);
	ASSERT_NE(GUndo, nullptr);
	EXPECT_EQ(GUndo->GetContext().Description, "Replacement");
	EXPECT_TRUE(Buffer->IsActive());
	Buffer->Cancel();
}

TEST(FTransBufferTests, PreFinalizeFaultIsTerminal)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem();
		Tests::FTestTransactorOwner Buffer;
		int Value = 0;
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
		ASSERT_EQ(Buffer->Begin("test", "Throwing listener"), 0);
		const auto Handle = Buffer->OnPreTransactionFinalized().AddLambda([&](const FTransactionContext&) {
			Value = 2;
			throw std::runtime_error("Pre-finalize failed");
		});
		Buffer->End();
	};
	EXPECT_DEATH(Run(), "Pre-finalize failed");
}

TEST(FTransBufferTests, FinalizedPropertyObserverFaultIsTerminal)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem();
		Tests::FTestEditorTransactorOwner Buffer;
		TStrongObjectPtr<DTransactionRecordParticipant> Object(NewObject<DTransactionRecordParticipant>(nullptr, "PublicationFailure"));
		auto* Property = Object->GetClass()->FindPropertyByName("Value");
		ASSERT_NE(Property, nullptr);
		Object->Value = 37;
		FPropertyValueSnapshot Proposed;
		ASSERT_TRUE(CapturePropertyValue(Property, Object.Get(), 0, Proposed));
		Object->Value = 0;
		Durin::PropertyEditor::FPropertyEdit Edit;
		ASSERT_TRUE(Edit.Begin(FPropertyEditTarget::ForMember(Object.Get(), Property), "Set value", *Buffer));
		ASSERT_TRUE(Edit.Apply(Proposed));
		Object->TransactionObserver = [](const auto&) { throw std::runtime_error("Finalization observer failed"); };
		Edit.Commit();
	};
	EXPECT_DEATH(Run(), "Finalization observer failed");
}

TEST(FTransBufferTests, PublishedDirtyNotificationFaultIsTerminal)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem();
		Tests::FTestTransactorOwner Buffer;
		int Value = 0;
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
		ASSERT_TRUE(Buffer->Undo());
		ASSERT_TRUE(Buffer->CanUndo());
		ASSERT_TRUE(Buffer->CanRedo());

		(void)MakeSoftPath();
		TStrongObjectPtr<DPackage> First(NewObject<DPackage>(nullptr, "PublicationFirst"));
		TStrongObjectPtr<DPackage> Second(NewObject<DPackage>(nullptr, "PublicationSecond"));
		for (auto* Package : {First.Get(), Second.Get()})
		{
			FPackagePath Path;
			ASSERT_TRUE(FPackagePath::TryCreate(Package == First.Get() ? "/TransactionTests/PublicationFirst" : "/TransactionTests/PublicationSecond", Path));
			Package->InitializeAssetPackage(Path);
		}
		TStrongObjectPtr<DTransactionRecordParticipant> A(NewObject<DTransactionRecordParticipant>(First.Get(), "FirstOwner"));
		TStrongObjectPtr<DTransactionRecordParticipant> B(NewObject<DTransactionRecordParticipant>(Second.Get(), "SecondOwner"));
		ASSERT_EQ(Buffer->Begin("test", "Published edit"), 0);
		ASSERT_TRUE(A->Modify(false));
		ASSERT_TRUE(B->Modify(false));
		A->Value = 11;
		B->Value = 22;
		First->ClearDirty();
		Second->ClearDirty();
		const auto Listener = OnAssetPackagesChanged().AddLambda([&] {
			EXPECT_EQ(Buffer->GetQueueLength(), 2u);
			EXPECT_EQ(Buffer->GetUndoCount(), 0u);
			EXPECT_EQ(GUndo, nullptr);
			EXPECT_FALSE(Buffer->CanTransact());
			throw std::runtime_error("Injected post-insertion dirty notification failure.");
		});
		Buffer->End();
	};
	EXPECT_DEATH(Run(), "Injected post-insertion dirty notification failure");
}

TEST(FTransBufferTests, InspectionFaultIsReportedBeforeAnyDirtyNotification)
{
	auto Run = [&] {
		using namespace Durin;
		InitializeDObjectSystem();
		Tests::FTestTransactorOwner Buffer;
		int Value = 0;
		ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
		ASSERT_TRUE(Buffer->Undo());
		ASSERT_TRUE(Buffer->CanRedo());
		(void)MakeSoftPath();
		FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/RejectedCandidate", Path));
		TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, Path.GetPackageName()));
		Package->InitializeAssetPackage(Path);
		TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Participant"));
		ASSERT_EQ(Buffer->Begin("test", "Rejected edit"), 0);
		ASSERT_TRUE(Target->Modify(false));
		Target->Value = 4;
		auto Change = std::make_unique<FManagementProbeChange>();
		Change->OnInspect = [] { throw std::runtime_error("Inspection failed"); };
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
		Package->ClearDirty();
		const auto Handle = OnAssetPackagesChanged().AddLambda([] { throw std::runtime_error("Cleanup callback failed"); });
		Buffer->End();
	};
	EXPECT_DEATH(Run(), "Inspection failed");
}

TEST(FTransBufferTests, UndoWithoutRedoRetiresCommandsUnderGuard)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	const auto Id = Buffer->GetUndoContext().TransactionId;
	auto Change = std::make_unique<FManagementProbeChange>();
	auto* Probe = Change.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Change)));
	Probe->OnMeasure = [] { throw std::runtime_error("Discarded command measured during Undo"); };
	bool Destroyed = false;
	Probe->OnDestroy = [&] {
		Destroyed = true;
		EXPECT_FALSE(Buffer->CanTransact());
		Buffer->Reset(); EXPECT_FALSE(Buffer->CanTransact());
		EXPECT_FALSE(Buffer->CanRedo());
	};
	ASSERT_TRUE(Buffer->Undo(false));
	EXPECT_TRUE(Destroyed);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_FALSE(Buffer->CanRedo());
	EXPECT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
}

TEST(FTransBufferTests, ResetPurgesActiveScopesWithoutReentryOrChangingReason)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	DTransactor& Transactor = *Buffer;
	static_assert(std::is_same_v<decltype(Transactor.Reset()), void>);
	static_assert(std::is_same_v<decltype(Transactor.ContainsPieObjects()), bool>);
	static_assert(std::is_same_v<decltype(Transactor.GetCurrentUndoBarrier()), int32>);
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ActiveResetPackage"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ActiveResetPackage", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Target"));
	Package->ClearDirty();
	std::string Reason(256, 'R'); const auto OriginalReason = Reason;
	int Destroyed = 0;
	ITransaction* Recording = nullptr;
	auto OnDestroy = [&](bool bPending) {
		return [&, bPending] {
			++Destroyed; Reason.assign(Reason.size(), 'X');
			ASSERT_FALSE(Transactor.CanTransact()); EXPECT_FALSE(Transactor.CanUndo());
			EXPECT_EQ(Transactor.GetQueueLength(), 0u); EXPECT_EQ(Transactor.GetUndoSize(), 0u);
			EXPECT_EQ(Transactor.GetUndoContext().TransactionId, 0u); EXPECT_EQ(Transactor.GetRedoContext().TransactionId, 0u);
			EXPECT_EQ(Transactor.GetTransaction(0), nullptr);
			Transactor.Reset(); EXPECT_FALSE(Transactor.CanTransact()); EXPECT_LT(Transactor.Begin("Probe", "Reentry"), 0);
			EXPECT_TRUE(Transactor.ContainsPieObjects());
			if (bPending)
			{
				EXPECT_FALSE(Recording->BeginOperation()); EXPECT_FALSE(Recording->EndOperation());
				EXPECT_FALSE(Recording->ContainsPieObjects());
			}
		};
	};
	auto HistoryChange = std::make_unique<FManagementProbeChange>(); auto* HistoryProbe = HistoryChange.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(Transactor, std::move(HistoryChange))); Transactor.SetUndoBarrier();
	Durin::Tests::FBufferRecording Outer(&Transactor, {.Description = "Pending outer"});
	Durin::Tests::FBufferRecording Inner(&Transactor, {.Description = "Pending inner"});
	ASSERT_TRUE(Target->Modify()); Target->Value = 37;
	auto PendingChange = std::make_unique<FManagementProbeChange>(); auto* PendingProbe = PendingChange.get();
	ASSERT_TRUE(Transactor.StoreUndo(std::move(PendingChange))); Recording = GUndo;
	HistoryProbe->OnDestroy = OnDestroy(false); PendingProbe->OnDestroy = OnDestroy(true);
	Transactor.Reset(Reason); EXPECT_EQ(Destroyed, 2);
	EXPECT_EQ(Transactor.GetQueueLength(), 0u); EXPECT_EQ(Transactor.GetUndoSize(), 0u);
	EXPECT_EQ(Transactor.GetCurrentUndoBarrier(), -1); EXPECT_EQ(GUndo, nullptr);
	EXPECT_FALSE(Transactor.IsActive());
	EXPECT_EQ(Target->Value, 37); EXPECT_TRUE(Package->IsDirty()); std::string Explanation; EXPECT_FALSE(Transactor.CanUndo(&Explanation)); EXPECT_EQ(Explanation, OriginalReason);
	EXPECT_TRUE(ReadTransactionEvents(*Buffer).empty());
	EXPECT_EQ(Inner.End(), 0); Outer.Cancel();
	ASSERT_GE(Transactor.Begin("Replacement", "New recording"), 0); EXPECT_TRUE(Transactor.IsActive()); Transactor.Cancel();
}

TEST(FTransBufferTests, PieQueriesPreservePositiveAndFailedInspectionAcrossRedo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	DTransactor& Transactor = *Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "QueryDependency"));
	auto Change = std::make_unique<FManagementProbeChange>(); auto* Probe = Change.get();
	Probe->Reference = NewObject<DObject>(Package.Get(), "Dependency");
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(Transactor, std::move(Change))); EXPECT_FALSE(Transactor.ContainsPieObjects());
	Probe->OnInspect = [] { throw std::runtime_error("Injected PIE inspection failure"); };
	EXPECT_TRUE(Transactor.ContainsPieObjects());
	Probe->OnInspect = {};
	EXPECT_FALSE(Transactor.ContainsPieObjects());
	Package->SetPlayInEditor(true);
	EXPECT_TRUE(Transactor.ContainsPieObjects());
	Package->SetPlayInEditor(false); ASSERT_TRUE(Transactor.Undo()); EXPECT_TRUE(Transactor.CanRedo());
	Package->SetPlayInEditor(true); EXPECT_TRUE(Transactor.ContainsPieObjects());
	Package->SetPlayInEditor(false);
}

TEST(FTransBufferTests, RetainedPieReferenceRejectsReplayAndPreservesBothHeads)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ReplayAdmissionDependency"));
	(void)MakeSoftPath();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ReplayAdmission", Path));
	Package->InitializeAssetPackage(Path);
	auto Change = std::make_unique<FManagementProbeChange>();
	Change->Reference = NewObject<DObject>(Package.Get(), "Dependency");
	Change->Participants.push_back(Change->Reference.Get());
	ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::move(Change)));
	int Value = 0;
	ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	ASSERT_TRUE(Buffer->Undo());
	const auto UndoId = Buffer->GetUndoContext().TransactionId;
	const auto RedoId = Buffer->GetRedoContext().TransactionId;
	Package->SetPlayInEditor(true);
	Package->ClearDirty();
	std::string Reason;
	EXPECT_FALSE(Buffer->UndoExpected(UndoId, &Reason));
	EXPECT_NE(Reason.find("Play-in-Editor"), std::string::npos);
	EXPECT_FALSE(Package->IsDirty());
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, UndoId);
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, RedoId);
	Package->SetPlayInEditor(false);
	ASSERT_TRUE(Buffer->RedoExpected(RedoId));
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->UndoExpected(RedoId));
	ASSERT_TRUE(Buffer->UndoExpected(UndoId));
}

TEST(FTransBufferTests, PieQueriesGuardHistoryAndPendingRecordsAgainstReentry)
{
	using namespace Durin;
	InitializeDObjectSystem();
	for (bool bPending : {false, true})
	{
		Tests::FTestTransactorOwner Buffer;
		std::optional<Durin::Tests::FBufferRecording> Scope;
		Scope.emplace(Buffer.Get(), FTransactionContext{.Description = "Pending query"});
		auto Change = std::make_unique<FManagementProbeChange>(); auto* Probe = Change.get();
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
		if (!bPending) { Scope.reset(); Buffer->SetUndoBarrier(); }
		int Visits = 0;
		Probe->OnInspect = [&] {
			++Visits;
			ASSERT_FALSE(Buffer->CanTransact());
			EXPECT_FALSE(Buffer->CanUndo()); EXPECT_FALSE(Buffer->CanRedo());
			const auto Barrier = Buffer->GetCurrentUndoBarrier();
			Buffer->SetUndoBarrier(); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), Barrier);
			Buffer->RemoveUndoBarrier(); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), Barrier);
			Buffer->ClearUndoBarriers(); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), Barrier);
			Buffer->Reset(); EXPECT_FALSE(Buffer->CanTransact());
			EXPECT_TRUE(Buffer->ContainsPieObjects()); EXPECT_FALSE(Buffer->Undo());
			if (GUndo) { EXPECT_FALSE(GUndo->ContainsPieObjects()); EXPECT_FALSE(GUndo->EndOperation()); }
		};
		EXPECT_FALSE(Buffer->ContainsPieObjects()); EXPECT_EQ(Visits, 1);
		EXPECT_FALSE(Buffer->ContainsPieObjects());
		Probe->OnInspect = {};
		EXPECT_EQ(Visits, 2);
		EXPECT_TRUE(Buffer->CanTransact()); EXPECT_EQ(Buffer->IsActive(), bPending);
		Scope.reset();
		Buffer->ClearUndoBarriers();
		ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo());
	}
}
TEST(FTransBufferTests, TransientPieEndKeepsDestructorsGuarded)
{
	using namespace Durin;
	InitializeDObjectSystem();

	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	ASSERT_TRUE(Buffer->Undo()); const auto RedoId = Buffer->GetRedoContext().TransactionId;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "RejectedQueryPie"));
	Package->SetPlayInEditor(true);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CaptureTarget"));
	auto* Dependency = NewObject<DObject>(Package.Get(), "Dependency"); const FObjectKey Key(Dependency);
	Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Rejected finalization"});
	ITransaction* Recording = GUndo; const auto PendingId = Recording->GetContext().TransactionId;
	auto Change = std::make_unique<FManagementProbeChange>(); Change->Reference = Dependency;
	int Inspected = 0, Destroyed = 0;
	Change->OnInspect = [&] {
		++Inspected; EXPECT_FALSE(Buffer->CanTransact());
	};
	Change->OnDestroy = [&, Recording] {
		++Destroyed; ASSERT_FALSE(Buffer->CanTransact());
		Buffer->Reset(); EXPECT_FALSE(Buffer->CanTransact()); EXPECT_LT(Buffer->Begin("Reentry", "Destructor"), 0);
		EXPECT_FALSE(Recording->BeginOperation()); EXPECT_FALSE(Recording->ContainsPieObjects());
		EXPECT_EQ(Buffer->GetTransaction(0), nullptr); EXPECT_EQ(Buffer->GetRedoContext().TransactionId, 0u);
		RequestGarbageCollection(); EXPECT_TRUE(IsGarbageCollectionRequested()); EXPECT_NE(ResolveObjectKey(Key), nullptr);
	};
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	EXPECT_EQ(Scope.End(), 1);
	const auto Events = ReadTransactionEvents(*Buffer);
	ASSERT_FALSE(Events.empty());
	EXPECT_EQ(Events.back().Type, ETransactionEventType::Discarded);
	EXPECT_EQ(Events.back().Id, PendingId);
	EXPECT_EQ(Inspected, 1); EXPECT_EQ(Destroyed, 1); EXPECT_TRUE(Buffer->CanTransact());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetRedoContext().TransactionId, RedoId);
	CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_EQ(ResolveObjectKey(Key), nullptr); EXPECT_TRUE(Buffer->Redo());
}

TEST(FTransBufferTests, FinalizationMetadataRemainsGuardedAfterPieQuery)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	auto Retired = std::make_shared<bool>(false);
	auto OldRedo = std::make_unique<FManagementProbeChange>(); OldRedo->OnDestroy = [Retired] { *Retired = true; };
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(OldRedo))); ASSERT_TRUE(Buffer->Undo());
	Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Finalization metadata"});
	ITransaction* Recording = GUndo;
	auto Change = std::make_unique<FManagementProbeChange>(); auto* Probe = Change.get();
	bool bInspected = false; int DetailsCalls = 0;
	Probe->OnInspect = [&] { bInspected = true; };
	Probe->OnDetails = [&] {
		if (*Retired) throw std::runtime_error("Details callback after Redo retirement");
		++DetailsCalls; EXPECT_TRUE(bInspected); ASSERT_FALSE(Buffer->CanTransact());
		if (DetailsCalls == 1) EXPECT_FALSE(Recording->BeginOperation());
		Buffer->Reset(); EXPECT_FALSE(Buffer->CanTransact()); EXPECT_LT(Buffer->Begin("Reentry", "Details"), 0);
		RequestGarbageCollection(); EXPECT_TRUE(IsGarbageCollectionRequested());
	};
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change))); EXPECT_TRUE(Scope.End());
	Probe->OnInspect = {}; Probe->OnDetails = {};
	EXPECT_GT(DetailsCalls, 0); EXPECT_TRUE(*Retired); EXPECT_TRUE(Buffer->CanTransact());
	CollectGarbage(Durin::EObjectFlags::Standalone); ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo());
}
TEST(FTransBufferTests, UnknownFinalizationExceptionIsDiagnosed)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto Run = [] {
		Tests::FTestTransactorOwner Buffer;
		Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Throwing metadata"});
		auto Change = std::make_unique<FManagementProbeChange>();
		Change->OnDetails = [] { throw 42; };
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
		Scope.End();
	};
	EXPECT_DEATH(Run(), "unknown exception");
}

TEST(FTransBufferTests, EndReplayAndRedoBranchReplacementSkipMeasurement)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	auto Change = std::make_unique<FManagementProbeChange>();
	Change->OnMeasure = [] { ADD_FAILURE() << "Unexpected history measurement"; };
	const auto Id = Tests::RecordTestEdit(*Buffer, std::move(Change));
	ASSERT_NE(Id, 0u);
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_EQ(Buffer->Begin("Edit", "Temporary branch"), 0);
	Buffer->Cancel();
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Id);
	ASSERT_TRUE(Buffer->RedoExpected(Id));
}

TEST(FTransBufferTests, AggregateSizeOverflowRejectsBudgetedBeginAndZeroBudgetCanSetAsideAllEntries)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	auto First = std::make_unique<FManagementProbeChange>();
	auto* FirstProbe = First.get();
	ASSERT_TRUE(Tests::RecordTestEdit(*Buffer, std::move(First)));
	auto Second = std::make_unique<FManagementProbeChange>();
	auto* SecondProbe = Second.get();
	const auto Head = Tests::RecordTestEdit(*Buffer, std::move(Second));
	ASSERT_NE(Head, 0u);
	FirstProbe->AllocatedBytes = SecondProbe->AllocatedBytes = std::numeric_limits<size_t>::max() / 2;
	Buffer->SetUndoBarrier();
	EXPECT_EQ(Buffer->GetUndoSize(), std::numeric_limits<size_t>::max());
	EXPECT_LT(Buffer->Begin("Edit", "Cannot measure total"), 0);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 2);
	ASSERT_TRUE(Buffer->SetMemoryBudget(0));
	ASSERT_EQ(Buffer->Begin("Edit", "Set aside all"), 0);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	Buffer->Cancel();
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	Buffer->RemoveUndoBarrier();
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Head);
}

TEST(FTransBufferTests, ScopedDestructionTerminatesOnFinalizationFault)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	auto EndScope = [&]
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Destructor finalization"});
		auto Change = std::make_unique<FManagementProbeChange>();
		Change->OnDetails = [] { throw std::runtime_error("Scoped finalization exception"); };
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	};
	EXPECT_DEATH(EndScope(), "Scoped finalization exception");
}
TEST(FTransBufferTests, ExposesReflectedIdentityAndProductionMemoryBudget)
{
	InitializeDObjectSystem();
	EXPECT_TRUE(Durin::DTransactor::StaticClass()->HasAnyClassFlags(
		Durin::EClassFlags::Abstract));
	EXPECT_FALSE(Durin::DTransBuffer::StaticClass()->HasAnyClassFlags(
		Durin::EClassFlags::Abstract));
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "IdentityTransBuffer");
	ASSERT_NE(Buffer, nullptr);
	EXPECT_TRUE(Buffer->IsA<Durin::DTransactor>());
	EXPECT_EQ(Buffer->GetMemoryBudget(), 64u * 1024u * 1024u);
}

TEST(FTransBufferTests, CompositeParticipantsReceiveHooksWithoutEditingGcDependencies)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(nullptr, "CompositeFirst"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(nullptr, "CompositeSecond"));
	TStrongObjectPtr<DTransactionRecordParticipant> Dependency(NewObject<DTransactionRecordParticipant>(nullptr, "CompositeDependency"));
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FDeclaredParticipantChange>(First.Get(), Second.Get(), Dependency.Get())));
	const auto* Transaction = Buffer->GetTransaction(0);
	ASSERT_NE(Transaction, nullptr);
	EXPECT_TRUE(Transaction->ContainsObject(First.Get()));
	EXPECT_TRUE(Transaction->ContainsObject(Second.Get()));
	EXPECT_FALSE(Transaction->ContainsObject(Dependency.Get()));
	EXPECT_EQ(Transaction->GetTransactionObjects().size(), 2u);
	const auto FirstKey = FObjectKey(First.Get());
	First = TStrongObjectPtr<DTransactionRecordParticipant>(nullptr);
	CollectGarbage(Durin::EObjectFlags::Standalone);
	auto* FirstObject = static_cast<DTransactionRecordParticipant*>(ResolveObjectKey(FirstKey));
	ASSERT_NE(FirstObject, nullptr);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(FirstObject->Value, 0);
	EXPECT_EQ(Second->Value, 0);
	EXPECT_EQ(FirstObject->UndoBegins, 1);
	EXPECT_EQ(FirstObject->UndoCompletions, 1);
	EXPECT_EQ(Second->UndoBegins, 1);
	EXPECT_EQ(Second->UndoCompletions, 1);
	EXPECT_EQ(Dependency->UndoBegins, 0);
	EXPECT_EQ(Dependency->UndoCompletions, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(FirstObject->Value, 3);
	EXPECT_EQ(Second->Value, 5);
	EXPECT_EQ(FirstObject->UndoCompletions, 2);
	Buffer->Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(ResolveObjectKey(FirstKey), nullptr);
}

TEST(FTransBufferTests, CompositeParticipantPackagesAndExpiryAreExplicit)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> FirstPackage(NewObject<DPackage>(nullptr, "ParticipantPackageFirst"));
	TStrongObjectPtr<DPackage> SecondPackage(NewObject<DPackage>(nullptr, "ParticipantPackageSecond"));
	TStrongObjectPtr<DTransactionRecordParticipant> First(NewObject<DTransactionRecordParticipant>(FirstPackage.Get(), "First"));
	TStrongObjectPtr<DTransactionRecordParticipant> Second(NewObject<DTransactionRecordParticipant>(SecondPackage.Get(), "Second"));
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FDeclaredParticipantChange>(First.Get(), Second.Get(), nullptr)));
	const auto Packages = Buffer->GetTransaction(0)->GetAffectedPackages();
	EXPECT_EQ(Packages.size(), 2u);
	EXPECT_TRUE(std::ranges::contains(Packages, FirstPackage.Get()));
	EXPECT_TRUE(std::ranges::contains(Packages, SecondPackage.Get()));
	MarkAsGarbage(Second.Get());
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(First->Value, 3);
	EXPECT_EQ(First->UndoBegins, 0);
	EXPECT_EQ(First->UndoCompletions, 0);
}

TEST(FTransBufferTests, RuntimeBoundaryPreservesOperationContextAndRejectsInvalidRequests)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RuntimeBoundChange"));
	ASSERT_EQ(Buffer->Begin("runtime", "Runtime bound command"), 0);
	ITransaction& Recording = *GUndo;
	Recording.SetPrimaryObject(Target.Get());
	const auto Context = Recording.GetContext();
	EXPECT_NE(Context.TransactionId, 0u);
	EXPECT_NE(Context.OperationId, 0u);
	EXPECT_EQ(Context.PrimaryObject, Target.Get());
	EXPECT_FALSE(Recording.BeginOperation());
	ASSERT_EQ(Buffer->Begin("nested", "Nested"), 1);
	EXPECT_EQ(GUndo->GetContext().OperationId, Context.OperationId);
	EXPECT_EQ(Buffer->End(), 2);
	EXPECT_FALSE(Recording.SaveArray({.Object = Target.Get()}));
	ASSERT_TRUE(Recording.ContainsPieObjects());
	EXPECT_FALSE(*Recording.ContainsPieObjects());
	std::vector<uint64> ReplayOperations;
	auto Change = std::make_unique<FTestCustomChange>(Target->Value, 0, 9);
	Change->ReplayProbe = [&] { ReplayOperations.push_back(Buffer->GetTransaction(0)->GetContext().OperationId); };
	Target->Value = 9;
	ASSERT_TRUE(Recording.StoreUndo(Target.Get(), std::move(Change)));
	EXPECT_EQ(Buffer->End(), 1);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetContext().OperationId, 0u);
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Buffer->Redo());
	ASSERT_EQ(ReplayOperations.size(), 2u);
	EXPECT_NE(ReplayOperations[0], 0u);
	EXPECT_NE(ReplayOperations[0], ReplayOperations[1]);
	EXPECT_NE(ReplayOperations[0], Context.OperationId);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetContext().OperationId, 0u);
}

TEST(FTransBufferTests, ModifyUsesVirtualSerializationAndOneRecordPerObject)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "VirtualModify"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Native and reflected data"});
		ASSERT_TRUE(Target->Modify()); ASSERT_TRUE(Target->Modify());
		EXPECT_EQ(Target->SerializationSaves, 1);
		Target->Value = 3; Target->NativeValue = 7;
	}
	EXPECT_EQ(Target->SerializationSaves, 2);
	ASSERT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->NativeValue, 0);
	EXPECT_EQ(Target->SerializationLoads, 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->NativeValue, 7);
	EXPECT_EQ(Target->SerializationLoads, 2);
}

TEST(FTransBufferTests, ExplicitRecordsAndModifyShareFirstBeforeAndFinalAfter)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "RepeatedModify"));
	Target->NativeValue = 5;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Repeated whole capture"});
		ASSERT_TRUE(Target->Modify(false));
		Target->Value = 1;
		ASSERT_TRUE(Target->Modify()); Target->Value = 4; Target->SessionValue = 9; Target->NativeValue = 10;
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SessionValue, 0); EXPECT_EQ(Target->NativeValue, 5);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 4); EXPECT_EQ(Target->SessionValue, 9); EXPECT_EQ(Target->NativeValue, 10);
}

TEST(FTransBufferTests, SerializedMetadataRestoresNameOuterAndFlagsWithoutChangingIdentity)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DObject> First(NewObject<DObject>(nullptr, "MetadataFirstOuter"));
	TStrongObjectPtr<DObject> Second(NewObject<DObject>(nullptr, "MetadataSecondOuter"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(First.Get(), "BeforeMetadata"));
	const auto Key = FObjectKey(Target.Get());
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Metadata"}); ASSERT_TRUE(Target->Modify());
		Target->Rename("AfterMetadata"); Target->SetOuterPrivate(Second.Get()); Target->SetFlags(EObjectFlags::Public);
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->GetFName(), FName("BeforeMetadata")); EXPECT_EQ(Target->GetOuter(), First.Get());
	EXPECT_FALSE(Target->HasAnyObjectFlags(EObjectFlags::Public)); EXPECT_EQ(FObjectKey(Target.Get()), Key);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->GetFName(), FName("AfterMetadata")); EXPECT_EQ(Target->GetOuter(), Second.Get());
	EXPECT_TRUE(Target->HasAnyObjectFlags(EObjectFlags::Public)); EXPECT_EQ(FObjectKey(Target.Get()), Key);
}

TEST(FTransBufferTests, WholeObjectFlipRetainsLivePreUndoStateForRedo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "LiveFlipState"));
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Flip state"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; Target->NativeValue = 2; }
	Target->Value = 11; Target->NativeValue = 22;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->NativeValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 11); EXPECT_EQ(Target->NativeValue, 22);
}

TEST(FTransBufferTests, NativeReplayFaultIsTerminal)
{
	using namespace Durin;
	InitializeDObjectSystem();
	for (bool bNativeException : {false, true})
	{
		SCOPED_TRACE(bNativeException);
		Tests::FTestTransactorOwner Buffer;
		TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "FailedNativeReplay"));
		{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Native replay failure"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
		Target->Value = 11; Target->NativeValue = 22;
		Target->bThrowLoadOnce = bNativeException;
		Target->FailedLoadsRemaining = bNativeException ? 0 : 2;
		EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
	}
}

TEST(FTransBufferTests, WholeObjectContainersPreserveExcludedFieldsOfMatchingElements)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NestedContainerState"));
	Target->NestedArray = {{.Count = 1, .Session = 2, .Ignored = 3}};
	Target->NestedMap["same"] = {.Count = 4, .Session = 5, .Ignored = 6};
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Nested container projection"}); ASSERT_TRUE(Target->Modify());
		Target->NestedArray[0] = {.Count = 10, .Session = 20, .Ignored = 30};
		Target->NestedArray.push_back({.Count = 11});
		Target->NestedMap["same"] = {.Count = 40, .Session = 50, .Ignored = 60};
		Target->NestedMap["added"] = {.Count = 12};
	}
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_EQ(Target->NestedArray.size(), 1u);
	EXPECT_EQ(Target->NestedArray[0].Count, 1); EXPECT_EQ(Target->NestedArray[0].Session, 2); EXPECT_EQ(Target->NestedArray[0].Ignored, 30);
	ASSERT_EQ(Target->NestedMap.size(), 1u);
	EXPECT_EQ(Target->NestedMap.at("same").Count, 4); EXPECT_EQ(Target->NestedMap.at("same").Session, 5); EXPECT_EQ(Target->NestedMap.at("same").Ignored, 60);
	Target->NestedArray[0].Ignored = 31; Target->NestedMap.at("same").Ignored = 61;
	ASSERT_TRUE(Buffer->Redo());
	ASSERT_EQ(Target->NestedArray.size(), 2u); ASSERT_EQ(Target->NestedMap.size(), 2u);
	EXPECT_EQ(Target->NestedArray[0].Count, 10); EXPECT_EQ(Target->NestedArray[0].Ignored, 31);
	EXPECT_EQ(Target->NestedMap.at("same").Count, 40); EXPECT_EQ(Target->NestedMap.at("same").Ignored, 61);
}

TEST(FTransBufferTests, ThrowingWholeObjectNotificationTerminatesUndo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingObjectObserver"));
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Observer exception"}); ASSERT_TRUE(Target->Modify()); Target->Value = 4; }
	Target->bThrowPropertyNotification = true;
	EXPECT_DEATH((void)Buffer->Undo(), "during Undo");
}

TEST(FTransBufferTests, MultipleWholeCapturesCoalesceAcrossNestedMemberEdits)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CanonicalWholeMembers"));
	auto RecordEdit = [&](int32& Value, int32 Next) -> bool {
		if (!Target->Modify(false)) return false;
		Value = Next;
		return true;
	};
	{
		Durin::Tests::FBufferRecording Outer(Buffer.Get(), {"test", "Multiple members"});
		ASSERT_TRUE(RecordEdit(Target->Value, 1));
		{ Durin::Tests::FBufferRecording Inner(Buffer.Get(), {"test", "Nested member"}); ASSERT_TRUE(RecordEdit(Target->SessionValue, 2)); }
		ASSERT_TRUE(RecordEdit(Target->Value, 3));
	}
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->SessionValue, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->SessionValue, 2);
	{
		Durin::Tests::FBufferRecording Outer(Buffer.Get(), {"test", "Modify before nested member edit"}); ASSERT_TRUE(Target->Modify());
		ASSERT_TRUE(RecordEdit(Target->Value, 4)); Target->NativeValue = 6;
	}
	EXPECT_EQ(Buffer->GetTransaction(1)->GetRecordCount(), 1u);
	const auto Loads = Target->SerializationLoads;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 3); EXPECT_EQ(Target->NativeValue, 0); EXPECT_EQ(Target->SerializationLoads, Loads + 1);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 4); EXPECT_EQ(Target->NativeValue, 6);
}

TEST(FTransBufferTests, NativeOnlyChangeRetainsHistoryAndUnchangedNativeStatePrunes)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NativeOnlyChange"));
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Native state"}); ASSERT_TRUE(Target->Modify()); Target->NativeValue = 13; }
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->NativeValue, 0);
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Native no-op"}); ASSERT_TRUE(Target->Modify()); Target->NativeValue = 7; Target->NativeValue = 0; }
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_TRUE(Buffer->CanRedo());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->NativeValue, 13);
}

TEST(FTransBufferTests, ScopedModifyRecordsChangedMembersAndPrunesNoOps)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ModifyTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "ModifyTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);

	{
		Durin::Tests::FBufferRecording NoChange(Buffer, {"test", "No change"});
		(void)Target->Modify();
		(void)Target->Modify();
		EXPECT_EQ(NoChange.End(), 1);
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);

	{
		Durin::Tests::FBufferRecording Change(Buffer, {"test", "Change value"});
		(void)Target->Modify();
		Target->Value = 42;
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 42);

	{
		Durin::Tests::FBufferRecording Canceled(Buffer, {"test", "Canceled value"});
		(void)Target->Modify();
		Target->Value = 81;
		Canceled.Cancel();
		EXPECT_FALSE(Buffer->IsActive());
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Target->Value, 81);
}

TEST(FTransBufferTests, UndoCountTracksUndoneEntriesAcrossReplayAndBranchReplacement)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "UndoCountBuffer"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "UndoCountTarget"));
	auto ExpectCounts = [&](size_t Undoable, size_t Undone) {
		EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), Undoable);
		EXPECT_EQ(Buffer->GetUndoCount(), Undone);
		EXPECT_EQ(Buffer->GetQueueLength(), Undoable + Undone);
	};
	auto Commit = [&] {
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Change value"});
		EXPECT_TRUE(Target->Modify());
		++Target->Value;
	};
	ExpectCounts(0, 0);
	Commit(); Commit(); Commit();
	ExpectCounts(3, 0);
	ASSERT_TRUE(Buffer->Undo());
	ExpectCounts(2, 1);
	ASSERT_TRUE(Buffer->Undo());
	ExpectCounts(1, 2);
	ASSERT_TRUE(Buffer->Undo());
	ExpectCounts(0, 3);
	ASSERT_TRUE(Buffer->Redo());
	ExpectCounts(1, 2);
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Cancel recording"});
		Scope.Cancel();
	}
	ExpectCounts(1, 2);
	Commit();
	ExpectCounts(2, 0);
	EXPECT_FALSE(Buffer->CanRedo());
	Buffer->Reset();
	ExpectCounts(0, 0);
}

TEST(FTransBufferTests, NestedModifyPreservesOutermostFinalState)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "NestedModifyBuffer"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NestedModifyTarget"));
	Durin::Tests::FBufferRecording Outer(Buffer.Get(), {"test", "Nested object modification"});
	(void)Target.Get()->Modify();
	Target->Value = 1;
	{
		Durin::Tests::FBufferRecording Inner(Buffer.Get(), {"test", "Inner modification"});
		(void)Target.Get()->Modify();
		Target->Value = 2;
	}
	Target->Value = 3;
	ASSERT_TRUE(Outer.End());
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 3);
}

TEST(FTransBufferTests, EditorScopeIsImmobileAndCancelIsIdempotent)
{
	using namespace Durin;
	static_assert(!std::is_move_constructible_v<FScopedTransaction>);
	static_assert(!std::is_move_assignable_v<FScopedTransaction>);
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Editor;
	FScopedTransaction Scope("Cancel");
	ASSERT_TRUE(Scope.IsOutstanding());
	{
		FScopedTransaction Disabled("Disabled", false);
		EXPECT_FALSE(Disabled.IsOutstanding());
		Disabled.Cancel();
		EXPECT_TRUE(Editor->IsActive());
	}
	Scope.Cancel();
	Scope.Cancel();
	EXPECT_FALSE(Scope.IsOutstanding());
	EXPECT_FALSE(Editor->IsActive());
}

TEST(FTransBufferTests, MaintainsCursorBranchIdsAndEvents)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "HistoryTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "HistoryTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Commit = [&](std::string Description) {
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", std::move(Description)});
		(void)Target->Modify();
		++Target->Value;
		Scope.End();
		return Buffer->GetUndoContext().TransactionId;
	};

	const auto First = Commit("First");
	const auto Second = Commit("Second");
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);
	EXPECT_LT(First, Second);
	EXPECT_TRUE(Buffer->Undo());
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, First);
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Second);
	EXPECT_TRUE(Buffer->Redo());
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Second);
	EXPECT_TRUE(Buffer->Undo());
	const auto Branch = Commit("Branch");
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_GT(Branch, Second);

	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ(Buffer->GetUndoContext(false).Description, "Branch");
	const auto Events = ReadTransactionEvents(*Buffer);
	ASSERT_GE(Events.size(), 7u);
	EXPECT_EQ(Events.back().Type, Durin::ETransactionEventType::Executed);

	Buffer->Reset();
	const auto AfterReset = Commit("After reset");
	EXPECT_GT(AfterReset, Branch);
}

TEST(FTransBufferTests, OversizedRecordingRemainsUndoableAndPreservesFailedUndoCursor)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "FailureTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "FailureTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	Durin::FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, 1);
	Durin::Tests::FBufferRecording Oversized(Buffer, {"test", "Oversized"});
	(void)Target->Modify();
	Target->Numbers.push_back(1);
	ASSERT_TRUE(Oversized.End());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_GT(Buffer->GetUndoSize(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_TRUE(Target->Numbers.empty());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Numbers, std::vector<int32>{1});

	Durin::Tests::FBufferRecording Valid(Buffer, {"test", "Stale"});
	(void)Target->Modify();
	++Target->Value;
	ASSERT_TRUE(Valid.End());
	const auto Id = Buffer->GetUndoContext().TransactionId;
	TargetRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	Durin::MarkAsGarbage(Target);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(Buffer->UndoExpected(Buffer->GetUndoContext().TransactionId));
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 1u);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
}

TEST(FTransBufferTests, ExpectedIdsPreserveHistoryPosition)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "AddressedTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "AddressedTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	auto Commit = [&](std::string Description) {
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", std::move(Description)});
		(void)Target->Modify();
		++Target->Value;
		Scope.End();
		return Buffer->GetUndoContext().TransactionId;
	};
	const auto First = Commit("First");
	const auto Second = Commit("Second");
	ASSERT_TRUE(First);
	ASSERT_TRUE(Second);

	EXPECT_FALSE(Buffer->UndoExpected(First));
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Second);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 2u);
	ASSERT_TRUE(Buffer->UndoExpected(Second));
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Second);
	EXPECT_FALSE(Buffer->RedoExpected(First));
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Second);

	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 1u);
	ASSERT_TRUE(Buffer->RedoExpected(Second));
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Second);
	EXPECT_EQ((Buffer->GetQueueLength() - Buffer->GetUndoCount()), 2u);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
}

TEST(FTransBufferTests, ForgetPackageRetiresHardSnapshotDependenciesOnBothSidesOfCursor)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "PackageDependencyBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "RetiredPackage");
	auto* Participant = Durin::NewObject<Durin::DObject>(Package, "RetiredParticipant");
	auto* Target = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "ExternalTarget");
	Durin::TStrongObjectPtr<Durin::DObject> TargetRoot(Target);
	Durin::TWeakObjectPtr<Durin::DObject> WeakParticipant(Participant);
	{
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", "Unrelated value"});
		(void)Target->Modify();
		++Target->Value;
		ASSERT_TRUE(Scope.End());
	}
	const auto UnrelatedId = Buffer->GetUndoContext().TransactionId;
	{
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", "Hard after dependency"});
		(void)Target->Modify();
		Target->Hard = Participant;
		ASSERT_TRUE(Scope.End());
	}
	{
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", "Hard before dependency"});
		(void)Target->Modify();
		Target->Hard = nullptr;
		ASSERT_TRUE(Scope.End());
	}
	ASSERT_TRUE(Buffer->Undo());
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, UnrelatedId);
	EXPECT_FALSE(Buffer->CanRedo());
	Target->Hard = nullptr;
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(WeakParticipant.IsValid());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
}

TEST(FTransBufferTests, ForgetPackageRetiresCustomLifetimeDependencies)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "CustomPackageDependencyBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Package = Durin::NewObject<Durin::DPackage>(nullptr, "CustomRetiredPackage");
	auto* Participant = Durin::NewObject<Durin::DObject>(Package, "CustomRetiredParticipant");
	Durin::TWeakObjectPtr<Durin::DObject> WeakParticipant(Participant);
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1, Participant)));
	Buffer->ForgetPackage(*Package);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Buffer->GetUndoSize(), 0u);
	EXPECT_EQ(Value, 1);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_FALSE(WeakParticipant.IsValid());
}

TEST(FTransBufferTests, CancellationEvictionAndDestructionReleaseCollectorEdges)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ReleaseTransBuffer");
	Durin::FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, 1);
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto MakeTarget = [](std::string_view Name) {
		return Durin::NewObject<DTransactionRecordParticipant>(nullptr, Durin::FName(Name));
	};

	auto* Canceled = MakeTarget("CanceledTarget");
	const auto CanceledHandle = Durin::FObjectKey(Canceled);
	{
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", "Canceled"});
		(void)Canceled->Modify();
		Scope.Cancel();
		EXPECT_FALSE(Buffer->IsActive());
	}
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(CanceledHandle), nullptr);

	auto Commit = [&](DTransactionRecordParticipant* Target, std::string Description) {
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", std::move(Description)});
		(void)Target->Modify();
		++Target->Value;
		EXPECT_TRUE(Scope.End());
	};
	auto* Evicted = MakeTarget("EvictedTarget");
	const auto EvictedHandle = Durin::FObjectKey(Evicted);
	Commit(Evicted, "Evicted");
	auto* Retained = MakeTarget("RetainedTarget");
	Durin::TStrongObjectPtr<Durin::DObject> RetainedRoot(Retained);
	Commit(Retained, "Retained");
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(EvictedHandle), nullptr);

	const auto RetainedHandle = Durin::FObjectKey(Retained);
	RetainedRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	Buffer->BeginDestroy();
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(RetainedHandle), nullptr);
}

TEST(FTransBufferTests, CollectorRetainsPendingAndHistoryEdgesAndReleasesBranches)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "GCTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* First = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "GCFirstTarget");
	auto* Hard = Durin::NewObject<Durin::DObject>(nullptr, "GCHardValue");
	auto* Weak = Durin::NewObject<Durin::DObject>(nullptr, "GCWeakValue");
	First->Hard = Hard;
	First->Weak = Weak;
	const auto FirstHandle = Durin::FObjectKey(First);
	const auto HardHandle = Durin::FObjectKey(Hard);
	const auto WeakHandle = Durin::FObjectKey(Weak);
	{
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", "First GC"});
		(void)First->Modify();
		First->Hard = nullptr;
		First->Weak = nullptr;
		Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
		EXPECT_NE(Durin::ResolveObjectKey(FirstHandle), nullptr);
		EXPECT_NE(Durin::ResolveObjectKey(HardHandle), nullptr);
		EXPECT_EQ(Durin::ResolveObjectKey(WeakHandle), nullptr);
	}
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_NE(Durin::ResolveObjectKey(FirstHandle), nullptr);
	EXPECT_NE(Durin::ResolveObjectKey(HardHandle), nullptr);
	ASSERT_TRUE(Buffer->Undo());

	auto* Second = Durin::NewObject<DTransactionRecordParticipant>(nullptr, "GCSecondTarget");
	Durin::TStrongObjectPtr<Durin::DObject> SecondRoot(Second);
	{
		Durin::Tests::FBufferRecording Scope(Buffer, {"test", "Branch GC"});
		(void)Second->Modify();
		++Second->Value;
	}
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(FirstHandle), nullptr);
	EXPECT_EQ(Durin::ResolveObjectKey(HardHandle), nullptr);
	Buffer->Reset();
	SecondRoot = Durin::TStrongObjectPtr<Durin::DObject>(nullptr);
	const auto SecondHandle = Durin::FObjectKey(Second);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(SecondHandle), nullptr);
}

TEST(FTransBufferTests, DetachedOwnedChildSurvivesCollectionUndoRedoAndReleasesWithHistory)
{
	using namespace Durin;
	InitializeDObjectSystem();
	auto* Buffer = NewObject<DTransBuffer>(nullptr, "DetachedChildHistory");
	TStrongObjectPtr<DObject> BufferRoot(Buffer);
	auto* Owner = NewObject<DTransactionRecordParticipant>(nullptr, "GraphOwner");
	TStrongObjectPtr<DObject> OwnerRoot(Owner);
	auto* Child = NewObject<DTransactionRecordParticipant>(Owner, "Expression");
	Child->Value = 71;
	Owner->Hard = Child;
	auto* HistoryOuter = NewObject<DObject>(nullptr, "DetachedGraphObjects");
	const auto ChildHandle = FObjectKey(Child);
	const auto HistoryHandle = FObjectKey(HistoryOuter);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FDetachedChildChange>(Owner, Child, HistoryOuter)));
	CollectGarbage(Durin::EObjectFlags::Standalone);
	ASSERT_EQ(ResolveObjectKey(ChildHandle), Child);
	EXPECT_EQ(Child->GetOuter(), HistoryOuter);
	EXPECT_EQ(Owner->Hard.Get(), nullptr);
	EXPECT_TRUE(GDObjectArray.GetObjectsWithOuter(Owner, EObjectQueryScope::LiveOnly).empty());
	ASSERT_TRUE(Buffer->Undo());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Owner->Hard.Get(), Child);
	EXPECT_EQ(Child->GetOuter(), Owner);
	EXPECT_EQ(Child->Value, 71);
	ASSERT_TRUE(Buffer->Redo());
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(ResolveObjectKey(ChildHandle), Child);
	EXPECT_EQ(Child->GetOuter(), HistoryOuter);
	Buffer->Reset();
	CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(ResolveObjectKey(ChildHandle), nullptr);
	EXPECT_EQ(ResolveObjectKey(HistoryHandle), nullptr);
	EXPECT_EQ(Owner->Hard.Get(), nullptr);
}

TEST(FTransBufferTests, SynchronousChangesJoinNestedRecordingAndPreserveOuterContext)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "GroupedCustomChanges"));
	int Value = 1;
	const auto Outer = BeginRecording(*Buffer, {"outer", "Grouped edit"});
	ASSERT_TRUE(Outer);
	const auto Inner = BeginRecording(*Buffer, {"inner", "Ignored description"});
	ASSERT_TRUE(Inner);
	auto First = std::make_unique<FTestCustomChange>(Value, 1, 2);

	const auto Joined = Durin::Tests::RecordTestEdit(*Buffer, std::move(First));
	ASSERT_TRUE(Joined);
	EXPECT_EQ(Joined, Outer.TransactionId);
	EXPECT_EQ(Joined, Outer.TransactionId);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->End());
	auto Second = std::make_unique<FTestCustomChange>(Value, 2, 3);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Second)));
	EXPECT_FALSE(Buffer->UndoExpected(Buffer->GetUndoContext().TransactionId));
	ASSERT_TRUE(Buffer->End());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoContext(false).Description, "Grouped edit");
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 3);
}

TEST(FTransBufferTests, CancelingJoinedChangeDiscardsRecordingWithoutRevertingEdits)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CanceledOwner"));
	int CommandValue = 0;
	std::optional<FScopedTransaction> Outer(std::in_place, "Outer edit");
	ASSERT_TRUE(Outer->IsRecordingCurrent());
	ASSERT_TRUE(Target->Modify(false));
	Target->Value = 1;
	{
		FScopedTransaction Inner("Custom edit");
		ASSERT_TRUE(Inner.IsRecordingCurrent());
		CommandValue = 2;
		ASSERT_TRUE(GUndo->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(CommandValue, 0, 2)));
		Inner.Cancel();
		EXPECT_FALSE(Inner.IsOutstanding());
	}
	EXPECT_TRUE(Outer->IsOutstanding());
	EXPECT_FALSE(Outer->IsRecordingCurrent());
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(GUndo, nullptr);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_EQ(Target->Value, 1);
	EXPECT_EQ(CommandValue, 2);
	EXPECT_FALSE(Target->Modify(false));
	{
		FScopedTransaction Replacement("Replacement edit");
		ASSERT_TRUE(Replacement.IsRecordingCurrent());
		Outer.reset();
		ASSERT_TRUE(Replacement.IsRecordingCurrent());
		ASSERT_TRUE(Target->Modify(false));
		Target->Value = 3;
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 1);
	EXPECT_EQ(CommandValue, 2);
}

TEST(FTransBufferTests, JoinedAppliedChangeAndPropertyCaptureShareTheVisibleCandidate)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "MixedGroupedChanges"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "GroupedProperty"));
	int Value = 1;
	const auto Prior = Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2));
	ASSERT_TRUE(Prior);
	ASSERT_TRUE(Buffer->Undo());
	Durin::Tests::FBufferRecording Outer(Buffer.Get(), {.Description = "Property and applied change"});
	ASSERT_TRUE(Target->Modify());
	Target->Value = 42;
	Value = 3;
	auto Change = std::make_unique<FTestCustomChange>(Value, 1, 3);
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, 0u);
	ASSERT_NE(Buffer->GetTransaction(0), nullptr);
	EXPECT_NE(Buffer->GetTransaction(0)->GetId(), Prior);
	EXPECT_EQ(Buffer->GetTransaction(0), GUndo);
	ASSERT_TRUE(Outer.End());
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 3);
	EXPECT_EQ(Target->Value, 42);
}

TEST(FTransBufferTests, GroupedRedoFaultReportsItsRecord)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "GroupedRecovery"));
	auto* FirstTarget = NewObject<DTransactionRecordParticipant>(nullptr, "GroupedFirstReference");
	auto* FailedTarget = NewObject<DTransactionRecordParticipant>(nullptr, "GroupedFailedReference");
	int Value = 1;
	const auto Outer = BeginRecording(*Buffer);
	auto First = std::make_unique<FTestCustomChange>(Value, 1, 2, FirstTarget);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(First)));
	auto Bad = std::make_unique<FTestCustomChange>(Value, 2, 3, FailedTarget);
	auto* FailedCommand = Bad.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Bad)));
	ASSERT_TRUE(Buffer->End());
	ASSERT_TRUE(Buffer->Undo());
	FailedCommand->bThrowRedo = true;
	EXPECT_DEATH((void)Buffer->Redo(), "during Redo record 1");
}

TEST(FTransBufferTests, CustomReferencesAndModuleDrainReleaseHistory)
{
	InitializeDObjectSystem();
	auto* Buffer = Durin::NewObject<Durin::DTransBuffer>(nullptr, "ModuleCustomTransBuffer");
	Durin::TStrongObjectPtr<Durin::DObject> BufferRoot(Buffer);
	auto* Referenced = Durin::NewObject<Durin::DObject>(nullptr, "CustomReferencedObject");
	const Durin::FObjectKey Handle = Durin::FObjectKey(Referenced);
	int Value = 1;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(
		Value, 1, 2, Referenced, "TestModule")));
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_NE(Durin::ResolveObjectKey(Handle), nullptr);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("TestModule"));
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	Durin::CollectGarbage(Durin::EObjectFlags::Standalone);
	EXPECT_EQ(Durin::ResolveObjectKey(Handle), nullptr);
}

TEST(FTransBufferTests, LaterCommandRefusalPrecedesAllReplayHooksAndWrites)
{
	using namespace Durin;
	class FRejectableChange final : public FCommandChange
	{
	public:
		bool Available = false;
		auto ValidateReplay(ETransactionOperation, bool) const -> FTransactionCaptureResult override
		{
			if (!Available) return std::unexpected("Target is unavailable");
			return {};
		}
		auto Apply() -> FCommandChangeResult override { return {}; }
		auto Revert() -> FCommandChangeResult override { return {}; }
		auto GetDescription() const -> std::string_view override { return "Rejectable command"; }
		auto GetModifiedObjects() const -> std::vector<DObject*> override { return {}; }
	};
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "PreflightTarget"));
	auto Change = std::make_unique<FRejectableChange>();
	auto* Probe = Change.get();
	ASSERT_EQ(Buffer->Begin("test", "Preflight"), 0);
	ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	ASSERT_TRUE(Target->Modify());
	Target->Value = 17;
	ASSERT_EQ(Buffer->End(), 1);
	const auto Head = Buffer->GetUndoContext().TransactionId;
	std::string Reason;
	EXPECT_FALSE(Buffer->UndoExpected(Head, &Reason));
	EXPECT_EQ(Reason, "Target is unavailable");
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Head);
	EXPECT_EQ(Target->Value, 17);
	EXPECT_EQ(Target->UndoBegins, 0);
	Probe->Available = true;
	ASSERT_TRUE(Buffer->UndoExpected(Head));
	EXPECT_EQ(Target->Value, 0);
}

TEST(FTransBufferTests, PartialUndoFaultReportsOriginalRecord)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int First = 1, Second = 2;
	{
		Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Partial Undo"});
		auto Failing = std::make_unique<FTestCustomChange>(First, 0, 1);
		Failing->bFailUndo = true;
		Failing->ReplayProbe = [&] {
			if (Second != 0) throw std::runtime_error("Second record has not been undone");
		};
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Failing)));
		ASSERT_TRUE(Buffer->StoreUndo(std::make_unique<FTestCustomChange>(Second, 0, 2)));
	}
	const auto Id = Buffer->GetUndoContext().TransactionId;
	EXPECT_DEATH((void)Buffer->UndoExpected(Id), std::format("Transaction {} during Undo record 0: .*Injected mutation failure", Id));
}

TEST(FTransBufferTests, ValidationRejectsUnavailableCustomRecordBeforeExecution)
{
	using namespace Durin;
	int Value = 1;
	FTransaction Transaction(42, {.Description = "Typed validation"});
	FTransBufferTestAccess::AddCommand(Transaction, std::make_unique<FTestCustomChange>(Value, 1, 2));
	FTransBufferTestAccess::AddCommand(Transaction, std::unique_ptr<FActorCommandChange>{});
	const auto Rejected = Transaction.Apply(false, Durin::EPropertyChangeOrigin::Redo);
	ASSERT_FALSE(Rejected);
	EXPECT_EQ(Rejected.error().RecordIndex, 1u);
	const auto RejectionMessage = Rejected.error().Message;
	EXPECT_EQ(Value, 1);
	FTransaction Retry(43, {.Description = "Valid retry"});
	FTransBufferTestAccess::AddCommand(Retry, std::make_unique<FTestCustomChange>(Value, 1, 2));
	const auto Applied = Retry.Apply(false, Durin::EPropertyChangeOrigin::Redo);
	ASSERT_TRUE(Applied);
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Rejected.error().Message, RejectionMessage);
}

TEST(FTransBufferTests, DirectRedoFaultReportsOriginalRecord)
{
	using namespace Durin;
	int First = 1, Second = 2;
	FTransaction Transaction(43, {.Description = "Partial Redo"});
	FTransBufferTestAccess::AddCommand(Transaction, std::make_unique<FTestCustomChange>(First, 1, 10));
	auto FailingChange = std::make_unique<FTestCustomChange>(Second, 2, 20);
	FailingChange->bFailRedo = true;
	FTransBufferTestAccess::AddCommand(Transaction, std::move(FailingChange));
	EXPECT_DEATH((void)Transaction.Apply(false, EPropertyChangeOrigin::Redo), "Transaction 43 during Redo record 1");
}

TEST(FTransBufferTests, CustomCleanupFaultTerminatesRedo)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "InternalCompensationTransBuffer"));
	TStrongObjectPtr<DTransactionRecordParticipant> Participant(NewObject<DTransactionRecordParticipant>(nullptr, "RecoveryParticipant"));
	int Value = 0;
	auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, Participant.Get(), "RecoveryModule");
	auto* Replay = Change.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Change)));
	ASSERT_TRUE(Buffer->Undo());
	Replay->bFailRedo = true;
	Replay->bFailCleanup = true;
	EXPECT_DEATH((void)Buffer->Redo(), "during Redo");
}

TEST(FTransBufferTests, ThrowingReplayReportsOriginalCause)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "ThrowingReplayRecovery"));
	TStrongObjectPtr<DTransactionRecordParticipant> Participant(NewObject<DTransactionRecordParticipant>(nullptr, "ThrowingReplayParticipant"));
	int Value = 0;
	auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, Participant.Get());
	auto* Replay = Change.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Change)));
	ASSERT_TRUE(Buffer->Undo());
	Replay->bThrowRedo = true;
	EXPECT_DEATH((void)Buffer->Redo(), "replay exception fixture");
}

TEST(FTransBufferTests, BudgetPrefixIsHiddenAtBeginAndRestoredByCancelAndEmptyEnd)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Measurement;
	int MeasurementValue = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Measurement, std::make_unique<FTestCustomChange>(MeasurementValue, 0, 1)));
	TStrongObjectPtr<DTransBuffer> Buffer(NewObject<DTransBuffer>(nullptr, "SoftBudgetTransBuffer"));
	FTransBufferTestAccess::InitializeMemoryBudget(*Buffer, Measurement->GetUndoSize() + 1);
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	auto Growing = std::make_unique<FTestCustomChange>(Value, 1, 2);
	Growing->bGrowOnRedo = true;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Growing)));
	EXPECT_EQ(Value, 2);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_GT(Buffer->GetUndoSize(), Buffer->GetMemoryBudget());
	const auto Head = Buffer->GetUndoContext().TransactionId;
	const auto Bytes = Buffer->GetUndoSize();
	const auto Canceled = BeginRecording(*Buffer, {.Description = "Canceled over-budget edit"});
	ASSERT_TRUE(Canceled);
	EXPECT_EQ(Buffer->GetUndoContext(false).TransactionId, GUndo->GetContext().TransactionId);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	Buffer->Cancel(Canceled.Index);
	EXPECT_FALSE(Buffer->IsActive());
	const auto NoOp = BeginRecording(*Buffer, {.Description = "No-op over-budget edit"});
	ASSERT_TRUE(NoOp);
	EXPECT_EQ(Buffer->End(), 1);
	auto Failing = std::make_unique<FTestCustomChange>(Value, 2, 3);
	Failing->bFailRedo = true;
	EXPECT_FALSE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Failing)));
	EXPECT_EQ(Buffer->GetUndoSize(), Bytes);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Head);
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 1);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 2);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 2, 3)));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, ByteBudgetDoesNotImposeAnEntryCountLimit)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	for (int Index = 0; Index < 300; ++Index)
		ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, Index, Index + 1)));
	EXPECT_EQ(Buffer->GetQueueLength(), 300u);
	EXPECT_LT(Buffer->GetUndoSize(), Buffer->GetMemoryBudget());
	for (int Index = 0; Index < 300; ++Index) ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
	for (int Index = 0; Index < 300; ++Index) ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 300);
}

TEST(FTransBufferTests, OldHistoryAtTheByteBudgetIsTrimmedBeforeAppending)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Measurement;
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Measurement, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	Tests::FTestTransactorOwner Buffer("ExactByteBudget", Measurement->GetUndoSize());
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	EXPECT_EQ(Buffer->GetUndoSize(), Buffer->GetMemoryBudget());
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 2, 3)));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, RedoBranchReplacementDoesNotAlsoTrimTheUndoPrefix)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Measurement;
	int MeasurementValue = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Measurement, std::make_unique<FTestCustomChange>(MeasurementValue, 0, 1)));
	Tests::FTestTransactorOwner Buffer("OverBudgetRedo", Measurement->GetUndoSize() + 1);
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	auto Growing = std::make_unique<FTestCustomChange>(Value, 1, 2);
	Growing->bGrowOnRedo = true;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Growing)));
	ASSERT_GT(Buffer->GetUndoSize(), Buffer->GetMemoryBudget());
	ASSERT_TRUE(Buffer->Undo());
	const auto UndoId = Buffer->GetUndoContext().TransactionId;
	const auto RedoId = Buffer->GetRedoContext().TransactionId;
	const auto Canceled = BeginRecording(*Buffer, {.Description = "Canceled branch"});
	ASSERT_TRUE(Canceled);
	Buffer->Cancel(Canceled.Index);
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, UndoId);
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, RedoId);
	(void)ReadTransactionEvents(*Buffer);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 3)));
	EXPECT_EQ(Buffer->GetQueueLength(), 2u);
	EXPECT_EQ(Buffer->GetUndoCount(), 0u);
	const auto Events = ReadTransactionEvents(*Buffer);
	EXPECT_TRUE(std::ranges::none_of(Events, [](const auto& Event) { return Event.Type == ETransactionEventType::Evicted; }));
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
}

TEST(FTransBufferTests, NativeBeginEndAndInnerCancelUseUEActiveCounts)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CancelCounts"));
	EXPECT_EQ(Buffer->Begin("test", "Outer"), 0);
	ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	const auto Inner = Buffer->Begin("test", "Inner"); EXPECT_EQ(Inner, 1);
	ASSERT_TRUE(Target->Modify()); Target->Value = 2;
	Buffer->Cancel(Inner);
	EXPECT_FALSE(Buffer->IsActive()); EXPECT_EQ(Buffer->End(), 0);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_EQ(Target->Value, 2);
	EXPECT_EQ(GUndo, nullptr);
	EXPECT_EQ(Buffer->Begin("test", "Next"), 0);
	EXPECT_EQ(Buffer->Begin("test", "Nested"), 1);
	EXPECT_EQ(Buffer->End(), 2); EXPECT_TRUE(Buffer->IsActive());
	EXPECT_EQ(Buffer->End(), 1); EXPECT_FALSE(Buffer->IsActive());
}

TEST(FTransBufferTests, TransientAndNonTransactionalFiltersAreIndependent)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "TransactionFilters"));
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Filters"});
	  ASSERT_TRUE(Target->Modify()); Target->SessionValue = 10; Target->UnrecordedValue = 20; }
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->SessionValue, 0); EXPECT_EQ(Target->UnrecordedValue, 20);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->SessionValue, 10);
}

TEST(FTransBufferTests, HistoricalRestoreDoesNotRunNewEditValidation)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ReplayValidation"));
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Original edit"}); ASSERT_TRUE(Target->Modify()); Target->Value = 17; }
	Target->bRejectNewEdits = true;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 17);
	EXPECT_EQ(Target->ValidationCalls, 0);
}

TEST(FTransBufferTests, ModifyMarksDirtyWithoutRequiringAnActiveRecording)
{
	using namespace Durin;
	InitializeDObjectSystem();
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ModifyDirty"));
	(void)MakeSoftPath(); FPackagePath PackagePath;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ModifyDirty", PackagePath)); Package->InitializeAssetPackage(PackagePath);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Target"));
	Package->ClearDirty(); EXPECT_FALSE(Target->Modify(false)); EXPECT_FALSE(Package->IsDirty());
	EXPECT_FALSE(Target->Modify()); EXPECT_TRUE(Package->IsDirty());
	Package->ClearDirty(); Target->ClearFlags(EObjectFlags::Transactional);
	Tests::FTestTransactorOwner Buffer; EXPECT_EQ(Buffer->Begin("test", "Unrecorded"), 0);
	EXPECT_FALSE(Target->Modify()); Target->Value = 5; EXPECT_EQ(Buffer->End(), 1);
	EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransBufferTests, ModifyFalseRecordsWithoutDirtyingUntilCommit)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ModifyFalse"));
	(void)MakeSoftPath();
	FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ModifyFalse", Path));
	Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Target"));
	Package->ClearDirty();

	{
		Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Capture only"});
		ASSERT_TRUE(Target->Modify(false));
	}
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
	EXPECT_FALSE(Package->IsDirty());

	{
		Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Edit value"});
		ASSERT_TRUE(Target->Modify(false));
		Target->Value = 17;
		EXPECT_FALSE(Package->IsDirty());

	}
	EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 17);
	EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransBufferTests, IndependentListenersReceiveTheSamePostTransitionEvents)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	std::vector<FTransactionEvent> First, Second;
	const auto A = Buffer->AddTransactionListener([&](const auto& Event) { First.push_back(Event); EXPECT_FALSE(Buffer->IsActive()); });
	const auto B = Buffer->AddTransactionListener([&](const auto& Event) { Second.push_back(Event); });
	int Value = 0; ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	EXPECT_TRUE(First.empty()); Buffer->DispatchEvents();
	ASSERT_EQ(First.size(), 1u); ASSERT_EQ(Second.size(), 1u); EXPECT_EQ(First[0].Id, Second[0].Id);
	Buffer->RemoveTransactionListener(A); ASSERT_TRUE(Buffer->Undo()); Buffer->DispatchEvents();
	EXPECT_EQ(First.size(), 1u); EXPECT_EQ(Second.size(), 2u); Buffer->RemoveTransactionListener(B);
}

TEST(FTransBufferTests, CustomOwnerSuppressesLaterModifySnapshot)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "CustomOwner"));
	{
		FScopedTransaction Outer("Custom owner");
		ASSERT_TRUE(Outer.IsRecordingCurrent());
		Target->Value = 1;
		ASSERT_TRUE(GUndo->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 1)));
		{
			FScopedTransaction Inner("Modify owner");
			ASSERT_TRUE(Inner.IsRecordingCurrent());
			ASSERT_TRUE(Target->Modify());
			EXPECT_EQ(Target->SerializationSaves, 0);
			Target->Value = 2;
		}
		EXPECT_TRUE(Outer.IsRecordingCurrent());
		EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 1);
}

TEST(FTransBufferTests, AdditionalCustomParticipantCanStillCaptureAWholeBaseline)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Primary(NewObject<DTransactionRecordParticipant>(nullptr, "PrimaryParticipant"));
	TStrongObjectPtr<DTransactionRecordParticipant> Additional(NewObject<DTransactionRecordParticipant>(nullptr, "AdditionalParticipant"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Participant ownership"});
		auto Command = std::make_unique<FManagementProbeChange>();
		Command->Participants.push_back(Additional.Get());
		ASSERT_TRUE(Buffer->StoreUndo(Primary.Get(), std::move(Command)));
		EXPECT_TRUE(GUndo->ContainsObject(Additional.Get()));
		ASSERT_TRUE(Primary->Modify());
		EXPECT_EQ(Primary->SerializationSaves, 0);
		ASSERT_TRUE(Additional->Modify());
		EXPECT_EQ(Additional->SerializationSaves, 1);
		Additional->Value = 9;
	}
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Primary->Value, 0);
	EXPECT_EQ(Additional->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Primary->Value, 0);
	EXPECT_EQ(Additional->Value, 9);
}

TEST(FTransBufferTests, UnboundCommandLeavesItsSoleParticipantEligibleForModify)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SoleParticipant"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Command then ordinary field"});
		auto Change = std::make_unique<FManagementProbeChange>();
		Change->Participants.push_back(Target.Get());
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
		EXPECT_TRUE(GUndo->ContainsObject(Target.Get()));
		EXPECT_EQ(Target->SerializationSaves, 0);
		ASSERT_TRUE(Target->Modify(false));
		EXPECT_EQ(Target->SerializationSaves, 1);
		Target->Value = 9;
	}
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 9);
}

TEST(FTransBufferTests, BatchCaptureIndexesOneBaselinePerObjectAndReleasesIndex)
{
	using namespace Durin;
	InitializeDObjectSystem();
	for (const size_t Count : {64u, 256u})
	{
		Tests::FTestTransactorOwner Buffer;
		std::vector<TStrongObjectPtr<DTransactionRecordParticipant>> Objects;
		{
			Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Batch edit"});
			for (size_t Slot = 0; Slot < Count; ++Slot)
			{
				auto& Object = Objects.emplace_back(NewObject<DTransactionRecordParticipant>(nullptr, FName(std::format("Batch{}_{}", Count, Slot))));
				ASSERT_TRUE(Object->Modify());
				Object->Value = 1;
			}
			auto* Recording = static_cast<FTransaction*>(GUndo);
			const auto* Index = FTransBufferTestAccess::CaptureIndex(*Recording);
			ASSERT_NE(Index, nullptr);
			ASSERT_EQ(Index->size(), Count);
			size_t BucketVisits = 0;
			for (const auto& Object : Objects)
			{
				ASSERT_TRUE(Object->Modify(false));
				ASSERT_TRUE(Object->Modify());
				EXPECT_EQ(Object->SerializationSaves, 1);
				const FPersistentObjectRef Key(Object.Get());
				BucketVisits += Index->bucket_size(Index->bucket(Key));
			}
			EXPECT_LE(BucketVisits, 8 * Count);
			EXPECT_EQ(Recording->GetRecordCount(), Count);
		}
		const auto* Retained = Buffer->GetTransaction(0);
		ASSERT_NE(Retained, nullptr);
		EXPECT_EQ(FTransBufferTestAccess::CaptureIndex(*Retained), nullptr);
		EXPECT_TRUE(Retained->ContainsObject(Objects.back().Get()));
		ASSERT_TRUE(Buffer->Undo());
		ASSERT_TRUE(Buffer->Redo());
	}
}

TEST(FTransBufferTests, ModifyBeforeCustomPreservesOrderedReplay)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(
		NewObject<DTransactionRecordParticipant>(nullptr, "ModifyBeforeCustom"));
	std::vector<int32> ValuesBeforeCustomReplay;
	{
		FScopedTransaction Outer("Serialized then custom");
		ASSERT_TRUE(Outer.IsRecordingCurrent());
		ASSERT_TRUE(Target->Modify());
		Target->Value = 1;
		{
			FScopedTransaction Inner("Append custom change");
			ASSERT_TRUE(Inner.IsRecordingCurrent());
			auto Change = std::make_unique<FTestCustomChange>(Target->Value, 1, 2);
			Change->ReplayProbe = [&] { ValuesBeforeCustomReplay.push_back(Target->Value); };
			Target->Value = 2;
			ASSERT_TRUE(GUndo->StoreUndo(Target.Get(), std::move(Change)));
		}
		EXPECT_TRUE(Outer.IsRecordingCurrent());
		EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 2u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 2);
	EXPECT_EQ(ValuesBeforeCustomReplay, (std::vector<int32>{2, 2}));
}

TEST(FTransBufferTests, DisabledScopeLeavesOuterCaptureAndLifetimeIntact)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestEditorTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(
		NewObject<DTransactionRecordParticipant>(nullptr, "DisabledNestedScope"));
	{
		FScopedTransaction Outer("Outer");
		ASSERT_TRUE(Outer.IsRecordingCurrent());
		auto* Recording = GUndo;
		{
			FScopedTransaction Disabled("Disabled", false);
			EXPECT_FALSE(Disabled.IsOutstanding());
			EXPECT_EQ(GUndo, Recording);
			ASSERT_TRUE(Target->Modify());
			Target->Value = 7;
			Disabled.Cancel();
			EXPECT_FALSE(Disabled.IsOutstanding());
		}
		EXPECT_TRUE(Outer.IsOutstanding());
		EXPECT_EQ(GUndo, Recording);
	}
	EXPECT_EQ(GUndo, nullptr);
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 7);
}

TEST(FTransBufferTests, NoOpAndCanceledRecordingPreserveRedoBranch)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(
		NewObject<DTransactionRecordParticipant>(nullptr, "PreservedRedo"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Original"});
		ASSERT_TRUE(Target->Modify());
		Target->Value = 5;
	}
	ASSERT_TRUE(Buffer->Undo());
	const auto RedoId = Buffer->GetRedoContext().TransactionId;
	{
		Durin::Tests::FBufferRecording NoOp(Buffer.Get(), {"test", "No-op"});
		ASSERT_TRUE(Target->Modify());
	}
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, RedoId);
	{
		Durin::Tests::FBufferRecording Canceled(Buffer.Get(), {"test", "Canceled"});
		ASSERT_TRUE(Target->Modify());
		Target->Value = 3;
		Canceled.Cancel();
	}
	EXPECT_EQ(Target->Value, 3);
	EXPECT_EQ(Buffer->GetRedoContext().TransactionId, RedoId);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Target->Value, 5);
}

TEST(FTransBufferTests, UndoWithoutRedoDiscardsTheRedoSuffix)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Undo(false));
	EXPECT_EQ(Value, 0); EXPECT_EQ(Buffer->GetQueueLength(), 0u); EXPECT_FALSE(Buffer->CanRedo());
}

TEST(FTransBufferTests, UndoFlipsRecordedDirtyBitAndCancelPreservesLiveDirtyBit)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "UnrecordedDirty"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/UnrecordedDirty", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Target"));
	Package->ClearDirty();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Recorded"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
	EXPECT_FALSE(Target->Modify()); Target->UnrecordedValue = 9;
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_FALSE(Package->IsDirty());
	SimulatePackageSave(*Package);
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Canceled"}); ASSERT_TRUE(Target->Modify()); Target->Value = 2; Scope.Cancel(); }
	EXPECT_EQ(Target->Value, 2); EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Package->IsDirty());
}

TEST(FTransBufferTests, PrimaryContextIsNotAnAffectedPackage)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> PrimaryPackage(NewObject<DPackage>(nullptr, "PrimaryContextPackage"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/PrimaryContext", Path)); PrimaryPackage->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Primary(NewObject<DTransactionRecordParticipant>(PrimaryPackage.Get(), "Primary"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ActualTarget"));
	PrimaryPackage->ClearDirty();
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Unrelated context", Primary.Get()});
		ASSERT_TRUE(Target->Modify()); Target->Value = 1;
	}
	EXPECT_FALSE(PrimaryPackage->IsDirty());
	const auto* Transaction = Buffer->GetTransaction(0); ASSERT_NE(Transaction, nullptr);
	EXPECT_EQ(Transaction->GetContext().PrimaryObject, Primary.Get());
	EXPECT_EQ(Transaction->GetTransactionObjects(), std::vector<DObject*>{Target.Get()});
	ASSERT_TRUE(Buffer->Undo()); EXPECT_FALSE(PrimaryPackage->IsDirty());
}

TEST(FTransBufferTests, TransactionFiltersApplyInsideReflectedStructs)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "NestedFieldFilters"));
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Nested field filters"});
		ASSERT_TRUE(Target->Modify()); Target->Nested.Count = 1; Target->Nested.Session = 2; Target->Nested.Ignored = 3;
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Nested.Count, 0); EXPECT_EQ(Target->Nested.Session, 0); EXPECT_EQ(Target->Nested.Ignored, 3);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Nested.Count, 1); EXPECT_EQ(Target->Nested.Session, 2); EXPECT_EQ(Target->Nested.Ignored, 3);
	const auto Count = Buffer->GetQueueLength();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Excluded field only"}); ASSERT_TRUE(Target->Modify()); Target->Nested.Ignored = 7; }
	EXPECT_EQ(Buffer->GetQueueLength(), Count);
}

TEST(FTransBufferTests, ExcludedStructReferencesDoNotAcquireHistoryRetention)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ExcludedReferenceOwner"));
	auto* Referenced = NewObject<DObject>(nullptr, "ExcludedHistoryReference");
	TWeakObjectPtr<DObject> Weak(Referenced);
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Excluded reference"}); ASSERT_TRUE(Target->Modify());
		Target->Nested.Count = 1; Target->Nested.IgnoredReference = Referenced;
	}
	Target->Nested.IgnoredReference = nullptr; CollectGarbage(Durin::EObjectFlags::Standalone); EXPECT_FALSE(Weak.IsValid());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Nested.Count, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Nested.Count, 1);
}

TEST(FTransBufferTests, CleanApplyFailureRetiresOnlyItsOwnRecord)
{
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	{
		Durin::Tests::FBufferRecording Outer(Buffer.Get(), {.Description = "Keep the outer edit"});
		ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
		auto Failed = std::make_unique<FTestCustomChange>(Value, 1, 2);
		Failed->bFailRedo = true;
		EXPECT_FALSE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Failed)));
		EXPECT_TRUE(Outer.IsOutstanding());
		EXPECT_EQ(Value, 1);
	}
	ASSERT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetRecordCount(), 1u);
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Value, 0);
	ASSERT_TRUE(Buffer->Redo());
	EXPECT_EQ(Value, 1);
}

TEST(FTransBufferTests, RedoWithOverflowRetainsHistoryUntilSizeIsNeeded)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0; auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1); auto* Retained = Change.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Change))); ASSERT_TRUE(Buffer->Undo());
	Retained->bOverflowOnRedo = true;
	const auto Id = Buffer->GetRedoContext().TransactionId;
	Retained->Details = "Redo completed";
	Buffer->DispatchEvents();
	ASSERT_TRUE(Buffer->RedoExpected(Id));
	const auto Events = ReadTransactionEvents(*Buffer);
	ASSERT_EQ(Events.size(), 1u);
	EXPECT_EQ(Events.front().Id, Id);
	EXPECT_EQ(Events.front().Type, ETransactionEventType::Redone);
	EXPECT_EQ(Events.front().Details, Retained->Details);
	EXPECT_EQ(Value, 1);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_EQ(Buffer->GetUndoSize(), std::numeric_limits<size_t>::max());
	EXPECT_LT(Buffer->Begin("Edit", "Budget check"), 0);
	EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Id);
	EXPECT_EQ(GUndo, nullptr);
	ASSERT_TRUE(Buffer->UndoExpected(Id));
	EXPECT_EQ(Value, 0);
	EXPECT_LT(Buffer->GetUndoSize(), std::numeric_limits<size_t>::max());
}

TEST(FTransBufferTests, UndoAndRedoShareAdmissionAndProtectTheReplayingRecord)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0; auto Change = std::make_unique<FTestCustomChange>(Value, 0, 1, nullptr, "ReplayOwnedModule"); auto* Retained = Change.get();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::move(Change)));
	int Calls = 0;
	Retained->ReplayProbe = [&] {
		++Calls;
		EXPECT_FALSE(Buffer->IsActive());
		EXPECT_EQ(GUndo, nullptr);
		EXPECT_FALSE(Buffer->CanTransact());
		EXPECT_FALSE(Buffer->CanUndo());
		EXPECT_FALSE(Buffer->CanRedo());
		EXPECT_FALSE(Buffer->Undo());
		EXPECT_FALSE(Buffer->Redo());
		EXPECT_LT(Buffer->Begin("test", "During replay"), 0);
		EXPECT_FALSE(Buffer->DiscardCustomChangesByModule("ReplayOwnedModule"));
		EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	};
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 0);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Value, 1);
	EXPECT_EQ(Calls, 2);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("ReplayOwnedModule")); EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST(FTransBufferTests, GeneratedReferencesRemainOwnedByConstruction)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "GeneratedReferenceOwner"));
	auto* Before = NewObject<DObject>(nullptr, "BeforeGenerated", EObjectConstructionPurpose::Generated);
	auto* After = NewObject<DObject>(nullptr, "AfterGenerated", EObjectConstructionPurpose::Generated);
	Target->Hard = Before;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"test", "Generated reference"}); ASSERT_TRUE(Target->Modify());
		Target->Value = 1; Target->Hard = After; MarkObjectHierarchyAsGarbage(Before);
	}
	CollectGarbage(Durin::EObjectFlags::Standalone); ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->Hard.Get(), After);
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 1); EXPECT_EQ(Target->Hard.Get(), After);
	Buffer->Reset(); std::string Reason; EXPECT_FALSE(Buffer->CanUndo(&Reason)); EXPECT_FALSE(Reason.empty());
}

TEST(FTransBufferTests, SelfCanceledScopeAllowsImmediateReplacement)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestEditorTransactorOwner Buffer;
	std::optional<FScopedTransaction> Old(std::in_place, "Old");
	Old->Cancel();
	ASSERT_FALSE(Old->IsOutstanding());
	FScopedTransaction Replacement("Replacement");
	ASSERT_TRUE(Replacement.IsOutstanding());
	Old->Cancel();
	Old.reset();
	EXPECT_TRUE(Buffer->IsActive());
	Replacement.Cancel();
	EXPECT_FALSE(Buffer->IsActive());
}

TEST(FTransBufferTests, ScopeCleanupAfterTransactorDestructionLeavesHistoryIdle)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Destroyed"});
	EXPECT_TRUE(Scope.IsOutstanding());
	Buffer->BeginDestroy();
	EXPECT_FALSE(Buffer->IsActive());
	EXPECT_EQ(Scope.End(), 0);
	EXPECT_EQ(nullptr, GUndo);
}

TEST(FTransBufferTests, OnlyOneBufferCanPublishTheCurrentRecording)
{
    using namespace Durin;
    using namespace Durin;
    InitializeDObjectSystem(); Tests::FTestTransactorOwner First("FirstRecording"), Second("SecondRecording");
    TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "SingleRecordingTarget"));
    Durin::Tests::FBufferRecording Scope(First.Get(), {.Description = "First"});
    auto* Current = GUndo;
    ASSERT_NE(Current, nullptr);
    EXPECT_EQ(Second->Begin("test", "Competing"), -1);
    EXPECT_FALSE(Second->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 1)));
    EXPECT_EQ(Second->GetQueueLength(), 0u);
    EXPECT_EQ(GUndo, Current);
    ASSERT_TRUE(Target->Modify()); Target->Value = 2;
    EXPECT_TRUE(Current->ContainsObject(Target.Get()));
    ASSERT_TRUE(Scope.End());
    EXPECT_EQ(GUndo, nullptr);
    Durin::Tests::FBufferRecording Next(Second.Get(), {.Description = "Second"});
    EXPECT_TRUE(Next.IsOutstanding());
    EXPECT_NE(GUndo, nullptr);
    Next.Cancel();
    EXPECT_EQ(GUndo, nullptr);
    ASSERT_TRUE(First->Undo()); EXPECT_EQ(Target->Value, 0);
}

TEST(FTransBufferTests, ContextQueriesFollowCursorAndAvailabilityThroughTheBaseInterface)
{
	using namespace Durin;
	using namespace Durin;
	InitializeDObjectSystem();
	Tests::FTestTransactorOwner Buffer("ContextQueries");
	DTransactor* History = Buffer.Get();
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "ContextQueryTarget"));
	auto Record = [&](std::string_view Description, int Value) {
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"ContextQuery", std::string(Description), Target.Get()});
		EXPECT_TRUE(Scope.IsOutstanding());
		EXPECT_TRUE(Target->Modify());
		Target->Value = Value;
	};
	EXPECT_TRUE(History->GetUndoContext().Description.empty());
	EXPECT_TRUE(History->GetRedoContext().Description.empty());
	Record("First", 1);
	Record("Second", 2);
	const auto SavedContext = History->GetUndoContext();
	EXPECT_EQ(SavedContext.Name, "ContextQuery");
	EXPECT_EQ(SavedContext.Description, "Second");
	EXPECT_EQ(SavedContext.PrimaryObject, Target.Get());
	ASSERT_TRUE(History->Undo());
	EXPECT_EQ(History->GetUndoContext().Description, "First");
	EXPECT_EQ(History->GetRedoContext().Description, "Second");
	{
		Durin::Tests::FBufferRecording Pending(Buffer.Get(), {.Description = "Pending"});
		EXPECT_TRUE(History->GetUndoContext().Description.empty());
		EXPECT_EQ(History->GetUndoContext(false).Description, "Pending");
		EXPECT_TRUE(History->GetRedoContext().Description.empty());
		Pending.Cancel();
	}
	EXPECT_EQ(History->GetRedoContext().Description, "Second");
	ASSERT_TRUE(History->Redo());
	EXPECT_EQ(History->GetUndoContext().Description, "Second");
	ASSERT_TRUE(History->Undo());
	Record("Branch", 3);
	EXPECT_EQ(History->GetUndoContext().Description, "Branch");
	EXPECT_TRUE(History->GetRedoContext().Description.empty());
	EXPECT_EQ(History->GetQueueLength(), 2u);
	History->Reset();
	EXPECT_TRUE(History->GetUndoContext(false).Description.empty());
	EXPECT_TRUE(History->GetRedoContext().Description.empty());
	EXPECT_EQ(SavedContext.Description, "Second");
}

TEST(FTransBufferTests, EditorConvenienceScopesExposeContextsAndRetireBeforeReplacement)
{
    using namespace Durin;
    using namespace Durin;
    InitializeDObjectSystem();
    TStrongObjectPtr<DEditorEngine> First(NewObject<DEditorEngine>(nullptr, "FirstEditorLifetime"));
    ASSERT_TRUE(First->ActivateEditor());
    TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "EditorContextTarget"));
    {
        FScopedTransaction Scope("test", "Editor edit", Target.Get());
        ASSERT_TRUE(Scope.IsOutstanding());
        ASSERT_TRUE(Target->Modify()); Target->Value = 3;
    }
    auto* Buffer = First->GetTransactor();
    EXPECT_EQ(Buffer->GetQueueLength(), 1u);
    EXPECT_EQ(Buffer->GetUndoContext().Description, "Editor edit");
    EXPECT_EQ(Buffer->GetUndoContext().PrimaryObject, Target.Get());
    ASSERT_TRUE(Buffer->Undo());
    EXPECT_EQ(Buffer->GetUndoCount(), 1u);
    EXPECT_EQ(Buffer->GetRedoContext().Description, "Editor edit");
    ASSERT_TRUE(Buffer->Redo());
    {
        FScopedTransaction BeforeShutdown("Before shutdown");
        ASSERT_TRUE(BeforeShutdown.IsOutstanding());
    }
    First->PreExit();
    EXPECT_EQ(GEditor, nullptr); EXPECT_EQ(GUndo, nullptr);
    TStrongObjectPtr<DEditorEngine> Second(NewObject<DEditorEngine>(nullptr, "SecondEditorLifetime"));
    ASSERT_TRUE(Second->ActivateEditor());
    FScopedTransaction Next("After shutdown");
    ASSERT_TRUE(Next.IsOutstanding());
    Next.Cancel(); Second->PreExit();
    EXPECT_EQ(GUndo, nullptr); EXPECT_EQ(GEditor, nullptr);
}

TEST(FTransBufferTests, UndoBarrierStackUsesOnlyTheTopCursorAndNeverBlocksRedo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), -1);
	Buffer->RemoveUndoBarrier();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	Buffer->SetUndoBarrier();
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 1u);
	std::string Reason;
	EXPECT_FALSE(Buffer->CanUndo(&Reason)); EXPECT_FALSE(Reason.empty());
	EXPECT_FALSE(Buffer->Undo());
	EXPECT_EQ(Value, 1); EXPECT_TRUE(Buffer->GetUndoContext().Description.empty());
	EXPECT_FALSE(Buffer->GetUndoContext(false).Description.empty());
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	Buffer->SetUndoBarrier();
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 2u);
	EXPECT_FALSE(Buffer->Undo());
	Buffer->RemoveUndoBarrier();
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 1); EXPECT_FALSE(Buffer->Undo());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Value, 2);
	Buffer->ClearUndoBarriers();
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 0);
	Buffer->SetUndoBarrier(); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 0u);
	Buffer->Reset("Barrier reset"); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), -1);
	EXPECT_FALSE(Buffer->CanUndo(&Reason)); EXPECT_EQ(Reason, "Barrier reset");
}

TEST(FTransBufferTests, RedoBranchReplacementAndUndoWithoutRedoPreserveBarrierAdmission)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	Buffer->SetUndoBarrier();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 9)));
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 1u);
	ASSERT_TRUE(Buffer->Undo(false)); EXPECT_EQ(Value, 1);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_FALSE(Buffer->CanRedo()); EXPECT_FALSE(Buffer->Undo());
	Buffer->RemoveUndoBarrier(); ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 0);
}
TEST(FTransBufferTests, BudgetChangesWaitForCommitAndRemapBarrierAfterPrefixEviction)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	Buffer->SetUndoBarrier(); ASSERT_TRUE(Buffer->SetMemoryBudget(0));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_EQ(Buffer->Begin("test", "Canceled budget"), 0);
	EXPECT_FALSE(Buffer->SetMemoryBudget(100)); Buffer->ClearUndoBarriers();
	Buffer->Cancel(); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 1u);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 1, 2)));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 0u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Value, 1); EXPECT_FALSE(Buffer->Undo());
}

TEST(FTransBufferTests, ModuleRetirementRemapsBarriersAndPublishesWholeEntryDiscard)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int First = 0, Second = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(First, 0, 1, nullptr, "RetiredModule")));
	Buffer->SetUndoBarrier();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Second, 0, 2)));
	(void)ReadTransactionEvents(*Buffer);
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("RetiredModule"));
	EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 0u); EXPECT_EQ(Buffer->GetQueueLength(), 1u);
	const auto Events = ReadTransactionEvents(*Buffer);
	ASSERT_EQ(Events.size(), 1u); EXPECT_EQ(Events[0].Type, ETransactionEventType::Discarded);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(First, 1); EXPECT_EQ(Second, 0);
}

TEST(FTransBufferTests, PieOwnerAndBothSerializedSidesAreInspectedAndPreserveRedo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PieInspection")); Package->SetPlayInEditor(true);
	TStrongObjectPtr<DTransactionRecordParticipant> Pie(NewObject<DTransactionRecordParticipant>(Package.Get(), "Pie"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "Editor"));
	int Value = 0;
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1)));
	ASSERT_TRUE(Buffer->Undo()); const auto RedoId = Buffer->GetRedoContext().TransactionId;
	for (int Side = 0; Side != 3; ++Side)
	{
		Target->Hard = Side == 1 ? Pie.Get() : nullptr;
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "PIE transient"});
		auto* Owner = Side == 0 ? Pie.Get() : Target.Get();
		ASSERT_TRUE(Owner->Modify()); Owner->Value += 1;
		Target->Hard = Side == 2 ? Pie.Get() : nullptr;
		const auto PieReferences = GUndo->ContainsPieObjects(); ASSERT_TRUE(PieReferences); EXPECT_EQ(*PieReferences, Side != 2);
		EXPECT_EQ(Buffer->ContainsPieObjects(), Side != 2);
		EXPECT_EQ(Scope.End(), 1);
		EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetRedoContext().TransactionId, RedoId);
		EXPECT_EQ(Owner->Value, Side == 0 ? 1 : Side); // discarded recording leaves live writes intact
	}
	EXPECT_FALSE(Buffer->ContainsPieObjects());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Value, 1);
}

TEST(FTransBufferTests, PieInspectionIncludesDeclaredCustomAndAnnotationDependencies)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PieDependencies")); Package->SetPlayInEditor(true);
	TStrongObjectPtr<DObject> Pie(NewObject<DObject>(Package.Get(), "Dependency"));
	int Value = 0;
	const auto Result = Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, 0, 1, Pie.Get()));
	ASSERT_TRUE(Result); EXPECT_EQ(Value, 1);
	const auto Events = ReadTransactionEvents(*Buffer);
	ASSERT_FALSE(Events.empty());
	EXPECT_EQ(Events.back().Type, ETransactionEventType::Discarded);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "AnnotatedPie"));
	Target->bAnnotationEnabled = true; Target->AnnotationReference = Pie.Get();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "PIE annotation"}); ASSERT_TRUE(Target->Modify()); Target->Value = 3; }
	EXPECT_EQ(Buffer->GetQueueLength(), 0u);
}

TEST(FTransBufferTests, PieInspectionDoesNotPromoteWeakReferences)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PieWeak")); Package->SetPlayInEditor(true);
	TStrongObjectPtr<DObject> Pie(NewObject<DObject>(Package.Get(), "WeakDependency"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "WeakOwner"));
	Target->Weak = Pie.Get();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Weak PIE"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_FALSE(Buffer->ContainsPieObjects());
	Package->SetPlayInEditor(false); EXPECT_FALSE(Package->IsPlayInEditor());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0);
}

TEST(FTransBufferTests, ScopedStoreUndoRecordsAlreadyAppliedWritesWithoutApplyingAgain)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	int Value = 2; bool bApplied = false;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {"Test", "Already applied"});
		Value = 7;
		auto Change = std::make_unique<FTestCustomChange>(Value, 2, 7);
		Change->ReplayProbe = [&] { bApplied = true; };
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
	}
	EXPECT_FALSE(bApplied); EXPECT_EQ(Value, 7);
	EXPECT_EQ(Buffer->GetTransaction(0)->GetContext().OperationId, 0u);
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(bApplied); EXPECT_EQ(Value, 2);
}

TEST(FTransBufferTests, ModuleRetirementCannotResurrectAnEarlierCleanCheckpoint)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ModuleCheckpoint"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ModuleCheckpoint", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Owner"));
	Package->ClearDirty(); int DomainValue = 1;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Mixed module entry"});
		ASSERT_TRUE(Target->Modify()); Target->Value = 1;
		ASSERT_TRUE(Buffer->StoreUndo(std::make_unique<FTestCustomChange>(DomainValue, 0, 1, nullptr, "RetiredCheckpointModule")));
	}
	Buffer->SetUndoBarrier();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Surviving entry"}); ASSERT_TRUE(Target->Modify()); Target->Value = 2; }
	SimulatePackageSave(*Package);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->DiscardCustomChangesByModule("RetiredCheckpointModule"));
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 0u);
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Package->IsDirty());
	SimulatePackageSave(*Package);
	EXPECT_FALSE(Package->IsDirty());
}

TEST(FTransBufferTests, ForgetPackageRemapsBarriersAndInvalidatesSurvivingParticipants)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> First(NewObject<DPackage>(nullptr, "ForgottenParticipant"));
	TStrongObjectPtr<DPackage> Second(NewObject<DPackage>(nullptr, "RetainedParticipant"));
	(void)MakeSoftPath(); FPackagePath FirstPath, SecondPath;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ForgottenParticipant", FirstPath)); First->InitializeAssetPackage(FirstPath);
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/RetainedParticipant", SecondPath)); Second->InitializeAssetPackage(SecondPath);
	TStrongObjectPtr<DTransactionRecordParticipant> A(NewObject<DTransactionRecordParticipant>(First.Get(), "A"));
	TStrongObjectPtr<DTransactionRecordParticipant> B(NewObject<DTransactionRecordParticipant>(Second.Get(), "B"));
	First->ClearDirty(); Second->ClearDirty();
	ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FDeclaredParticipantChange>(A.Get(), B.Get(), nullptr)));
	Buffer->SetUndoBarrier();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Retained package entry"}); ASSERT_TRUE(B->Modify()); B->Value = 8; }
	SimulatePackageSave(*Second);
	Buffer->ForgetPackage(*First);
	EXPECT_EQ(Buffer->GetQueueLength(), 1u); EXPECT_EQ(Buffer->GetCurrentUndoBarrier(), 0u);
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Second->IsDirty());
}
TEST(FTransBufferTests, ReviewSaveDuringRecordingCannotMakeAnOldStateClean)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "RecordingSaveFence"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/RecordingSaveFence", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Owner"));
	Package->ClearDirty();
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Save inside recording"});
		ASSERT_TRUE(Target->Modify()); Target->Value = 7;
		SimulatePackageSave(*Package);
	}
	ASSERT_TRUE(Buffer->Undo()); EXPECT_EQ(Target->Value, 0); EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_EQ(Target->Value, 7); EXPECT_TRUE(Package->IsDirty());
	SimulatePackageSave(*Package);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_FALSE(Package->IsDirty());
}

TEST(FTransBufferTests, UndoRestoresCapturedDirtyBitAfterUnrecordedEdits)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "StandaloneOutsideWrites"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/StandaloneOutsideWrites", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Owner"));
	Package->ClearDirty();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Recorded"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
	EXPECT_FALSE(Target->Modify()); Target->UnrecordedValue = 17;
	{
		Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Scoped command"});
		Target->Value = 2;
		ASSERT_TRUE(Buffer->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 1, 2)));
	}
	ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->Value, 0); EXPECT_EQ(Target->UnrecordedValue, 17); EXPECT_FALSE(Package->IsDirty());
}

TEST(FTransBufferTests, RejectedForeignStoreUndoPreservesTheRecordedDirtyBit)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer, Foreign;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "RejectedOutsideWrites"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/RejectedOutsideWrites", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Owner"));
	Package->ClearDirty();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Recorded"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
	EXPECT_FALSE(Target->Modify()); Target->UnrecordedValue = 29;
	ASSERT_EQ(Foreign->Begin("test", "Foreign scope"), 0);
	EXPECT_FALSE(Buffer->StoreUndo(Target.Get(), std::make_unique<FTestCustomChange>(Target->Value, 0, 1)));
	Foreign->Cancel(); ASSERT_TRUE(Buffer->Undo());
	EXPECT_EQ(Target->UnrecordedValue, 29); EXPECT_FALSE(Package->IsDirty());
}

TEST(FTransBufferTests, ReviewHistoryQueriesMatchGlobalAdmissionAndBarrierDiagnostics)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer, Foreign;
	int Value = 0;
	for (int Index = 0; Index != 2; ++Index)
		ASSERT_TRUE(Durin::Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(Value, Index, Index + 1)));
	ASSERT_TRUE(Buffer->Undo());
	ASSERT_EQ(Foreign->Begin("test", "Foreign scope"), 0);
	std::string Reason;
	EXPECT_FALSE(Buffer->CanUndo(&Reason)); EXPECT_FALSE(Reason.empty());
	EXPECT_FALSE(Buffer->CanRedo(&Reason)); EXPECT_FALSE(Reason.empty());
	Foreign->Cancel(); Buffer->SetUndoBarrier();
	std::string ReplayError;
	EXPECT_FALSE(Buffer->UndoExpected(Buffer->GetUndoContext().TransactionId, &ReplayError));
	EXPECT_NE(ReplayError.find("barrier"), std::string::npos);
	ASSERT_TRUE(Buffer->Redo()); Buffer->RemoveUndoBarrier(); ASSERT_TRUE(Buffer->Undo()); ASSERT_TRUE(Buffer->Undo());
}

TEST(FTransBufferTests, LivePieInverseTerminatesUndoAndRedo)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "PieInverse")); Package->SetPlayInEditor(true);
	TStrongObjectPtr<DObject> Pie(NewObject<DObject>(Package.Get(), "Dependency"));
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "EditorOwner"));
	for (const bool bUndo : {true, false})
	{
		{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Ordinary history"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
		if (!bUndo) ASSERT_TRUE(Buffer->Undo());
		const auto Value = Target->Value; Target->Hard = Pie.Get();
		EXPECT_DEATH((void)(bUndo ? Buffer->Undo() : Buffer->Redo()), "Play-in-Editor");
		Target->Hard = nullptr;
		Target->Value = 0;
		Buffer->Reset();
	}
}
TEST(FTransBufferTests, SaveDuringReplayAdvancesTheDirtyFence)
{
	using namespace Durin;
	InitializeDObjectSystem(); Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "ReplaySaveFence"));
	(void)MakeSoftPath(); FPackagePath Path;
	ASSERT_TRUE(FPackagePath::TryCreate("/TransactionTests/ReplaySaveFence", Path)); Package->InitializeAssetPackage(Path);
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Package.Get(), "Owner"));
	Package->ClearDirty();
	{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Recorded"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
	Target->Trace = [&](std::string_view Event) {
		if (Event == "post") SimulatePackageSave(*Package);
	};
	ASSERT_TRUE(Buffer->Undo()); EXPECT_TRUE(Package->IsDirty());
	Target->Trace = {};
	SimulatePackageSave(*Package);
	EXPECT_FALSE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Redo()); EXPECT_TRUE(Package->IsDirty());
	ASSERT_TRUE(Buffer->Undo()); EXPECT_FALSE(Package->IsDirty());
}

TEST(FTransBufferTests, CompletionCallbacksKeepHistoryAndExchangeDirtyState)
{
	using namespace Durin;
	InitializeDObjectSystem();
	for (const bool FromObserver : {false, true})
	{
		Tests::FTestTransactorOwner Buffer;
		TStrongObjectPtr<DPackage> Package(NewObject<DPackage>(nullptr, "CompletionPieFence"));
		TStrongObjectPtr<DObject> Dependency(NewObject<DObject>(Package.Get(), "Dependency"));
		TStrongObjectPtr<DPackage> Asset(NewObject<DPackage>(nullptr, "CompletionAsset"));
		(void)MakeSoftPath(); FPackagePath Path;
		ASSERT_TRUE(FPackagePath::TryCreate(FromObserver ? "/TransactionTests/ObserverAsset" : "/TransactionTests/PostUndoAsset", Path));
		Asset->InitializeAssetPackage(Path);
		TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(Asset.Get(), "Owner"));
		Asset->ClearDirty();
		int PreviousValue = 0;
		const auto Previous = Tests::RecordTestEdit(*Buffer, std::make_unique<FTestCustomChange>(PreviousValue, 0, 1));
		ASSERT_NE(Previous, 0u);
		Target->Hard = Dependency.Get();
		{ Durin::Tests::FBufferRecording Scope(Buffer.Get(), {.Description = "Recorded"}); ASSERT_TRUE(Target->Modify()); Target->Value = 1; }
		const auto Head = Buffer->GetUndoContext().TransactionId;
		Buffer->DispatchEvents();
		if (FromObserver) Target->TransactionObserver = [&](const auto&) { Package->SetPlayInEditor(true); };
		else Target->Trace = [&](std::string_view Event) { if (Event == "post") Package->SetPlayInEditor(true); };
		ASSERT_TRUE(Buffer->Undo());
		EXPECT_EQ(Target->Value, 0);
		EXPECT_TRUE(Buffer->CanTransact());
		EXPECT_FALSE(Asset->IsDirty());
		EXPECT_EQ(Buffer->GetQueueLength(), 2u);
		EXPECT_EQ(Buffer->GetUndoContext().TransactionId, Previous);
		EXPECT_EQ(Buffer->GetRedoContext().TransactionId, Head);
		const auto Events = ReadTransactionEvents(*Buffer);
		ASSERT_EQ(Events.size(), 1u);
		EXPECT_EQ(Events.front().Type, ETransactionEventType::Undone);
		EXPECT_EQ(Events.front().Id, Head);
		EXPECT_EQ(Events.front().Details.find("Play-in-Editor"), std::string::npos);
		EXPECT_TRUE(Buffer->ContainsPieObjects());
		EXPECT_FALSE(Buffer->RedoExpected(Head));
		Target->Trace = {};
		Target->TransactionObserver = {};
		Package->SetPlayInEditor(false);
		ASSERT_TRUE(Buffer->RedoExpected(Head));
		EXPECT_EQ(Target->Value, 1);
		EXPECT_TRUE(Asset->IsDirty());
	}
}
TEST(FTransBufferTests, UndoDoesNotInspectReferencesAfterObjectNotification)
{
	using namespace Durin;
	InitializeDObjectSystem();
	bool bThrowDuringInspection = false;
	Tests::FTestTransactorOwner Buffer;
	TStrongObjectPtr<DTransactionRecordParticipant> Target(NewObject<DTransactionRecordParticipant>(nullptr, "InspectionFailureOwner"));
	auto Change = std::make_unique<FManagementProbeChange>();
	auto* Probe = Change.get();
	{
		Tests::FBufferRecording Recording(Buffer.Get(), {.Description = "Edit with native references"});
		ASSERT_TRUE(Buffer->StoreUndo(std::move(Change)));
		ASSERT_TRUE(Target->Modify());
		Target->Value = 1;
	}
	Probe->OnInspect = [&] {
		if (bThrowDuringInspection) throw std::runtime_error("Unknown inspection effects");
	};
	Target->TransactionObserver = [&](const auto&) { bThrowDuringInspection = true; };
	ASSERT_TRUE(Buffer->Undo());
	EXPECT_TRUE(bThrowDuringInspection);
	EXPECT_EQ(Target->Value, 0);
}
