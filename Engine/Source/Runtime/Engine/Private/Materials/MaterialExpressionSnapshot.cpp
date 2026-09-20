#include "MaterialExpressionGraphBuilder.h"
#include "Asset/Asset.h"

#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Threading/RunnableThread.h"
#include <cmath>

namespace Durin
{
	auto MIR::FGraphBuilderImpl::FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs)
		-> MIR::FBuildResult
	{
		const std::array Inputs{Outputs.BaseColor, Outputs.Normal, Outputs.Metallic, Outputs.Roughness,
			Outputs.AmbientOcclusion, Outputs.Emissive, Outputs.Opacity, Outputs.OpacityMask};
		std::array<FMaterialProgramLiteral, 8> Defaults;
		for (uint32 Index = 0; Index < Defaults.size(); ++Index)
		{
			const auto Values = ReadMaterialOutputDefault(Outputs, static_cast<EMaterialOutputPin>(Index));
			Defaults[Index] = {Values.empty() ? 0.f : Values[0], Values.size() > 1 ? Values[1] : 0.f, Values.size() > 2 ? Values[2] : 0.f};
		}
		auto OutputError = [&](uint32 Index, FMaterialError Error) {
			if (Result.Diagnostics.empty())
				Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Type,
					.LocationKind = EMaterialProgramDiagnosticLocationKind::SurfaceOutput,
					.LocationIndex = Index, .Error = std::move(Error)});
		};
		auto ValidSelector = [](const FMaterialExpressionInput& Input) {
			return Input.ExpressionId.IsValid() || (Input.OutputIndex == 0 && !Input.OutputId.IsValid());
		};
		BuildAllExpressions();
		const bool bAggregate = Outputs.bUseMaterialAttributes;
		const FMaterialSurfaceOutputs StandardDefaults;
		const auto OutputLinks = std::ranges::count_if(Inputs, [](const auto& Input) { return Input.Connection.ExpressionId.IsValid(); })
			+ (Outputs.Surface.ExpressionId.IsValid() ? 1 : 0);
		if (AuthoredLinks + OutputLinks > MaterialProgramMaxLinkCount) OutputError(0, EMaterialExpressionError::OutputConnectionsExceedAuthoredLinkBound);
		if (!ValidSelector(Outputs.Surface)) OutputError(0, EMaterialExpressionError::DisconnectedSurfaceOutputOutputSelector);
		for (uint32 Index = 0; Index < Inputs.size() && Result.Diagnostics.empty(); ++Index)
		{
			const auto& Input = Inputs[Index].Connection;
			const auto& Retained = Inputs[Index].Constant;
			AuthoringCodeHash.UpdateValue(Inputs[Index].UseConstant);
			AuthoringCodeHash.UpdateValue(static_cast<uint32>(Retained.size()));
			for (const auto V : Retained) AuthoringCodeHash.UpdateValue(V);
			const auto& Default = Defaults[Index];
			if (Retained.size() != static_cast<uint32>(GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index))) + 1
				|| !std::ranges::all_of(Retained, [](float V) { return std::isfinite(V); }) || !ValidSelector(Input) || !std::isfinite(Default.X) || !std::isfinite(Default.Y) || !std::isfinite(Default.Z))
			{
				OutputError(Index, EMaterialExpressionError::OutputInvalidSelectorRetainedDefault);
				break;
			}
			auto& Root = Result.IR.SurfaceRoot.Inputs[Index];
			Root.Literal = bAggregate ? GetMaterialSurfaceOutputDefault(StandardDefaults,
				static_cast<EMaterialSurfaceOutput>(Index)) : Default;
			if (!Input.ExpressionId.IsValid()) continue;
			const auto ExpressionIndex = *BroadcastScalar(ResolveIndex(Input), Root.Type).GetIndex();
			if (!bAggregate) { Root.ExpressionIndex = ExpressionIndex; Root.bExpression = true; }
			if (Result.Diagnostics.empty() && Result.IR.Nodes[ExpressionIndex].ResultType != Root.Type)
				{
				FMaterialError Error(EMaterialExpressionError::OutputSourceIncompatibleType);
				Error.ExpectedType = Root.Type;
				Error.ActualType = Result.IR.Nodes[ExpressionIndex].ResultType;
				OutputError(Index, std::move(Error));
			}
		}
		if (Outputs.Surface.ExpressionId.IsValid() && Result.Diagnostics.empty())
		{
			auto& Root = Result.IR.SurfaceRoot;
			const auto ExpressionIndex = ResolveIndex(Outputs.Surface);
			if (bAggregate) { Root.bAggregate = true; Root.AggregateExpressionIndex = ExpressionIndex; }
			if (Result.Diagnostics.empty() && Result.IR.Nodes[ExpressionIndex].ResultType != EMaterialProgramValueType::Surface)
				OutputError(0, EMaterialExpressionError::AggregateMaterialOutputRequiresSurfaceExpression);
		}
		return Finish({});
	}

	auto MIR::FGraphBuilderImpl::ValidateSurface(std::span<DMaterialExpression* const> Expressions,
		const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		MIR::FGraphBuilderImpl Context(Expressions);
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
				else if (const auto* Swizzle = std::get_if<MIR::FSwizzle>(&Node.Payload))
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

	auto SnapshotMaterialCompilerInput(const DMaterialInterface& Material, FMaterialCompilerEnvironment Environment)
		-> FMaterialCompilerSnapshotResult
	{
		check(IsInGameThread());
		FMaterialCompilerSnapshotResult Result;
		const DMaterialInterface* Root = &Material;
		for (uint32 Depth = 0; Root && Root->GetParent() && Depth < MaterialMaximumParentDepth; ++Depth) Root = Root->GetParent();
		const auto* Owner = Cast<DMaterial>(Root);
		if (!Owner)
		{
			Result.Diagnostics.push_back({.Error = EMaterialExpressionError::NoTypedExpressionOwner});
			return Result;
		}
		std::vector<DMaterialExpression*> Expressions;
		for (const auto& Expression : Owner->GetExpressionCollection().Expressions) Expressions.push_back(Expression.Get());
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		MIR::FGraphBuilderImpl Context(Expressions, {.FindFunction = [&](const DMaterialFunctionInterface& Function)
			-> std::optional<MIR::FFunctionBody> {
			const auto* Concrete = Cast<DMaterialFunction>(&Function);
			if (!Concrete) return std::nullopt;
			Owners.push_back({FObjectKey(const_cast<DMaterialFunction*>(Concrete)), Concrete->GetObjectPath(), Concrete->GetFunctionRevision()});
			return Concrete->GetExpressionBody();
		}});
		auto Built = Context.FinishSurface(Owner->GetExpressionOutputs());
		if (!Built)
		{
			Result.Diagnostics = std::move(Built.Diagnostics);
			return Result;
		}
		MIR::FCompilerInput Snapshot{.IR = std::move(Built.IR), .Parameters = std::move(Built.Parameters),
			.StaticProperties = Material.GetStaticProperties(), .Environment = std::move(Environment), .Sources = std::move(Built.Sources)};
		std::ranges::sort(Snapshot.Environment.Dependencies, {}, &FMaterialCompilerDependency::VirtualPath);
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		Result.Snapshot.emplace(FMaterialCompilerSnapshot{std::move(Snapshot), std::move(Owners)});
		return Result;
	}

	auto AreMaterialFunctionOwnersCurrent(std::span<const FMaterialFunctionOwnerStamp> Owners) -> bool
	{
		check(IsInGameThread());
		for (const auto& Stamp : Owners)
		{
			const auto* Owner = Cast<DMaterialFunctionInterface>(ResolveObjectKey(Stamp.Owner));
			if (!IsValid(Owner) || Owner->GetFunctionRevision() != Stamp.Revision || Owner->GetObjectPath() != Stamp.AssetPath) return false;
		}
		return true;
	}

	auto DMaterialInterface::GetParameterReachability() const
		-> std::shared_ptr<const FMaterialParameterReachability>
	{
		check(IsInGameThread());
		if (IsDynamicInstance() || GetAssetRuntimeConfiguration().RequiresCookedPayload())
		{
			const auto Program = GetAcceptedCompiledProgram();
			if (Program && ParameterReachability && ParameterReachabilityCookedProgram.lock() == Program)
				return ParameterReachability;
			auto Result = std::make_shared<FMaterialParameterReachability>();
			if (!Program)
			{
				Result->Validation.Diagnostics.push_back({.Error = EMaterialExpressionError::NoAcceptedCompiledProgram});
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
		const auto Error = ResolveMaterialProperties(*this, Properties);
		if (!Error)
		{
			auto Result = std::make_shared<FMaterialParameterReachability>();
			Result->Validation.Diagnostics.push_back({.Error = std::move(Error.Error)});
			return Result;
		}
		const auto* Root = Cast<DMaterial>(ResolveObjectKey(Properties.Root));
		// Structural reachability is independent of instance values and static properties.
		// Store it on the graph owner so all instances share the same analysis.
		if (Root != this) return Root->GetParameterReachability();
		const auto Revision = Root->GetMaterialProgramRevision();
		if (ParameterReachability && ParameterReachabilityProgramRevision == Revision
			&& AreMaterialFunctionOwnersCurrent(ParameterReachabilityFunctionOwners)) return ParameterReachability;

		auto Result = std::make_shared<FMaterialParameterReachability>();
		auto Capture = SnapshotMaterialCompilerInput(*this, {});
		Result->Validation = {.bSucceeded = static_cast<bool>(Capture), .Diagnostics = std::move(Capture.Diagnostics)};
		if (!Capture) return Result;
		const auto& Snapshot = Capture.Snapshot->Input;
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
		ParameterReachabilityFunctionOwners = std::move(Capture.Snapshot->FunctionOwners);
		ParameterReachability = Result;
		return Result;
	}
}
