#include "Materials/MaterialExpressionBuild.h"

#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Threading/RunnableThread.h"
#include <cmath>

namespace Durin
{
	auto FMaterialExpressionBuildContext::FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs)
		-> FMaterialExpressionBuildResult
	{
		const std::array Inputs{Outputs.BaseColor, Outputs.Normal, Outputs.Metallic, Outputs.Roughness,
			Outputs.AmbientOcclusion, Outputs.Emissive, Outputs.Opacity, Outputs.OpacityMask};
		const auto Vector = [](const FVector3& Value) {
			return FMaterialProgramLiteral{static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)};
		};
		const std::array Defaults{Vector(Outputs.BaseColorDefault), Vector(Outputs.NormalDefault),
			FMaterialProgramLiteral{Outputs.MetallicDefault}, FMaterialProgramLiteral{Outputs.RoughnessDefault},
			FMaterialProgramLiteral{Outputs.AmbientOcclusionDefault}, Vector(Outputs.EmissiveDefault),
			FMaterialProgramLiteral{Outputs.OpacityDefault}, FMaterialProgramLiteral{Outputs.OpacityMaskDefault}};
		auto OutputError = [&](uint32 Index, std::string Message) {
			if (Result.Diagnostics.empty())
				Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Type,
					.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					.LocationIndex = Index, .Message = std::move(Message)});
		};
		auto ValidSelector = [](const FMaterialExpressionInput& Input) {
			return Input.ExpressionId.IsValid() || (Input.OutputIndex == 0 && !Input.OutputId.IsValid());
		};
		BuildAllExpressions();
		const auto OutputLinks = std::ranges::count_if(Inputs, [](const auto& Input) { return Input.ExpressionId.IsValid(); })
			+ (Outputs.Surface.ExpressionId.IsValid() ? 1 : 0);
		if (AuthoredLinks + OutputLinks > MaterialProgramMaxLinkCount) OutputError(0, "Material output connections exceed the authored link bound.");
		if (!ValidSelector(Outputs.Surface)) OutputError(0, "Disconnected Surface output has an output selector.");
		for (uint32 Index = 0; Index < Inputs.size() && Result.Diagnostics.empty(); ++Index)
		{
			const auto& Input = Inputs[Index];
			const auto& Default = Defaults[Index];
			if (!ValidSelector(Input) || !std::isfinite(Default.X) || !std::isfinite(Default.Y) || !std::isfinite(Default.Z))
			{
				OutputError(Index, "Material output has an invalid selector or retained default.");
				break;
			}
			auto& Root = Result.IR.SurfaceRoot.Inputs[Index];
			Root.Literal = Default;
			if (!Input.ExpressionId.IsValid()) continue;
			if (Outputs.Surface.ExpressionId.IsValid())
			{
				OutputError(Index, "Aggregate Surface and individual output connections cannot be combined.");
				break;
			}
			Root.ExpressionIndex = ResolveIndex(Input);
			Root.bExpression = true;
			if (Result.Diagnostics.empty() && Result.IR.Nodes[Root.ExpressionIndex].ResultType != Root.Type)
				OutputError(Index, "Material output source has an incompatible type.");
		}
		if (Outputs.Surface.ExpressionId.IsValid() && Result.Diagnostics.empty())
		{
			auto& Root = Result.IR.SurfaceRoot;
			Root.bAggregate = true;
			Root.AggregateExpressionIndex = ResolveIndex(Outputs.Surface);
			if (Result.Diagnostics.empty() && Result.IR.Nodes[Root.AggregateExpressionIndex].ResultType != EMaterialProgramValueType::Surface)
				OutputError(0, "Aggregate material output requires a Surface expression.");
		}
		return Finish({});
	}

	auto SnapshotMaterialCompilerInput(const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment,
		FMaterialIRCompilerInput& OutInput, std::vector<FMaterialFunctionOwnerStamp>* OutOwners)
		-> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Validation;
		const DMaterialInterface* Root = &Material;
		for (uint32 Depth = 0; Root && Root->GetParent() && Depth < MaterialMaximumParentDepth; ++Depth) Root = Root->GetParent();
		const auto* Owner = Cast<DMaterial>(Root);
		if (!Owner)
		{
			Validation.Diagnostics.push_back({.Message = "Material has no typed expression owner."});
			return Validation;
		}
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Owner->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		FMaterialExpressionBuildContext Context(Expressions, {.FindFunction = [&](const DMaterialFunctionInterface& Function)
			-> std::optional<FMaterialExpressionFunctionBody> {
			const auto* Concrete = Cast<DMaterialFunction>(&Function);
			if (!Concrete) return std::nullopt;
			Owners.push_back({MakeObjectHandle(const_cast<DMaterialFunction*>(Concrete)), Concrete->GetObjectPath(), Concrete->GetFunctionRevision()});
			return Concrete->GetExpressionBody();
		}});
		auto Built = Context.FinishSurface(Owner->GetExpressionOutputs());
		if (!Built)
		{
			Validation.Diagnostics = std::move(Built.Diagnostics);
			return Validation;
		}
		FMaterialIRCompilerInput Snapshot{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
			.StaticProperties = Material.GetStaticProperties(), .Environment = std::move(Environment), .Sources = std::move(Built.Sources)};
		std::ranges::sort(Snapshot.Environment.Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		OutInput = std::move(Snapshot);
		if (OutOwners) *OutOwners = std::move(Owners);
		Validation.bSucceeded = true;
		return Validation;
	}

	auto AreMaterialFunctionOwnersCurrent(std::span<const FMaterialFunctionOwnerStamp> Owners) -> bool
	{
		check(IsInGameThread());
		for (const auto& Stamp : Owners)
		{
			const auto* Owner = Cast<DMaterialFunctionInterface>(ResolveObjectHandle(Stamp.Owner));
			if (!IsValid(Owner) || Owner->GetFunctionRevision() != Stamp.Revision || Owner->GetObjectPath() != Stamp.AssetPath) return false;
		}
		return true;
	}
}
