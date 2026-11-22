#pragma once

#include "CoreMinimal.h"
#include "MaterialGraphOperations.h"

namespace Durin { struct FMaterialConfiguration; }

namespace Durin::MaterialEditor::MaterialEditing
{
	MATERIALEDITOR_API auto SetConfiguration(DMaterial& Material, const FMaterialConfiguration& Configuration,
		ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto SetUseMaterialAttributes(DMaterial& Material, bool bEnabled, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;

	// Creates a node; an existing name reuses its compatible parameter definition.
	MATERIALEDITOR_API auto CreateParameter(DMaterial& Material, FMaterialParameterDefinition Definition,
		ETransactionMode Mode = ETransactionMode::Record) -> TMaterialGraphResult<DMaterialExpression*>;
	// Renames all references while preserving the parameter ID and instance overrides.
	MATERIALEDITOR_API auto RenameParameter(DMaterial& Material, const FGuid& ParameterId, FName Name,
		ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	// Removes every node referencing this parameter; RemoveNodes deletes individual references.
	MATERIALEDITOR_API auto DeleteParameter(DMaterial& Material, const FGuid& ParameterId,
		ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto PromoteConstantToParameter(DMaterial& Material, const FGuid& NodeId, FName Name,
		ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto SetParameterValue(DMaterial& Material, const FGuid& ParameterId,
		FMaterialParameterValue Value,
		ETransactionMode Mode = ETransactionMode::Record)
		-> FMaterialGraphCommandResult;
	struct FMaterialGraphSurfaceNodeRequest
	{
		EMaterialProperty Output = EMaterialProperty::MP_BaseColor;
		int32 X = 0;
		int32 Y = 0;
	};

	MATERIALEDITOR_API auto CanPromoteSurfaceOutput(const DMaterial& Material, EMaterialProperty Property) -> bool;
	MATERIALEDITOR_API auto SurfaceTextureOutput(const DMaterial& Material, EMaterialProperty Property) -> std::optional<EMaterialSampleOutput>;
	MATERIALEDITOR_API auto PromoteSurfaceOutputToParameter(DMaterial& Material, const FMaterialGraphSurfaceNodeRequest& Request,
		ETransactionMode Mode = ETransactionMode::Record)
		-> TMaterialGraphResult<DMaterialExpression*>;
	MATERIALEDITOR_API auto AddTextureToSurfaceOutput(DMaterial& Material, const FMaterialGraphSurfaceNodeRequest& Request,
		ETransactionMode Mode = ETransactionMode::Record)
		-> TMaterialGraphResult<DMaterialExpression*>;
}
