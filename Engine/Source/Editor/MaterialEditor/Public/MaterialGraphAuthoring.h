#pragma once

#include "MaterialEditorAPI.h"
#include "Materials/Material.h"

#include <span>
#include <string>
#include <vector>

namespace Durin::Editor
{
	class FTransactionManager;
}

namespace Durin::Editor::Material
{
	inline constexpr uint32 CurrentMaterialGraphClipboardSchemaVersion = 1;

	// Identifies the stable outcome of one graph inspection or mutation request.
	enum class EMaterialGraphCommandStatus : uint8
	{
		Succeeded,
		NoChange,
		Rejected,
		StaleOwner,
	};

	// Carries bounded validation evidence and stable IDs produced by one command.
	struct FMaterialGraphCommandResult
	{
		EMaterialGraphCommandStatus Status = EMaterialGraphCommandStatus::Rejected;
		std::vector<FGuid> AffectedNodeIds;
		std::vector<FGuid> GeneratedNodeIds;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		std::string Message;

		explicit operator bool() const
		{
			return Status == EMaterialGraphCommandStatus::Succeeded
				|| Status == EMaterialGraphCommandStatus::NoChange;
		}
	};

	struct FMaterialGraphPinView
	{
		uint32 InputIndex = 0;
		FMaterialProgramLink Link;
		EMaterialProgramValueType SourceType = EMaterialProgramValueType::Float;
		std::vector<EMaterialProgramValueType> AcceptedTypes;
	};

	// Describes one creatable closed-domain node shape and its stable pin types.
	struct FMaterialGraphCatalogEntry
	{
		std::string Name;
		FMaterialProgramNode NodeTemplate;
		std::vector<std::vector<EMaterialProgramValueType>> AcceptedInputTypes;
	};

	// Describes one node and its optional shared authored position without exposing mutable storage.
	struct FMaterialGraphNodeView
	{
		FMaterialProgramNode Node;
		std::vector<FMaterialGraphPinView> Inputs;
		std::optional<FMaterialGraphNodePresentation> Presentation;
	};

	// Is a detached deterministic snapshot used by widgets, tests, and automation.
	struct FMaterialGraphView
	{
		std::vector<FMaterialGraphNodeView> Nodes;
		FMaterialSurfaceOutputs Outputs;
	};

	struct FMaterialGraphCreateNodeRequest
	{
		FMaterialProgramNode Node;
		int32 X = 0;
		int32 Y = 0;
	};

	struct FMaterialGraphConnectRequest
	{
		FGuid SourceNodeId;
		uint8 SourceOutputIndex = 0;
		FGuid DestinationNodeId;
		uint32 DestinationInputIndex = 0;
		bool bReplaceExisting = false;
	};

	struct FMaterialGraphSurfaceOutputRequest
	{
		EMaterialSurfaceOutput Output = EMaterialSurfaceOutput::BaseColor;
		FGuid SourceNodeId;
		uint8 SourceOutputIndex = 0;
	};

	struct FMaterialGraphClipboardNode
	{
		FMaterialProgramNode Node;
		int32 RelativeX = 0;
		int32 RelativeY = 0;
	};

	// Carries one bounded versioned selection without an asset or viewport owner.
	struct FMaterialGraphClipboardPayload
	{
		uint32 SchemaVersion = CurrentMaterialGraphClipboardSchemaVersion;
		std::vector<FMaterialGraphClipboardNode> Nodes;
	};

	// Provides candidate-validated graph authoring with no widget or viewport dependency.
	class FMaterialGraphService
	{
	public:
		MATERIALEDITOR_API static auto Inspect(const DMaterial& Material)
			-> FMaterialGraphView;
		MATERIALEDITOR_API static auto EnumerateCatalog(const DMaterial& Material)
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto CreateNode(
			DMaterial& Material,
			FMaterialGraphCreateNodeRequest Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto ReplaceNode(
			DMaterial& Material,
			FMaterialProgramNode Node,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto RemoveNodes(
			DMaterial& Material,
			std::span<const FGuid> NodeIds,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto Connect(
			DMaterial& Material,
			const FMaterialGraphConnectRequest& Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto DisconnectInput(
			DMaterial& Material,
			const FGuid& DestinationNodeId,
			uint32 DestinationInputIndex,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto AssignSurfaceOutput(
			DMaterial& Material,
			const FMaterialGraphSurfaceOutputRequest& Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto MoveNodes(
			DMaterial& Material,
			std::span<const FMaterialGraphNodePresentation> Positions,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto Layout(
			DMaterial& Material,
			std::span<const FGuid> NodeIds = {},
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto CopySelection(
			const DMaterial& Material,
			std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto Paste(
			DMaterial& Material,
			const FMaterialGraphClipboardPayload& Payload,
			int32 X,
			int32 Y,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto DuplicateNodes(
			DMaterial& Material,
			std::span<const FGuid> NodeIds,
			int32 OffsetX = 40,
			int32 OffsetY = 40,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto CutSelection(
			DMaterial& Material,
			std::span<const FGuid> NodeIds,
			FMaterialGraphClipboardPayload& OutPayload,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
	};

	// Coalesces presentation previews from one pointer gesture into one transaction.
	class FMaterialGraphMoveSession
	{
	public:
		MATERIALEDITOR_API FMaterialGraphMoveSession();
		MATERIALEDITOR_API ~FMaterialGraphMoveSession();
		FMaterialGraphMoveSession(const FMaterialGraphMoveSession&) = delete;
		auto operator=(const FMaterialGraphMoveSession&)
			-> FMaterialGraphMoveSession& = delete;

		MATERIALEDITOR_API auto Begin(
			DMaterial& Material,
			std::span<const FGuid> NodeIds,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Apply(
			std::span<const FMaterialGraphNodePresentation> Positions)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Commit() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Cancel() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto IsActive() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
