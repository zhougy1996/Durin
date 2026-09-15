#include "MaterialExpressionOwnership.h"

#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin::Private
{
	auto MigrateSampleRGOutputs(DObject& Owner, FMaterialExpressionCollection& Collection,
		FMaterialExpressionSurfaceOutputs* Outputs, std::vector<FMaterialGraphNodePresentation>& Positions) -> bool
	{
		std::unordered_set<FGuid> Samples;
		for (const auto& Expression : Collection.Expressions)
			if (Cast<DMaterialExpressionTextureSample2D>(Expression.Get())
				|| Cast<DMaterialExpressionTextureSampleParameter2D>(Expression.Get())) Samples.insert(Expression->Id);
		std::map<FGuid, std::vector<FMaterialExpressionInput*>> Links;
		const auto Visit = [&](FMaterialExpressionInput& Input) {
			if (Input.OutputIndex == 6 && !Input.OutputId.IsValid() && Samples.contains(Input.ExpressionId))
				Links[Input.ExpressionId].push_back(&Input);
		};
		for (const auto& Expression : Collection.Expressions)
		{
			Expression->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (Property->GetKind() != DurinCodeGen::EPropertyGenFlags::Struct
					|| static_cast<FStructProperty*>(Property)->GetStruct() != FMaterialExpressionInput::StaticStruct()) return;
				for (uint32 Element = 0; Element < Property->GetArrayDim(); ++Element)
					Visit(*static_cast<FMaterialExpressionInput*>(Property->GetValuePtr(Expression.Get(), Element)));
			});
			if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()))
				for (auto& Binding : Call->Inputs) Visit(Binding.Input);
			if (auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(Expression.Get()))
				for (auto& Attribute : Surface->Attributes) Visit(Attribute.Source);
		}
		if (Outputs)
			for (auto* Input : {&Outputs->Surface, &Outputs->BaseColor, &Outputs->Normal, &Outputs->Metallic,
				&Outputs->Roughness, &Outputs->AmbientOcclusion, &Outputs->Emissive, &Outputs->Opacity, &Outputs->OpacityMask}) Visit(*Input);
		if (Collection.Expressions.size() + Links.size() > MaterialProgramMaxNodeCount) return false;
		for (const auto& [Sample, Consumers] : Links)
		{
			auto* Swizzle = NewObject<DMaterialExpressionSwizzle>(&Owner, NAME_None);
			Swizzle->Id = FGuid::NewGuid();
			Swizzle->Input = {Sample};
			Swizzle->Components = {0, 1};
			Collection.Expressions.emplace_back(Swizzle);
			int32 X = 320, Y = 0;
			if (const auto Position = std::ranges::find(Positions, Sample, &FMaterialGraphNodePresentation::NodeId); Position != Positions.end())
			{
				X = std::clamp(Position->X, -MaterialGraphPresentationCoordinateLimit, MaterialGraphPresentationCoordinateLimit - 320) + 320;
				Y = std::clamp(Position->Y, -MaterialGraphPresentationCoordinateLimit, MaterialGraphPresentationCoordinateLimit);
			}
			Positions.push_back({Swizzle->Id, X, Y});
			for (auto* Input : Consumers) *Input = {Swizzle->Id};
		}
		return true;
	}

	auto ValidateExpressionOwnership(const DObject& Owner,
		const FMaterialExpressionCollection& Collection, std::string& OutError) -> bool
	{
		std::unordered_set<const DObject*> Owned;
		for (const auto& Expression : Collection.Expressions)
			if (!IsValid(Expression.Get()) || Expression->GetOuter() != &Owner || !Owned.insert(Expression.Get()).second)
			{
				OutError = "Expression collection contains a missing, shared, or wrongly owned child.";
				return false;
			}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(&Owner, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				OutError = "Owner contains an abandoned expression child outside its collection.";
				return false;
			}
		return true;
	}

	auto ReplaceOwnedExpressions(DObject& Owner, FMaterialExpressionCollection& Collection,
		std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "MaterialExpressionApply"));
		FMaterialExpressionCollection Copies;
		Copies.Expressions.reserve(Expressions.size());
		for (auto* Expression : Expressions)
		{
			auto* Copy = DuplicateObject(Expression, Staging.Get(), FName(std::string("Expression_") + Expression->Id.ToString()));
			if (!Copy)
			{
				FMaterialProgramValidationResult Result;
				Result.Diagnostics.push_back({.Message = "Unable to duplicate the expression candidate."});
				return Result;
			}
			Copies.Expressions.emplace_back(Copy);
		}
		TStrongObjectPtr<DObject> Retired(NewObject<DObject>(nullptr, "RetiredMaterialExpressions"));
		for (auto& Expression : Collection.Expressions) if (Expression) Expression->SetOuterPrivate(Retired.Get());
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(&Owner);
		Collection = std::move(Copies);
		return {.bSucceeded = true};
	}
}
