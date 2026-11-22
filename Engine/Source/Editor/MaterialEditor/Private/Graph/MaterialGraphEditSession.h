#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "MaterialGraphSchema.h"
#include "MaterialGraphEditInternals.h"
#include "Materials/MaterialExpressionEditing.h"
#include "DObject/Archive.h"

namespace Durin::MaterialEditor::GraphEditInternals
{
	// Replacement is a private draft with the ID of a current member.
	auto CommitExpressionReplacement(DObject& Owner, DMaterialExpression& Replacement, DTransactor* Transactions)
		-> FMaterialGraphCommandResult;

	// Edits the owner's actual collection. Only touched objects are serialized;
	// an unfinished scope restores writes without notifying observers.
	class MATERIALEDITOR_API FGraphEditSession
	{
	public:
		explicit FGraphEditSession(DObject& Owner);
		~FGraphEditSession();
		FGraphEditSession(const FGraphEditSession&) = delete;
		FGraphEditSession(FGraphEditSession&&) = delete;
		auto IncludeCallOutput(DMaterialExpression* Source, FGuid OutputId) -> void;
		auto Modify(DMaterialExpression& Expression) -> void;
		auto ReplaceExpression(TObjectPtr<DMaterialExpression>& Slot, DMaterialExpression& Replacement) -> void;
		auto Assign(DMaterialExpression& Target, const DMaterialExpression& Source) -> FMaterialGraphCommandResult;
		auto GetRootInput(EMaterialProperty Property) const -> const FMaterialExpressionInput*;
		auto EditNumericRoot(EMaterialProperty Property) -> FMaterialNumericInput*;
		auto EditRootInput(EMaterialProperty Property) -> FMaterialExpressionInput*;
		void SetRootMode(bool UseAttributes);
		void ResetRoot();
		auto Commit(std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult;
		// Work counter for profiling and bounded-inference regression checks.
		uint32 InferredNumericNodes = 0;
		bool bFunction;
		std::vector<TObjectPtr<DMaterialExpression>>& Expressions;
		FMaterialGraphPresentation Presentation;
	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

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
	using FProposedLinks = std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>;
	using FProposedNumerics = std::unordered_map<const FMaterialNumericInput*, FMaterialNumericInput>;
	auto EditExpressionInputs(DObject& Owner, DTransactor* Transactions, std::string Description,
		std::vector<DMaterialExpression*> Objects, const std::function<void()>& Apply,
		const FProposedLinks& Links = {}, const FProposedNumerics& Numerics = {},
		std::map<DMaterialExpression*, EMaterialProgramValueType> KnownOutputs = {}) -> FMaterialGraphCommandResult;
	auto ValidateGraphEdit(DObject& Owner, const std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>& Links = {}) -> FMaterialGraphCommandResult;
	auto AdaptNumericTypes(FGraphEditSession& State, std::span<DMaterialExpression* const> ChangedNodes, std::span<DMaterialExpression* const> ChangedOutputNodes) -> bool;
}
