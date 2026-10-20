#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Misc/Transaction.h"

namespace Durin
{
	class DObject;
	class FProperty;

	// Uses stable reflected ownership rather than a live container address.
	// Typed range payloads and mutation adapters are supplied by the owning module.
	enum class ETransactionArrayOperation : int8 { Remove = -1, Replace = 0, Insert = 1 };
	struct FTransactionArrayCapture
	{
		DObject* Object = nullptr;
		const FProperty* Member = nullptr;
		uint32 MemberIndex = 0;
		size_t Index = 0;
		size_t Count = 0;
		ETransactionArrayOperation Operation = ETransactionArrayOperation::Replace;
	};

	// Runtime object capture boundary. Editor owns the current recording's lifetime.
	class ITransaction
	{
	public:
		virtual ~ITransaction() = default;
		virtual auto SaveObject(DObject* Object) -> void = 0;
		virtual auto ContainsObject(const DObject* Object) const -> bool = 0;
		virtual auto GetContext() const -> FTransactionContext = 0;
		virtual auto SetPrimaryObject(DObject* Object) -> void = 0;
		virtual auto BeginOperation() -> FTransactionCaptureResult = 0;
		virtual auto EndOperation() -> FTransactionCaptureResult = 0;
		virtual auto Finalize() -> FTransactionCaptureResult = 0;
		virtual auto StoreUndo(DObject* Object, std::unique_ptr<FCommandChange> Change) -> FTransactionCaptureResult = 0;
		virtual auto SnapshotObject(DObject* Object, std::span<const FProperty* const> Properties = {}) -> FTransactionCaptureResult = 0;
		virtual auto SaveArray(const FTransactionArrayCapture& Capture) -> FTransactionCaptureResult = 0;
		// Unsupported inspection must be reported, never interpreted as no PIE references.
		virtual auto ContainsPieObjects() const -> std::expected<bool, std::string> = 0;
	};
}
