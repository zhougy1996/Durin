#pragma once

#include "CoreMinimal.h"
#include "EditorEngineTestSupport.h"
#include "NativeTestSupport.h"

#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Transactions/Transactor.h"

namespace Durin
{
	struct FTransBufferTestAccess
	{
		static auto AddCommand(FTransaction& Transaction, std::unique_ptr<FCommandChange> Change) -> void
		{ Transaction.AddRecord(std::move(Change)); }

		static auto CaptureIndex(const FTransaction& Transaction) { return Transaction.CaptureIndex.get(); }

		static auto InitializeMemoryBudget(DTransBuffer& Buffer, size_t Bytes) -> void
		{
			check(Bytes != 0 && Buffer.NextTransactionId == 1 && !Buffer.Pending && Buffer.History.empty());
			Buffer.MemoryBudget = Bytes;
		}
		static auto SetHistoryBusy(DTransBuffer& Buffer, bool bBusy) -> void
		{
			Buffer.Guard = bBusy ? DTransBuffer::EHistoryGuard::Mutating : DTransBuffer::EHistoryGuard::None;
		}
	};
}

namespace Durin::Tests
{
	inline auto SaveTransactionTestPackage(DPackage& Package) -> testing::AssertionResult
	{
		const auto Directory = Testing::CreateTestWorkSubdirectory("TransactionSaves");
		const auto Result = Package.Save({.Destination = Directory / "Package.dasset", .Mode = EPackageSaveMode::Complete});
		if (!Result) return testing::AssertionFailure() << Result.Error->ToString();
		return testing::AssertionSuccess();
	}

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
		if (!Scope.IsOutstanding()) return 0;
		Change->Apply();
		const auto Id = GUndo->GetContext().TransactionId;
		return Buffer.StoreUndo(std::move(Change)) ? Id : 0;
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
