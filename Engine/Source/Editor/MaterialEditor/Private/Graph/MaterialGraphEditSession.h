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
		auto Assign(DMaterialExpression& Target, const DMaterialExpression& Source) -> bool;
		auto GetOutputs() -> FMaterialExpressionSurfaceOutputs&;
		auto Commit(std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult;
		bool bFunction;
		std::vector<TObjectPtr<DMaterialExpression>>& Expressions;
		FMaterialGraphPresentation Presentation;
	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	auto AdaptNumericTypes(FGraphEditSession& State) -> bool;
	inline auto CommitGraphEdit(DObject&, FGraphEditSession& State, std::string Description,
		DTransactor* Transactions) -> FMaterialGraphCommandResult
	{
		return State.Commit(std::move(Description), Transactions);
	}
}
