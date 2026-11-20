#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialFunctionTypes.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "DObject/StrongObjectPtr.h"

#include "MaterialEditorAPI.h"
#include "Transactions/Transaction.h"
#include "Materials/MaterialProgramTypes.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	class DMaterial;
	class DTransactor;
}

namespace Durin::MaterialEditor
{
	class DMaterialGraph;
	class DMaterialGraphNode;

	inline constexpr uint32 CurrentMaterialGraphClipboardSchemaVersion = 14;

	// Identifies the stable outcome of one graph inspection or mutation request.
	enum class EMaterialGraphCommandStatus : uint8
	{
		Succeeded,
		NoChange,
		Rejected,
		StaleOwner,
	};

	enum class EMaterialGraphPinKind : uint8 { Input, FunctionInput, MaterialAttribute, MaterialSurface, Output, FunctionOutput, AttributeOverride };
	struct FMaterialGraphPinAddress
	{
		FGuid NodeId;
		EMaterialGraphPinKind Kind = EMaterialGraphPinKind::Input;
		uint32 Index = 0;
		FGuid PortId;
		auto operator==(const FMaterialGraphPinAddress&) const -> bool = default;
		static auto Input(FGuid Node, uint32 Index, FGuid Port = {}) -> FMaterialGraphPinAddress
		{ return {Node, Port.IsValid() ? EMaterialGraphPinKind::FunctionInput : EMaterialGraphPinKind::Input, Port.IsValid() ? 0u : Index, Port}; }
		static auto MaterialOutput(FGuid Node, std::optional<EMaterialProperty> Attribute = {}) -> FMaterialGraphPinAddress
		{ return {Node, Attribute ? EMaterialGraphPinKind::MaterialAttribute : EMaterialGraphPinKind::MaterialSurface,
			Attribute ? static_cast<uint32>(*Attribute) : static_cast<uint32>(EMaterialOutputPin::Surface)}; }
		static auto AttributeOverride(FGuid Node, EMaterialProperty Property) -> FMaterialGraphPinAddress
		{ return {Node, EMaterialGraphPinKind::AttributeOverride, static_cast<uint32>(Property)}; }
		static auto Output(FMaterialProgramLink Link) -> FMaterialGraphPinAddress
		{ return {Link.SourceNodeId, Link.SourceOutputId.IsValid() ? EMaterialGraphPinKind::FunctionOutput : EMaterialGraphPinKind::Output, Link.SourceOutputIndex, Link.SourceOutputId}; }
	};

	// Status controls flow; editor-owned text explains rejection. Material diagnostics
	// retain node/port locations for graph navigation, independently of command status.
	struct FMaterialGraphCommandResult
	{
		EMaterialGraphCommandStatus Status = EMaterialGraphCommandStatus::Succeeded;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
		std::string Message;

		auto HasError() const -> bool
		{
			return Status == EMaterialGraphCommandStatus::Rejected
				|| Status == EMaterialGraphCommandStatus::StaleOwner;
		}
		explicit operator bool() const { return !HasError(); }
		auto GetStatus() const -> EMaterialGraphCommandStatus { return Status; }
	};

	MATERIALEDITOR_API auto FormatMaterialGraphCommandResult(const FMaterialGraphCommandResult& Result) -> std::string;

	template<class T> using TMaterialGraphResult = std::expected<T, FMaterialGraphCommandResult>;

	template<class T>
	auto FormatMaterialGraphCommandResult(const TMaterialGraphResult<T>& Result) -> std::string
	{
		return Result ? std::string{} : FormatMaterialGraphCommandResult(Result.error());
	}

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
		FMaterialFunctionValueConstraint Constraint;
		FMaterialInputDefault InlineDefault;
		FMaterialInputDefault RetainedConstant;
		bool bSupportsConstant = false;
		bool bUseConstant = false;
		bool bAdvanced = false;
		bool bActive = true;
	};

	struct FMaterialGraphOutputPinView
	{
		// Slot on the expression, independent of display order. Unbound signature pins have no slot.
		int32 SourceIndex = 0;
		uint8 OutputIndex = 0;
		FGuid PortId;
		std::string Name;
		EMaterialProgramValueType Type = EMaterialProgramValueType::Float;
		FMaterialFunctionValueConstraint Constraint;
		bool bMissing = false;
	};

	// Class plus result type identifies an authored shape; Opcode carries operation semantics.
	// Creation search selects exposed variants without treating MIR opcodes as registry keys.
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
	struct FMaterialGraphCollectionParameterCreation
	{
		std::string CollectionPath;
		FGuid ParameterId;
		std::string ParameterName;
	};
	// Detached action metadata and payload shared by all creation entry points.
	struct FMaterialGraphCreationAction
	{
		std::string Id, Name, Category, Keywords, Description;
		bool bMaterial = true, bFunction = true;
		std::variant<FMaterialGraphCatalogEntry, std::string,
			FMaterialGraphPortCreation,
			FMaterialGraphCollectionParameterCreation> Payload;
		MATERIALEDITOR_API auto PerformAction(DMaterialGraph& Graph, int32 X, int32 Y,
			std::optional<FMaterialGraphPinAddress> Source = {}, ETransactionMode Mode = ETransactionMode::Record,
			std::vector<DMaterialGraphNode*>* AuxiliaryNodes = nullptr) const -> TMaterialGraphResult<DMaterialGraphNode*>;
	};
	MATERIALEDITOR_API auto MakeCreationAction(const FMaterialGraphCatalogEntry& Entry) -> FMaterialGraphCreationAction;
	MATERIALEDITOR_API auto MakeFunctionCreationAction(std::string Path) -> FMaterialGraphCreationAction;
	MATERIALEDITOR_API auto MakeCollectionParameterCreationAction(
		std::string Path, FGuid ParameterId, std::string ParameterName)
		-> FMaterialGraphCreationAction;
	MATERIALEDITOR_API auto MakePortCreationAction(bool bOutput, EMaterialProgramValueType Type) -> FMaterialGraphCreationAction;

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
		FMaterialAttributeValues Outputs;
		bool bFunction = false;
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
		FMaterialExpressionInput AggregateSurface;
	};

	// Defines stable logical canvas dimensions shared by layout, rendering, and tests.
}
