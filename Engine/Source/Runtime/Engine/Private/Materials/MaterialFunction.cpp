#include "Materials/MaterialFunction.h"
#include "MaterialExpressionOwnership.h"

#include "DObject/Property.h"
#include "Materials/MaterialExpressionBuild.h"
#include "DObject/Archive.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"

namespace Durin
{


	auto DMaterialFunction::GetExpressionBody() const -> FMaterialExpressionFunctionBody
	{
		FMaterialExpressionFunctionBody Body{.Signature = Signature, .AssetPath = GetObjectPath(), .Revision = Revision};
		for (const auto& Expression : ExpressionCollection.Expressions) Body.Expressions.push_back(Expression.Get());
		return Body;
	}

	auto DMaterialFunction::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context, std::string& OutError) const -> bool
	{
		if (Context.bCooked) return true;
		if (GraphOwnershipVersion != 3)
		{
			OutError = "Unsupported material function expression schema; rebuild this function.";
			return false;
		}
		if (!Private::ValidateExpressionOwnership(*this, ExpressionCollection, OutError)) return false;
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : ExpressionCollection.Expressions) Expressions.push_back(Expression.Get());
		if (!FMaterialExpressionBuildContext::ValidateFunction(Expressions, Signature))
		{
			OutError = "Function expression collection or signature is invalid.";
			return false;
		}
		return true;
	}

	auto DMaterialFunction::SetFunctionExpressions(FMaterialFunctionSignature InSignature,
		std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		auto Result = FMaterialExpressionBuildContext::ValidateFunction(Expressions, InSignature);
		if (!Result) return Result;
		Result = Private::ReplaceOwnedExpressions(*this, ExpressionCollection, Expressions);
		if (!Result) return Result;
		Signature = std::move(InSignature);
		Revision = Revision == std::numeric_limits<uint64>::max() ? 1 : Revision + 1;
		NotifyMaterialFunctionChanged(*this);
		MarkPackageDirty();
		return Result;
	}

	auto DMaterialFunction::Serialize(FArchive& Ar) -> void
	{
		if (Ar.IsLoading()) GraphOwnershipVersion = 0;
		Super::Serialize(Ar);
		if (GraphOwnershipVersion != 3 || Ar.HasError())
		{
			Ar.Fail(EArchiveFailureCode::UnsupportedVersion,
				"Unsupported material function schema; rebuild this function.");
			return;
		}
		if (!Ar.HasError() && !IsTemplateObject() && Ar.IsSaving() && Ar.GetPurpose() == EArchivePurpose::AuthoredPackage)
		{
			std::string Error;
			if (!ValidateLoadedObjectGraph({}, Error)) Ar.Fail(EArchiveFailureCode::InvalidData, Error);
		}
	}

	auto DMaterialFunction::SetAuthoringSource(std::string Source, uint32 Version) -> void
	{
		check(IsInGameThread());
		if (AuthoringSource == Source && AuthoringSourceVersion == Version) return;
		AuthoringSource = std::move(Source);
		AuthoringSourceVersion = Version;
		MarkPackageDirty();
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
		const FGuid InputId{0x3461fcad, 0x92ac4714, 0x83d68f1e, 0x219814c4};
		const FGuid OutputId{0x690923fd, 0x879544a9, 0x864a3518, 0x81f59b74};
		const FGuid InputNodeId{0xc5e3f95b, 0x818a4f46, 0x90564e64, 0x45f39324};
		const FGuid OutputNodeId{0x61a6db91, 0xb6d741cf, 0xa4e8d91d, 0xc14794f2};
		Signature.Inputs.push_back({.Id = InputId, .Type = EMaterialProgramValueType::Surface,
			.Name = "Surface", .Default = {.Kind = EMaterialFunctionDefaultKind::Surface}});
		Signature.Outputs.push_back({.Id = OutputId, .Type = EMaterialProgramValueType::Surface, .Name = "Surface"});
		if (!IsTemplateConstructionPurpose(Initializer.Purpose)
			&& Initializer.Purpose != EObjectConstructionPurpose::AssetLoad
			&& Initializer.Purpose != EObjectConstructionPurpose::Duplication)
		{
			auto* Input = NewObject<DMaterialExpressionFunctionInput>(this, "FunctionInput");
			auto* Output = NewObject<DMaterialExpressionFunctionOutput>(this, "FunctionOutput");
			Input->Id = InputNodeId; Input->PortId = InputId;
			Output->Id = OutputNodeId; Output->PortId = OutputId; Output->Source = {InputNodeId};
			ExpressionCollection.Expressions = {Input, Output};
		}

		Presentation.Nodes = {{InputNodeId, 0, 0}, {OutputNodeId, 320, 0}};
	}

	auto DMaterialFunction::GetFunctionDependencies() const
		-> std::vector<TObjectPtr<DMaterialFunctionInterface>>
	{
		check(IsInGameThread());
		std::vector<TObjectPtr<DMaterialFunctionInterface>> Dependencies;
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression.Get()); Call && Call->Function
				&& std::ranges::find(Dependencies, Call->Function) == Dependencies.end()) Dependencies.push_back(Call->Function);

		return Dependencies;
	}


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
		return true;
	}

	auto DMaterialFunction::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		if (Event.MemberProperty && (Event.MemberProperty->NamePrivate == FName("ExpressionCollection") || Event.MemberProperty->NamePrivate == FName("Signature")))
		{
			AdvanceFunctionRevision(Revision);
			NotifyMaterialFunctionChanged(*this);
		}
	}
}
