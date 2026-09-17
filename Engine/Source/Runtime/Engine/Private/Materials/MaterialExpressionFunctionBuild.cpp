#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialFunctionInterface.h"

namespace Durin
{
	auto DMaterialExpressionFunctionInput::Build(FMaterialExpressionBuildContext& Context,
		uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail(EMaterialFunctionError::InputTerminalPrimaryOutput);
		return Context.FunctionInput(Port.Id);
	}

	auto DMaterialExpressionFunctionOutput::Build(FMaterialExpressionBuildContext& Context,
		uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail(EMaterialFunctionError::OutputTerminalPrimaryOutput);
		return Context.FunctionOutput(Port.Id, Source);
	}

	auto DMaterialExpressionFunctionCall::Build(FMaterialExpressionBuildContext& Context,
		uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || !OutputId.IsValid()) return Context.Fail(EMaterialFunctionError::CallConnectionsRequireOutputGUIDZeroOutputIndex);
		return Context.FunctionCall(*this, OutputId);
	}

	auto FMaterialExpressionBuildContext::OpaqueAuthoringValue(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType Type, std::vector<uint32> Inputs) -> uint32
	{
		check(bValidateAuthoring);
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		if (Type == EMaterialProgramValueType::Surface)
		{
			FMaterialIRNode Surface{.Opcode = EMaterialProgramOpcode::MakeSurface, .ResultType = Type};
			for (uint8 Attribute = 0; Attribute < 8; ++Attribute)
				Surface.Inputs.push_back(OpaqueAuthoringValue(Opcode,
					GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Attribute)), Inputs));
			return Emit(std::move(Surface));
		}
		if (Type > EMaterialProgramValueType::Surface || Result.IR.Nodes.size() >= MaterialFunctionMaxExpandedNodes
			|| LinkCount + Inputs.size() > MaterialFunctionMaxExpandedLinks)
			return Fail(EMaterialFunctionError::AuthoringValueInvalidTypeExceedsGraphBounds);
		uint32 Depth = 1;
		for (const auto Input : Inputs)
		{
			if (Input >= Depths.size()) return Fail(EMaterialFunctionError::AuthoringInputInvalid);
			Depth = std::max(Depth, Depths[Input] + 1);
		}
		if (Depth > MaterialProgramMaxDepth) return Fail(EMaterialFunctionError::AuthoringValueExceedsExpressionDepth);
		const auto Index = static_cast<uint32>(Result.IR.Nodes.size());
		LinkCount += static_cast<uint32>(Inputs.size());
		Result.IR.Nodes.push_back({.Opcode = Opcode, .ResultType = Type, .Inputs = std::move(Inputs)});
		Depths.push_back(Depth);
		return Index;
	}

	auto FMaterialExpressionBuildContext::ValidateAuthoringCall(const DMaterialExpressionFunctionCall& Call,
		FGuid OutputId) -> FMaterialExpressionBuildValue
	{
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.empty() || Call.Outputs.size() > MaterialFunctionMaxOutputs)
			return Fail(EMaterialFunctionError::CallPortBindingsExceedBounds);
		const auto Callee = FObjectKey(Call.Function.Get());
		AuthoringCodeHash.UpdateValue(Call.Id);
		AuthoringCodeHash.UpdateValue(Callee.GetHash());
		AuthoringCodeHash.UpdateValue(static_cast<uint32>(Call.Inputs.size()));
		AuthoringCodeHash.UpdateValue(static_cast<uint32>(Call.Outputs.size()));
		std::set<FGuid> InputIds, OutputIds;
		std::vector<uint32> Inputs;
		for (const auto& Binding : Call.Inputs)
		{
			if (!Binding.InputId.IsValid() || !InputIds.insert(Binding.InputId).second
				|| Binding.ExpectedType > EMaterialProgramValueType::Surface)
				return Fail(EMaterialFunctionError::CallInputRequiresUniqueValidTypedPort, Binding.InputId);
			AuthoringCodeHash.UpdateValue(Binding.InputId);
			AuthoringCodeHash.UpdateValue(Binding.ExpectedType);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Binding.ExpectedType
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Fail(EMaterialFunctionError::RetainedFunctionBindingDefaultInvalidTypeComponent, Binding.InputId);
			AuthoringCodeHash.UpdateValue(static_cast<uint32>(Default.size()));
			for (const auto Value : Default) AuthoringCodeHash.UpdateValue(Value);
			if (!Binding.Input.ExpressionId.IsValid() && (Binding.Input.OutputIndex != 0 || Binding.Input.OutputId.IsValid()))
				return Fail(EMaterialFunctionError::DisconnectedFunctionBindingOutputSelector, Binding.InputId);
			if (Binding.Input.ExpressionId.IsValid())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = BroadcastScalar(Resolve(Binding.Input), Binding.ExpectedType);
				PortStack.pop_back();
				if (!MatchesType(Value, Binding.ExpectedType)) return Fail(EMaterialFunctionError::BindingTypeMismatch, Binding.InputId);
				if (Value.GetIndex()) Inputs.push_back(*Value.GetIndex());
			}
		}
		std::map<FGuid, FMaterialExpressionBuildValue> Outputs;
		std::map<EMaterialProgramValueType, uint32> TypeValues;
		for (const auto& Output : Call.Outputs)
		{
			if (!Output.OutputId.IsValid() || InputIds.contains(Output.OutputId) || !OutputIds.insert(Output.OutputId).second
				|| Output.ExpectedType > EMaterialProgramValueType::Surface)
				return Fail(EMaterialFunctionError::CallOutputRequiresUniqueValidTypedPort, Output.OutputId);
			AuthoringCodeHash.UpdateValue(Output.OutputId);
			AuthoringCodeHash.UpdateValue(Output.ExpectedType);
			auto [TypeValue, bInserted] = TypeValues.try_emplace(Output.ExpectedType, InvalidMaterialExpressionIndex);
			if (bInserted) TypeValue->second = OpaqueAuthoringValue(EMaterialProgramOpcode::FunctionCall, Output.ExpectedType, Inputs);
			Outputs.emplace(Output.OutputId, TypeValue->second);
		}
		if (!Outputs.contains(OutputId)) return Fail(EMaterialFunctionError::CallOutputGUIDBound, OutputId);
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		CallOutputs.emplace(Call.Id, std::move(Outputs));
		return CallOutputs.at(Call.Id).at(OutputId);
	}

	auto FMaterialExpressionBuildContext::ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		const auto Signature = DeriveMaterialFunctionSignature(Expressions);
		auto Validation = ValidateMaterialFunctionSignature(Signature);
		if (!Validation) return Validation;
		FMaterialExpressionBuildContext Context(Expressions);
		Context.bValidateAuthoring = true;
		Context.Signature = &Signature;
		for (const auto& [Id, Expression] : Context.Expressions)
			if (Cast<DMaterialExpressionParameter>(Expression) || Cast<DMaterialExpressionMaterialOutput>(Expression))
				Context.Fail(EMaterialFunctionError::UnsupportedMaterialExpression);
		auto Built = Context.Finish({});
		return {.bSucceeded = static_cast<bool>(Built), .Diagnostics = std::move(Built.Diagnostics)};
	}

	auto FMaterialExpressionBuildContext::FunctionInput(FGuid PortId) -> FMaterialExpressionBuildValue
	{
		if (!Signature) return Fail(EMaterialFunctionError::InputTerminalNoOwningInvocation);
		const auto Port = std::ranges::find(Signature->Inputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Inputs.end()) return Fail(EMaterialFunctionError::InputTerminalNoMatchingDeclaration);
		if (bValidateAuthoring) return OpaqueAuthoringValue(EMaterialProgramOpcode::FunctionInput, Port->Type);
		if (const auto Bound = BoundInputs.find(PortId); Bound != BoundInputs.end()) return Bound->second;
		if (Port->bRequired) return Fail(EMaterialFunctionError::RequiredFunctionInputNoBinding);
		if (PortStack.size() >= MaterialFunctionMaxInputs || std::ranges::find(PortStack, PortId) != PortStack.end())
			return Fail(EMaterialFunctionError::InputDefaultsContainCycle);
		PortStack.push_back(PortId);
		FMaterialExpressionBuildValue Value;
		const auto& Default = Port->Default;
		switch (Default.Kind)
		{
		case EMaterialFunctionDefaultKind::Numeric:
		{
			const std::array Components{Default.Numeric.X, Default.Numeric.Y, Default.Numeric.Z, Default.Numeric.W};
			if (Port->Type > EMaterialProgramValueType::Float4) { Fail(EMaterialFunctionError::NumericFunctionDefaultNonNumericType); break; }
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
		default: Fail(EMaterialFunctionError::InputNoUsableBindingDefault); break;
		}
		if (!MatchesType(Value, Port->Type)) Fail(EMaterialFunctionError::DefaultTypeMismatch);
		PortStack.pop_back();
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		BoundInputs.emplace(PortId, Value);
		return Value;
	}

	auto FMaterialExpressionBuildContext::FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source)
		-> FMaterialExpressionBuildValue
	{
		if (!Signature) return Fail(EMaterialFunctionError::OutputTerminalNoOwningInvocation);
		const auto Port = std::ranges::find(Signature->Outputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Outputs.end()) return Fail(EMaterialFunctionError::OutputTerminalNoMatchingDeclaration);
		const auto Value = BroadcastScalar(Resolve(Source), Port->Type);
		if (!MatchesType(Value, Port->Type)) return Fail(EMaterialFunctionError::OutputTypeMismatch);
		return Value;
	}

	auto FMaterialExpressionBuildContext::FunctionCall(const DMaterialExpressionFunctionCall& Call, FGuid OutputId)
		-> FMaterialExpressionBuildValue
	{
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		if (const auto Built = CallOutputs.find(Call.Id); Built != CallOutputs.end())
		{
			const auto Output = Built->second.find(OutputId);
			return Output != Built->second.end() ? Output->second : FMaterialExpressionBuildValue(Fail(EMaterialFunctionError::CallOutputGUIDBound));
		}
		if (bValidateAuthoring) return ValidateAuthoringCall(Call, OutputId);
		const auto* Function = Call.Function.Get();
		if (!IsValid(Function) || !Environment.FindFunction) return Fail(EMaterialFunctionError::CallNoAvailableExpressionBody);
		if (Shared->ActiveFunctions.size() >= MaterialFunctionMaxCallDepth
			|| std::ranges::find(Shared->ActiveFunctions, Function) != Shared->ActiveFunctions.end())
			return Fail(EMaterialFunctionError::BuildContainsRecursionExceedsCallDepthBound);
		auto Found = Shared->Functions.find(Function);
		if (Found == Shared->Functions.end())
		{
			if (Shared->Functions.size() >= MaterialFunctionMaxDependencies) return Fail(EMaterialFunctionError::BuildExceedsDependencyBound);
			auto Body = Environment.FindFunction(*Function);
			if (!Body) return Fail(EMaterialFunctionError::ExpressionBodyUnavailable);
			if (Body->Expressions.size() > MaterialProgramMaxNodeCount || Body->Signature.Inputs.size() > MaterialFunctionMaxInputs
				|| Body->Signature.Outputs.empty() || Body->Signature.Outputs.size() > MaterialFunctionMaxOutputs)
				return Fail(EMaterialFunctionError::ExpressionBodyExceedsNodeSignatureBounds);
			uint64 Bytes = Body->AssetPath.size() + Body->Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body->Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body->Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || Shared->ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Fail(EMaterialFunctionError::ExpressionBodyMetadataExceedsClosureByteBound);
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
			return Fail(EMaterialFunctionError::CallPortBindingsExceedBounds);
		std::set<FGuid> InputIds, OutputIds;
		for (const auto& Output : Call.Outputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Outputs, Output.OutputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Outputs.end() || Port->Type != Output.ExpectedType || !OutputIds.insert(Output.OutputId).second)
				return Fail(EMaterialFunctionError::InvalidOutputBinding, Output.OutputId);
		}
		if (!OutputIds.contains(OutputId)) return Fail(EMaterialFunctionError::CallOutputGUIDBound);
		FMaterialExpressionBuildContext Child(*this, Body, Call.Id);
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		for (const auto& Binding : Call.Inputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !InputIds.insert(Binding.InputId).second)
				return Fail(EMaterialFunctionError::InvalidInputBinding, Binding.InputId);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Port->Type
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Fail(EMaterialFunctionError::RetainedFunctionBindingDefaultInvalidTypeComponent, Binding.InputId);
			if (!Binding.Input.ExpressionId.IsValid() && (Binding.Input.OutputIndex != 0 || Binding.Input.OutputId.IsValid()))
				return Fail(EMaterialFunctionError::DisconnectedFunctionBindingOutputSelector, Binding.InputId);
			if (Binding.Input.ExpressionId.IsValid() || !Default.empty())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = Binding.Input.ExpressionId.IsValid()
					? BroadcastScalar(Resolve(Binding.Input), Port->Type) : FMaterialExpressionBuildValue(Literal(Default));
				PortStack.pop_back();
				if (!MatchesType(Value, Port->Type)) return Fail(EMaterialFunctionError::BindingTypeMismatch, Binding.InputId);
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
			const auto PortId = Input ? Input->Port.Id : Output->Port.Id;
			const auto& Ports = Input ? Body.Signature.Inputs : Body.Signature.Outputs;
			if (std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id) == Ports.end() || !TerminalIds.insert(PortId).second)
				return Child.Fail(EMaterialFunctionError::InvalidTerminalPort);
			if (Output) OutputTerminals.emplace(PortId, Id);
		}
		for (const auto& Output : Body.Signature.Outputs)
			if (!OutputTerminals.contains(Output.Id)) return Child.Fail(EMaterialFunctionError::OutputNoTerminalExpression);
		Shared->ActiveFunctions.push_back(Function);
		for (const auto& Input : Body.Signature.Inputs)
			if (Input.bRequired && !Child.BoundInputs.contains(Input.Id)) Child.Fail(EMaterialFunctionError::RequiredFunctionInputNoBinding, Input.Id);
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
