#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorEngine.h"
#include "DObject/ObjectLifecycle.h"

namespace Durin::Editor
{
	// Defer collection and host teardown across native construction and registration
	// callbacks, and reject recursive edits while preparation has not entered history.
	template<typename F>
	auto RunActorEdit(DEditorEngine* Host, bool& bEditing, F Operation) -> std::invoke_result_t<F>
	{
		struct FScope
		{
			bool& Flag;
			FGarbageCollectionDeferralScope Deferral;
			explicit FScope(bool& InFlag) : Flag(InFlag) { Flag = true; }
			~FScope() { Flag = false; }
		} Scope(bEditing);
		std::invoke_result_t<F> Result;
		Result.Error = decltype(Result.Error)::Unavailable;
		Result.Message = "The actor editing host retired before execution.";
		Host->DispatchSubsystemCallback([&] { Result = Operation(); });
		return Result;
	}
}
