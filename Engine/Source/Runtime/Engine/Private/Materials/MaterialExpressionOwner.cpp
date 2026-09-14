#include "Materials/Material.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunctionInterface.h"

#include "Asset/Asset.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin
{
	auto DMaterial::ValidateExpressionGraph(const FMaterialExpressionCollection& Collection,
		const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Collection.Expressions) Expressions.push_back(Expression.Get());
		return FMaterialExpressionBuildContext::ValidateSurface(Expressions, Outputs, OutCodeFingerprint);
	}

	auto DMaterial::DeriveExpressionParameterSchema(const FMaterialExpressionCollection& Collection,
		std::vector<FMaterialParameterDefinition>& OutDefinitions) -> FMaterialProgramValidationResult
	{
		const auto Fail = [](FGuid Id, std::string Message) {
			FMaterialProgramValidationResult Result;
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Schema,
				.LocationKind = EMaterialProgramDiagnosticLocationKind::Node, .NodeId = Id,
				.Message = std::move(Message)});
			return Result;
		};
		if (Collection.Expressions.size() > MaterialProgramMaxNodeCount)
			return Fail({}, "Material graph exceeds the node limit.");
		std::unordered_set<FGuid> NodeIds;
		for (const auto& Expression : Collection.Expressions)
			if (!IsValid(Expression.Get()) || !Expression->Id.IsValid() || !NodeIds.insert(Expression->Id).second)
				return Fail({}, "Material expression identity is missing or duplicated.");
		std::vector<FMaterialParameterDefinition> Definitions;
		for (const auto& Expression : Collection.Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			{
				auto Definition = Parameter->GetParameterDefinition();
				if (!Definition.Id.IsValid() || NodeIds.contains(Definition.Id))
					return Fail(Expression->Id, "Parameter identity must be valid and distinct from node identity.");
				Definitions.push_back(std::move(Definition));
			}
		const auto Validation = ValidateMaterialParameterDefinitions(Definitions);
		if (!Validation) return Fail({}, std::string(GetMaterialParameterErrorText(Validation.Error)));
		std::ranges::sort(Definitions, {}, &FMaterialParameterDefinition::Id);
		OutDefinitions = std::move(Definitions);
		return {.bSucceeded = true};
	}




	auto DMaterial::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context, std::string& OutError) const -> bool
	{
		if (Context.bCooked) return true;
		if (GraphOwnershipVersion != 2)
		{
			OutError = "Unsupported material expression schema; rebuild this material.";
			return false;
		}
		std::unordered_set<const DObject*> Owned;
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (!IsValid(Expression.Get()) || Expression->GetOuter() != this || !Owned.insert(Expression.Get()).second)
			{
				OutError = "Material expression collection contains a missing, shared, or wrongly owned child.";
				return false;
			}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(this, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				OutError = "Material contains an abandoned expression child outside its collection.";
				return false;
			}
		const auto Validation = ValidateExpressionGraph(ExpressionCollection, ExpressionOutputs);
		if (!Validation)
		{
			OutError = Validation.Diagnostics.empty() ? "Invalid material expression graph." : Validation.Diagnostics.front().Message;
			return false;
		}
		return true;
	}

	auto DMaterial::SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions,
		FMaterialExpressionSurfaceOutputs Outputs) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		FMaterialExpressionCollection Candidate;
		for (auto* Expression : Expressions) Candidate.Expressions.emplace_back(Expression);
		std::vector<FMaterialParameterDefinition> Schema;
		Result = DeriveExpressionParameterSchema(Candidate, Schema);
		if (!Result) return Result;
		FXxHash128 Code;
		Result = ValidateExpressionGraph(Candidate, Outputs, &Code);
		if (!Result) return Result;
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "MaterialExpressionApply"));
		FMaterialExpressionCollection Copies;
		for (const auto& Expression : Candidate.Expressions)
		{
			auto* Copy = DuplicateObject(Expression.Get(), Staging.Get(), FName(std::string("Expression_") + Expression->Id.ToString()));
			if (!Copy)
			{
				Result.bSucceeded = false;
				Result.Diagnostics.push_back({.Message = "Unable to duplicate the material expression candidate."});
				return Result;
			}
			Copies.Expressions.emplace_back(Copy);
		}
		const bool bShaderChanged = Code != ObservedExpressionCode;
		TStrongObjectPtr<DObject> Retired(NewObject<DObject>(nullptr, "RetiredMaterialExpressions"));
		for (auto& Expression : ExpressionCollection.Expressions) if (Expression) Expression->SetOuterPrivate(Retired.Get());
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(this);
		ExpressionCollection = std::move(Copies);
		ExpressionOutputs = std::move(Outputs);
		ObservedExpressionCode = Code;
		auto Advance = [](uint64& Revision) { Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1; };
		if (ParameterSchema != Schema) Advance(ParameterDefinitionSchemaRevision);
		ParameterSchema = std::move(Schema);
		Advance(MaterialProgramRevision);
		if (bShaderChanged)
		{
			AdvanceAuthoredRevision();
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this);
		}
		MarkPackageDirty();
		MarkRenderDataDirty(bShaderChanged ? EMaterialRenderDirtyFlags::ShaderMap : EMaterialRenderDirtyFlags::DynamicParameters);
		return Result;
	}
}
