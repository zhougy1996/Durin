#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class DObject;

	// Runtime object capture boundary. Editor owns the current recording's lifetime.
	class ITransaction
	{
	public:
		virtual ~ITransaction() = default;
		virtual auto SaveObject(DObject* Object) -> void = 0;
		virtual auto ContainsObject(const DObject* Object, bool bWholeObjectOnly = false) const -> bool = 0;
		virtual auto GetContext() const -> FTransactionContext = 0;
		virtual auto SetPrimaryObject(DObject* Object) -> void = 0;
		virtual auto BeginOperation() -> FTransactionCaptureResult = 0;
		virtual auto EndOperation() -> FTransactionCaptureResult = 0;
		virtual auto Apply() -> void = 0;
		virtual auto Finalize() -> void = 0;
		// Store an already-applied command. Its explicit target suppresses later Modify() snapshots.
		// Call Modify() before the first write to keep a whole-object baseline alongside the command.
		virtual auto StoreUndo(DObject* Target, std::unique_ptr<FCommandChange> Change) -> FTransactionCaptureResult = 0;
		// Unsupported inspection must be reported, never interpreted as no PIE references.
		virtual auto ContainsPieObjects() const -> std::expected<bool, std::string> = 0;
	};
}
