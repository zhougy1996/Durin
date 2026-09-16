#pragma once
#include "MaterialGraphDocument.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Property.h"
#include "DObject/Class.h"

namespace Durin::Editor::Material::GraphEditInternals
{
	// Detached export/import state for explicit bulk callers. Ordinary commands
	// and transaction history use live object edits, never graph snapshots.
	struct FOwnedGraphSnapshot : FMaterialGraphDocumentState
	{
		auto Modify(DMaterialExpression&) -> void {}

		auto Capture(const DObject& Owner) -> bool
		{
			const FMaterialExpressionCollection* Collection = nullptr;
			if (const auto* Material = Cast<DMaterial>(&Owner))
			{
				Collection = &Material->GetExpressionCollection();
				Presentation = Material->GetMaterialGraphPresentation();
			}
			else if (const auto* Function = Cast<DMaterialFunction>(&Owner))
			{
				bFunction = true;
				Collection = &Function->GetExpressionCollection();
				Presentation.Nodes = Function->GetFunctionPresentation().Nodes;
			}
			else return false;
			for (const auto& Expression : Collection->Expressions)
			{
				auto* Copy = DuplicateObject(Expression.Get(), nullptr, NAME_None);
				if (!Copy) return false;
				Expressions.emplace_back(Copy);
			}
			return true;
		}

	};

	MATERIALEDITOR_API auto CommitOwnedExpressions(DObject& Owner, FOwnedGraphSnapshot Candidate,
		std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult;
}
