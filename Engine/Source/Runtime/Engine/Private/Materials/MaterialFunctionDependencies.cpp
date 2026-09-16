#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunction.h"
#include "Threading/RunnableThread.h"

#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	auto ValidateMaterialFunctionCallSignature(const DMaterialExpressionFunctionCall& Call,
		const FMaterialFunctionSignature& Signature) -> FMaterialProgramValidationResult
	{
		auto Result = ValidateMaterialFunctionSignature(Signature);
		if (!Result) return Result;
		const auto Fail = [&](FGuid Port, std::string Message) {
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Dependency,
				.NodeId = Call.Id, .Message = std::move(Message), .PortId = Port});
		};
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.size() > MaterialFunctionMaxOutputs)
			Fail({}, "Function call exceeds port bounds.");
		else
		{
			std::unordered_set<FGuid> Inputs, Outputs;
			for (const auto& Binding : Call.Inputs)
			{
				const auto Port = std::ranges::find(Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
				if (Port == Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !Inputs.insert(Binding.InputId).second)
					Fail(Binding.InputId, "Function input was removed, retyped or bound more than once.");
			}
			for (const auto& Binding : Call.Outputs)
			{
				const auto Port = std::ranges::find(Signature.Outputs, Binding.OutputId, &FMaterialFunctionPort::Id);
				if (Port == Signature.Outputs.end() || Port->Type != Binding.ExpectedType || !Outputs.insert(Binding.OutputId).second)
					Fail(Binding.OutputId, "Function output was removed, retyped or bound more than once.");
			}
			for (const auto& Port : Signature.Inputs)
				if (Port.bRequired)
				{
					const auto Binding = std::ranges::find(Call.Inputs, Port.Id, &FMaterialExpressionFunctionInputBinding::InputId);
					if (Binding == Call.Inputs.end() || (!Binding->Input.ExpressionId.IsValid() && Binding->InputDefault.empty()))
						Fail(Port.Id, "Required function input is not connected or bound.");
				}
		}
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}

	auto ValidateMaterialFunctionDependencies(std::span<DMaterialFunctionInterface* const> Roots,
		std::vector<FMaterialFunctionOwnerStamp>& OutOwners) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		std::unordered_map<DMaterialFunctionInterface*, uint32> Heights;
		std::unordered_set<DMaterialFunctionInterface*> Active;
		std::unordered_set<std::string> Paths;
		std::vector<FGuid> CallPath;
		uint64 ClosureBytes = 0;
		const auto Fail = [&](std::string Message, std::string Path,
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Dependency) {
			Result.Diagnostics.push_back({.Category = Category, .Message = std::move(Message),
				.FunctionAssetPath = std::move(Path), .CallPath = CallPath});
			return false;
		};
		const auto Append = [&](FMaterialProgramValidationResult Validation, const std::string& Path) {
			for (auto& Diagnostic : Validation.Diagnostics)
			{
				Diagnostic.FunctionAssetPath = Path; Diagnostic.CallPath = CallPath;
				Result.Diagnostics.push_back(std::move(Diagnostic));
			}
		};
		const auto Visit = [&](auto&& Self, DMaterialFunctionInterface* Function, uint32 Depth) -> bool {
			if (!IsValid(Function)) return Fail("Function dependency is missing.", {});
			const auto Path = Function->GetObjectPath();
			if (Depth > MaterialFunctionMaxCallDepth)
				return Fail("Function call depth exceeds the closure bound.", Path, EMaterialProgramDiagnosticCategory::Bounds);
			if (Active.contains(Function)) return Fail("Recursive function calls are not supported.", Path);
			if (const auto Found = Heights.find(Function); Found != Heights.end())
				return Depth + Found->second - 1 <= MaterialFunctionMaxCallDepth
					|| Fail("Shared function dependency exceeds the call depth bound.", Path, EMaterialProgramDiagnosticCategory::Bounds);
			if (Paths.size() >= MaterialFunctionMaxDependencies || Path.size() > MaterialProgramMaxStringBytes)
				return Fail("Function dependency count or path exceeds the closure bound.", Path, EMaterialProgramDiagnosticCategory::Bounds);
			if (!Paths.insert(Path).second) return Fail("Distinct function owners have the same asset path.", Path);
			const auto* Concrete = Cast<DMaterialFunction>(Function);
			if (!Concrete) return Fail("Function has no typed expression body.", Path);
			const auto Body = Concrete->GetExpressionBody();
			auto Validation = FMaterialExpressionBuildContext::ValidateFunction(Body.Expressions);
			if (!Validation) { Append(std::move(Validation), Path); return false; }
			uint64 Bytes = Path.size() + Body.Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body.Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body.Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Fail("Function metadata exceeds the closure byte bound.", Path, EMaterialProgramDiagnosticCategory::Bounds);
			ClosureBytes += Bytes;
			Active.insert(Function);
			uint32 Height = 1;
			for (auto* Expression : Body.Expressions)
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
				{
					auto* Dependency = Call->Function.Get();
					if (!IsValid(Dependency)) return Fail("Function dependency is missing.", Path);
					Validation = ValidateMaterialFunctionCallSignature(*Call, Dependency->GetFunctionSignature());
					if (!Validation) { Append(std::move(Validation), Path); return false; }
					CallPath.push_back(Call->Id);
					if (!Self(Self, Dependency, Depth + 1)) return false;
					CallPath.pop_back();
					Height = std::max(Height, 1 + Heights.at(Dependency));
				}
			if (Function->GetFunctionRevision() != Body.Revision || Function->GetObjectPath() != Body.AssetPath)
				return Fail("Function changed while its dependencies were validated.", Path);
			Active.erase(Function); Heights.emplace(Function, Height);
			Owners.push_back({MakeObjectHandle(Function), Path, Body.Revision});
			return true;
		};
		if (Roots.size() > MaterialProgramMaxNodeCount)
		{
			Fail("Function root count exceeds the authored node bound.", {}, EMaterialProgramDiagnosticCategory::Bounds);
			return Result;
		}
		for (auto* Root : Roots) if (!Visit(Visit, Root, 1)) return Result;
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		OutOwners = std::move(Owners); Result.bSucceeded = true;
		return Result;
	}
}
