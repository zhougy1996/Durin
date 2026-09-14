#include "Materials/MaterialFunction.h"

#include "DObject/Property.h"
#include "Materials/MaterialExpressionBuild.h"
#include "MaterialExpressionAuthoring.h"
#include "DObject/DObjectArray.h"
#include "DObject/Archive.h"
#include "DObject/Package.h"
#include "Threading/RunnableThread.h"
#include <unordered_set>

namespace Durin
{
	auto DMaterialFunction::ProjectExpressions(const FMaterialFunctionSignature& InSignature,
		const FMaterialExpressionCollection& Collection, FMaterialFunctionGraph& OutGraph) const -> bool
	{
		if (Collection.Expressions.size() > MaterialProgramMaxNodeCount) return false;
		FMaterialFunctionGraph Candidate;
		Candidate.Signature = InSignature;
		std::unordered_set<FGuid> Ids;
		for (const auto& Expression : Collection.Expressions)
		{
			if (!IsValid(Expression.Get()) || !Expression->Id.IsValid() || !Ids.insert(Expression->Id).second) return false;
			FMaterialProgramNode Node;
			if (!Expression->Lower(Node, {.Signature = &Candidate.Signature, .Calls = &Candidate.Calls,
				.bValidateFunctionReferences = false})) return false;
			if (const auto Position = std::ranges::find(Presentation.Nodes, Node.Id, &FMaterialGraphNodePresentation::NodeId);
				Position != Presentation.Nodes.end()) Node.DisplayName = Position->DisplayName;
			Candidate.Nodes.push_back(std::move(Node));
		}
		OutGraph = std::move(Candidate);
		return true;
	}

	auto DMaterialFunction::GetFunctionGraph() const -> FMaterialFunctionGraph
	{
		FMaterialFunctionGraph Graph;
		if (!ProjectExpressions(Signature, ExpressionCollection, Graph)) Graph = {.Signature = Signature};
		return Graph;
	}

	auto DMaterialFunction::GetExpressionBody() const -> FMaterialExpressionFunctionBody
	{
		FMaterialExpressionFunctionBody Body{.Signature = Signature, .AssetPath = GetObjectPath(), .Revision = Revision};
		for (const auto& Expression : ExpressionCollection.Expressions) Body.Expressions.push_back(Expression.Get());
		return Body;
	}

	auto DMaterialFunction::ValidateLoadedObjectGraph(const FObjectGraphLoadContext& Context, std::string& OutError) const -> bool
	{
		if (Context.bCooked) return true;
		if (GraphOwnershipVersion != 2)
		{
			OutError = "Unsupported material function expression schema; rebuild this function.";
			return false;
		}
		std::unordered_set<const DObject*> Owned;
		for (const auto& Expression : ExpressionCollection.Expressions)
			if (!IsValid(Expression.Get()) || Expression->GetOuter() != this || !Owned.insert(Expression.Get()).second)
			{
				OutError = "Function expression collection contains a missing, shared, or wrongly owned child.";
				return false;
			}
		for (const DObject* Child : GDObjectArray.GetObjectsWithOuter(this, EObjectQueryScope::LiveOnly))
			if (Child->IsA(DMaterialExpression::StaticClass()) && !Owned.contains(Child))
			{
				OutError = "Function contains an abandoned expression child outside its collection.";
				return false;
			}
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
		auto Result = ValidateMaterialFunctionSignature(InSignature);
		if (!Result) return Result;
		FMaterialExpressionCollection Candidate;
		for (auto* Expression : Expressions) Candidate.Expressions.emplace_back(Expression);
		Result = FMaterialExpressionBuildContext::ValidateFunction(Expressions, InSignature);
		if (!Result) return Result;
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "FunctionExpressionApply"));
		FMaterialExpressionCollection Copies;
		for (const auto& Expression : Candidate.Expressions)
		{
			auto* Copy = DuplicateObject(Expression.Get(), Staging.Get(), FName(std::string("Expression_") + Expression->Id.ToString()));
			if (!Copy)
			{
				Result.bSucceeded = false;
				Result.Diagnostics.push_back({.Message = "Unable to duplicate the function expression candidate."});
				return Result;
			}
			Copies.Expressions.emplace_back(Copy);
		}
		TStrongObjectPtr<DObject> Retired(NewObject<DObject>(nullptr, "RetiredFunctionExpressions"));
		for (auto& Expression : ExpressionCollection.Expressions) if (Expression) Expression->SetOuterPrivate(Retired.Get());
		for (auto& Expression : Copies.Expressions) Expression->SetOuterPrivate(this);
		ExpressionCollection = std::move(Copies);
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
		if (GraphOwnershipVersion != 2 || Ar.HasError())
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

	auto DMaterialFunction::SetFunctionGraph(FMaterialFunctionGraph Candidate)
		-> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		auto Result = ValidateMaterialFunctionGraph(Candidate);
		if (!Result || Candidate == GetFunctionGraph()) return Result;
		TStrongObjectPtr<DObject> Staging(NewObject<DObject>(nullptr, "FunctionCandidate"));
		FMaterialExpressionCollection Collection;
		if (!Private::ConstructFunctionExpressions(Staging.Get(), Candidate, Collection))
		{
			Result.bSucceeded = false;
			Result.Diagnostics.push_back({.Message = "Function candidate cannot be represented by supported expression classes."});
			return Result;
		}
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Collection.Expressions) Expressions.push_back(Expression.Get());
		Result = SetFunctionExpressions(Candidate.Signature, Expressions);
		if (Result)
		{
			for (const auto& Node : Candidate.Nodes)
			{
				const auto Position = std::ranges::find(Presentation.Nodes, Node.Id, &FMaterialGraphNodePresentation::NodeId);
				if (Position != Presentation.Nodes.end()) Position->DisplayName = Node.DisplayName;
				else if (!Node.DisplayName.empty()) Presentation.Nodes.push_back({.NodeId = Node.Id, .DisplayName = Node.DisplayName});
			}
		}
		return Result;
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
