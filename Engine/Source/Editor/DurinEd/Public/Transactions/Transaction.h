#pragma once

#include "CoreMinimal.h"

#include "Misc/Transaction.h"

namespace Durin
{
	class DPackage;
	class FReferenceCollector;
}

namespace Durin::Editor
{
	// Explicit editor admission policy; unrecorded edits cannot join a live recording.
	enum class ETransactionMode : uint8 { Record, NotTransactable };

	using ::Durin::FTransactionId;
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

	using ::Durin::ETransactionOperation;
	// Carries one user-visible transaction history event.
	struct FTransactionEvent
	{
		ETransactionEventType Type = ETransactionEventType::Executed;
		ETransactionOperation Operation = ETransactionOperation::Execute;
		FTransactionId Id = 0;
		std::string Description;
		std::string Details;
		auto operator==(const FTransactionEvent&) const -> bool = default;
	};

	using ::Durin::FCommandChange;
	using ::Durin::FCommandChangeError;
	using ::Durin::FCommandChangeResult;
	using ::Durin::FTransactionContext;

	// Describes one package's editor-session revision state.
	struct FPackageRevisionState
	{
		FRevisionId CurrentRevision = 0;
		FRevisionId SavedRevision = 0;
		bool bCheckpointValid = false;
	};
}
