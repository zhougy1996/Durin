#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"

#include "MaterialExpressionAuthoring.h"
#include "MaterialProgramValidation.h"
#include "Asset/Asset.h"
#include "DObject/Archive.h"
#include "DObject/DObjectArray.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin
{
	auto DMaterial::ProjectExpressions(const FMaterialExpressionCollection& Collection,
		const FMaterialExpressionSurfaceOutputs& Outputs, FMaterialProgram& OutProgram,
		std::vector<FMaterialFunctionCall>& OutCalls) const -> bool
	{
		if (Collection.Expressions.size() > MaterialProgramMaxNodeCount) return false;
		FMaterialProgram Candidate;
		Candidate.Outputs = Private::ProjectMaterialOutputs(Outputs);
		std::vector<FMaterialFunctionCall> Calls;
		std::unordered_set<FGuid> Ids;
		for (const auto& Expression : Collection.Expressions)
		{
			if (!IsValid(Expression.Get()) || !Expression->Id.IsValid() || !Ids.insert(Expression->Id).second) return false;
			FMaterialProgramNode Node;
			if (!Expression->Lower(Node, {.Calls = &Calls, .bValidateFunctionReferences = false})) return false;
			if (const auto Position = std::ranges::find(GraphPresentation.Nodes, Node.Id, &FMaterialGraphNodePresentation::NodeId);
				Position != GraphPresentation.Nodes.end()) Node.DisplayName = Position->DisplayName;
			Candidate.Nodes.push_back(std::move(Node));
		}
		OutProgram = std::move(Candidate);
		OutCalls = std::move(Calls);
		return true;
	}

	auto DMaterial::RefreshExpressionProjection() const -> void
	{
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload()) return;
		if (!ProjectExpressions(ExpressionCollection, ExpressionOutputs, Program, FunctionCalls))
		{
			Program = {.SchemaVersion = 0};
			FunctionCalls.clear();
		}
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
		FMaterialProgram Candidate;
		std::vector<FMaterialFunctionCall> Calls;
		std::vector<FMaterialParameterDefinition> Schema;
		if (!ProjectExpressions(ExpressionCollection, ExpressionOutputs, Candidate, Calls)
			|| !DeriveMaterialParameterSchema(Candidate, Schema))
		{
			OutError = "Invalid material expression collection or parameter owners.";
			return false;
		}
		std::vector<FMaterialFunctionCallSnapshot> Snapshots;
		for (const auto& Call : Calls) Snapshots.push_back({Call.NodeId,
			Call.Function ? Call.Function->GetObjectPath() : std::string{}, Call.Inputs, Call.Outputs});
		if (!ValidateMaterialProgram(Candidate, Schema, Snapshots))
		{
			OutError = "Invalid material expression connections or outputs.";
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
		FMaterialProgram Projection;
		std::vector<FMaterialFunctionCall> Calls;
		if (!ProjectExpressions(Candidate, Outputs, Projection, Calls))
		{
			Result.Diagnostics.push_back({.Message = "Invalid material expression candidate."});
			return Result;
		}
		std::vector<FMaterialParameterDefinition> Schema;
		Result = DeriveMaterialParameterSchema(Projection, Schema);
		if (!Result) return Result;
		Result = ValidateMaterialProgramWithFunctions(Projection, Schema, Calls);
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
		auto Code = Projection;
		for (auto& Node : Code.Nodes)
		{
			Node.Parameter = {.Id = Node.Parameter.Id, .Type = Node.Parameter.Type};
			Node.DisplayName.clear();
		}
		const bool bShaderChanged = Code != ObservedCodeProgram || Calls != FunctionCalls;
		TStrongObjectPtr<DObject> Retired(NewObject<DObject>(nullptr, "RetiredMaterialExpressions"));
		for (auto& Expression : ExpressionCollection.Expressions) if (Expression) Expression->SetOuterPrivate(Retired.Get());
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(this);
		ExpressionCollection = std::move(Copies);
		ExpressionOutputs = std::move(Outputs);
		ObservedCodeProgram = std::move(Code);
		auto Advance = [](uint64& Revision) { Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1; };
		if (ParameterSchema != Schema) Advance(ParameterDefinitionSchemaRevision);
		ParameterSchema = std::move(Schema);
		Program = std::move(Projection);
		FunctionCalls = std::move(Calls);
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
