#pragma once

#include "CoreMinimal.h"
#include "Actors/EditorActorSubsystem.h"
#include "Threading/RunnableThread.h"
#include "Templates/GuardValue.h"

namespace Durin
{
	inline auto ActorEditBlockReason(const bool& bEditing) -> const char*
	{
		if (!IsInGameThread()) return "Actor edits require the game thread.";
		if (bEditing) return "Another actor edit is in progress.";
		return nullptr;
	}

	template<typename F>
	auto DEditorActorSubsystem::RunActorEdit(const FActorEditContext& Context, DLevel* Level, AActor* Owner,
		F Operation, std::string* OutError) -> std::invoke_result_t<F>
	{
		if (const char* Reason = ActorEditBlockReason(bEditing))
		{
			if (OutError) *OutError = Reason;
			return {};
		}
		const TGuardValue EditScope(bEditing, true);
		if (!ValidateEditTarget(Context, Level, Owner, OutError)) return {};
		return Operation();
	}
}
