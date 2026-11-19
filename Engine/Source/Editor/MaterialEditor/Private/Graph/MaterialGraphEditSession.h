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
		auto Modify(DMaterialExpression& Expression) -> void;
		auto ReplaceExpression(TObjectPtr<DMaterialExpression>& Slot, DMaterialExpression& Replacement) -> void;
		auto Assign(DMaterialExpression& Target, const DMaterialExpression& Source) -> FMaterialGraphCommandResult;
		auto GetOutputs() -> FMaterialAttributeRoot&;
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
		const FMaterialGraphPinAddress& Source, bool Replace = false, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto WriteInputDefault(DObject* Owner, const FGuid& NodeId, uint32 InputIndex,
		FMaterialInputDefault Value, FGuid PortId = {}, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	MATERIALEDITOR_API auto SetInputConstantEnabled(DObject* Owner, const FGuid& NodeId, uint32 InputIndex,
		bool Enabled, ETransactionMode Mode = ETransactionMode::Record) -> FMaterialGraphCommandResult;
	// Prepare only the fields that will be written. The transactor owns the undo data.
	class FExpressionEdit
	{
	public:
		auto SetLink(DMaterialExpression& Object, FMaterialExpressionInput& Field, FMaterialExpressionInput Value) -> void
		{
			Links.emplace(&Field, Value);
			Set(Object, Field, Value);
		}
		auto SetNumeric(DMaterialExpression& Object, FMaterialNumericInput& Field, FMaterialNumericInput Value) -> void
		{
			Numerics.emplace(&Field, Value);
			Set(Object, Field, std::move(Value));
		}
		template<class T> auto Set(DMaterialExpression& Object, T& Field, T Value) -> void
		{
			if (!std::ranges::contains(Objects, &Object)) Objects.push_back(&Object);
			Writes.emplace_back([&Field, Next = std::move(Value)]() mutable { std::swap(Field, Next); });
		}
		auto Commit(DObject& Owner, DTransactor* Transactions, std::string Description) -> FMaterialGraphCommandResult;
		auto CommitRename(DObject& Owner, DMaterialExpression& Expression, std::string NewName,
			DTransactor* Transactions) -> FMaterialGraphCommandResult;
		auto CommitInferred(DObject& Owner, DTransactor* Transactions, std::string Description,
			std::map<DMaterialExpression*, EMaterialProgramValueType> KnownOutputs = {}) -> FMaterialGraphCommandResult;
	private:
		std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput> Links;
		std::unordered_map<const FMaterialNumericInput*, FMaterialNumericInput> Numerics;
		std::vector<DMaterialExpression*> Objects;
		std::vector<std::function<void()>> Writes;
	};
	auto ValidateGraphEdit(DObject& Owner, const std::unordered_map<const FMaterialExpressionInput*, FMaterialExpressionInput>& Links = {}) -> FMaterialGraphCommandResult;
	auto AdaptNumericTypes(FGraphEditSession& State, std::span<DMaterialExpression* const> ChangedNodes, std::span<DMaterialExpression* const> ChangedOutputNodes) -> bool;
}
