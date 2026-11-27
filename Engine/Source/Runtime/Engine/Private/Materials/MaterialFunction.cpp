#include "Materials/MaterialObjectValidation.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialCustomVersion.h"
#include "MaterialExpressionOwnership.h"

#include "DObject/Property.h"
#include "Materials/MaterialExpressionBuild.h"
#include "DObject/Archive.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"

namespace Durin
{

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterialFunction::InvalidateSignature() -> bool
	{
		const auto Previous = std::move(CachedSignature);
		bSignatureCached = false;
		return GetFunctionSignature() != Previous;
	}
#endif

	auto DMaterialFunction::GetFunctionSignature() const -> const FMaterialFunctionSignature&
	{
#if DURIN_WITH_EDITORONLY_DATA
		if (!bSignatureCached)
		{
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : ExpressionCollection.Expressions) Expressions.push_back(Expression.Get());
			CachedSignature = DeriveMaterialFunctionSignature(Expressions);
			bSignatureCached = true;
		}
#endif
		return CachedSignature;
	}

	auto DMaterialFunction::PostLoad() -> void
	{
		Super::PostLoad();
		bSignatureCached = false;
#if DURIN_WITH_EDITORONLY_DATA
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get())) Call->UpdateFromFunctionResource(false);
		GraphChanges.Publish(*this);
#endif
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterialFunction::GetExpressionBody() const -> MIR::FFunctionBody
	{
		MIR::FFunctionBody Body{.Signature = GetFunctionSignature(), .AssetPath = GetObjectPath(), .Revision = Revision};
		for (const auto& Expression : ExpressionCollection.Expressions) Body.Expressions.push_back(Expression.Get());
		return Body;
	}
#endif

	auto DMaterialFunction::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context) const -> std::expected<void, FObjectValidationError>
	{
		if (Context.bCooked) return {};
#if DURIN_WITH_EDITORONLY_DATA
		const auto OwnershipError = Private::ValidateExpressionOwnership(*this, ExpressionCollection);
		if (!OwnershipError) return RejectMaterialObjectGraph(GetObjectPath(), {{.Message = OwnershipError.error()}});
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : ExpressionCollection.Expressions) Expressions.push_back(Expression.Get());
		auto Validation = MIR::FGraphBuilder::ValidateFunction(Expressions);
		if (!Validation)
		{
			return RejectMaterialObjectGraph(GetObjectPath(), std::move(Validation.Diagnostics));
		}
		return {};
#else
		return RejectLoadedObjectGraph(GetObjectPath(),
			"Authored material functions require editor-only data support.");
#endif
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterialFunction::SetFunctionExpressions(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		auto Result = MIR::FGraphBuilder::ValidateFunction(Expressions);
		if (!Result) return Result;
		Result = Private::ReplaceOwnedExpressions(*this, ExpressionCollection, Expressions);
		if (!Result) return Result;
		const bool SignatureChanged = InvalidateSignature();
		Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1;
		NotifyMaterialFunctionChanged(*this, SignatureChanged);
		MarkPackageDirty();
		GraphChanges.Publish(*this);
		return Result;
	}
#endif

	auto DMaterialFunction::Serialize(FArchive& Ar) -> void
	{
		if (!FMaterialGraphVersion::Serialize(Ar) || !FMaterialFunctionVersion::Serialize(Ar)) return;
		Super::Serialize(Ar);
		if (!Ar.IsError() && !IsTemplateObject() && Ar.IsSaving() && Ar.GetPurpose() == EArchivePurpose::AuthoredPackage)
		{
			const auto Validation = ValidateLoadedObjectGraph({});
			if (!Validation)
			{
				if (auto* ObjectArchive = dynamic_cast<FObjectArchive*>(&Ar)) ObjectArchive->FailValidation(Validation.error());
				else Ar.Fail(EArchiveFailureCode::InvalidData, ToString(Validation.error()));
			}
		}
	}

	namespace
	{
		auto AdvanceFunctionRevision(uint64& Revision) -> void
			{ Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1; }
	}

	DMaterialFunctionInterface::DMaterialFunctionInterface(const FObjectInitializer& Initializer)
		: Super(Initializer) {}

	DMaterialFunction::DMaterialFunction(const FObjectInitializer& Initializer)
		: Super(Initializer)
	{
#if DURIN_WITH_EDITORONLY_DATA
		if (!IsTemplateConstructionPurpose(Initializer.Purpose)
			&& Initializer.Purpose != EObjectConstructionPurpose::AssetLoad
			&& Initializer.Purpose != EObjectConstructionPurpose::Duplication)
		{
			auto* Input = NewObject<DMaterialExpressionFunctionInput>(this, "FunctionInput");
			auto* Output = NewObject<DMaterialExpressionFunctionOutput>(this, "FunctionOutput");
			Input->Id = FGuid::NewGuid();
			Input->Port = {.Id = FGuid::NewGuid(), .Type = EMaterialProgramValueType::MaterialAttributes,
				.Name = "Surface", .Default = {.Kind = EMaterialFunctionDefaultKind::MaterialAttributes}};
			Output->Id = FGuid::NewGuid();
			Output->Port = {.Id = FGuid::NewGuid(), .Type = EMaterialProgramValueType::MaterialAttributes, .Name = "Surface"};
			Output->Source = {Input};
			Input->EditorLayout = {.bAuthored = true};
			Output->EditorLayout = {.bAuthored = true, .X = 320};
			ExpressionCollection.Expressions = {Input, Output};
		}
#endif
	}

	auto DMaterialFunction::GetFunctionDependencies() const
		-> std::vector<TObjectPtr<DMaterialFunctionInterface>>
	{
		check(IsInGameThread());
		std::vector<TObjectPtr<DMaterialFunctionInterface>> Dependencies;
#if DURIN_WITH_EDITORONLY_DATA
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()); Call && Call->Function
				&& !std::ranges::contains(Dependencies, Call->Function)) Dependencies.push_back(Call->Function);
#endif

		return Dependencies;
	}

#if DURIN_WITH_EDITORONLY_DATA
	auto DMaterialFunction::GetFunctionPresentation() const -> FMaterialFunctionPresentation
	{
		return {.Nodes = Private::CollectExpressionLayout(ExpressionCollection).Nodes};
	}

	auto DMaterialFunction::SetFunctionPresentation(FMaterialFunctionPresentation Candidate) -> bool
	{
		check(IsInGameThread());
		if (Candidate.SchemaVersion != CurrentMaterialFunctionPresentationSchemaVersion) return false;
		if (!Private::ApplyExpressionLayout(ExpressionCollection, {.Nodes = std::move(Candidate.Nodes)})) return true;
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
		return true;
	}
#endif

	auto DMaterialFunction::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		if ((Event.Origin == EPropertyChangeOrigin::Undo || Event.Origin == EPropertyChangeOrigin::Redo)
			&& Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("ExpressionCollection")) return;
		Super::PostEditChangeProperty(Event);
#if DURIN_WITH_EDITORONLY_DATA
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("ExpressionCollection"))
		{
			const bool SignatureChanged = InvalidateSignature();
			AdvanceFunctionRevision(Revision);
			NotifyMaterialFunctionChanged(*this, SignatureChanged);
		}
		GraphChanges.Publish(*this);
#endif
	}
}
