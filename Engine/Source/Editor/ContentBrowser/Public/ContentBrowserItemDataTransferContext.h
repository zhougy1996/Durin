#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserOperationResult.h"
#include "ContentBrowserItemData.h"

namespace Durin::ContentBrowser
{
	// Collision keys are opaque to generic dispatch. Shared directories may merge;
	// two file owners, or a file and a directory, may never claim the same target.
	struct FContentBrowserTransferDestination
	{
		std::string Key;
		bool bDirectory = false;
	};
	class FContentBrowserItemDataTransferContext
	{
	public:
		using FValidate = std::function<bool(std::string*)>;
		using FExecute = std::function<FContentBrowserOperationResult(const std::function<bool()>&)>;
		FContentBrowserItemDataTransferContext(std::vector<FContentBrowserItemContributionKey> InItems,
			std::vector<FContentBrowserTransferDestination> InDestinations, FValidate InValidate, FExecute InExecute)
			: Items(std::move(InItems)), Destinations(std::move(InDestinations)), ValidateRequest(std::move(InValidate)), ExecuteRequest(std::move(InExecute)) {}
		FContentBrowserItemDataTransferContext(const FContentBrowserItemDataTransferContext&) = delete;
		auto operator=(const FContentBrowserItemDataTransferContext&) -> FContentBrowserItemDataTransferContext& = delete;
		auto GetDestinations() const -> std::span<const FContentBrowserTransferDestination> { return Destinations; }
		auto SetResultItemGetter(std::function<FContentBrowserItemData()> Getter) -> void { ResultItemGetter = std::move(Getter); }
		auto GetResultItemData() const -> FContentBrowserItemData { const auto Getter = ResultItemGetter; return Outcome->bHasItem && Getter ? Getter() : FContentBrowserItemData{}; }
		auto Validate(std::string* Error = nullptr) const -> bool
		{
			if (Error) Error->clear();
			const auto DestinationsSnapshot = Destinations;
			const auto ValidateSnapshot = ValidateRequest;
			const bool Available = !bConsumed;
			return Available && ValidClaims(DestinationsSnapshot, Error) && ValidateSnapshot && ValidateSnapshot(Error);
		}
		auto RequestCancel() -> void { Cancelled->store(true); }
		auto Execute() -> FContentBrowserOperationResult
		{
			// Own all state before invoking provider callbacks. Dismissing a pending
			// operation during validation cannot free an executing callback/capture.
			const auto CapturedItems = Items;
			const auto CapturedDestinations = Destinations;
			const auto ValidateSnapshot = ValidateRequest;
			auto Commit = std::move(ExecuteRequest);
			const auto Cancellation = Cancelled;
			const auto ResultState = Outcome;
			const bool Consumed = std::exchange(bConsumed, true);
			FContentBrowserOperationResult Result;
			std::string Error;
			if (Consumed || Cancellation->load() || !ValidClaims(CapturedDestinations, &Error) || !ValidateSnapshot || !ValidateSnapshot(&Error) || Cancellation->load() || !Commit)
			{
				Result.bCancelled = Cancellation->load();
				Result.bStopped = true;
				for (const auto& Item : CapturedItems) Result.Add(Item, EContentBrowserEffectStatus::Unattempted, Error.empty() ? "Transfer was cancelled or consumed." : Error);
				return Result;
			}
			Result = Commit([Cancellation] { return Cancellation->load(); });
			ResultState->bHasItem = Result.HasCommittedEffects() || Result.IsComplete();
			return Result;
		}

	private:
		std::vector<FContentBrowserItemContributionKey> Items;
		std::vector<FContentBrowserTransferDestination> Destinations;
		FValidate ValidateRequest;
		FExecute ExecuteRequest;
		std::function<FContentBrowserItemData()> ResultItemGetter;
		struct FOutcome { bool bHasItem = false; };
		std::shared_ptr<FOutcome> Outcome = std::make_shared<FOutcome>();
		bool bConsumed = false;
		std::shared_ptr<std::atomic_bool> Cancelled = std::make_shared<std::atomic_bool>(false);
		static auto ValidClaims(std::span<const FContentBrowserTransferDestination> Destinations, std::string* Error) -> bool
		{
			std::unordered_map<std::string, bool> Claims;
			for (const auto& Destination : Destinations)
			{
				auto [Existing, Inserted] = Claims.try_emplace(Destination.Key, Destination.bDirectory);
				if (!Inserted && (!Existing->second || !Destination.bDirectory))
				{ if (Error) *Error = "Multiple selected items claim the same destination."; return false; }
			}
			return true;
		}
	};
}
