#pragma once

#include "CoreMinimal.h"
#include "Editor/EditorEngine.h"

namespace Durin::Editor
{
	// Defer host teardown across native construction and registration callbacks,
	// and reject recursive edits while preparation has not entered history.
	template<typename F>
	auto RunActorEdit(DEditorEngine* Host, bool& bEditing, F Operation) -> std::invoke_result_t<F>
	{
		struct FScope
		{
			bool& Flag;
			explicit FScope(bool& InFlag) : Flag(InFlag) { Flag = true; }
			~FScope() { Flag = false; }
		} Scope(bEditing);
		std::invoke_result_t<F> Result{};
		Host->DispatchSubsystemCallback([&] { Result = Operation(); });
		return Result;
	}
}
