#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionEditing.h"
#include "Threading/RunnableThread.h"
#include "Materials/MaterialExpressionInputs.h"
#include "Materials/Material.h"

namespace Durin
{
#if DURIN_WITH_EDITORONLY_DATA
	namespace
	{
		auto RefreshCall(DMaterialExpressionFunctionCall& Call, DMaterialFunctionInterface* Function,
			const FMaterialFunctionSignature* PreviousSignature, bool bPublish) -> bool
		{
			FMaterialFunctionSignature Signature;
			if (Function) Signature = Function->GetFunctionSignature();
			for (auto* Ports : {&Signature.Inputs, &Signature.Outputs})
				std::ranges::stable_sort(*Ports, {}, &FMaterialFunctionPort::DisplayOrder);

			// A replacement function has independent IDs. Only that operation matches names.
			const auto OldId = [](const FMaterialFunctionPort& Port, const std::vector<FMaterialFunctionPort>* Previous) {
				if (!Previous) return Port.Id;
				const auto It = std::ranges::find_if(*Previous, [&](const auto& Old) { return FName(Old.Name) == FName(Port.Name); });
				return It == Previous->end() ? FGuid{} : It->Id;
			};
			std::vector<FMaterialExpressionFunctionInputBinding> Inputs;
			for (const auto& Port : Signature.Inputs)
			{
				auto& Binding = Inputs.emplace_back();
				const auto Id = OldId(Port, PreviousSignature ? &PreviousSignature->Inputs : nullptr);
				const auto Old = std::ranges::find(Call.FunctionInputs, Id, &FMaterialExpressionFunctionInputBinding::InputId);
				if (Old != Call.FunctionInputs.end())
				{
					Binding.Input = Old->Input;
					if (Old->ExpectedType == Port.Type) Binding.InputDefault = Old->InputDefault;
				}
				Binding.InputId = Port.Id;
				Binding.ExpectedType = Port.Type;
			}
			std::vector<FMaterialFunctionOutputBinding> Outputs;
			std::vector<int32> NewIndices(Call.FunctionOutputs.size(), -1);
			for (const auto& Port : Signature.Outputs)
			{
				const auto Id = OldId(Port, PreviousSignature ? &PreviousSignature->Outputs : nullptr);
				const auto Old = std::ranges::find(Call.FunctionOutputs, Id, &FMaterialFunctionOutputBinding::OutputId);
				if (Old != Call.FunctionOutputs.end()) NewIndices[Old - Call.FunctionOutputs.begin()] = static_cast<int32>(Outputs.size());
				Outputs.push_back({Port.Id, Port.Type});
			}
			if (Call.Function.Get() == Function && Call.FunctionInputs == Inputs && Call.FunctionOutputs == Outputs) return false;
			if (auto* Owner = Call.GetOuter(); Cast<DMaterial>(Owner) || Cast<DMaterialFunction>(Owner))
				for (const auto& Expression : FMaterialExpressionEditing::GetExpressions(*Owner))
					VisitStoredMaterialInputs(*Expression, [&](FMaterialExpressionInput& Input) {
						if (Input.Expression.Get() != &Call) return;
						if (Input.OutputIndex < 0 || static_cast<size_t>(Input.OutputIndex) >= NewIndices.size()
							|| NewIndices[Input.OutputIndex] < 0) Input = {};
						else Input.OutputIndex = NewIndices[Input.OutputIndex];
					});
			Call.Function = Function;
			Call.FunctionInputs = std::move(Inputs);
			Call.FunctionOutputs = std::move(Outputs);
			auto* Owner = Call.GetOuter();
			if (bPublish && (Cast<DMaterial>(Owner) || Cast<DMaterialFunction>(Owner))
				&& std::ranges::any_of(FMaterialExpressionEditing::GetExpressions(*Owner),
					[&](const auto& Expression) { return Expression.Get() == &Call; }))
				FMaterialExpressionEditing::Publish(*Owner);
			return true;
		}
	}

	auto DMaterialExpressionFunctionCall::UpdateFromFunctionResource(bool bRecreateAndLinkNode) -> bool
	{
		return RefreshCall(*this, Function.Get(), nullptr, bRecreateAndLinkNode);
	}

	auto DMaterialExpressionFunctionCall::SetMaterialFunction(DMaterialFunctionInterface* NewFunction) -> bool
	{
		if (NewFunction && GetOuter())
		{
			std::vector<FMaterialFunctionOwnerStamp> Closure;
			const std::array<DMaterialFunctionInterface*, 1> Roots{NewFunction};
			if (!ValidateMaterialFunctionDependencies(Roots, Closure, EMaterialFunctionValidationMode::Editing)
				|| std::ranges::any_of(Closure, [&](const auto& Entry) { return Entry.AssetPath == GetOuter()->GetObjectPath(); })) return false;
		}
		const auto* Previous = Function && NewFunction != Function.Get() ? &Function->GetFunctionSignature() : nullptr;
		RefreshCall(*this, NewFunction, Previous, true);
		return NewFunction != nullptr;
	}
#endif

