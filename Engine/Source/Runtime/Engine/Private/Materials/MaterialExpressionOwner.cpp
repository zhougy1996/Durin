#include "ObjectCacheContext.h"
#include "Materials/Material.h"
#include "MaterialExpressionOwnership.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunctionInterface.h"

#include "Asset/Asset.h"
#include "DObject/Archive.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin
{
	auto DMaterial::GetOutputNode() const -> const DMaterialExpressionMaterialOutput*
	{
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (const auto* Output = Cast<DMaterialExpressionMaterialOutput>(Expression.Get())) return Output;
		return nullptr;
	}

	auto DMaterial::GetExpressionOutputs() const -> const FMaterialExpressionSurfaceOutputs&
	{
		if (const auto* Output = GetOutputNode()) return Output->Outputs;
		static const FMaterialExpressionSurfaceOutputs Empty;
		return Empty; // Cooked assets contain the compiled projection, not an authored graph.
	}

	auto DMaterial::ValidateExpressionGraph(const FMaterialExpressionCollection& Collection,
		const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		const auto Count = std::ranges::count_if(Collection.Expressions, [](const auto& Expression) {
			return Cast<DMaterialExpressionMaterialOutput>(Expression.Get()) != nullptr;
		});
		if (Count != 1)
		{
			FMaterialProgramValidationResult Invalid;
			Invalid.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Graph,
				.Message = "A material graph requires exactly one material output node."});
			return Invalid;
		}
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
				const auto Existing = std::ranges::find(Definitions, Definition.Id, &FMaterialParameterDefinition::Id);
				if (Existing != Definitions.end())
				{
					if (*Existing != Definition) return Fail(Expression->Id, "Shared parameter definitions disagree.");
				}
				else Definitions.push_back(std::move(Definition));
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
		if (GetMaterialDomainOutputPins(Domain).empty()) { OutError = "Unsupported material domain."; return false; }
		if (!Private::ValidateExpressionOwnership(*this, ExpressionCollection, OutError)) return false;
		const auto Validation = ValidateExpressionGraph(ExpressionCollection, GetExpressionOutputs());
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
		// Recipe convenience: publish an ordinary output node together with the supplied expressions.
		TStrongObjectPtr<DMaterialExpressionMaterialOutput> Terminal(NewObject<DMaterialExpressionMaterialOutput>(nullptr, NAME_None));
		const auto* Existing = GetOutputNode();
		Terminal->Id = Existing ? Existing->Id : FGuid::NewGuid();
		Terminal->Outputs = std::move(Outputs);
		std::vector<DMaterialExpression*> Nodes;
		bool bHasOutput = false;
		for (auto* Expression : Expressions)
		{
			if (const auto* Output = Cast<DMaterialExpressionMaterialOutput>(Expression))
			{
				if (bHasOutput) return {.Diagnostics = {{.Message = "A material graph cannot contain multiple output nodes."}}};
				bHasOutput = true; Terminal->Id = Output->Id; continue;
			}
			Nodes.push_back(Expression);
		}
		Nodes.push_back(Terminal.Get());
		return SetMaterialExpressions(Nodes);
	}

	auto DMaterial::SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		FMaterialExpressionCollection Candidate;
		for (auto* Expression : Expressions) Candidate.Expressions.emplace_back(Expression);
		std::vector<FMaterialParameterDefinition> Schema;
		Result = DeriveExpressionParameterSchema(Candidate, Schema);
		if (!Result) return Result;
		FXxHash128 Code;
		const DMaterialExpressionMaterialOutput* Output = nullptr;
		for (auto* Expression : Expressions)
			if (const auto* Terminal = Cast<DMaterialExpressionMaterialOutput>(Expression))
			{
				if (Output) return {.Diagnostics = {{.Message = "A material graph requires exactly one output node."}}};
				Output = Terminal;
			}
		if (!Output) return {.Diagnostics = {{.Message = "The material graph has no output node."}}};
		if (GetMaterialDomainOutputPins(Domain).empty()) return {.Diagnostics = {{.Message = "Unsupported material domain."}}};
		Result = ValidateExpressionGraph(Candidate, Output->Outputs, &Code);
		if (!Result) return Result;
		std::vector<DMaterialExpression*> Ordered;
		for (auto* Expression : Expressions)
			if (Expression != Output) Ordered.push_back(Expression);
		Ordered.push_back(const_cast<DMaterialExpressionMaterialOutput*>(Output));
		Result = Private::ReplaceOwnedExpressions(*this, ExpressionCollection, Ordered);
		if (!Result) return Result;
		FObjectCacheContext Context;
		const bool bShaderChanged = Code != ObservedExpressionCode;
		ObservedExpressionCode = Code;
		auto Advance = [](uint64& Revision) { Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1; };
		ParameterSchema = std::move(Schema);
		Advance(MaterialProgramRevision);
		if (bShaderChanged)
		{
			AdvanceAuthoredRevision(&Context);
			Private::FMaterialCompilationLifecycle::ScheduleEdit(*this, &Context);
		}
		MarkPackageDirty();
		MarkRenderDataDirty(bShaderChanged ? EMaterialRenderDirtyFlags::ShaderMap : EMaterialRenderDirtyFlags::DynamicParameters, true, &Context);
		GraphChanges.Publish(*this);
		return Result;
	}
}
