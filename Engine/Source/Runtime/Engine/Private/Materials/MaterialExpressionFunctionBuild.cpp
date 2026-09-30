#include "MaterialExpressionGraphBuilder.h"
#include "Materials/MaterialFunctionInterface.h"

namespace Durin
{
	auto DMaterialExpressionFunctionInput::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.FunctionInput(Port.Id));
	}

	auto DMaterialExpressionFunctionOutput::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.FunctionOutput(Port.Id, Source));
	}

	auto DMaterialExpressionFunctionCall::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.FunctionCall(*this);
	}

	auto MIR::FGraphBuilderImpl::OpaqueAuthoringValue(EMaterialProgramOpcode Opcode,
		EMaterialProgramValueType Type, std::vector<uint32> Inputs) -> uint32
	{
		check(bValidateAuthoring);
		if (!Result.Diagnostics.empty()) return MIR::InvalidIndex;
		if (Type == EMaterialProgramValueType::Surface)
		{
			MIR::FNode Surface{.Opcode = EMaterialProgramOpcode::MakeSurface, .ResultType = Type};
			for (uint8 Attribute = 0; Attribute < 8; ++Attribute)
			{
				const auto Output = static_cast<EMaterialSurfaceOutput>(Attribute);
				const auto Value = OpaqueAuthoringValue(Opcode,
					GetMaterialSurfaceOutputType(Output), Inputs);
				if (Value != InvalidIndex)
				{
					const auto Semantics = GetMaterialSurfaceOutputSemantics(Output);
					auto& Node = Result.IR.Nodes[Value];
					Node.LegalStages = Semantics.Stages;
					Node.SpatialKind = Semantics.Kind;
					Node.CoordinateSpace = Semantics.Space;
				}
				Surface.Inputs.push_back(Value);
			}
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

	auto MIR::FGraphBuilderImpl::ValidateAuthoringCall(const DMaterialExpressionFunctionCall& Call,
		MIR::FEmitter& Emitter) -> void
	{
#if !DURIN_WITH_EDITORONLY_DATA
		return Emitter.Fail(EMaterialFunctionError::CallNoAvailableExpressionBody);
#else
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.empty() || Call.Outputs.size() > MaterialFunctionMaxOutputs)
			return Emitter.Fail(EMaterialFunctionError::CallPortBindingsExceedBounds);
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
				return Emitter.Fail(EMaterialFunctionError::CallInputRequiresUniqueValidTypedPort, Binding.InputId);
			AuthoringCodeHash.UpdateValue(Binding.InputId);
			AuthoringCodeHash.UpdateValue(Binding.ExpectedType);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Binding.ExpectedType
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Emitter.Fail(EMaterialFunctionError::RetainedFunctionBindingDefaultInvalidTypeComponent, Binding.InputId);
			AuthoringCodeHash.UpdateValue(static_cast<uint32>(Default.size()));
			for (const auto Value : Default) AuthoringCodeHash.UpdateValue(Value);
			if (!Binding.Input.ExpressionId.IsValid() && (Binding.Input.OutputIndex != 0 || Binding.Input.OutputId.IsValid()))
				return Emitter.Fail(EMaterialFunctionError::DisconnectedFunctionBindingOutputSelector, Binding.InputId);
			if (Binding.Input.ExpressionId.IsValid())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = BroadcastScalar(Resolve(Binding.Input), Binding.ExpectedType);
				PortStack.pop_back();
				if (!MatchesType(Value, Binding.ExpectedType)) return Emitter.Fail(EMaterialFunctionError::BindingTypeMismatch, Binding.InputId);
				if (Value.GetIndex()) Inputs.push_back(*Value.GetIndex());
			}
		}
		std::map<std::tuple<EMaterialProgramValueType, EMaterialEvaluationStage,
			EMaterialSpatialKind, EMaterialCoordinateSpace>, uint32> TypeValues;
		for (const auto& Output : Call.Outputs)
		{
			if (!Output.OutputId.IsValid() || InputIds.contains(Output.OutputId) || !OutputIds.insert(Output.OutputId).second
				|| Output.ExpectedType > EMaterialProgramValueType::Surface)
				return Emitter.Fail(EMaterialFunctionError::CallOutputRequiresUniqueValidTypedPort, Output.OutputId);
			AuthoringCodeHash.UpdateValue(Output.OutputId);
			AuthoringCodeHash.UpdateValue(Output.ExpectedType);
			FMaterialFunctionValueConstraint Constraint;
			if (IsValid(Call.Function.Get()))
			{
				const auto& Signature = Call.Function->GetFunctionSignature();
				const auto Port = std::ranges::find(
					Signature.Outputs, Output.OutputId, &FMaterialFunctionPort::Id);
				if (Port != Signature.Outputs.end()) Constraint = Port->Constraint;
			}
			const auto Kind = Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact
				? Constraint.Kind : EMaterialSpatialKind::None;
			const auto Space = Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact
				? Constraint.Space : EMaterialCoordinateSpace::None;
			const auto Key = std::tuple{Output.ExpectedType, Constraint.Stages, Kind, Space};
			auto [Value, bInserted] = TypeValues.try_emplace(Key, InvalidIndex);
			if (bInserted)
			{
				Value->second = OpaqueAuthoringValue(
					EMaterialProgramOpcode::FunctionCall, Output.ExpectedType, Inputs);
				if (Value->second != InvalidIndex && Output.ExpectedType <= EMaterialProgramValueType::Float4)
				{
					auto& Node = Result.IR.Nodes[Value->second];
					Node.LegalStages = Constraint.Stages;
					Node.SpatialKind = Kind;
					Node.CoordinateSpace = Space;
				}
			}
			Emitter.Output(Output.OutputId, Value->second);
		}
		if (!Result.Diagnostics.empty()) return;
