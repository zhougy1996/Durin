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
	inline constexpr uint32 CurrentMaterialGraphClipboardSchemaVersion = 2;

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
		std::string Name;
		FMaterialProgramLink Link;
		EMaterialProgramValueType SourceType = EMaterialProgramValueType::Float;
		std::vector<EMaterialProgramValueType> AcceptedTypes;
	};

	// Describes one creatable closed-domain node shape and its stable pin types.
	struct FMaterialGraphCatalogEntry
	{
		std::string Name;
		std::string OperationName;
		std::string SecondaryName;
		std::string Category;
		std::string Description;
		FMaterialProgramNode NodeTemplate;
		std::vector<std::string> InputNames;
		std::vector<std::vector<EMaterialProgramValueType>> AcceptedInputTypes;
	};

	// Describes one node and its optional shared authored position without exposing mutable storage.
	struct FMaterialGraphNodeView
	{
		FMaterialProgramNode Node;
		std::string PrimaryLabel;
		std::string SecondaryLabel;
		std::vector<FMaterialGraphPinView> Inputs;
		std::optional<FMaterialGraphNodePresentation> Presentation;
	};

	// Is a detached deterministic snapshot used by widgets, tests, and automation.
	struct FMaterialGraphView
	{
		std::vector<FMaterialGraphNodeView> Nodes;
		FMaterialSurfaceOutputs Outputs;
		std::optional<std::pair<int32, int32>> MaterialOutputPosition;
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

	struct FMaterialGraphSurfaceDefaultRequest
	{
		EMaterialSurfaceOutput Output = EMaterialSurfaceOutput::BaseColor;
		FMaterialProgramLiteral Value;
	};

	struct FMaterialGraphSurfaceNodeRequest
	{
		EMaterialSurfaceOutput Output = EMaterialSurfaceOutput::BaseColor;
		int32 X = 0;
		int32 Y = 0;
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
		bool bConnectAggregateSurface = false;
		FGuid AggregateSourceNodeId;
	};

	// Defines stable logical canvas dimensions shared by layout, rendering, and tests.
	struct FMaterialGraphCanvasMetrics
	{
		float NodeWidth = 224.0f;
		float HeaderHeight = 30.0f;
		float SecondaryHeight = 20.0f;
		float PinRowHeight = 24.0f;
		float BodyPadding = 10.0f;
		float ColumnGap = 96.0f;
		float RowGap = 28.0f;
		float SurfaceWidth = 360.0f;
		float SurfaceHeaderHeight = 48.0f;
		float SurfaceLabelWidth = 144.0f;
		float SurfaceValueGap = 12.0f;
		float SurfaceValueWidth = 184.0f;
		float MinimumHitDiameter = 16.0f;
	};

	enum class EMaterialGraphDetailLevel : uint8
	{
		Overview,
		Readable,
		Editing,
	};

	// Provides deterministic presentation calculations without mutable ImGui state.
	class FMaterialGraphGeometry
	{
	public:
		MATERIALEDITOR_API static auto GetMetrics()
			-> const FMaterialGraphCanvasMetrics&;
		MATERIALEDITOR_API static auto GetNodeHeight(uint32 InputCount) -> float;
		MATERIALEDITOR_API static auto GetSurfacePinOffset(uint32 InputIndex) -> float;
		MATERIALEDITOR_API static auto SelectDetailLevel(
			float Zoom,
			EMaterialGraphDetailLevel Previous) -> EMaterialGraphDetailLevel;
	};

	// Provides candidate-validated graph editing with no widget or viewport dependency.
	class FMaterialGraphOperations
	{
	public:
		MATERIALEDITOR_API static auto Inspect(const DMaterial& Material)
			-> FMaterialGraphView;
		MATERIALEDITOR_API static auto Inspect(
			const DMaterial& Material,
			std::span<const FMaterialGraphCatalogEntry> Catalog)
			-> FMaterialGraphView;
		MATERIALEDITOR_API static auto EnumerateCatalog(const DMaterial& Material)
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto SearchCatalog(
			const DMaterial& Material,
			std::string_view Query,
			std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto SearchCatalog(
			std::span<const FMaterialGraphCatalogEntry> Catalog,
			std::string_view Query,
			std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto CreateNode(
			DMaterial& Material,
			FMaterialGraphCreateNodeRequest Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto CreateNodeWithDefaultInputs(
			DMaterial& Material,
			FMaterialGraphCreateNodeRequest Request,
			std::span<const std::vector<EMaterialProgramValueType>> AcceptedInputTypes,
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
		MATERIALEDITOR_API static auto AssignAggregateSurface(
			DMaterial& Material,
			const FGuid& SourceNodeId,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto DisconnectAggregateSurface(
			DMaterial& Material,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto DisconnectSurfaceOutput(
			DMaterial& Material,
			EMaterialSurfaceOutput Output,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto SetSurfaceDefault(
			DMaterial& Material,
			const FMaterialGraphSurfaceDefaultRequest& Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto ResetSurfaceDefault(
			DMaterial& Material,
			EMaterialSurfaceOutput Output,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto SetParameterValue(
			DMaterial& Material,
			const FGuid& ParameterId,
			FMaterialParameterValue Value,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto PromoteSurfaceOutputToParameter(
			DMaterial& Material,
			const FMaterialGraphSurfaceNodeRequest& Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto AddTextureToSurfaceOutput(
			DMaterial& Material,
			const FMaterialGraphSurfaceNodeRequest& Request,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto MoveNodes(
			DMaterial& Material,
			std::span<const FMaterialGraphNodePresentation> Positions,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto MoveMaterialOutput(
			DMaterial& Material,
			int32 X,
			int32 Y,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto Layout(
			DMaterial& Material,
			std::span<const FGuid> NodeIds = {},
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		// Calculates the same deterministic layout without mutating the material.
		MATERIALEDITOR_API static auto CalculateLayout(
			const DMaterial& Material,
			std::span<const FGuid> NodeIds,
			FMaterialGraphPresentation& OutPresentation)
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
		MATERIALEDITOR_API auto BeginMaterialOutput(
			DMaterial& Material,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Apply(
			std::span<const FMaterialGraphNodePresentation> Positions)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto ApplyMaterialOutput(int32 X, int32 Y)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Commit() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Cancel() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto IsActive() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	// Publishes parameter previews during one pointer gesture and records one undo step.
	class FMaterialGraphParameterEditSession
	{
	public:
		MATERIALEDITOR_API FMaterialGraphParameterEditSession();
		MATERIALEDITOR_API ~FMaterialGraphParameterEditSession();
		FMaterialGraphParameterEditSession(const FMaterialGraphParameterEditSession&) = delete;
		auto operator=(const FMaterialGraphParameterEditSession&)
			-> FMaterialGraphParameterEditSession& = delete;

		MATERIALEDITOR_API auto Begin(
			DMaterial& Material,
			const FGuid& ParameterId,
			FTransactionManager* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Apply(FMaterialParameterValue Value)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Commit() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Cancel() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto IsActive() const -> bool;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
