#pragma once
#include "DObject/StrongObjectPtr.h"
#include "Materials/MaterialExpressions.h"

#include "MaterialEditorAPI.h"
#include "Materials/Material.h"
#include "DObject/WeakObjectPtr.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
	class DTransactor;
}

namespace Durin::Editor::Material
{
	inline constexpr uint32 CurrentMaterialGraphClipboardSchemaVersion = 9;

	// Identifies the stable outcome of one graph inspection or mutation request.
	enum class EMaterialGraphCommandStatus : uint8
	{
		Succeeded,
		NoChange,
		Rejected,
		StaleOwner,
	};

	enum class EMaterialGraphPinKind : uint8 { Input, FunctionInput, MaterialAttribute, MaterialSurface, Output, FunctionOutput };
	struct FMaterialGraphPinAddress
	{
		FGuid NodeId;
		EMaterialGraphPinKind Kind = EMaterialGraphPinKind::Input;
		uint32 Index = 0;
		FGuid PortId;
		auto operator==(const FMaterialGraphPinAddress&) const -> bool = default;
		static auto Input(FGuid Node, uint32 Index, FGuid Port = {}) -> FMaterialGraphPinAddress
		{ return {Node, Port.IsValid() ? EMaterialGraphPinKind::FunctionInput : EMaterialGraphPinKind::Input, Port.IsValid() ? 0u : Index, Port}; }
		static auto MaterialOutput(FGuid Node, std::optional<EMaterialSurfaceOutput> Attribute = {}) -> FMaterialGraphPinAddress
		{ return {Node, Attribute ? EMaterialGraphPinKind::MaterialAttribute : EMaterialGraphPinKind::MaterialSurface,
			Attribute ? static_cast<uint32>(*Attribute) : static_cast<uint32>(EMaterialOutputPin::Surface)}; }
		static auto Output(FMaterialProgramLink Link) -> FMaterialGraphPinAddress
		{ return {Link.SourceNodeId, Link.SourceOutputId.IsValid() ? EMaterialGraphPinKind::FunctionOutput : EMaterialGraphPinKind::Output, Link.SourceOutputIndex, Link.SourceOutputId}; }
	};

	// Status controls flow; editor-owned text explains rejection. Material diagnostics
	// retain node/port locations for graph navigation, independently of command status.
	struct FMaterialGraphCommandResult
	{
		EMaterialGraphCommandStatus Status = EMaterialGraphCommandStatus::Succeeded;
		std::vector<FGuid> AffectedNodeIds;
		std::vector<FGuid> GeneratedNodeIds;
		std::vector<FGuid> AffectedParameterIds;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		std::string Message;
		std::string CleanupMessage;

		auto HasError() const -> bool
		{
			return Status == EMaterialGraphCommandStatus::Rejected
				|| Status == EMaterialGraphCommandStatus::StaleOwner;
		}
		explicit operator bool() const { return !HasError(); }
		auto GetStatus() const -> EMaterialGraphCommandStatus { return Status; }
	};

	MATERIALEDITOR_API auto FormatMaterialGraphCommandResult(const FMaterialGraphCommandResult& Result) -> std::string;

	struct FMaterialGraphPinView
	{
		uint32 InputIndex = 0;
		std::string Name;
		FMaterialProgramLink Link;
		EMaterialProgramValueType SourceType = EMaterialProgramValueType::Float;
		std::vector<EMaterialProgramValueType> AcceptedTypes;
		FGuid PortId;
		bool bRequired = false;
		bool bMissing = false;
		FMaterialFunctionDefault Default;
		FMaterialInputDefault InlineDefault;
		FMaterialInputDefault RetainedConstant;
		bool bSupportsConstant = false;
		bool bUseConstant = false;
		bool bAdvanced = false;
		bool bActive = true;
	};

	struct FMaterialGraphOutputPinView
	{
		uint8 OutputIndex = 0;
		FGuid PortId;
		std::string Name;
		EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
		bool bMissing = false;
	};

	// Describes a material-independent node shape; creation search selects exposed variants.
	struct FMaterialGraphCatalogEntry
	{
		std::string OperationName;
		std::string Category;
		std::string Description;
		DClass* ExpressionClass = nullptr;
		EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		std::vector<std::string> InputNames;
		std::vector<std::vector<EMaterialProgramValueType>> AcceptedInputTypes;
		// Prepared once with the catalog so repeated palette searches do not
		// allocate and normalize every searchable field.
		std::array<std::string, 4> NormalizedSearchFields;
	};

