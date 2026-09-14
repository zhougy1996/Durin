#pragma once

#include "MaterialGraphOperations.h"
#include "Materials/MaterialFunction.h"

namespace Durin::Editor::Material
{
	// A detached candidate; Capture clones expressions and Commit takes an independent copy.
	struct FMaterialGraphDocumentState
	{
		bool bFunction = false;
		std::vector<TStrongObjectPtr<DMaterialExpression>> Expressions;
		FMaterialExpressionSurfaceOutputs Outputs;
		FMaterialFunctionSignature Signature;
		FMaterialGraphPresentation Presentation;
	};

	// The shared owning-thread editing boundary; retains no asset or preview owner.
	class FMaterialGraphDocument
	{
	public:
		MATERIALEDITOR_API explicit FMaterialGraphDocument(DObject& Owner);
		MATERIALEDITOR_API auto Capture(FMaterialGraphDocumentState& OutState) const -> bool;
		MATERIALEDITOR_API auto Inspect() const -> FMaterialGraphView;
		MATERIALEDITOR_API auto Inspect(std::span<const FMaterialGraphCatalogEntry> Catalog) const -> FMaterialGraphView;
		MATERIALEDITOR_API auto Commit(FMaterialGraphDocumentState Candidate,
			std::string Description, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetSignature(FMaterialFunctionSignature Signature,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto AddPort(bool bOutput, FMaterialFunctionPort Port,
			FMaterialProgramLink Source = {}, int32 X = 0, int32 Y = 0,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto RemovePort(bool bOutput, const FGuid& PortId,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateCatalogNode(const FMaterialGraphCatalogEntry& Entry, int32 X = 0, int32 Y = 0,
			FMaterialExpressionInput FirstInput = {}, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateExpression(const DMaterialExpression& Expression, int32 X = 0, int32 Y = 0,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ReplaceExpression(const DMaterialExpression& Expression,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetConstantValue(const FGuid& NodeId, const FMaterialParameterValue& Value,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetSwizzleComponents(const FGuid& NodeId, std::span<const uint8> Components,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto RemoveNodes(std::span<const FGuid> NodeIds,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ConnectInput(const FGuid& NodeId, uint32 InputIndex,
			FMaterialProgramLink Source, bool bReplaceExisting = false,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		// No attribute selects the aggregate Surface output; an empty link disconnects.
		MATERIALEDITOR_API auto AssignMaterialOutput(std::optional<EMaterialSurfaceOutput> Attribute,
			FMaterialProgramLink Source, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InsertFunctionCall(DMaterialFunctionInterface& Function,
			int32 X, int32 Y, std::span<const FMaterialFunctionInputBinding> Inputs,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ConnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
			FMaterialProgramLink Source, bool bReplaceExisting = false,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto DisconnectCallInput(const FGuid& CallNodeId, const FGuid& InputId,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto SetInputDefault(const FGuid& NodeId, uint32 InputIndex,
			FMaterialInputDefault Value, FGuid PortId = {}, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ExtractInputDefault(const FGuid& NodeId, uint32 InputIndex,
			FGuid PortId = {}, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto InlineInputNode(const FGuid& NodeId, uint32 InputIndex,
			FGuid PortId = {}, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ExtractUVSettings(const FGuid& NodeId,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CopySelection(std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Paste(const FMaterialGraphClipboardPayload& Payload,
			int32 X, int32 Y, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CutSelection(std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
	private:
		TWeakObjectPtr<DObject> Owner;
	};
}
