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
	// Owning-thread editor access. The caller must retain removed objects, record
	// edits before writing and publish the settled graph.
	// Compilation validity is deliberately separate from editable storage validity.
	struct FMaterialExpressionEditing
	{
		ENGINE_API static auto GetExpressions(DObject& Owner) -> std::vector<TObjectPtr<DMaterialExpression>>&;
		ENGINE_API static auto GetParameterDeclarations(const DMaterial& Owner, const FMaterialParameterDefinition& Definition)
			-> std::expected<std::vector<DMaterialExpressionParameter*>, std::string>;
		ENGINE_API static auto SetRootInputs(DMaterial& Owner, FMaterialRootInputs Inputs) -> bool;
		ENGINE_API static void VisitRootConnections(DMaterial& Owner, const std::function<void(FMaterialExpressionInput&)>& Visit);
		ENGINE_API static auto ValidateStorage(DObject& Owner) -> FMaterialProgramValidationResult;
		ENGINE_API static auto ValidateStorage(const DObject& Owner, const FMaterialExpressionCollection& Collection,
			const FMaterialParameterDefinition* SharedParameter = nullptr) -> FMaterialProgramValidationResult;
		ENGINE_API static auto ReconcileOwnership(DObject& Owner,
			std::span<DMaterialExpression* const> Previous) -> void;
		ENGINE_API static auto Publish(DObject& Owner, uint64 OperationId = 0) -> void;
	};
#endif
}
