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
				.Error = EMaterialExpressionError::InvalidMaterialOutputCount});
			return Invalid;
		}
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Collection.Expressions) Expressions.push_back(Expression.Get());
		return FMaterialExpressionGraphBuilder::ValidateSurface(Expressions, Outputs, OutCodeFingerprint);
	}

	auto DMaterial::DeriveExpressionParameterSchema(const FMaterialExpressionCollection& Collection,
		std::vector<FMaterialParameterDefinition>& OutDefinitions) -> FMaterialProgramValidationResult
	{
		const auto Fail = [](FGuid Id, FMaterialError Error) {
			FMaterialProgramValidationResult Result;
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Schema,
				.LocationKind = EMaterialProgramDiagnosticLocationKind::Node, .NodeId = Id,
				.Error = std::move(Error)});
			return Result;
		};
		if (Collection.Expressions.size() > MaterialProgramMaxNodeCount)
			return Fail({}, EMaterialExpressionError::GraphExceedsNodeLimit);
		std::unordered_set<FGuid> NodeIds;
		for (const auto& Expression : Collection.Expressions)
			if (!IsValid(Expression.Get()) || !Expression->Id.IsValid() || !NodeIds.insert(Expression->Id).second)
				return Fail({}, EMaterialExpressionError::IdentityMissingDuplicated);
		std::vector<FMaterialParameterDefinition> Definitions;
		for (const auto& Expression : Collection.Expressions)
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression.Get()))
			{
				auto Definition = Parameter->GetParameterDefinition();
				if (!Definition.Id.IsValid() || NodeIds.contains(Definition.Id))
					return Fail(Expression->Id, EMaterialExpressionError::ParameterIdentityValidDistinctNodeIdentity);
				const auto Existing = std::ranges::find(Definitions, Definition.Id, &FMaterialParameterDefinition::Id);
				if (Existing != Definitions.end())
				{
					if (*Existing != Definition) return Fail(Expression->Id, EMaterialExpressionError::SharedParameterDefinitionsDisagree);
				}
				else Definitions.push_back(std::move(Definition));
			}
		const auto Validation = ValidateMaterialParameterDefinitions(Definitions);
		if (!Validation) return Fail({}, FMaterialError(Validation));
		std::ranges::sort(Definitions, {}, &FMaterialParameterDefinition::Id);
		OutDefinitions = std::move(Definitions);
		return {.bSucceeded = true};
	}


	auto DMaterial::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context, std::string& OutError) const -> bool
	{
		if (Context.bCooked) return true;
		if (GetMaterialDomainOutputPins(Domain).empty()) { OutError = FormatMaterialError(EMaterialExpressionError::UnsupportedMaterialDomain); return false; }
		const auto OwnershipError = Private::ValidateExpressionOwnership(*this, ExpressionCollection);
		if (!OwnershipError)
		{ OutError = FormatMaterialError(OwnershipError.Error); return false; }
		const auto Validation = ValidateExpressionGraph(ExpressionCollection, GetExpressionOutputs());
		if (!Validation)
		{
			OutError = FormatMaterialError(Validation.Diagnostics.empty() ? FMaterialError(EMaterialExpressionError::InvalidMaterialExpressionGraph) : Validation.Diagnostics.front().Error);
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
				if (bHasOutput) return {.Diagnostics = {{.Error = EMaterialExpressionError::MultipleOutputNodes}}};
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
				if (Output) return {.Diagnostics = {{.Error = EMaterialExpressionError::InvalidOutputCount}}};
				Output = Terminal;
			}
		if (!Output) return {.Diagnostics = {{.Error = EMaterialExpressionError::MaterialGraphNoOutputNode}}};
		if (GetMaterialDomainOutputPins(Domain).empty()) return {.Diagnostics = {{.Error = EMaterialExpressionError::UnsupportedMaterialDomain}}};
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
