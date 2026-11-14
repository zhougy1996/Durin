#pragma once
#include "CoreMinimal.h"

namespace Durin::ContentStorage
{
	enum class EContentDeletionEffectStatus : uint8 { Committed, Failed, Unattempted };
	enum class EContentDeletionStatus : uint8 { Complete, Partial, Failed, Cancelled };
	// Native confirmed-deletion facts used to publish removals and stop unsafe work.
	struct FContentDeletionResult
	{
		struct FEffect
		{
			std::string PhysicalPath;
			EContentDeletionEffectStatus Status = EContentDeletionEffectStatus::Unattempted;
			std::string Diagnostic;
			std::vector<std::string> RecoveryLocations;
		};
		std::vector<FEffect> Effects;
		bool bCancelled = false;
		bool bStopped = false;
		bool bNoChanges = false;
		auto HasCommittedEffects() const -> bool
		{
			return std::ranges::any_of(Effects, [](const auto& Effect) { return Effect.Status == EContentDeletionEffectStatus::Committed; });
		}
		auto GetStatus() const -> EContentDeletionStatus
		{
			if (bCancelled) return EContentDeletionStatus::Cancelled;
			const bool Complete = (!Effects.empty() || bNoChanges) && std::ranges::all_of(Effects, [](const auto& Effect) { return Effect.Status == EContentDeletionEffectStatus::Committed; });
			return Complete ? EContentDeletionStatus::Complete : HasCommittedEffects() ? EContentDeletionStatus::Partial : EContentDeletionStatus::Failed;
		}
		auto IsComplete() const -> bool { return GetStatus() == EContentDeletionStatus::Complete; }
		auto Add(std::string PhysicalPath, EContentDeletionEffectStatus Status, std::string Diagnostic = {}) -> void
		{
			Effects.push_back({std::move(PhysicalPath), Status, std::move(Diagnostic), {}});
		}
	};
}
