#pragma once

#include "CoreMinimal.h"

namespace Durin
{
	class DObject;
	class DPackage;
	class FReferenceCollector;

	using FTransactionId = uint64;

	// Owns presentation values; PrimaryObject is borrowed from the transaction owner.
	// IDs are zero outside their corresponding transaction/operation lifetime.
	struct FTransactionContext
	{
		std::string Name;
		std::string Description;
		DObject* PrimaryObject = nullptr;
		FTransactionId TransactionId = 0;
		uint64 OperationId = 0;
		uint64 HistoryId = 0;
		std::string Details;
	};

	using FTransactionCaptureResult = std::expected<void, std::string>;

	// Owns synchronous domain Undo/Redo data independently of any editor module.
	class FCommandChange
	{
	public:
		virtual ~FCommandChange() = default;
		virtual auto Apply(DObject* Target) -> void = 0;
		virtual auto Revert(DObject* Target) -> void = 0;
		// A missing target is passed as nullptr so tools can expire the entire entry.
		virtual auto HasExpired(const DObject* Target) const noexcept -> bool { return false; }
		virtual auto GetDescription() const -> std::string_view = 0;
		virtual auto GetDetails() const -> std::string { return {}; }
		virtual auto GetAffectedPackages() const -> std::span<DPackage* const> { return {}; }
		virtual auto AddReferencedObjects(FReferenceCollector&) -> void {}
		virtual auto GetAllocatedSize() const -> size_t { return 0; }
		virtual auto GetOwningModule() const -> std::string_view { return {}; }
	};

	// Identifies the undo buffer whose records are being applied on this thread.
	class CORE_API FScopedTransactionHistoryReplay
	{
	public:
		explicit FScopedTransactionHistoryReplay(uint64 HistoryId);
		~FScopedTransactionHistoryReplay();
		FScopedTransactionHistoryReplay(const FScopedTransactionHistoryReplay&) = delete;
		auto operator=(const FScopedTransactionHistoryReplay&) -> FScopedTransactionHistoryReplay& = delete;
		static auto GetHistoryId() -> uint64;

	private:
		friend class FTransaction;
		friend class FPersistentObjectRef;
		friend class DObject;
		FScopedTransactionHistoryReplay(uint64 HistoryId, std::span<DObject* const> PendingKill);
		static auto IncludesPendingKill(const DObject* Object) -> bool;
		std::span<DObject* const> PreviousPendingKill;
		uint64 PreviousHistoryId;
	};

	// Allocates a process-wide operation identity; zero reports exhausted identity space.
	CORE_API auto AllocateTransactionOperationId() -> uint64;
}
