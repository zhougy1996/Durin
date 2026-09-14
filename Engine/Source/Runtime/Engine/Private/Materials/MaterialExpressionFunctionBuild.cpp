#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunctionInterface.h"

namespace Durin
{
	auto DMaterialExpressionFunctionInput::Build(FMaterialExpressionBuildContext& Context,
		uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Function input terminal has only its primary output.");
		return Context.FunctionInput(PortId);
	}

	auto DMaterialExpressionFunctionOutput::Build(FMaterialExpressionBuildContext& Context,
		uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Function output terminal has only its primary output.");
		return Context.FunctionOutput(PortId, Source);
	}

	auto DMaterialExpressionFunctionCall::Build(FMaterialExpressionBuildContext& Context,
		uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || !OutputId.IsValid()) return Context.Fail("Function call connections require an output GUID and zero output index.");
		return Context.FunctionCall(*this, OutputId);
	}

	auto FMaterialExpressionBuildContext::FunctionInput(FGuid PortId) -> FMaterialExpressionBuildValue
	{
		if (!Signature) return Fail("Function input terminal has no owning invocation.");
		const auto Port = std::ranges::find(Signature->Inputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Inputs.end()) return Fail("Function input terminal has no matching declaration.");
		if (const auto Bound = BoundInputs.find(PortId); Bound != BoundInputs.end()) return Bound->second;
		if (Port->bRequired) return Fail("Required function input has no binding.");
		if (PortStack.size() >= MaterialFunctionMaxInputs || std::ranges::find(PortStack, PortId) != PortStack.end())
			return Fail("Function input defaults contain a cycle.");
		PortStack.push_back(PortId);
		FMaterialExpressionBuildValue Value;
		const auto& Default = Port->Default;
		switch (Default.Kind)
		{
		case EMaterialFunctionDefaultKind::Numeric:
		{
			const std::array Components{Default.Numeric.X, Default.Numeric.Y, Default.Numeric.Z, Default.Numeric.W};
			if (Port->Type > EMaterialProgramValueType::Float4) { Fail("Numeric function default has a non-numeric type."); break; }
			Value = Literal(std::span(Components).first(static_cast<size_t>(Port->Type) + 1));
			break;
		}
		case EMaterialFunctionDefaultKind::Texture:
			Value = FMaterialExpressionTextureDefault{Default.Sampler, Default.TextureFallback};
			break;
		case EMaterialFunctionDefaultKind::Input:
			Value = FunctionInput(Default.InputId);
			break;
		case EMaterialFunctionDefaultKind::UV0:
		{
			const std::array Zero{0.f};
			const auto Channel = Literal(Zero);
			Value = Emit({.Opcode = EMaterialProgramOpcode::UVChannel,
				.ResultType = EMaterialProgramValueType::Float2, .Inputs = {Channel}});
			break;
		}
		case EMaterialFunctionDefaultKind::Surface:
		{
			FMaterialIRNode Surface{.Opcode = EMaterialProgramOpcode::MakeSurface, .ResultType = EMaterialProgramValueType::Surface};
			for (uint8 Index = 0; Index < 8; ++Index)
			{
				const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
				const auto& Numeric = GetMaterialSurfaceOutputDefault(Default.Surface, Attribute);
				const std::array Components{Numeric.X, Numeric.Y, Numeric.Z, Numeric.W};
				Surface.Inputs.push_back(Literal(std::span(Components).first(static_cast<size_t>(GetMaterialSurfaceOutputType(Attribute)) + 1)));
			}
			Value = Emit(std::move(Surface));
			break;
		}
		default: Fail("Function input has no usable binding or default."); break;
		}
		if (!MatchesType(Value, Port->Type)) Fail("Function default type does not match its input declaration.");
		PortStack.pop_back();
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		BoundInputs.emplace(PortId, Value);
		return Value;
	}

	auto FMaterialExpressionBuildContext::FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source)
		-> FMaterialExpressionBuildValue
	{
		if (!Signature) return Fail("Function output terminal has no owning invocation.");
		const auto Port = std::ranges::find(Signature->Outputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Outputs.end()) return Fail("Function output terminal has no matching declaration.");
		const auto Value = Resolve(Source);
		if (!MatchesType(Value, Port->Type)) return Fail("Function output source does not match its declared type.");
		return Value;
	}

	auto FMaterialExpressionBuildContext::FunctionCall(const DMaterialExpressionFunctionCall& Call, FGuid OutputId)
		-> FMaterialExpressionBuildValue
	{
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		if (const auto Built = CallOutputs.find(Call.Id); Built != CallOutputs.end())
		{
			const auto Output = Built->second.find(OutputId);
			return Output != Built->second.end() ? Output->second : FMaterialExpressionBuildValue(Fail("Function call output GUID is not bound."));
		}
		const auto* Function = Call.Function.Get();
		if (!IsValid(Function) || !Environment.FindFunction) return Fail("Function call has no available expression body.");
		if (Shared->ActiveFunctions.size() >= MaterialFunctionMaxCallDepth
			|| std::ranges::find(Shared->ActiveFunctions, Function) != Shared->ActiveFunctions.end())
			return Fail("Function Build contains recursion or exceeds the call depth bound.");
		auto Found = Shared->Functions.find(Function);
		if (Found == Shared->Functions.end())
		{
			if (Shared->Functions.size() >= MaterialFunctionMaxDependencies) return Fail("Function Build exceeds the dependency bound.");
			auto Body = Environment.FindFunction(*Function);
			if (!Body) return Fail("Function expression body is unavailable.");
			if (Body->Expressions.size() > MaterialProgramMaxNodeCount || Body->Signature.Inputs.size() > MaterialFunctionMaxInputs
				|| Body->Signature.Outputs.empty() || Body->Signature.Outputs.size() > MaterialFunctionMaxOutputs)
				return Fail("Function expression body exceeds node or signature bounds.");
			uint64 Bytes = Body->AssetPath.size() + Body->Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body->Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body->Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || Shared->ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Fail("Function expression body metadata exceeds the closure byte bound.");
			auto Validation = ValidateMaterialFunctionSignature(Body->Signature);
			if (!Validation)
			{
				Result.Diagnostics = std::move(Validation.Diagnostics);
				for (auto& Diagnostic : Result.Diagnostics)
				{
					Diagnostic.FunctionAssetPath = Body->AssetPath;
					Diagnostic.CallPath = CallPath;
					Diagnostic.CallPath.push_back(Call.Id);
				}
				return InvalidMaterialExpressionIndex;
			}
			Result.Dependencies.push_back({Body->AssetPath, Body->Revision});
			Shared->ClosureBytes += Bytes;
			Found = Shared->Functions.emplace(Function, std::move(*Body)).first;
		}
		const auto& Body = Found->second;
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.empty() || Call.Outputs.size() > MaterialFunctionMaxOutputs)
			return Fail("Function call port bindings exceed their bounds.");
		std::set<FGuid> InputIds, OutputIds;
		for (const auto& Output : Call.Outputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Outputs, Output.OutputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Outputs.end() || Port->Type != Output.ExpectedType || !OutputIds.insert(Output.OutputId).second)
				return Fail("Function call output binding does not match a unique typed port.", Output.OutputId);
		}
		if (!OutputIds.contains(OutputId)) return Fail("Function call output GUID is not bound.");
		FMaterialExpressionBuildContext Child(*this, Body, Call.Id);
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		for (const auto& Binding : Call.Inputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !InputIds.insert(Binding.InputId).second)
				return Fail("Function call input binding does not match a unique typed port.", Binding.InputId);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Port->Type
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Fail("Retained function binding default has an invalid type or component.", Binding.InputId);
			if (!Binding.Input.ExpressionId.IsValid() && (Binding.Input.OutputIndex != 0 || Binding.Input.OutputId.IsValid()))
				return Fail("Disconnected function binding has an output selector.", Binding.InputId);
			if (Binding.Input.ExpressionId.IsValid() || !Default.empty())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = Binding.Input.ExpressionId.IsValid() ? Resolve(Binding.Input) : FMaterialExpressionBuildValue(Literal(Default));
				PortStack.pop_back();
				if (!MatchesType(Value, Port->Type)) return Fail("Function binding value does not match its declared type.", Binding.InputId);
				Child.BoundInputs.emplace(Binding.InputId, Value);
			}
		}
		std::set<FGuid> TerminalIds;
		std::map<FGuid, FGuid> OutputTerminals;
		for (const auto& [Id, Expression] : Child.Expressions)
		{
			const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression);
			const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression);
			if (!Input && !Output) continue;
			const auto PortId = Input ? Input->PortId : Output->PortId;
			const auto& Ports = Input ? Body.Signature.Inputs : Body.Signature.Outputs;
			if (std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id) == Ports.end() || !TerminalIds.insert(PortId).second)
				return Child.Fail("Function terminal does not match a unique declared port.");
			if (Output) OutputTerminals.emplace(PortId, Id);
		}
		for (const auto& Output : Body.Signature.Outputs)
			if (!OutputTerminals.contains(Output.Id)) return Child.Fail("Function output has no terminal expression.");
		Shared->ActiveFunctions.push_back(Function);
		for (const auto& Input : Body.Signature.Inputs)
			if (Input.bRequired && !Child.BoundInputs.contains(Input.Id)) Child.Fail("Required function input has no binding.", Input.Id);
		// Admit every authored expression, including disconnected nodes and unused calls.
		for (const auto& [Id, Expression] : Child.Expressions)
		{
			if (!Result.Diagnostics.empty()) break;
			FMaterialExpressionInput Probe{Id};
			if (const auto* NestedCall = Cast<DMaterialExpressionFunctionCall>(Expression); NestedCall && !NestedCall->Outputs.empty())
				Probe.OutputId = NestedCall->Outputs.front().OutputId;
			if (const auto* Attributes = Cast<DMaterialExpressionGetSurfaceAttributes>(Expression))
				while (Probe.OutputIndex < 8 && !(Attributes->AttributeMask & (1u << Probe.OutputIndex))) ++Probe.OutputIndex;
			Child.Resolve(Probe);
		}
		std::map<FGuid, FMaterialExpressionBuildValue> Outputs;
		for (const auto& Output : Call.Outputs)
			Outputs.emplace(Output.OutputId, Child.Resolve({OutputTerminals.at(Output.OutputId)}));
		Shared->ActiveFunctions.pop_back();
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		CallOutputs.emplace(Call.Id, std::move(Outputs));
		return CallOutputs.at(Call.Id).at(OutputId);
	}
}
