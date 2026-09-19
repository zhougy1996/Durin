#pragma once

#include "MaterialGraphDocument.h"
#include "MaterialGraphEditInternals.h"
#include "Materials/MaterialExpressionEditing.h"
#include "DObject/Archive.h"

namespace Durin::Editor::Material::GraphEditInternals
{
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
		auto Assign(DMaterialExpression& Target, const DMaterialExpression& Source) -> FMaterialGraphCommandResult;
		auto GetOutputs() -> FMaterialExpressionSurfaceOutputs&;
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

	auto AdaptNumericTypes(FGraphEditSession& State, std::span<const FGuid> ChangedNodes, std::span<const FGuid> ChangedOutputNodes) -> bool;
}
