#include "Materials/MaterialObjectValidation.h"
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
		GraphChanges.Publish(*this);
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
		if (!OwnershipError) return RejectMaterialObjectGraph(GetObjectPath(), OwnershipError.Error);
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : ExpressionCollection.Expressions) Expressions.push_back(Expression.Get());
		auto Validation = MIR::FGraphBuilder::ValidateFunction(Expressions);
		if (!Validation)
		{
			const auto Error = Validation.Diagnostics.empty()
				? FMaterialError(EMaterialExpressionError::InvalidMaterialExpressionGraph) : Validation.Diagnostics.front().Error;
			return RejectMaterialObjectGraph(GetObjectPath(), Error, std::move(Validation.Diagnostics));
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
		bSignatureCached = false;
		Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1;
		NotifyMaterialFunctionChanged(*this);
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
		const FGuid InputId{0x3461fcad, 0x92ac4714, 0x83d68f1e, 0x219814c4};
		const FGuid OutputId{0x690923fd, 0x879544a9, 0x864a3518, 0x81f59b74};
		const FGuid InputNodeId{0xc5e3f95b, 0x818a4f46, 0x90564e64, 0x45f39324};
		const FGuid OutputNodeId{0x61a6db91, 0xb6d741cf, 0xa4e8d91d, 0xc14794f2};
		if (!IsTemplateConstructionPurpose(Initializer.Purpose)
			&& Initializer.Purpose != EObjectConstructionPurpose::AssetLoad
			&& Initializer.Purpose != EObjectConstructionPurpose::Duplication)
		{
			auto* Input = NewObject<DMaterialExpressionFunctionInput>(this, "FunctionInput");
			auto* Output = NewObject<DMaterialExpressionFunctionOutput>(this, "FunctionOutput");
			Input->Id = InputNodeId; Input->Port = {.Id = InputId, .Type = EMaterialProgramValueType::Surface,
				.Name = "Surface", .Default = {.Kind = EMaterialFunctionDefaultKind::Surface}};
			Output->Id = OutputNodeId; Output->Port = {.Id = OutputId, .Type = EMaterialProgramValueType::Surface, .Name = "Surface"}; Output->Source = {InputNodeId};
			ExpressionCollection.Expressions = {Input, Output};
		}

		Presentation.Nodes = {{InputNodeId, 0, 0}, {OutputNodeId, 320, 0}};
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
	auto DMaterialFunction::SetFunctionPresentation(FMaterialFunctionPresentation Candidate) -> bool
	{
		check(IsInGameThread());
		if (Candidate.SchemaVersion != CurrentMaterialFunctionPresentationSchemaVersion) return false;
		std::vector<FGuid> Ids;
		for (const auto& Expression : ExpressionCollection.Expressions) if (Expression) Ids.push_back(Expression->Id);
		FMaterialGraphPresentation Positions;
		Positions.Nodes = std::move(Candidate.Nodes);
		Candidate.Nodes = SanitizeMaterialGraphPresentation(Positions, Ids).Nodes;
		if (Candidate == Presentation) return true;
		Presentation = std::move(Candidate);
		MarkPackageDirty();
		GraphChanges.PublishPresentation(*this);
		return true;
	}
#endif

	auto DMaterialFunction::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
#if DURIN_WITH_EDITORONLY_DATA
		if (Event.MemberProperty && Event.MemberProperty->NamePrivate == FName("ExpressionCollection"))
		{
			bSignatureCached = false;
			AdvanceFunctionRevision(Revision);
			NotifyMaterialFunctionChanged(*this);
		}
		GraphChanges.Publish(*this);
#endif
	}
}
