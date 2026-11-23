#include "Materials/MaterialObjectValidation.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
#include "Materials/ObjectCacheContext.h"
#include "Materials/Material.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "MaterialExpressionOwnership.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunctionInterface.h"

#include "Asset/Asset.h"
#include "DObject/Archive.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Threading/RunnableThread.h"

namespace Durin
{
#if DURIN_WITH_EDITORONLY_DATA
	namespace
	{
		auto Components(float& Scalar) -> std::span<float> { return {&Scalar, 1}; }
		auto Components(const float& Scalar) -> std::span<const float> { return {&Scalar, 1}; }
		auto Components(float (&Vector)[3]) -> std::span<float> { return Vector; }
		auto Components(const float (&Vector)[3]) -> std::span<const float> { return Vector; }

		auto View(const auto& Field) -> FMaterialNumericInputView
		{
			return {Field.Connection, Field.UseConstant, Components(Field.Constant)};
		}
		void Assign(auto& Field, const FMaterialNumericInput& Input)
		{
			auto Target = Components(Field.Constant);
			check(Target.size() == Input.Constant.size());
			Field.Connection = Input.Connection;
			Field.UseConstant = Input.UseConstant;
			std::ranges::copy(Input.Constant, Target.begin());
		}
	}
	void DMaterial::VisitNumericField(auto& Owner, EMaterialProperty Property, auto&& Visit)
	{
		switch (Property)
		{
		case EMaterialProperty::MP_BaseColor: Visit(Owner.BaseColor); break;
		case EMaterialProperty::MP_Normal: Visit(Owner.Normal); break;
		case EMaterialProperty::MP_Metallic: Visit(Owner.Metallic); break;
		case EMaterialProperty::MP_Roughness: Visit(Owner.Roughness); break;
		case EMaterialProperty::MP_AmbientOcclusion: Visit(Owner.AmbientOcclusion); break;
		case EMaterialProperty::MP_EmissiveColor: Visit(Owner.EmissiveColor); break;
		case EMaterialProperty::MP_Opacity: Visit(Owner.Opacity); break;
		case EMaterialProperty::MP_OpacityMask: Visit(Owner.OpacityMask); break;
		case EMaterialProperty::MP_WorldPositionOffset: Visit(Owner.WorldPositionOffset); break;
		case EMaterialProperty::MP_Specular: Visit(Owner.Specular); break;
		case EMaterialProperty::MP_ClearCoat: Visit(Owner.ClearCoat); break;
		case EMaterialProperty::MP_ClearCoatRoughness: Visit(Owner.ClearCoatRoughness); break;
		case EMaterialProperty::MP_Refraction: Visit(Owner.Refraction); break;
		default: break;
		}
	}

	auto DMaterial::GetNumericInputForProperty(EMaterialProperty Property) const -> std::optional<FMaterialNumericInputView>
	{
		const auto Description = GetExpressionInputDescription(Property);
		return Description ? Description->Numeric : std::nullopt;
	}


	void DMaterial::ApplyRootInputs(FMaterialRootInputs Inputs)
	{
		for (const auto& Definition : FMaterialAttributeDefinitionMap::GetDefinitions())
			VisitNumericField(*this, Definition.Property, [&](auto& Field) { Assign(Field, Inputs.Get(Definition.Property)); });
		MaterialAttributes = Inputs.Attributes;
		bUseMaterialAttributes = Inputs.bUseMaterialAttributes;
	}

	auto DMaterial::CaptureRootInputs() const -> FMaterialRootInputs
	{
		FMaterialRootInputs Result;
		Result.Attributes = MaterialAttributes;
		Result.bUseMaterialAttributes = bUseMaterialAttributes;
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			Result.Get(Field.Property) = GetNumericInputForProperty(Field.Property)->Capture();
		return Result;
	}

	auto DMaterial::GetExpressionInputForProperty(EMaterialProperty Property) const -> const FMaterialExpressionInput*
	{
		const auto Description = GetExpressionInputDescription(Property);
		return Description ? Description->Connection : nullptr;
	}

