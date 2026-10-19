#pragma once

#include "CoreMinimal.h"

#include "MaterialGraphOperations.h"
#include "MaterialGraphSchema.h"
#include "Materials/MaterialFunction.h"

namespace Durin::Editor::Material
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
		MATERIALEDITOR_API auto CanCreate(const FMaterialGraphCreationAction& Action,
			std::optional<EMaterialProgramValueType> SourceType = {}) const -> bool;
		MATERIALEDITOR_API auto Connect(const FMaterialGraphPinAddress& Target, const FMaterialGraphPinAddress& Source,
			bool bReplaceExisting = false, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Disconnect(const FMaterialGraphPinAddress& Target,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Inspect() const -> FMaterialGraphView;
		MATERIALEDITOR_API auto Inspect(std::span<const FMaterialGraphCatalogEntry> Catalog) const -> FMaterialGraphView;
		MATERIALEDITOR_API auto InspectNodes(std::span<const FGuid> Nodes,
			std::span<const FMaterialGraphCatalogEntry> Catalog) const -> FMaterialGraphView;
		MATERIALEDITOR_API auto SetPort(bool bOutput, FMaterialFunctionPort Port,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto AddPort(bool bOutput, FMaterialFunctionPort Port,
			FMaterialProgramLink Source = {}, int32 X = 0, int32 Y = 0,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto RemovePort(bool bOutput, const FGuid& PortId,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X = 0, int32 Y = 0,
			FMaterialExpressionInput FirstInput = {}, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateExpression(const DMaterialExpression& Expression, int32 X = 0, int32 Y = 0,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ReplaceExpression(const DMaterialExpression& Expression,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetConstantValue(const FGuid& NodeId, const FMaterialParameterValue& Value,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetSwizzleComponents(const FGuid& NodeId, std::span<const uint8> Components,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto RemoveNodes(std::span<const FGuid> NodeIds,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetUseMaterialAttributes(bool bEnabled, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetInputConstantEnabled(const FGuid& NodeId, uint32 InputIndex,
			bool Enabled, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetInputDefault(const FGuid& NodeId, uint32 InputIndex,
			FMaterialInputDefault Value, FGuid PortId = {}, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ExtractInputDefault(const FGuid& NodeId, uint32 InputIndex,
			FGuid PortId = {}, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InlineInputNode(const FGuid& NodeId, uint32 InputIndex,
			FGuid PortId = {}, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CopySelection(std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Paste(const FMaterialGraphClipboardPayload& Payload,
			int32 X, int32 Y, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto DuplicateNodes(std::span<const FGuid> NodeIds,
			int32 OffsetX = 40, int32 OffsetY = 40, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CutSelection(std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload, ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		// Resolves the material terminal and moves it using the common position command.
		MATERIALEDITOR_API auto MoveMaterialOutput(int32 X, int32 Y,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		// Position commands ignore labels and preserve unrelated presentation fields.
		MATERIALEDITOR_API auto MoveNodes(std::span<const FMaterialGraphNodePresentation> Positions,
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CalculateLayout(std::span<const FGuid> NodeIds,
			FMaterialGraphPresentation& OutPresentation) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Layout(std::span<const FGuid> NodeIds = {},
			ETransactionMode Mode = ETransactionMode::Record) const -> FMaterialGraphCommandResult;

	private:
		friend class FMaterialGraphMoveSession;
		auto MoveNodesInternal(std::span<const FMaterialGraphNodePresentation> Positions,
			DTransactor* Transactions) const -> FMaterialGraphCommandResult;
		auto InspectSelection(std::span<const FMaterialGraphCatalogEntry> Catalog,
			const std::unordered_set<FGuid>* Selection) const -> FMaterialGraphView;
		TWeakObjectPtr<DObject> Owner;
		const FMaterialGraphSchema& Schema;
	};
}
