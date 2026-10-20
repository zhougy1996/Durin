#pragma once

#include "CoreMinimal.h"
#include "CoreDObjectAPI.h"
#include "Misc/Transaction.h"
#include "DObject/TransactionObjectId.h"

namespace Durin
{
	class ITransactionObjectAnnotation;
	class DObject;

	enum class ETransactionObjectEventType : uint8 { Snapshot, Finalized, UndoRedo };
	enum class ETransactionObjectChangeCreatedBy : uint8 { TransactionRecord, TransactionAnnotation };

	// Owns an object's original identity and actual changed-field summary.
	struct FTransactionObjectChange : FTransactionObjectId
	{
		FTransactionObjectChange() = default;
		FTransactionObjectChange(FTransactionObjectId Identity, EObjectFlags Flags)
			: FTransactionObjectId(std::move(Identity)), OriginalFlags(Flags) {}
		EObjectFlags OriginalFlags = EObjectFlags::NoFlags;
		std::vector<FName> ChangedProperties;
		bool bNameChanged = false;
		bool bOuterChanged = false;
		bool bNonPropertyDataChanged = false;
		bool bEditorPresenceChanged = false;
		auto HasChanges() const -> bool
		{ return !ChangedProperties.empty() || bNameChanged || bOuterChanged || bNonPropertyDataChanged || bEditorPresenceChanged; }
	};

	// Delivered synchronously while the operation context and annotation are live.
	// History-listener queuing is editor policy, separate from this object contract.
	struct FTransactionObjectEvent
	{
		FTransactionId TransactionId = 0;
		uint64 OperationId = 0;
		ETransactionObjectEventType Type = ETransactionObjectEventType::Finalized;
		ETransactionOperation Operation = ETransactionOperation::Execute;
		FTransactionObjectChange Change;
		std::shared_ptr<ITransactionObjectAnnotation> Annotation;
		ETransactionObjectChangeCreatedBy CreatedBy = ETransactionObjectChangeCreatedBy::TransactionRecord;
	};
}
