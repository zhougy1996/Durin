#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Threading/RunnableThread.h"

#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	auto ValidateMaterialFunctionCallSignature(const DMaterialExpressionFunctionCall& Call,
		const FMaterialFunctionSignature& Signature, EMaterialFunctionValidationMode Mode) -> FMaterialProgramValidationResult
	{
		auto Result = ValidateMaterialFunctionSignature(Signature);
		if (!Result) return Result;
		const auto Fail = [&](FGuid Port, FMaterialError Error) {
			Result.Diagnostics.push_back({.Category = EMaterialProgramDiagnosticCategory::Dependency,
				.NodeId = Call.Id, .Error = std::move(Error), .PortId = Port});
		};
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.size() > MaterialFunctionMaxOutputs)
			Fail({}, EMaterialFunctionError::CallExceedsPortBounds);
		else
		{
			std::unordered_set<FGuid> Inputs, Outputs;
			for (const auto& Binding : Call.Inputs)
			{
				const auto Port = std::ranges::find(Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
				if (Port == Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !Inputs.insert(Binding.InputId).second)
					Fail(Binding.InputId, EMaterialFunctionError::InputRemovedRetypedBoundMoreThanOnce);
			}
			for (const auto& Binding : Call.Outputs)
			{
				const auto Port = std::ranges::find(Signature.Outputs, Binding.OutputId, &FMaterialFunctionPort::Id);
				if (Port == Signature.Outputs.end() || Port->Type != Binding.ExpectedType || !Outputs.insert(Binding.OutputId).second)
					Fail(Binding.OutputId, EMaterialFunctionError::OutputRemovedRetypedBoundMoreThanOnce);
			}
			for (const auto& Port : Signature.Inputs)
				if (Port.bRequired && Mode == EMaterialFunctionValidationMode::Compilation)
				{
					const auto Binding = std::ranges::find(Call.Inputs, Port.Id, &FMaterialExpressionFunctionInputBinding::InputId);
					if (Binding == Call.Inputs.end() || (!Binding->Input.ExpressionId.IsValid() && Binding->InputDefault.empty()))
						Fail(Port.Id, EMaterialFunctionError::RequiredInputUnbound);
				}
		}
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}

	auto ValidateMaterialFunctionDependencies(std::span<DMaterialFunctionInterface* const> Roots,
		std::vector<FMaterialFunctionOwnerStamp>& OutOwners, EMaterialFunctionValidationMode Mode) -> FMaterialProgramValidationResult
	{
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		std::unordered_map<DMaterialFunctionInterface*, uint32> Heights;
		std::unordered_set<DMaterialFunctionInterface*> Active;
		std::unordered_set<std::string> Paths;
		std::vector<FGuid> CallPath;
		uint64 ClosureBytes = 0;
		const auto Fail = [&](FMaterialError Error, std::string Path,
			EMaterialProgramDiagnosticCategory Category = EMaterialProgramDiagnosticCategory::Dependency) {
			Result.Diagnostics.push_back({.Category = Category, .Error = std::move(Error),
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
			if (!IsValid(Function)) return Fail(EMaterialFunctionError::DependencyMissing, {});
			const auto Path = Function->GetObjectPath();
			if (Depth > MaterialFunctionMaxCallDepth)
				return Fail(EMaterialFunctionError::CallDepthExceedsClosureBound, Path, EMaterialProgramDiagnosticCategory::Bounds);
			if (Active.contains(Function)) return Fail(EMaterialFunctionError::RecursiveCall, Path);
			if (const auto Found = Heights.find(Function); Found != Heights.end())
				return Depth + Found->second - 1 <= MaterialFunctionMaxCallDepth
					|| Fail(EMaterialFunctionError::SharedFunctionDependencyExceedsCallDepthBound, Path, EMaterialProgramDiagnosticCategory::Bounds);
			if (Paths.size() >= MaterialFunctionMaxDependencies || Path.size() > MaterialProgramMaxStringBytes)
				return Fail(EMaterialFunctionError::DependencyCountPathExceedsClosureBound, Path, EMaterialProgramDiagnosticCategory::Bounds);
			if (!Paths.insert(Path).second) return Fail(EMaterialFunctionError::DistinctFunctionOwnersSameAssetPath, Path);
			const auto* Concrete = Cast<DMaterialFunction>(Function);
			if (!Concrete) return Fail(EMaterialFunctionError::NoTypedExpressionBody, Path);
			const auto Body = Concrete->GetExpressionBody();
			auto Validation = Mode == EMaterialFunctionValidationMode::Editing
				? FMaterialExpressionEditing::ValidateStorage(*Function)
				: FMaterialExpressionGraphBuilder::ValidateFunction(Body.Expressions);
			if (!Validation) { Append(std::move(Validation), Path); return false; }
			uint64 Bytes = Path.size() + Body.Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body.Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body.Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Fail(EMaterialFunctionError::MetadataExceedsClosureByteBound, Path, EMaterialProgramDiagnosticCategory::Bounds);
			ClosureBytes += Bytes;
			Active.insert(Function);
			uint32 Height = 1;
			for (auto* Expression : Body.Expressions)
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
				{
					auto* Dependency = Call->Function.Get();
					if (!IsValid(Dependency)) return Fail(EMaterialFunctionError::DependencyMissing, Path);
					if (Mode == EMaterialFunctionValidationMode::Compilation)
					{
						Validation = ValidateMaterialFunctionCallSignature(*Call, Dependency->GetFunctionSignature());
						if (!Validation) { Append(std::move(Validation), Path); return false; }
					}
					CallPath.push_back(Call->Id);
					if (!Self(Self, Dependency, Depth + 1)) return false;
					CallPath.pop_back();
					Height = std::max(Height, 1 + Heights.at(Dependency));
				}
			if (Function->GetFunctionRevision() != Body.Revision || Function->GetObjectPath() != Body.AssetPath)
				return Fail(EMaterialFunctionError::ChangedDependenciesWereValidated, Path);
			Active.erase(Function); Heights.emplace(Function, Height);
			Owners.push_back({FObjectKey(Function), Path, Body.Revision});
			return true;
		};
		if (Roots.size() > MaterialProgramMaxNodeCount)
		{
			Fail(EMaterialFunctionError::RootCountExceedsAuthoredNodeBound, {}, EMaterialProgramDiagnosticCategory::Bounds);
			return Result;
		}
		for (auto* Root : Roots) if (!Visit(Visit, Root, 1)) return Result;
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		OutOwners = std::move(Owners); Result.bSucceeded = true;
		return Result;
	}
}
