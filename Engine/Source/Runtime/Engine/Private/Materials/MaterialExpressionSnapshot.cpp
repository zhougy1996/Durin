#include "Materials/MaterialExpressionBuild.h"
#include "Asset/Asset.h"

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

	auto FMaterialExpressionBuildContext::ValidateSurface(std::span<DMaterialExpression* const> Expressions,
		const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		FMaterialExpressionBuildContext Context(Expressions);
		Context.bValidateAuthoring = true;
		auto Built = Context.FinishSurface(Outputs);
		if (Built && OutCodeFingerprint)
		{
			// A transient edit checkpoint, not shader identity or a persisted encoding.
			// Hash fields individually: no object pointers, aggregate padding, parameter defaults or presentation.
			auto& Hash = Context.AuthoringCodeHash;
			const auto Literal = [&](const FMaterialProgramLiteral& Value) {
				Hash.UpdateValue(Value.X); Hash.UpdateValue(Value.Y);
				Hash.UpdateValue(Value.Z); Hash.UpdateValue(Value.W);
			};
			Hash.UpdateValue(static_cast<uint32>(Built.IR.Nodes.size()));
			for (const auto& Node : Built.IR.Nodes)
			{
				Hash.UpdateValue(Node.Opcode); Hash.UpdateValue(Node.ResultType);
				Hash.UpdateValue(static_cast<uint32>(Node.Inputs.size()));
				for (const auto Input : Node.Inputs) Hash.UpdateValue(Input);
				Hash.UpdateValue(static_cast<uint32>(Node.Payload.index()));
				if (const auto* Value = std::get_if<FMaterialProgramLiteral>(&Node.Payload)) Literal(*Value);
				else if (const auto* Id = std::get_if<FGuid>(&Node.Payload)) Hash.UpdateValue(*Id);
				else if (const auto* Swizzle = std::get_if<FMaterialIRSwizzle>(&Node.Payload))
				{
					Hash.UpdateValue(Swizzle->Length);
					for (const auto Component : Swizzle->Components) Hash.UpdateValue(Component);
				}
			}
			Hash.UpdateValue(Built.IR.SurfaceRoot.bAggregate);
			Hash.UpdateValue(Built.IR.SurfaceRoot.AggregateExpressionIndex);
			for (const auto& Input : Built.IR.SurfaceRoot.Inputs)
			{
				Hash.UpdateValue(Input.bExpression); Hash.UpdateValue(Input.ExpressionIndex);
				Hash.UpdateValue(Input.Type); Literal(Input.Literal);
			}
			*OutCodeFingerprint = Hash.Finalize();
		}
		return {.bSucceeded = static_cast<bool>(Built), .Diagnostics = std::move(Built.Diagnostics)};
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

	auto DMaterialInterface::GetParameterReachability() const
		-> std::shared_ptr<const FMaterialParameterReachability>
	{
		check(IsInGameThread());
		if (GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			const auto Program = GetAcceptedCompiledProgram();
			if (Program && ParameterReachability && ParameterReachabilityCookedProgram.lock() == Program)
				return ParameterReachability;
			auto Result = std::make_shared<FMaterialParameterReachability>();
			if (!Program)
			{
				Result->Validation.Diagnostics.push_back({.Message = "Material has no accepted compiled program."});
				return Result;
			}
			for (const auto& Parameter : Program->ActiveParameters) Result->ParameterIds.insert(Parameter.Id);
			Result->Validation.bSucceeded = true;
			ParameterReachabilityProgramRevision = 0;
			ParameterReachabilityFunctionOwners.clear();
			ParameterReachabilityCookedProgram = Program;
			ParameterReachability = Result;
			return Result;
		}

		FResolvedMaterialProperties Properties;
		std::string Error;
		if (!ResolveMaterialProperties(*this, Properties, Error))
		{
			auto Result = std::make_shared<FMaterialParameterReachability>();
			Result->Validation.Diagnostics.push_back({.Message = std::move(Error)});
			return Result;
		}
		const auto* Root = Cast<DMaterial>(ResolveObjectHandle(Properties.Root));
		// Structural reachability is independent of instance values and static properties.
		// Store it on the graph owner so all instances share the same analysis.
		if (Root != this) return Root->GetParameterReachability();
		const auto Revision = Root->GetMaterialProgramRevision();
		if (ParameterReachability && ParameterReachabilityProgramRevision == Revision
			&& AreMaterialFunctionOwnersCurrent(ParameterReachabilityFunctionOwners)) return ParameterReachability;

		auto Result = std::make_shared<FMaterialParameterReachability>();
		FMaterialIRCompilerInput Snapshot;
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		Result->Validation = SnapshotMaterialCompilerInput(*this, {}, Snapshot, &Owners);
		if (!Result->Validation) return Result;
		std::vector<uint32> Pending;
		if (Snapshot.IR.SurfaceRoot.bAggregate) Pending.push_back(Snapshot.IR.SurfaceRoot.AggregateExpressionIndex);
		else for (const auto& Input : Snapshot.IR.SurfaceRoot.Inputs)
			if (Input.bExpression) Pending.push_back(Input.ExpressionIndex);
		std::vector<bool> Visited(Snapshot.IR.Nodes.size());
		while (!Pending.empty())
		{
			const auto Index = Pending.back(); Pending.pop_back();
			if (Visited[Index]) continue;
			Visited[Index] = true;
			const auto& Node = Snapshot.IR.Nodes[Index];
			const auto Id = Node.GetParameterId();
			if (Id.IsValid()) Result->ParameterIds.insert(Id);
			Pending.insert(Pending.end(), Node.Inputs.begin(), Node.Inputs.end());
		}
		ParameterReachabilityCookedProgram.reset();
		ParameterReachabilityProgramRevision = Revision;
		ParameterReachabilityFunctionOwners = std::move(Owners);
		ParameterReachability = Result;
		return Result;
	}
}
