#pragma once

#include "Materials/MaterialExpressions.h"

namespace Durin
{
	// Owning-thread editor access. The caller must retain removed objects, record
	// edits before writing, and finish or restore the edit before publishing it.
	// Compilation validity is deliberately separate from editable storage validity.
	struct FMaterialExpressionEditing
	{
		ENGINE_API static auto GetExpressions(DObject& Owner) -> std::vector<TObjectPtr<DMaterialExpression>>&;
		ENGINE_API static auto ValidateStorage(DObject& Owner) -> FMaterialProgramValidationResult;
		ENGINE_API static auto ReconcileOwnership(DObject& Owner,
			std::span<DMaterialExpression* const> Previous) -> void;
		ENGINE_API static auto Publish(DObject& Owner) -> void;
	};
}
