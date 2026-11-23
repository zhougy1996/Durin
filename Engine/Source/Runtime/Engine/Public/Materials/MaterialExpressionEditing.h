#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialProgramTypes.h"


namespace Durin
{
class DMaterial;
class DMaterialExpressionParameter;
#if DURIN_WITH_EDITORONLY_DATA
	struct FMaterialRootInputEdits
	{
		std::map<EMaterialProperty, FMaterialNumericInput> Numeric;
		std::optional<FMaterialExpressionInput> Attributes;
		std::optional<bool> UseAttributes;
		ENGINE_API auto HasValidWidths(const DMaterial& Material) const -> bool;
	};
	// Owning-thread editor access. The caller must retain removed objects, record
	// edits before writing, and finish or restore the edit before publishing it.
	// Compilation validity is deliberately separate from editable storage validity.
	struct FMaterialExpressionEditing
	{
		ENGINE_API static auto GetExpressions(DObject& Owner) -> std::vector<TObjectPtr<DMaterialExpression>>&;
		ENGINE_API static auto GetParameterDeclarations(const DMaterial& Owner, const FMaterialParameterDefinition& Definition)
			-> std::expected<std::vector<DMaterialExpressionParameter*>, std::string>;
		ENGINE_API static void ApplyRootEdits(DMaterial& Owner, const FMaterialRootInputEdits& Edits);
		ENGINE_API static auto SetRootInputs(DMaterial& Owner, FMaterialRootInputs Inputs) -> bool;
		ENGINE_API static void VisitRootConnections(DMaterial& Owner, const std::function<void(FMaterialExpressionInput&)>& Visit);
		ENGINE_API static auto ValidateStorage(DObject& Owner) -> FMaterialProgramValidationResult;
		ENGINE_API static auto ReconcileOwnership(DObject& Owner,
			std::span<DMaterialExpression* const> Previous) -> void;
		ENGINE_API static auto Publish(DObject& Owner, uint64 OperationId = 0) -> void;
	};
#endif
}