	auto ValidateMaterialFunctionCallSignature(const DMaterialExpressionFunctionCall& Call,
		const FMaterialFunctionSignature& Signature, EMaterialFunctionValidationMode Mode) -> FMaterialProgramValidationResult
	{
		auto Result = ValidateMaterialFunctionSignature(Signature);
		if (!Result) return Result;
		const auto Fail = [&](FGuid Port, std::string Message) {
			Result.Diagnostics.push_back({.NodeId = Call.Id, .Message = std::move(Message), .PortId = Port});
		};
		if (Call.FunctionInputs.size() > MaterialFunctionMaxInputs || Call.FunctionOutputs.size() > MaterialFunctionMaxOutputs)
			Fail({}, "Function call exceeds port bounds.");
		else
		{
			std::unordered_set<FGuid> Inputs, Outputs;
			for (const auto& Binding : Call.FunctionInputs)
			{
				const auto Port = std::ranges::find(Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
				if (Port == Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !Inputs.insert(Binding.InputId).second)
					Fail(Binding.InputId, "Function input was removed, retyped or bound more than once.");
			}
			for (const auto& Binding : Call.FunctionOutputs)
			{
				const auto Port = std::ranges::find(Signature.Outputs, Binding.OutputId, &FMaterialFunctionPort::Id);
				if (Port == Signature.Outputs.end() || Port->Type != Binding.ExpectedType || !Outputs.insert(Binding.OutputId).second)
					Fail(Binding.OutputId, "Function output was removed, retyped or bound more than once.");
			}
			for (const auto& Port : Signature.Inputs)
				if (Port.bRequired && Mode == EMaterialFunctionValidationMode::Compilation)
				{
					const auto Binding = std::ranges::find(Call.FunctionInputs, Port.Id, &FMaterialExpressionFunctionInputBinding::InputId);
					if (Binding == Call.FunctionInputs.end() || (!Binding->Input.IsConnected() && Binding->InputDefault.empty()))
						Fail(Port.Id, "Required function input is not connected or bound.");
				}
		}
		Result.bSucceeded = Result.Diagnostics.empty();
		return Result;
	}

	auto ValidateMaterialFunctionDependencies(std::span<DMaterialFunctionInterface* const> Roots,
		std::vector<FMaterialFunctionOwnerStamp>& OutOwners, EMaterialFunctionValidationMode Mode) -> FMaterialProgramValidationResult
	{
#if !DURIN_WITH_EDITORONLY_DATA
		OutOwners.clear();
		FMaterialProgramValidationResult Unavailable;
		Unavailable.Diagnostics.push_back({.Message = "Function has no typed expression body."});
		return Unavailable;
#else
		check(IsInGameThread());
		FMaterialProgramValidationResult Result;
		std::vector<FMaterialFunctionOwnerStamp> Owners;
		std::unordered_map<DMaterialFunctionInterface*, uint32> Heights;
		std::unordered_set<DMaterialFunctionInterface*> Active;
		std::unordered_set<std::string> Paths;
		std::vector<FGuid> CallPath;
		uint64 ClosureBytes = 0;
		const auto Fail = [&](std::string Message, std::string Path) {
			Result.Diagnostics.push_back({.Message = std::move(Message),
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
				return Fail("Function call depth exceeds the closure bound.", Path);
			if (Active.contains(Function)) return Fail("Recursive function calls are not supported.", Path);
			if (const auto Found = Heights.find(Function); Found != Heights.end())
				return Depth + Found->second - 1 <= MaterialFunctionMaxCallDepth
					|| Fail("Shared function dependency exceeds the call depth bound.", Path);
			if (Paths.size() >= MaterialFunctionMaxDependencies || Path.size() > MaterialProgramMaxStringBytes)
				return Fail("Function dependency count or path exceeds the closure bound.", Path);
			if (!Paths.insert(Path).second) return Fail("Distinct function owners have the same asset path.", Path);
			const auto* Concrete = Cast<DMaterialFunction>(Function);
			if (!Concrete) return Fail("Function has no typed expression body.", Path);
			const auto Body = Concrete->GetExpressionBody();
			auto Validation = Mode == EMaterialFunctionValidationMode::Editing
				? FMaterialExpressionEditing::ValidateStorage(*Function)
				: MIR::FGraphBuilder::ValidateFunction(Body.Expressions);
			if (!Validation) { Append(std::move(Validation), Path); return false; }
			uint64 Bytes = Path.size() + Body.Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body.Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body.Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Fail("Function metadata exceeds the closure byte bound.", Path);
			ClosureBytes += Bytes;
			Active.insert(Function);
			uint32 Height = 1;
			for (auto* Expression : Body.Expressions)
				if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression))
				{
					auto* Dependency = Call->Function.Get();
					if (!IsValid(Dependency)) return Fail("Function dependency is missing.", Path);
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
				return Fail("Function changed while its dependencies were validated.", Path);
			Active.erase(Function); Heights.emplace(Function, Height);
			Owners.push_back({FObjectKey(Function), Path, Body.Revision});
			return true;
		};
		if (Roots.size() > MaterialProgramMaxNodeCount)
		{
			Fail("Function root count exceeds the authored node bound.", {});
			return Result;
		}
		for (auto* Root : Roots) if (!Visit(Visit, Root, 1)) return Result;
		std::ranges::sort(Owners, {}, &FMaterialFunctionOwnerStamp::AssetPath);
		OutOwners = std::move(Owners); Result.bSucceeded = true;
		return Result;
#endif
	}
}
