#pragma once
#include "CoreMinimal.h"

namespace Durin::ContentStorage
{
	enum class EFileDeletionEffectStatus : uint8 { Committed, Failed, Unattempted };
	enum class EFileDeletionStatus : uint8 { Complete, Partial, Failed, Cancelled };
	// Native confirmed-deletion facts used to publish removals and stop unsafe work.
	struct FFileDeletionResult
	{
		struct FEffect
		{
			std::string PhysicalPath;
			EFileDeletionEffectStatus Status = EFileDeletionEffectStatus::Unattempted;
			std::string Diagnostic;
			std::vector<std::string> RecoveryLocations;
		};
		std::vector<FEffect> Effects;
		bool bCancelled = false;
		bool bStopped = false;
		bool bNoChanges = false;
		auto HasCommittedEffects() const -> bool
		{
			return std::ranges::any_of(Effects, [](const auto& Effect) { return Effect.Status == EFileDeletionEffectStatus::Committed; });
		}
		auto GetStatus() const -> EFileDeletionStatus
		{
			if (bCancelled) return EFileDeletionStatus::Cancelled;
			const bool Complete = (!Effects.empty() || bNoChanges) && std::ranges::all_of(Effects, [](const auto& Effect) { return Effect.Status == EFileDeletionEffectStatus::Committed; });
			return Complete ? EFileDeletionStatus::Complete : HasCommittedEffects() ? EFileDeletionStatus::Partial : EFileDeletionStatus::Failed;
		}
		auto IsComplete() const -> bool { return GetStatus() == EFileDeletionStatus::Complete; }
		auto Add(std::string PhysicalPath, EFileDeletionEffectStatus Status, std::string Diagnostic = {}) -> void
		{
			Effects.push_back({std::move(PhysicalPath), Status, std::move(Diagnostic), {}});
		}
	};
}
