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

	auto ResolveGraphOutput(DMaterialExpression* Source, int32 Index, FGuid Port, FProposedCallOutputs& CallOutputs) -> FMaterialExpressionInput;
	auto PrepareExpressionReplacement(FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root,
		FProposedLinks& Links, DMaterialExpression& Previous, DMaterialExpression& Replacement) -> void;
	MATERIALEDITOR_API auto SetNodeLayout(DMaterialExpression& Expression, int32 X, int32 Y, std::string Label = {}) -> void;
	MATERIALEDITOR_API auto CommitGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
		FMaterialExpressionCollection& Collection, FMaterialRootInputs* Root = nullptr, FProposedLinks Links = {},
		FProposedCallOutputs CallOutputs = {}, std::vector<DMaterialExpression*> Edited = {}, const std::function<void()>& Apply = {},
		const FProposedNumerics& Numerics = {}, DMaterialExpression* PreserveIdentity = nullptr,
		const FMaterialParameterDefinition* SharedParameter = nullptr) -> FMaterialGraphCommandResult;

	auto RecordGraphEdit(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DObject* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult;

	// Asset-level writeback also supports incomplete graphs used by repair tools.
	MATERIALEDITOR_API auto WriteExpressionConnection(DObject* Owner, const FMaterialGraphPinAddress& Target,
		const FMaterialGraphPinAddress& Source, bool Replace = false, ETransactionMode Mode = ETransactionMode::Record, std::string* OutError = nullptr) -> bool;
	MATERIALEDITOR_API auto WriteInputDefault(DObject* Owner, const FMaterialGraphPinAddress& Pin,
		FMaterialInputDefault Value, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto SetInputConstantEnabled(DObject* Owner, const FMaterialGraphPinAddress& Pin,
		bool Enabled, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	// The command supplies one synchronous mutation after all affected objects have been recorded.
	auto EditExpressions(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::span<DMaterialExpression* const> Objects, const std::function<void()>& Apply) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto EditExpressionInputs(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::vector<DMaterialExpression*> Objects, const std::function<void()>& Apply,
		const FProposedLinks& Links = {}, const FProposedNumerics& Numerics = {},
		std::map<DMaterialExpression*, EMaterialProgramValueType> KnownOutputs = {}, uint32* InferredCount = nullptr) -> FMaterialGraphCommandResult;
	auto ValidateProposedGraph(DObject& Owner, const FMaterialExpressionCollection& Collection,
		const FProposedLinks& Links, const FMaterialRootInputs* Root, const FMaterialParameterDefinition* SharedParameter) -> FMaterialGraphCommandResult;
	auto ValidateGraphEdit(DObject& Owner, const std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>& Links = {}) -> FMaterialGraphCommandResult;
}
