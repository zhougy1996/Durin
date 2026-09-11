#include "Materials/MaterialFunctionInterface.h"

#include "Threading/RunnableThread.h"

#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	auto ValidateMaterialProgramWithFunctions(const FMaterialProgram& Program,
		std::span<const FMaterialParameterDefinition> Definitions,
		std::span<const FMaterialFunctionCall> Calls) -> FMaterialProgramValidationResult
	{
		std::vector<FMaterialFunctionCallSnapshot> Bindings;
		if (Calls.size() > MaterialProgramMaxNodeCount)
			return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
				.Message = "Root function calls exceed the authored node bound."}}};
		for (const auto& Call : Calls)
		{
			if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.size() > MaterialFunctionMaxOutputs)
				return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
					.NodeId = Call.NodeId, .Message = "Function call ports exceed interface bounds."}}};
			Bindings.push_back({.NodeId = Call.NodeId, .Inputs = Call.Inputs, .Outputs = Call.Outputs});
		}
		return ValidateMaterialProgram(Program, Definitions, Bindings);
	}

	auto SnapshotMaterialFunctionCalls(std::span<const FMaterialFunctionCall> Calls,
		std::vector<FMaterialFunctionCallSnapshot>& OutCalls, FMaterialFunctionClosure& OutClosure,
		std::vector<FMaterialFunctionOwnerStamp>* OutOwners)
		-> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		std::vector<FMaterialFunctionCallSnapshot> Snapshots;
		std::vector<DMaterialFunctionInterface*> Roots;
		std::vector<FGuid> RootCallIds;
		if (Calls.size() > MaterialProgramMaxNodeCount)
			return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
				.Message = "Root function calls exceed the authored node bound."}}};
		for (const auto& Call : Calls)
		{
			if (!IsValid(Call.Function.Get()))
				return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Dependency,
					.LocationKind = EMaterialProgramDiagnosticLocationKind::Node,
					.NodeId = Call.NodeId, .Message = "Root function dependency is missing."}}};
			if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.size() > MaterialFunctionMaxOutputs)
				return {.Diagnostics = {{.Category = EMaterialProgramDiagnosticCategory::Bounds,
					.NodeId = Call.NodeId, .Message = "Function call ports exceed interface bounds."}}};
			Snapshots.push_back({Call.NodeId, Call.Function->GetObjectPath(), Call.Inputs, Call.Outputs});
			Result = ValidateMaterialFunctionCallSignature(Snapshots.back(), Call.Function->GetFunctionSignature());
			if (!Result)
			{
				for (auto& Diagnostic : Result.Diagnostics) Diagnostic.FunctionAssetPath.clear();
				return Result;
			}
			Roots.push_back(Call.Function.Get());
			RootCallIds.push_back(Call.NodeId);
		}
		FMaterialFunctionClosure Closure;
		Result = SnapshotMaterialFunctionClosure(Roots, Closure, RootCallIds, OutOwners);
		if (!Result) return Result;
		OutCalls = std::move(Snapshots);
		OutClosure = std::move(Closure);
		return Result;
	}

	auto SnapshotMaterialFunctionClosure(std::span<DMaterialFunctionInterface* const> Roots,
		FMaterialFunctionClosure& OutClosure, std::span<const FGuid> RootCallIds,
		std::vector<FMaterialFunctionOwnerStamp>* OutOwners) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		FMaterialFunctionClosure Closure;
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		std::unordered_map<DMaterialFunctionInterface*, uint8> States;
		std::unordered_map<DMaterialFunctionInterface*, uint32> Heights;
		std::unordered_set<std::string> Paths;
		uint64 ClosureBytes = 0;
		std::vector<FGuid> CallPath;
		const auto Fail = [&](std::string Message, std::string AssetPath,
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Dependency) {
			if (Result.Diagnostics.size() == MaterialProgramMaxDiagnosticCount) return;
			Message.resize(std::min<size_t>(Message.size(), MaterialProgramMaxDiagnosticMessageBytes));
			AssetPath.resize(std::min<size_t>(AssetPath.size(), MaterialProgramMaxStringBytes));
			Result.Diagnostics.push_back({.Category = Category,
				.LocationKind = EMaterialProgramDiagnosticLocationKind::Program,
				.Message = std::move(Message),
				.FunctionAssetPath = std::move(AssetPath), .CallPath = CallPath});
		};
		const auto AppendDiagnostics = [&](FMaterialProgramValidationResult Validation, std::string_view AssetPath) {
			for (auto& Diagnostic : Validation.Diagnostics)
			{
				if (Result.Diagnostics.size() == MaterialProgramMaxDiagnosticCount) break;
				Diagnostic.FunctionAssetPath = AssetPath;
				Diagnostic.CallPath = CallPath;
				Result.Diagnostics.push_back(std::move(Diagnostic));
			}
		};
		const auto Visit = [&](auto&& Self, DMaterialFunctionInterface* Function, uint32 Depth) -> bool {
			if (!IsValid(Function)) { Fail("Function dependency is missing.", {}); return false; }
			const std::string AssetPath = Function->GetObjectPath();
			if (Depth > MaterialFunctionMaxCallDepth)
			{
				Fail("Function call depth exceeds the closure bound.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
				return false;
			}
			if (const auto State = States.find(Function); State != States.end())
			{
				if (State->second == 2)
				{
					if (Depth + Heights.at(Function) - 1 <= MaterialFunctionMaxCallDepth) return true;
					Fail("Shared function dependency exceeds the call depth bound.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
					return false;
				}
				Fail("Recursive function calls are not supported.", AssetPath);
				return false;
			}
			if (States.size() == MaterialFunctionMaxDependencies || AssetPath.size() > MaterialProgramMaxStringBytes)
			{
				Fail("Function dependency count or path exceeds the closure bound.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
				return false;
			}
			if (!Paths.emplace(AssetPath).second)
			{
				Fail("Distinct function owners have the same asset path.", AssetPath);
				return false;
			}
			States.emplace(Function, 1);
			const uint64 Revision = Function->GetFunctionRevision();
			FMaterialFunctionSnapshot Snapshot;
			auto Validation = Function->BuildFunctionSnapshot(Snapshot);
			if (!Validation) { AppendDiagnostics(std::move(Validation), AssetPath); return false; }
			if (Snapshot.Revision != Revision || Function->GetFunctionRevision() != Revision
				|| Snapshot.AssetPath != AssetPath || Snapshot.Signature != Function->GetFunctionSignature())
			{
				Fail("Function changed while its compilation snapshot was being captured.", AssetPath);
				return false;
			}
			// Validate provider-owned snapshots without relying on concrete asset classes.
			if (Snapshot.Nodes.size() > MaterialProgramMaxNodeCount || Snapshot.Calls.size() > Snapshot.Nodes.size())
			{
				Fail("Function snapshot exceeds graph bounds.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
				return false;
			}
			Validation = ValidateMaterialFunctionSignature(Snapshot.Signature);
			if (!Validation) { AppendDiagnostics(std::move(Validation), AssetPath); return false; }
			uint64 Bytes = sizeof(Snapshot) + AssetPath.size();
			for (const auto& Node : Snapshot.Nodes)
			{
				if (Node.Inputs.size() > MaterialProgramMaxNodeInputCount || Node.SurfaceAttributes.size() > 8
					|| Node.DisplayName.size() > MaterialProgramMaxDisplayNameBytes)
				{
					Fail("Function snapshot node exceeds bounds.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
					return false;
				}
				Bytes += sizeof(Node) + Node.DisplayName.size() + Node.Inputs.size() * sizeof(FMaterialProgramLink)
					+ Node.SurfaceAttributes.size() * sizeof(FMaterialSurfaceAttributeBinding);
			}
			for (const auto& Port : Snapshot.Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Snapshot.Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Call : Snapshot.Calls)
			{
				if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.size() > MaterialFunctionMaxOutputs
					|| Call.FunctionPath.size() > MaterialProgramMaxStringBytes)
				{
					Fail("Function snapshot call exceeds bounds.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
					return false;
				}
				Bytes += sizeof(Call) + Call.FunctionPath.size() + Call.Inputs.size() * sizeof(FMaterialFunctionInputBinding)
					+ Call.Outputs.size() * sizeof(FMaterialFunctionOutputBinding);
			}
			ClosureBytes += Bytes;
			if (Bytes > MaterialProgramMaxCanonicalBytes || ClosureBytes > MaterialFunctionMaxClosureBytes)
			{
				Fail("Function snapshot payload exceeds document or closure bounds.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
				return false;
			}
			FMaterialFunctionGraph DetachedGraph;
			DetachedGraph.SchemaVersion = Snapshot.SchemaVersion;
			DetachedGraph.Signature = Snapshot.Signature;
			DetachedGraph.Nodes = Snapshot.Nodes;
			for (const auto& Call : Snapshot.Calls)
				DetachedGraph.Calls.push_back({.NodeId = Call.NodeId, .Inputs = Call.Inputs, .Outputs = Call.Outputs});
			Validation = ValidateMaterialFunctionGraph(DetachedGraph);
			if (!Validation) { AppendDiagnostics(std::move(Validation), AssetPath); return false; }
			const auto Dependencies = Function->GetFunctionDependencies();
			if (Dependencies.size() > MaterialFunctionMaxDependencies)
			{
				Fail("Function dependency query exceeds bounds.", AssetPath, EMaterialProgramDiagnosticCategory::Bounds);
				return false;
			}
			std::unordered_map<std::string, DMaterialFunctionInterface*> DependencyPaths;
			for (const auto& Dependency : Dependencies)
			{
				if (!IsValid(Dependency.Get())) { Fail("Function dependency is missing.", AssetPath); return false; }
				DependencyPaths.emplace(Dependency->GetObjectPath(), Dependency.Get());
			}
			for (const auto& Call : Snapshot.Calls)
			{
				const auto Dependency = DependencyPaths.find(Call.FunctionPath);
				if (Dependency == DependencyPaths.end())
				{
					Fail("Function snapshot call is absent from its dependency query.", AssetPath);
					return false;
				}
				Validation = ValidateMaterialFunctionCallSignature(Call, Dependency->second->GetFunctionSignature());
				if (!Validation) { AppendDiagnostics(std::move(Validation), AssetPath); return false; }
				CallPath.push_back(Call.NodeId);
				if (!Self(Self, Dependency->second, Depth + 1)) return false;
				CallPath.pop_back();
			}
			// Preserve interface-only dependencies supplied by other implementations too.
			uint32 Height = 1;
			for (const auto& Dependency : Dependencies)
			{
				if (!Self(Self, Dependency.Get(), Depth + 1)) return false;
				Height = std::max(Height, 1 + Heights.at(Dependency.Get()));
			}
			if (Function->GetFunctionRevision() != Revision)
			{
				Fail("Function changed while its dependency closure was being captured.", AssetPath);
				return false;
			}
			States[Function] = 2;
			Owners.push_back({MakeObjectHandle(Function), AssetPath, Revision});
			Heights[Function] = Height;
			Closure.Functions.push_back(std::move(Snapshot));
			return true;
		};
		if (Roots.size() > MaterialProgramMaxNodeCount || (!RootCallIds.empty() && RootCallIds.size() != Roots.size()))
		{
			Fail("Function root call count exceeds the authored node bound.", {}, EMaterialProgramDiagnosticCategory::Bounds);
			return Result;
		}
		for (size_t Index = 0; Index < Roots.size(); ++Index)
		{
			CallPath.clear();
			if (!RootCallIds.empty()) CallPath.push_back(RootCallIds[Index]);
			if (!Visit(Visit, Roots[Index], 1)) return Result;
		}
		std::ranges::sort(Closure.Functions, {}, &FMaterialFunctionSnapshot::AssetPath);
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		if (OutOwners) *OutOwners = std::move(Owners);
		OutClosure = std::move(Closure);
		Result.bSucceeded = true;
		return Result;
	}
}
