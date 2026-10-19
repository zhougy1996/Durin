#pragma once

#include "CoreMinimal.h"

#include "DurinEdAPI.h"

namespace Durin
{
	class DPackage;
	class FReferenceCollector;
}

namespace Durin::Editor
{
	// Explicit editor admission policy; unrecorded edits cannot join a live recording.
	enum class ETransactionMode : uint8 { Record, NotTransactable };

	using FTransactionId = uint64;
	using FRevisionId = uint64;

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

	struct FCommandChangeError
	{
		std::string Message;
		bool bRecoveryRequired = false;
	};
	using FCommandChangeResult = std::expected<void, FCommandChangeError>;

	// Owns synchronous domain Undo/Redo data; initial execution belongs to the domain.
	class FCommandChange
	{
	public:
		virtual ~FCommandChange() = default;
		virtual auto Apply() -> FCommandChangeResult = 0;
		virtual auto Revert() -> FCommandChangeResult = 0;
		virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails(ETransactionOperation) const -> std::string { return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		virtual auto MutatesMountedContent() const -> bool { return false; }
		virtual auto AddReferencedObjects(FReferenceCollector&) const -> void {}
		virtual auto GetAllocatedSize() const -> size_t { return 0; }
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
