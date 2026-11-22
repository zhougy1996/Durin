#pragma once

#include "CoreMinimal.h"
#include "EditorEngineTestSupport.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/StrongObjectPtr.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	struct FTransBufferTestAccess
	{
		static auto AddCommand(FTransaction& Transaction, std::unique_ptr<FCommandChange> Change) -> void
		{ Transaction.AddRecord(std::move(Change)); }

		static auto AddPackageTransition(FTransaction& Transaction, DPackage& Package) -> void
		{ Transaction.PackageTransitions.push_back({.Package = FPersistentObjectRef(&Package)}); }

		static auto CaptureIndex(const FTransaction& Transaction) { return Transaction.CaptureIndex.get(); }

		static auto InitializeMemoryBudget(DTransBuffer& Buffer, size_t Bytes) -> void
		{
			check(Bytes != 0 && Buffer.NextTransactionId == 1 && !Buffer.Pending && Buffer.History.empty());
			Buffer.MemoryBudget = Bytes;
		}
		static auto InstallHistory(DTransBuffer& Buffer, FTransaction Transaction) -> void
		{
			Buffer.History.push_back(std::move(Transaction));
			Buffer.Cursor = 1;
		}
		static auto GetState(const DTransBuffer& Buffer) -> ETransactorState { return Buffer.State; }
		static auto SetState(DTransBuffer& Buffer, ETransactorState State) -> void
		{
			Buffer.State = State;
		}
	};
}

namespace Durin::Tests
{
	// Pair raw Begin/End in low-level tests that deliberately run without an editor.
	class FBufferRecording
	{
	public:
		FBufferRecording(DTransactor* InBuffer, FTransactionContext Context, bool Enabled = true)
			: Buffer(InBuffer), Index(Enabled ? Buffer->Begin(Context.Name, Context.Description) : -1)
		{
			if (IsOutstanding()) Buffer->SetPrimaryUndoObject(Context.PrimaryObject);
		}
		~FBufferRecording() { (void)End(); }
		FBufferRecording(const FBufferRecording&) = delete;
		auto operator=(const FBufferRecording&) -> FBufferRecording& = delete;
		auto IsOutstanding() const -> bool { return Index >= 0; }
		auto End() -> int32 { return std::exchange(Index, -1) >= 0 ? Buffer->End() : 0; }
		auto Cancel() -> void
		{
			if (IsOutstanding() && Buffer->CanTransact()) Buffer->Cancel(std::exchange(Index, -1));
		}
	private:
		DTransactor* Buffer;
		int32 Index;
	};

	// Seed real history for consumers that exercise Undo, Redo or history inspection.
	inline auto RecordTestEdit(DTransactor& Buffer, std::unique_ptr<FCommandChange> Change) -> FTransactionId
	{
		FBufferRecording Scope(&Buffer, {"TestEdit", std::string(Change->GetDescription())});
		if (!Scope.IsOutstanding() || !Change->Apply()) return 0;
		const auto Id = GUndo->GetContext().TransactionId;
		return Buffer.StoreUndo(std::move(Change)) ? Id : 0;
	}

	inline auto CollectTransactionEvents(DTransactor& Buffer) -> std::vector<FTransactionEvent>
	{
		std::vector<FTransactionEvent> Events;
		const auto Id = Buffer.AddTransactionListener([&](const auto& Event) { Events.push_back(Event); });
		Buffer.DispatchEvents(); Buffer.RemoveTransactionListener(Id);
		return Events;
	}

	// Editor-command tests exercise the real process editor and its own history.
	class FTestEditorTransactorOwner final
	{
	public:
		explicit FTestEditorTransactorOwner(std::string_view NamePrefix = "TestEditor", size_t MemoryBudget = 64u * 1024u * 1024u)
			: Editor(NewObject<DEditorEngine>(nullptr, MakeName(NamePrefix)))
		{
			FTransBufferTestAccess::InitializeMemoryBudget(*Get(), MemoryBudget);
			if (!Editor->ActivateEditor()) throw std::runtime_error("Test editor activation failed.");
		}
		~FTestEditorTransactorOwner()
		{
			Editor->PreExit();
			MarkObjectHierarchyAsGarbage(Editor.Get());
		}
		auto Get() const -> DTransBuffer* { return static_cast<DTransBuffer*>(Editor->GetTransactor()); }
		auto operator*() const -> DTransBuffer& { return *Get(); }
		auto operator->() const -> DTransBuffer* { return Get(); }
	private:
		static auto MakeName(std::string_view Prefix) -> FName
		{
			static uint64 NextId = 1;
			return FName(std::string(Prefix) + std::to_string(NextId++));
		}
		TStrongObjectPtr<DEditorEngine> Editor;
	};

	// Gives a native test one uniquely named, strongly retained transaction buffer.
	class FTestTransactorOwner final
	{
	public:
		explicit FTestTransactorOwner(std::string_view NamePrefix = "TestTransactor", size_t MemoryBudget = 64u * 1024u * 1024u)
			: Transactor(NewObject<DTransBuffer>(nullptr, MakeName(NamePrefix)))
			, TransactorRoot(Transactor)
		{ FTransBufferTestAccess::InitializeMemoryBudget(*Transactor, MemoryBudget); }

		auto Get() const -> DTransBuffer* { return Transactor; }
		auto operator*() const -> DTransBuffer& { return *Transactor; }
		auto operator->() const -> DTransBuffer* { return Transactor; }

	private:
		static auto MakeName(std::string_view Prefix) -> FName
		{
			static uint64 NextId = 1;
			return FName(std::string(Prefix) + std::to_string(NextId++));
		}

		DTransBuffer* Transactor = nullptr;
		TStrongObjectPtr<DObject> TransactorRoot;
	};
}
