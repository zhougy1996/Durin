#pragma once

#include "MaterialGraphOperations.h"
#include "Materials/MaterialFunction.h"

namespace Durin::Editor::Material
{
	// Common authored state for material and function graph commands. Function
	// documents use Program.Nodes and Signature, with no root declarations/output.
	struct FMaterialGraphDocumentState
	{
		bool bFunction = false;
		FMaterialProgram Program;
		std::vector<FMaterialParameterDefinition> Definitions;
		FMaterialFunctionSignature Signature;
		std::vector<FMaterialFunctionCall> Calls;
		FMaterialGraphPresentation Presentation;
		auto operator==(const FMaterialGraphDocumentState&) const -> bool = default;
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
		MATERIALEDITOR_API auto CreateNode(FMaterialGraphCreateNodeRequest Request,
			DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto CreateNodeWithDefaultInputs(FMaterialGraphCreateNodeRequest Request,
			FMaterialProgramLink FirstInput = {}, DTransactor* Transactions = nullptr) const -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ReplaceNode(FMaterialProgramNode Node,
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
