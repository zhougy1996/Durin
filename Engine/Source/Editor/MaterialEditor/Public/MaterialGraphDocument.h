#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "MaterialGraphOperations.h"
#include "MaterialGraphSchema.h"
#include "Materials/MaterialFunction.h"

namespace Durin { struct FMaterialConfiguration; }

namespace Durin::MaterialEditor
{
	// The shared owning-thread editing boundary; retains no asset or preview owner.
	class FMaterialGraphDocument
	{
	public:
		// Owner must be a DMaterial or DMaterialFunction; instances do not own graphs.
		MATERIALEDITOR_API explicit FMaterialGraphDocument(DObject& Owner);
		auto GetOwner() const -> DObject* { return Owner.Get(); }
		auto GetSchema() const -> const FMaterialGraphSchema& { return Schema; }
		MATERIALEDITOR_API auto Create(const FMaterialGraphCreationRequest& Request,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto AddPort(bool bOutput, FMaterialFunctionPort Port,
			FMaterialProgramLink Source = {}, int32 X = 0, int32 Y = 0,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto RemovePort(bool bOutput, const FGuid& PortId,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X = 0, int32 Y = 0,
			FMaterialProgramLink Source = {}, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateExpression(const DMaterialExpression& Expression, int32 X = 0, int32 Y = 0,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ReplaceExpression(const DMaterialExpression& Expression,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetConfiguration(const FMaterialConfiguration& Configuration,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetUseMaterialAttributes(bool bEnabled, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;

		// Creates a node; an existing name reuses its compatible parameter definition.
		MATERIALEDITOR_API auto CreateParameter(
			FMaterialParameterDefinition Definition,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		// Renames all references while preserving the parameter ID and instance overrides.
		MATERIALEDITOR_API auto RenameParameter(
			const FGuid& ParameterId, FName Name,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		// Removes every node referencing this parameter; RemoveNodes deletes individual references.
		MATERIALEDITOR_API auto DeleteParameter(
			const FGuid& ParameterId,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto PromoteConstantToParameter(
			const FGuid& NodeId, FName Name,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetParameterValue(
			const FGuid& ParameterId,
			FMaterialParameterValue Value,
			ETransactionMode Mode = ETransactionMode::Record) const
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto PromoteSurfaceOutputToParameter(
			const FMaterialGraphSurfaceNodeRequest& Request,
			ETransactionMode Mode = ETransactionMode::Record) const
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto AddTextureToSurfaceOutput(
			const FMaterialGraphSurfaceNodeRequest& Request,
			ETransactionMode Mode = ETransactionMode::Record) const
			-> FMaterialGraphCommandResult;

	private:
		friend class DMaterialGraph;
		FMaterialGraphDocument(TWeakObjectPtr<DObject> InOwner, const FMaterialGraphSchema& InSchema)
			: Owner(InOwner), Schema(InSchema) {}
		TWeakObjectPtr<DObject> Owner;
		const FMaterialGraphSchema& Schema;
	};
}
