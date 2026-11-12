#pragma once
#include "CoreMinimal.h"
#include "ContentBrowserItemKey.h"

namespace Durin::ContentBrowser
{
	enum class EContentBrowserEffectStatus : uint8 { Committed, Reverted, Failed, Unattempted };
	enum class EContentBrowserOperationStatus : uint8 { Complete, Partial, Failed, Cancelled };
	// Native utilities and provider dispatch retain their own identity domains.
	template<typename TIdentity> struct TContentBrowserOperationResult
	{
		struct FEffect
		{
			TIdentity OriginalIdentity;
			std::optional<TIdentity> NewIdentity;
			EContentBrowserEffectStatus Status = EContentBrowserEffectStatus::Unattempted;
			std::string Diagnostic;
			std::vector<std::string> RecoveryLocations;
		};
		std::vector<FEffect> Effects;
		bool bCancelled = false;
		bool bStopped = false;
		bool bNoChanges = false;
		auto HasCommittedEffects() const -> bool
		{
			return std::ranges::any_of(Effects, [](const auto& Effect) { return Effect.Status == EContentBrowserEffectStatus::Committed; });
		}
		auto GetStatus() const -> EContentBrowserOperationStatus
		{
			if (bCancelled) return EContentBrowserOperationStatus::Cancelled;
			const bool Complete = (!Effects.empty() || bNoChanges) && std::ranges::all_of(Effects, [](const auto& Effect) { return Effect.Status == EContentBrowserEffectStatus::Committed; });
			return Complete ? EContentBrowserOperationStatus::Complete : HasCommittedEffects() ? EContentBrowserOperationStatus::Partial : EContentBrowserOperationStatus::Failed;
		}
		auto IsComplete() const -> bool { return GetStatus() == EContentBrowserOperationStatus::Complete; }
		auto Add(TIdentity Identity, EContentBrowserEffectStatus Status, std::string Diagnostic = {}) -> void
		{
			std::optional<TIdentity> NewIdentity;
			if (Status == EContentBrowserEffectStatus::Committed) NewIdentity = Identity;
			Effects.push_back({std::move(Identity), std::move(NewIdentity), Status, std::move(Diagnostic), {}});
		}
		auto Append(TContentBrowserOperationResult Other) -> void
		{
			bCancelled |= Other.bCancelled;
			bStopped |= Other.bStopped;
			bNoChanges |= Other.bNoChanges;
			Effects.insert(Effects.end(), std::make_move_iterator(Other.Effects.begin()), std::make_move_iterator(Other.Effects.end()));
		}
	};
	using FContentBrowserOperationResult = TContentBrowserOperationResult<FContentBrowserItemContributionKey>;
}