	struct FMaterialGraphPortCreation
	{
		bool bOutput = false;
		EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
	};
	// Detached action metadata and payload shared by all creation entry points.
	struct FMaterialGraphCreationAction
	{
		std::string Id, Name, Category, Keywords, Description;
		bool bMaterial = true, bFunction = true;
		std::variant<FMaterialGraphCatalogEntry, std::string, FMaterialGraphPortCreation> Payload;
	};
	struct FMaterialGraphCreationRequest
	{
		FMaterialGraphCreationAction Action;
		int32 X = 0, Y = 0;
		std::optional<FMaterialGraphPinAddress> Source;
	};
	MATERIALEDITOR_API auto MakeCreationAction(const FMaterialGraphCatalogEntry& Entry) -> FMaterialGraphCreationAction;
	MATERIALEDITOR_API auto MakeFunctionCreationAction(std::string Path) -> FMaterialGraphCreationAction;
	MATERIALEDITOR_API auto MakePortCreationAction(bool bOutput, EMaterialProgramValueType Type) -> FMaterialGraphCreationAction;
	MATERIALEDITOR_API auto IsGraphInputCompatible(std::span<const EMaterialProgramValueType> Accepted,
		EMaterialProgramValueType Source) -> bool;

	struct FMaterialGraphParameterInfo { FGuid Id; };
	struct FMaterialGraphSampleInfo { FGuid ParameterId; };

	// Detached display metadata; its payload contains only the selected node family.
	struct FMaterialGraphNodeDescriptor
	{
		FGuid Id;
		bool bMaterialOutput = false;
		EMaterialProgramOpcode Opcode = EMaterialProgramOpcode::Constant;
		EMaterialProgramValueType ResultType = EMaterialProgramValueType::Float;
		std::variant<std::monostate, FMaterialParameterValue, FMaterialGraphParameterInfo,
			FMaterialGraphSampleInfo, std::vector<uint8>> Data;
		auto GetParameterId() const -> FGuid
		{
			if (const auto* Parameter = std::get_if<FMaterialGraphParameterInfo>(&Data)) return Parameter->Id;
			if (const auto* Sample = std::get_if<FMaterialGraphSampleInfo>(&Data)) return Sample->ParameterId;
			return {};
		}
		auto IsSampleUVInput(uint32 Index) const -> bool
		{
			return (Opcode == EMaterialProgramOpcode::TextureSample2D && Index == 1)
				|| (Opcode == EMaterialProgramOpcode::TextureSampleParameter2D && Index == 0);
		}
		MATERIALEDITOR_API auto GetConstantLiteral() const -> FMaterialProgramLiteral;
	};

	// Describes one node and its shared authored position without exposing mutable storage.
	struct FMaterialGraphNodeView
	{
		FMaterialGraphNodeDescriptor Node;
		std::string PrimaryLabel;
		std::string SecondaryLabel;
		std::vector<FMaterialGraphPinView> Inputs;
		FMaterialGraphNodePresentation Presentation;
		std::vector<FMaterialGraphOutputPinView> Outputs;
		std::string FunctionPath;
		auto InputAddress(const FMaterialGraphPinView& Pin) const -> FMaterialGraphPinAddress
		{
			if (Node.bMaterialOutput)
				return {Node.Id, Pin.InputIndex == static_cast<uint32>(EMaterialOutputPin::Surface)
					? EMaterialGraphPinKind::MaterialSurface : EMaterialGraphPinKind::MaterialAttribute, Pin.InputIndex};
			return FMaterialGraphPinAddress::Input(Node.Id, Pin.InputIndex, Pin.PortId);
		}
	};

	// Is a detached deterministic snapshot used by widgets, tests, and automation.
	struct FMaterialGraphView
	{
		std::vector<FMaterialGraphNodeView> Nodes;
		FMaterialSurfaceOutputs Outputs;
		bool bFunction = false;
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
		TStrongObjectPtr<DMaterialExpression> Expression;
		std::string DisplayName;
		int32 RelativeX = 0;
		int32 RelativeY = 0;
	};

