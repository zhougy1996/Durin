#pragma once
#include "MaterialGraphDocument.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Archive.h"
#include "DObject/Property.h"
#include "DObject/Class.h"

namespace Durin::Editor::Material::GraphEditInternals
{
	// Command candidates and history own independent concrete expression copies.
	// Owner setters duplicate again so later edits cannot mutate a snapshot.
	struct FOwnedGraphSnapshot : FMaterialGraphDocumentState
	{

		auto Capture(const DObject& Owner) -> bool
		{
			const FMaterialExpressionCollection* Collection = nullptr;
			if (const auto* Material = Cast<DMaterial>(&Owner))
			{
				Collection = &Material->GetExpressionCollection();
				Outputs = Material->GetExpressionOutputs();
				Presentation = Material->GetMaterialGraphPresentation();
			}
			else if (const auto* Function = Cast<DMaterialFunction>(&Owner))
			{
				bFunction = true;
				Collection = &Function->GetExpressionCollection();
				Signature = Function->GetFunctionSignature();
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

		auto MatchesGraph(const DObject& Owner) const -> bool
		{
			const FMaterialExpressionCollection* Current = nullptr;
			if (bFunction)
			{
				const auto* Function = Cast<DMaterialFunction>(&Owner);
				if (!Function || Signature != Function->GetFunctionSignature()) return false;
				Current = &Function->GetExpressionCollection();
			}
			else
			{
				const auto* Material = Cast<DMaterial>(&Owner);
				if (!Material || Outputs != Material->GetExpressionOutputs()) return false;
				Current = &Material->GetExpressionCollection();
			}
			if (Expressions.size() != Current->Expressions.size()) return false;
			for (size_t Index = 0; Index < Expressions.size(); ++Index)
			{
				const auto* Before = Expressions[Index].Get();
				const auto* After = Current->Expressions[Index].Get();
				if (!Before || !After || Before->GetClass() != After->GetClass()) return false;
				bool bIdentical = true;
				Before->GetClass()->ForEachProperty([&](FProperty* Property) {
					for (uint32 Element = 0; bIdentical && Element < Property->GetArrayDim(); ++Element)
						bIdentical = ComparePropertyValues(Property, Before, Element, After, Element) == EPropertyIdentityResult::Identical;
				});
				if (!bIdentical) return false;
			}
			return true;
		}

		auto Apply(DObject& Owner) const -> bool
		{
			std::vector<DMaterialExpression*> Nodes;
			for (const auto& Expression : Expressions) Nodes.push_back(Expression.Get());
			if (bFunction)
			{
				auto* Function = Cast<DMaterialFunction>(&Owner);
				return Function && (MatchesGraph(Owner) || Function->SetFunctionExpressions(Signature, Nodes))
					&& Function->SetFunctionPresentation({.Nodes = Presentation.Nodes});
			}
			auto* Material = Cast<DMaterial>(&Owner);
			return Material && (MatchesGraph(Owner) || Material->SetMaterialExpressions(Nodes, Outputs))
				&& Material->SetMaterialGraphPresentation(Presentation);
		}

		auto AddReferencedObjects(FReferenceCollector& Collector) -> void
		{
			for (auto& Expression : Expressions)
			{
				DObject* Object = Expression.Get();
				Collector.AddReferencedObject(Object);
				Expression = Cast<DMaterialExpression>(Object);
			}
		}

		auto GetAllocatedSize() const -> size_t
		{
			// Managed expression allocations are owned by CoreDObject; count native
			// storage retained by this transaction, just as object records do.
			size_t Bytes = Expressions.capacity() * sizeof(TStrongObjectPtr<DMaterialExpression>)
				+ Presentation.Nodes.capacity() * sizeof(FMaterialGraphNodePresentation);
			for (const auto& Node : Presentation.Nodes) Bytes += Node.DisplayName.capacity();
			for (const auto* Ports : {&Signature.Inputs, &Signature.Outputs})
			{
				Bytes += Ports->capacity() * sizeof(FMaterialFunctionPort);
				for (const auto& Port : *Ports) Bytes += Port.Name.capacity();
			}
			return Bytes;
		}
	};

	MATERIALEDITOR_API auto CommitOwnedExpressions(DObject& Owner, FOwnedGraphSnapshot Candidate,
		std::string Description, DTransactor* Transactions) -> FMaterialGraphCommandResult;
}