	auto DMaterial::GetExpressionInputDescription(EMaterialProperty Property) const -> std::optional<FMaterialInputDescription>
	{
		if (Property == EMaterialProperty::MP_MaterialAttributes)
			return FMaterialInputDescription{MaterialAttributes};
		std::optional<FMaterialInputDescription> Result;
		VisitNumericField(*this, Property, [&](const auto& Input) {
			Result.emplace(Property, View(Input));
		});
		return Result;
	}

	void DMaterial::SetRootPosition(int32 X, int32 Y)
	{
		if (EditorX == X && EditorY == Y) return;
		EditorX = X;
		EditorY = Y;
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
	}

	auto DMaterial::DeriveExpressionParameterSchema(const FMaterialExpressionCollection& Collection,
		std::vector<FMaterialParameterDefinition>& OutDefinitions, const FMaterialParameterDefinition* SharedParameter) -> FMaterialProgramValidationResult
	{
		const auto Fail = [](FGuid Id, std::string Message) {
			FMaterialProgramValidationResult Result;
			Result.Diagnostics.push_back({.LocationKind = EMaterialProgramDiagnosticLocationKind::Node, .NodeId = Id,
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
				if (SharedParameter && Definition.Id == SharedParameter->Id)
				{
					if (Definition.Type != SharedParameter->Type) return Fail(Expression->Id, "Shared parameter types disagree.");
					Definition = *SharedParameter;
				}
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
		if (!Validation) return Fail({}, std::format("{} [parameter {}]", GetMaterialParameterErrorText(Validation.Error), Validation.ParameterId.ToString()));
		std::ranges::sort(Definitions, {}, &FMaterialParameterDefinition::Id);
		OutDefinitions = std::move(Definitions);
		return {.bSucceeded = true};
	}
#endif


	auto DMaterial::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (const auto Validation = ValidateMaterialStaticProperties(StaticProperties); !Validation)
			return RejectMaterialObjectGraph(GetObjectPath(), {{.Message = Validation.error()}});
		if (Context.bCooked)
			return CookedProgramData.GetMetadata().LogicalSize != 0 ? std::expected<void, FObjectValidationError>{}
				: RejectMaterialObjectGraph(GetObjectPath(), {{.Message = "Material cooked program is unavailable."}});
#if DURIN_WITH_EDITORONLY_DATA
		std::vector<FMaterialParameterDefinition> Schema;
		if (const auto Validation = DeriveExpressionParameterSchema(ExpressionCollection, Schema); !Validation)
			return RejectLoadedObjectGraph(GetObjectPath(), "Material graph has an invalid parameter schema.");
		if (GetMaterialDomainInputProperties(Domain).empty())
			return RejectMaterialObjectGraph(GetObjectPath(), {{.Message = "Unsupported material domain."}});
		const auto OwnershipError = Private::ValidateExpressionOwnership(*this, ExpressionCollection);
		if (!OwnershipError) return RejectMaterialObjectGraph(GetObjectPath(), {{.Message = OwnershipError.error()}});
		auto Validation = ValidateExpressionGraph();
		if (!Validation)
		{
			return RejectMaterialObjectGraph(GetObjectPath(), std::move(Validation.Diagnostics));
		}
		return {};
#else
		return RejectLoadedObjectGraph(GetObjectPath(),
			"Authored materials require editor-only data support.");
#endif
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterial::SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		return SetMaterialExpressions(Expressions, CaptureRootInputs());
	}

	auto DMaterial::SetMaterialExpressions(std::span<DMaterialExpression* const> Expressions, FMaterialRootInputs Outputs) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		FMaterialExpressionCollection Candidate;
		for (auto* Expression : Expressions) Candidate.Expressions.emplace_back(Expression);
		std::vector<FMaterialParameterDefinition> Schema;
		Result = DeriveExpressionParameterSchema(Candidate, Schema);
		if (!Result) return Result;
		FXxHash128 Code;
		if (GetMaterialDomainInputProperties(Domain).empty()) return {.Diagnostics = {{.Message = "Unsupported material domain."}}};
		Result = ValidateExpressionGraph(Candidate, Outputs, &Code);
		if (!Result) return Result;
		Result = Private::ReplaceOwnedExpressions(*this, ExpressionCollection, Expressions, &Outputs);
		if (!Result) return Result;
		ApplyRootInputs(std::move(Outputs));
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
#endif
}
