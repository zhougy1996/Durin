#pragma once

#include "DurinEdAPI.h"

namespace Durin
{
	class DPackage;
	class FReferenceCollector;
}

namespace Durin::Editor
{
	using FTransactionId = uint64;
	using FRevisionId = uint64;
	using FTransactionDeferredCompletion = std::function<void(bool)>;

	// Identifies the history transition reported by a transaction event.
	enum class ETransactionEventType : uint8
	{
		Executed,
		Undone,
		Redone,
		Failed,
		Discarded,
		Evicted,
	};

	// Identifies the transaction operation that produced an event or failure.
	enum class ETransactionOperation : uint8
	{
		Execute,
		Undo,
		Redo,
	};

	// Carries one user-visible transaction history event.
	struct FTransactionEvent
	{
		ETransactionEventType Type = ETransactionEventType::Executed;
		ETransactionOperation Operation = ETransactionOperation::Execute;
		FTransactionId Id = 0;
		std::string Description;
		std::string Details;
	};

	// Defines a reversible editor operation stored in transaction history.
	class ITransactionCustomChange
	{
	public:
		virtual ~ITransactionCustomChange() = default;
		DURINED_API virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails(ETransactionOperation Operation) const -> std::string { (void)Operation; return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		// True only when a successful transition changes files or discovery
		// identities beneath automatically scanned mounted content.
		virtual auto MutatesMountedContent() const -> bool { return false; }
		DURINED_API virtual auto Undo() -> bool = 0;
		DURINED_API virtual auto Redo() -> bool = 0;
		virtual auto IsDeferredOperationPending() const -> bool { return false; }
		virtual auto SetDeferredOperationCompletion(
			FTransactionDeferredCompletion Completion) -> void { (void)Completion; }
		virtual auto AddReferencedObjects(FReferenceCollector& Collector) const -> void
		{
			(void)Collector;
		}
		// Reports native allocations in addition to the interface object itself.
		virtual auto GetAllocatedSize() const -> size_t { return 0; }
		// Module-owned executable changes must be drained before this module retires.
		virtual auto GetOwningModule() const -> std::string_view { return {}; }
	};

	// Describes one package's editor-session revision state.
	struct FPackageRevisionState
	{
		FRevisionId CurrentRevision = 0;
		FRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};
}
