#pragma once

#include "CoreMinimal.h"

#include "Misc/Transaction.h"

namespace Durin
{
	class DPackage;
	class FReferenceCollector;
}

namespace Durin
{
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
}
