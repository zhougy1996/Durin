#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class DObject;
	class DPackage;
	class FReferenceCollector;
	class FObjectReplacementMap;
	using FTransactionId = uint64;
	// Identifies initial execution or one direction of historical replay.
	enum class ETransactionOperation : uint8 { Execute, Undo, Redo };

	// Owns presentation values; PrimaryObject is borrowed from the transaction owner.
	// IDs are zero outside their corresponding transaction/operation lifetime.
	struct FTransactionContext
	{
		std::string Name;
		std::string Description;
		DObject* PrimaryObject = nullptr;
		FTransactionId TransactionId = 0;
		uint64 OperationId = 0;
	};

	struct FCommandChangeError
	{
		std::string Message;
		bool bRecoveryRequired = false;
	};
	using FCommandChangeResult = std::expected<void, FCommandChangeError>;
	using FTransactionCaptureResult = std::expected<void, std::string>;

	// Owns synchronous domain Undo/Redo data independently of any editor module.
	class FCommandChange
	{
	public:
		virtual ~FCommandChange() = default;
		virtual auto Apply() -> FCommandChangeResult = 0;
		virtual auto Revert() -> FCommandChangeResult = 0;
		virtual auto GetDescription() const -> std::string_view = 0;
		// Enumerate edited objects explicitly; GC dependencies do not imply edits.
		// Recording freezes exact participant identities independently of GC traversal.
		virtual auto GetModifiedObjects() const -> std::vector<DObject*> = 0;
		virtual auto GetDetails(ETransactionOperation) const -> std::string { return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		virtual auto MutatesMountedContent() const -> bool { return false; }
		virtual auto AddReferencedObjects(FReferenceCollector&) const -> void {}
		virtual auto GetAllocatedSize() const -> size_t { return 0; }
		virtual auto GetOwningModule() const -> std::string_view { return {}; }
		// Opt in by returning independent immutable data with every participant
		// rebound. Unsupported commands cause coherent whole-entry retirement.
		virtual auto CreateReplacement(const FObjectReplacementMap&) const
			-> std::expected<std::unique_ptr<FCommandChange>, std::string>
		{ return std::unexpected("This command does not support object reinstancing."); }
	};

	// Allocates a process-wide operation identity; zero reports exhausted identity space.
	CORE_API auto AllocateTransactionOperationId() -> uint64;
}
