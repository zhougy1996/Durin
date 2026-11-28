#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "MaterialGraphSchema.h"
#include "MaterialGraphEditInternals.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Materials/MaterialExpressionFunctionCall.h"

namespace Durin::MaterialEditor::GraphEditInternals
{
	// Replacement is a private draft with the ID of a current member.
	auto CommitExpressionReplacement(DObject& Owner, DMaterialExpression& Replacement, DTransactor* Transactions)
		-> FMaterialGraphCommandResult;

	using FProposedLinks = std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>;
	using FProposedNumerics = std::unordered_map<const FMaterialNumericInput*, FMaterialNumericInput>;
	using FProposedCallOutputs = std::unordered_map<DMaterialExpressionFunctionCall*, std::vector<FMaterialFunctionOutputBinding>>;

	struct FExpressionOutputPin
	{
		FMaterialExpressionOutput Output;
		int32 SourceIndex;
	};

	auto GetExpressionOutputPin(const DMaterialExpression& Expression, int32 Ordinal) -> std::optional<FExpressionOutputPin>;
	auto FindExpressionOutputPin(const DMaterialExpression& Expression, int32 SourceIndex, FGuid PortId = {}) -> std::optional<FExpressionOutputPin>;
	auto GetExpressionInputFallback(const DMaterialExpression& Expression, int32 Index) -> std::vector<float>;
	auto ResolveGraphOutput(DMaterialExpression* Source, int32 Index, FGuid Port, FProposedCallOutputs& CallOutputs) -> FMaterialExpressionInput;
	auto PrepareExpressionReplacement(FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root,
		FProposedLinks& Links, DMaterialExpression& Previous, DMaterialExpression& Replacement) -> void;
	MATERIALEDITOR_API auto SetNodeLayout(DMaterialExpression& Expression, int32 X, int32 Y, std::string Label = {}) -> void;
	// Structural edits prepare detached members and any retained links before recording.
	MATERIALEDITOR_API auto CommitGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
		FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root = nullptr, FProposedLinks Links = {},
		FProposedCallOutputs CallOutputs = {}, FProposedNumerics Numerics = {}) -> FMaterialGraphCommandResult;

	// These bindings are appended on demand by the command that connects an output.
	auto CollectCallOutputEdits(FProposedCallOutputs& Outputs, std::vector<DObject*>& Objects) -> void;
	auto WriteCallOutputs(FProposedCallOutputs& Outputs) -> void;

	auto RecordGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DObject* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult;

	// Asset writes use the owning expression and its numeric slot or function port.
	using FExpressionInputKey = std::variant<uint32, FGuid>;
	MATERIALEDITOR_API auto WriteExpressionConnection(DObject* Owner, DMaterialExpression* Target, FExpressionInputKey Input,
		FMaterialExpressionInput Source, bool Replace = false, bool bShouldTransact = true,
		std::string* OutError = nullptr, FGuid SourcePort = {}) -> bool;
	MATERIALEDITOR_API auto WriteRootConnection(DMaterial* Material, EMaterialProperty Property,
		FMaterialExpressionInput Source, bool Replace = false, bool bShouldTransact = true,
		std::string* OutError = nullptr, FGuid SourcePort = {}) -> bool;
	MATERIALEDITOR_API auto WriteInputDefault(DObject* Owner, DMaterialExpression* Expression, FExpressionInputKey Input,
		FMaterialInputDefault Value, bool bShouldTransact = true) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto WriteInputDefault(DMaterial* Material, EMaterialProperty Property,
		FMaterialInputDefault Value, bool bShouldTransact = true) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto SetInputConstantEnabled(DObject* Owner, DMaterialExpression* Expression, uint32 Index,
		bool Enabled, bool bShouldTransact = true) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto SetInputConstantEnabled(DMaterial* Material, EMaterialProperty Property,
		bool Enabled, bool bShouldTransact = true) -> FMaterialGraphCommandResult;
	auto GetExpressionInputKey(const FMaterialGraphPinView& Input) -> FExpressionInputKey;
	// The command supplies one synchronous mutation after all affected objects have been recorded.
	auto EditExpressions(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DMaterialExpression* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto EditExpressionInputs(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::vector<DMaterialExpression*> Objects, const std::function<void()>& Apply,
		const FProposedLinks& Links = {}, const FProposedNumerics& Numerics = {},
		std::map<DMaterialExpression*, EMaterialProgramValueType> KnownOutputs = {}, uint32* InferredCount = nullptr,
		FProposedCallOutputs CallOutputs = {}) -> FMaterialGraphCommandResult;
	auto ValidateProposedGraph(DObject& Owner, const FMaterialExpressionCollection& Collection,
		const FProposedLinks& Links, const FMaterialRootInputs* Root, const FMaterialParameterDefinition* SharedParameter) -> FMaterialGraphCommandResult;
	auto ValidateGraphEdit(DObject& Owner, const std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>& Links = {}) -> FMaterialGraphCommandResult;
}