	// Retains texture defaults and called functions independently of the source owner.
	struct FMaterialGraphClipboardPayload
	{
		uint32 SchemaVersion = CurrentMaterialGraphClipboardSchemaVersion;
		TWeakObjectPtr<DObject> SourceRoot;
		std::vector<FMaterialGraphClipboardNode> Nodes;
		bool bConnectAggregateSurface = false;
		FGuid AggregateSourceNodeId;
		uint8 AggregateSourceOutputIndex = 0;
		FGuid AggregateSourceOutputId;
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
		float SurfaceWidth = 268.0f;
		float SurfaceLabelWidth = 144.0f;
		float SurfaceValueGap = 12.0f;
		float SurfaceValueWidth = 92.0f;
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
		MATERIALEDITOR_API static auto SelectDetailLevel(
			float Zoom,
			EMaterialGraphDetailLevel Previous) -> EMaterialGraphDetailLevel;
	};

	// Provides candidate-validated graph editing with no widget or viewport dependency.
	class FMaterialGraphOperations
	{
	public:
		MATERIALEDITOR_API static auto EnumerateCatalog()
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto SearchCatalog(
			std::string_view Query,
			std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto SearchCatalog(
			std::span<const FMaterialGraphCatalogEntry> Catalog,
			std::string_view Query,
			std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
			-> std::vector<FMaterialGraphCatalogEntry>;
		MATERIALEDITOR_API static auto SearchCatalogIndices(
			std::span<const FMaterialGraphCatalogEntry> Catalog,
			std::string_view Query,
			std::optional<EMaterialProgramValueType> SourceType = std::nullopt)
			-> std::vector<size_t>;
		// Creates a node; an existing name reuses its compatible parameter definition.
		MATERIALEDITOR_API static auto CreateParameter(
			DMaterial& Material, FMaterialParameterDefinition Definition,
			DTransactor* Transactions = nullptr) -> FMaterialGraphCommandResult;
		// Renames all references while preserving the parameter ID and instance overrides.
		MATERIALEDITOR_API static auto RenameParameter(
			DMaterial& Material, const FGuid& ParameterId, FName Name,
			DTransactor* Transactions = nullptr) -> FMaterialGraphCommandResult;
		// Removes every node referencing this parameter; RemoveNodes deletes individual references.
		MATERIALEDITOR_API static auto DeleteParameter(
			DMaterial& Material, const FGuid& ParameterId,
			DTransactor* Transactions = nullptr) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto PromoteConstantToParameter(
			DMaterial& Material, const FGuid& NodeId, FName Name,
			DTransactor* Transactions = nullptr) -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto SetSurfaceDefault(
			DMaterial& Material,
			const FMaterialGraphSurfaceDefaultRequest& Request,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto ResetSurfaceDefault(
			DMaterial& Material,
			EMaterialSurfaceOutput Output,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto SetParameterValue(
			DMaterial& Material,
			const FGuid& ParameterId,
			FMaterialParameterValue Value,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto PromoteSurfaceOutputToParameter(
			DMaterial& Material,
			const FMaterialGraphSurfaceNodeRequest& Request,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API static auto AddTextureToSurfaceOutput(
			DMaterial& Material,
			const FMaterialGraphSurfaceNodeRequest& Request,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
	};

	// Keeps detached position drafts and commits one position delta on pointer release.
	class FMaterialGraphMoveSession
	{
	public:
		MATERIALEDITOR_API FMaterialGraphMoveSession();
		MATERIALEDITOR_API ~FMaterialGraphMoveSession();
		FMaterialGraphMoveSession(const FMaterialGraphMoveSession&) = delete;
		auto operator=(const FMaterialGraphMoveSession&)
			-> FMaterialGraphMoveSession& = delete;

		MATERIALEDITOR_API auto Begin(
			DObject& Owner,
			std::span<const FGuid> NodeIds,
			DTransactor* Transactions = nullptr)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Apply(
			std::span<const FMaterialGraphNodePresentation> Positions)
			-> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Commit() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto Cancel() -> FMaterialGraphCommandResult;
		MATERIALEDITOR_API auto IsActive() const -> bool;

		MATERIALEDITOR_API auto IsCurrent(const DObject& Owner) const -> bool;
		MATERIALEDITOR_API auto GetPositions() const -> std::span<const FMaterialGraphNodePresentation>;

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
			DTransactor* Transactions = nullptr)
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