#endif
	}

	auto MIR::FGraphBuilderImpl::ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		const auto Signature = DeriveMaterialFunctionSignature(Expressions);
		auto Validation = ValidateMaterialFunctionSignature(Signature);
		if (!Validation) return Validation;
		MIR::FGraphBuilderImpl Context(Expressions);
		Context.bValidateAuthoring = true;
		Context.Signature = &Signature;
		for (const auto& [Id, Expression] : Context.Expressions)
			if (Cast<DMaterialExpressionParameter>(Expression) || Cast<DMaterialExpressionMaterialOutput>(Expression))
				Context.Fail(EMaterialFunctionError::UnsupportedMaterialExpression);
		auto Built = Context.Finish({});
		return {.bSucceeded = static_cast<bool>(Built), .Diagnostics = std::move(Built.Diagnostics)};
	}

	auto MIR::FGraphBuilderImpl::FunctionInput(FGuid PortId) -> MIR::FValue
	{
		if (!Signature) return Fail(EMaterialFunctionError::InputTerminalNoOwningInvocation);
		const auto Port = std::ranges::find(Signature->Inputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Inputs.end()) return Fail(EMaterialFunctionError::InputTerminalNoMatchingDeclaration);
		if (bValidateAuthoring)
		{
			if (Port->Type == EMaterialProgramValueType::Texture2D
				&& Port->Default.Kind == EMaterialFunctionDefaultKind::Texture)
				return MIR::FTextureDefault{Port->Default.Sampler,
					Port->Default.TextureFallback};
			const auto Value = OpaqueAuthoringValue(EMaterialProgramOpcode::FunctionInput, Port->Type);
			if (Value != InvalidIndex && Port->Type <= EMaterialProgramValueType::Float4)
			{
				auto& Node = Result.IR.Nodes[Value];
				Node.LegalStages = Port->Constraint.Stages;
				if (Port->Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact)
				{
					Node.SpatialKind = Port->Constraint.Kind;
					Node.CoordinateSpace = Port->Constraint.Space;
				}
			}
			return Value;
		}
		if (const auto Bound = BoundInputs.find(PortId); Bound != BoundInputs.end())
		{
			if (const auto Index = Bound->second.GetIndex(); Index
				&& !MatchesMaterialFunctionValueConstraint(
					GetNode(*Index).GetSemantics(), Port->Constraint))
				return Fail(EMaterialFunctionError::BindingValueConstraintMismatch, PortId);
			return Bound->second;
		}
		if (Port->bRequired) return Fail(EMaterialFunctionError::RequiredFunctionInputNoBinding);
		if (PortStack.size() >= MaterialFunctionMaxInputs || std::ranges::contains(PortStack, PortId))
			return Fail(EMaterialFunctionError::InputDefaultsContainCycle);
		PortStack.push_back(PortId);
		MIR::FValue Value;
		const auto& Default = Port->Default;
		switch (Default.Kind)
		{
		case EMaterialFunctionDefaultKind::Numeric:
		{
			const std::array Components{Default.Numeric.X, Default.Numeric.Y, Default.Numeric.Z, Default.Numeric.W};
			if (Port->Type > EMaterialProgramValueType::Float4) { Fail(EMaterialFunctionError::NumericFunctionDefaultNonNumericType); break; }
			Value = Literal(std::span(Components).first(static_cast<size_t>(Port->Type) + 1),
				Port->Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact
					? Port->Constraint.Kind : EMaterialSpatialKind::None,
				Port->Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact
					? Port->Constraint.Space : EMaterialCoordinateSpace::None);
			break;
		}
		case EMaterialFunctionDefaultKind::Texture:
			Value = MIR::FTextureDefault{Default.Sampler, Default.TextureFallback};
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
			MIR::FNode Surface{.Opcode = EMaterialProgramOpcode::MakeSurface, .ResultType = EMaterialProgramValueType::Surface};
			for (uint8 Index = 0; Index < 8; ++Index)
			{
				const auto Attribute = static_cast<EMaterialSurfaceOutput>(Index);
				const auto& Numeric = GetMaterialSurfaceOutputDefault(Default.Surface, Attribute);
				const std::array Components{Numeric.X, Numeric.Y, Numeric.Z, Numeric.W};
				Surface.Inputs.push_back(Literal(
					std::span(Components).first(static_cast<size_t>(
						GetMaterialSurfaceOutputType(Attribute)) + 1),
					Attribute == EMaterialSurfaceOutput::Normal
						? EMaterialSpatialKind::Normal : EMaterialSpatialKind::None,
					Attribute == EMaterialSurfaceOutput::Normal
						? EMaterialCoordinateSpace::Tangent
						: EMaterialCoordinateSpace::None));
			}
			Value = Emit(std::move(Surface));
			break;
		}
		default: Fail(EMaterialFunctionError::InputNoUsableBindingDefault); break;
		}
		if (!MatchesType(Value, Port->Type)) Fail(EMaterialFunctionError::DefaultTypeMismatch);
		if (const auto Index = Value.GetIndex(); Index
			&& !MatchesMaterialFunctionValueConstraint(
				GetNode(*Index).GetSemantics(), Port->Constraint))
			Fail(EMaterialFunctionError::DefaultValueConstraintMismatch, PortId);
		PortStack.pop_back();
		if (!Result.Diagnostics.empty()) return MIR::InvalidIndex;
		BoundInputs.emplace(PortId, Value);
		return Value;
	}

	auto MIR::FGraphBuilderImpl::FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source)
		-> MIR::FValue
	{
		if (!Signature) return Fail(EMaterialFunctionError::OutputTerminalNoOwningInvocation);
		const auto Port = std::ranges::find(Signature->Outputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Outputs.end()) return Fail(EMaterialFunctionError::OutputTerminalNoMatchingDeclaration);
		const auto Value = BroadcastScalar(Resolve(Source), Port->Type);
		if (!MatchesType(Value, Port->Type)) return Fail(EMaterialFunctionError::OutputTypeMismatch);
		if (const auto Index = Value.GetIndex(); Index
			&& !MatchesMaterialFunctionValueConstraint(
				GetNode(*Index).GetSemantics(), Port->Constraint))
			return Fail(EMaterialFunctionError::OutputValueConstraintMismatch, PortId);
		return Value;
	}

	auto MIR::FGraphBuilderImpl::FunctionCall(const DMaterialExpressionFunctionCall& Call, MIR::FEmitter& Emitter) -> void
	{
#if !DURIN_WITH_EDITORONLY_DATA
		return Emitter.Fail(EMaterialFunctionError::CallNoAvailableExpressionBody);
#else
		if (!Result.Diagnostics.empty()) return;
		if (bValidateAuthoring) return ValidateAuthoringCall(Call, Emitter);
		const auto* Function = Call.Function.Get();
		if (!IsValid(Function) || !Environment.FindFunction) return Emitter.Fail(EMaterialFunctionError::CallNoAvailableExpressionBody);
		if (Shared->ActiveFunctions.size() >= MaterialFunctionMaxCallDepth
			|| std::ranges::contains(Shared->ActiveFunctions, Function))
			return Emitter.Fail(EMaterialFunctionError::BuildContainsRecursionExceedsCallDepthBound);
		auto Found = Shared->Functions.find(Function);
		if (Found == Shared->Functions.end())
		{
			if (Shared->Functions.size() >= MaterialFunctionMaxDependencies) return Emitter.Fail(EMaterialFunctionError::BuildExceedsDependencyBound);
			auto Body = Environment.FindFunction(*Function);
			if (!Body) return Emitter.Fail(EMaterialFunctionError::ExpressionBodyUnavailable);
			if (Body->Expressions.size() > MaterialProgramMaxNodeCount || Body->Signature.Inputs.size() > MaterialFunctionMaxInputs
				|| Body->Signature.Outputs.empty() || Body->Signature.Outputs.size() > MaterialFunctionMaxOutputs)
				return Emitter.Fail(EMaterialFunctionError::ExpressionBodyExceedsNodeSignatureBounds);
			uint64 Bytes = Body->AssetPath.size() + Body->Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body->Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body->Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || Shared->ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Emitter.Fail(EMaterialFunctionError::ExpressionBodyMetadataExceedsClosureByteBound);
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
				return;
			}
			Result.Dependencies.push_back({Body->AssetPath, Body->Revision});
			Shared->ClosureBytes += Bytes;
			Found = Shared->Functions.emplace(Function, std::move(*Body)).first;
		}
		const auto& Body = Found->second;
		if (Call.Inputs.size() > MaterialFunctionMaxInputs || Call.Outputs.empty() || Call.Outputs.size() > MaterialFunctionMaxOutputs)
			return Emitter.Fail(EMaterialFunctionError::CallPortBindingsExceedBounds);
		std::set<FGuid> InputIds, OutputIds;
		for (const auto& Output : Call.Outputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Outputs, Output.OutputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Outputs.end() || Port->Type != Output.ExpectedType || !OutputIds.insert(Output.OutputId).second)
				return Emitter.Fail(EMaterialFunctionError::InvalidOutputBinding, Output.OutputId);
		}
		MIR::FGraphBuilderImpl Child(*this, Body, Call.Id);
		if (!Result.Diagnostics.empty()) return;
		for (const auto& Binding : Call.Inputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !InputIds.insert(Binding.InputId).second)
				return Emitter.Fail(EMaterialFunctionError::InvalidInputBinding, Binding.InputId);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Port->Type
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Emitter.Fail(EMaterialFunctionError::RetainedFunctionBindingDefaultInvalidTypeComponent, Binding.InputId);
			if (!Binding.Input.ExpressionId.IsValid() && (Binding.Input.OutputIndex != 0 || Binding.Input.OutputId.IsValid()))
				return Emitter.Fail(EMaterialFunctionError::DisconnectedFunctionBindingOutputSelector, Binding.InputId);
			if (Binding.Input.ExpressionId.IsValid() || !Default.empty())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = Binding.Input.ExpressionId.IsValid()
					? BroadcastScalar(Resolve(Binding.Input), Port->Type) : MIR::FValue(Literal(Default));
				PortStack.pop_back();
				if (!MatchesType(Value, Port->Type)) return Emitter.Fail(EMaterialFunctionError::BindingTypeMismatch, Binding.InputId);
				if (const auto Index = Value.GetIndex(); Index
					&& !MatchesMaterialFunctionValueConstraint(
						GetNode(*Index).GetSemantics(), Port->Constraint))
					return Emitter.Fail(EMaterialFunctionError::BindingValueConstraintMismatch, Binding.InputId);
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
			if (!std::ranges::contains(Ports, PortId, &FMaterialFunctionPort::Id) || !TerminalIds.insert(PortId).second)
				{ Child.Fail(EMaterialFunctionError::InvalidTerminalPort); return; }
			if (Output) OutputTerminals.emplace(PortId, Id);
		}
		for (const auto& Output : Body.Signature.Outputs)
			if (!OutputTerminals.contains(Output.Id)) { Child.Fail(EMaterialFunctionError::OutputNoTerminalExpression); return; }
		Shared->ActiveFunctions.push_back(Function);
		for (const auto& Input : Body.Signature.Inputs)
			if (Input.bRequired && !Child.BoundInputs.contains(Input.Id)) Child.Fail(EMaterialFunctionError::RequiredFunctionInputNoBinding, Input.Id);
		// Each invocation owns an independent node/output cache.
		Child.BuildAllExpressions();
		for (const auto& Output : Call.Outputs)
			Emitter.Output(Output.OutputId, Child.Resolve({OutputTerminals.at(Output.OutputId)}));
		Shared->ActiveFunctions.pop_back();
		if (!Result.Diagnostics.empty()) return;
#endif
	}
}
