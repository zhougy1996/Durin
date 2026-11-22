#include "MaterialExpressionGraphBuilder.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionFunctionCall.h"
#include "Materials/MaterialExpressionFunctionIO.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionParameter.h"
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
		if (Type == EMaterialProgramValueType::MaterialAttributes)
		{
			// These are validation-only opaque values. Equal semantic shapes can
			// share a placeholder without spending expanded graph nodes per field.
			std::map<std::tuple<EMaterialProgramValueType, EMaterialEvaluationStage,
				EMaterialSpatialKind, EMaterialCoordinateSpace>, uint32> Shapes;
			return Aggregate(InvalidIndex, [&](EMaterialProperty Output) -> std::optional<uint32> {
				const auto Semantics = GetMaterialSurfaceOutputSemantics(Output);
				const auto Key = std::tuple{Semantics.Type, Semantics.Stages, Semantics.Kind, Semantics.Space};
				auto [Shape, Inserted] = Shapes.try_emplace(Key, InvalidIndex);
				if (Inserted) Shape->second = OpaqueAuthoringValue(Opcode, Semantics.Type, Inputs);
				const auto Value = Shape->second;
				if (Value != InvalidIndex)
				{
					auto& Node = Result.IR.Nodes[Value];
					Node.LegalStages = Semantics.Stages;
					Node.SpatialKind = Semantics.Kind;
					Node.CoordinateSpace = Semantics.Space;
				}
				return Value;
			});
		}
		if (Type > EMaterialProgramValueType::MaterialAttributes || Result.IR.Nodes.size() >= MaterialFunctionMaxExpandedNodes
			|| LinkCount + Inputs.size() > MaterialFunctionMaxExpandedLinks)
			return Fail("Function authoring value has an invalid type or exceeds graph bounds.");
		uint32 Depth = 1;
		for (const auto Input : Inputs)
		{
			if (Input >= Depths.size()) return Fail("Function authoring input is invalid.");
			Depth = std::max(Depth, Depths[Input] + 1);
		}
		if (Depth > MaterialProgramMaxDepth) return Fail("Function authoring value exceeds expression depth.");
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
		return Emitter.Fail("Function call has no available expression body.");
#else
		if (!Call.Function && Call.FunctionInputs.empty() && Call.FunctionOutputs.empty()) return;
		if (Call.FunctionInputs.size() > MaterialFunctionMaxInputs || Call.FunctionOutputs.empty() || Call.FunctionOutputs.size() > MaterialFunctionMaxOutputs)
			return Emitter.Fail("Function call port bindings exceed their bounds.");
		const auto Callee = FObjectKey(Call.Function.Get());
		AuthoringCodeHash.UpdateValue(Call.Id);
		AuthoringCodeHash.UpdateValue(Callee.GetHash());
		AuthoringCodeHash.UpdateValue(static_cast<uint32>(Call.FunctionInputs.size()));
		AuthoringCodeHash.UpdateValue(static_cast<uint32>(Call.FunctionOutputs.size()));
		std::set<FGuid> InputIds, OutputIds;
		std::vector<uint32> Inputs;
		for (const auto& Binding : Call.FunctionInputs)
		{
			if (!Binding.InputId.IsValid() || !InputIds.insert(Binding.InputId).second
				|| Binding.ExpectedType > EMaterialProgramValueType::MaterialAttributes)
				return Emitter.Fail("Function call input requires a unique valid typed port.", Binding.InputId);
			AuthoringCodeHash.UpdateValue(Binding.InputId);
			AuthoringCodeHash.UpdateValue(Binding.ExpectedType);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Binding.ExpectedType
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Emitter.Fail("Retained function binding default has an invalid type or component.", Binding.InputId);
			AuthoringCodeHash.UpdateValue(static_cast<uint32>(Default.size()));
			for (const auto Value : Default) AuthoringCodeHash.UpdateValue(Value);
			if (!Binding.Input.IsConnected() && Binding.Input.OutputIndex != 0)
				return Emitter.Fail("Disconnected function binding has an output selector.", Binding.InputId);
			if (Binding.Input.IsConnected())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = TryValidCast(Resolve(Binding.Input), Binding.ExpectedType);
				PortStack.pop_back();
				if (!MatchesType(Value, Binding.ExpectedType)) return Emitter.Fail("Function binding value does not match its declared type.", Binding.InputId);
				if (Value.GetIndex()) Inputs.push_back(*Value.GetIndex());
			}
		}
		std::map<std::tuple<EMaterialProgramValueType, EMaterialEvaluationStage,
			EMaterialSpatialKind, EMaterialCoordinateSpace>, uint32> TypeValues;
		for (size_t Index = 0; Index < Call.FunctionOutputs.size(); ++Index)
		{
			const auto& Output = Call.FunctionOutputs[Index];
			if (!Output.OutputId.IsValid() || InputIds.contains(Output.OutputId) || !OutputIds.insert(Output.OutputId).second
				|| Output.ExpectedType > EMaterialProgramValueType::MaterialAttributes)
				return Emitter.Fail("Function call output requires a unique valid typed port.", Output.OutputId);
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
			Emitter.Output(static_cast<int32>(Index), Value->second);
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
			if (Cast<DMaterialExpressionParameter>(Expression))
				Context.Fail("Functions cannot declare material parameters.");
		auto Built = Context.Finish({});
		return {.bSucceeded = static_cast<bool>(Built), .Diagnostics = std::move(Built.Diagnostics)};
	}

	auto MIR::FGraphBuilderImpl::FunctionInput(FGuid PortId) -> MIR::FValue
	{
		if (!Signature) return Fail("Function input terminal has no owning invocation.");
		const auto Port = std::ranges::find(Signature->Inputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Inputs.end()) return Fail("Function input terminal has no matching declaration.");
		if (bValidateAuthoring)
		{
			if ((Port->Type == EMaterialProgramValueType::Texture2D || Port->Type == EMaterialProgramValueType::TextureCube)
				&& Port->Default.Kind == EMaterialFunctionDefaultKind::Texture)
				return MIR::FTextureDefault{Port->Default.Sampler,
					Port->Default.TextureFallback, Port->Type == EMaterialProgramValueType::TextureCube ? EMaterialTextureDimension::TextureCube : EMaterialTextureDimension::Texture2D};
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
				return Fail("Function binding does not satisfy its stage or spatial constraint.", PortId);
			return Bound->second;
		}
		if (Port->bRequired) return Fail("Required function input has no binding.");
		if (PortStack.size() >= MaterialFunctionMaxInputs || std::ranges::contains(PortStack, PortId))
			return Fail("Function input defaults contain a cycle.");
		PortStack.push_back(PortId);
		MIR::FValue Value;
		const auto& Default = Port->Default;
		switch (Default.Kind)
		{
		case EMaterialFunctionDefaultKind::Numeric:
		{
			const std::array Components{Default.Numeric.X, Default.Numeric.Y, Default.Numeric.Z, Default.Numeric.W};
			if (Port->Type > EMaterialProgramValueType::Float4) { Fail("Numeric function default has a non-numeric type."); break; }
			Value = Literal(std::span(Components).first(static_cast<size_t>(Port->Type) + 1),
				Port->Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact
					? Port->Constraint.Kind : EMaterialSpatialKind::None,
				Port->Constraint.Mode == EMaterialFunctionValueConstraintMode::Exact
					? Port->Constraint.Space : EMaterialCoordinateSpace::None);
			break;
		}
		case EMaterialFunctionDefaultKind::Texture:
			Value = MIR::FTextureDefault{Default.Sampler, Default.TextureFallback, Port->Type == EMaterialProgramValueType::TextureCube ? EMaterialTextureDimension::TextureCube : EMaterialTextureDimension::Texture2D};
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
		case EMaterialFunctionDefaultKind::MaterialAttributes:
		{
			Value = Aggregate(InvalidIndex, [&](EMaterialProperty Property) -> std::optional<uint32> {
				return AttributeLiteral(Property, GetMaterialSurfaceOutputDefault(Default.Surface, Property));
			});
			break;
		}
		default: Fail("Function input has no usable binding or default."); break;
		}
		if (!MatchesType(Value, Port->Type)) Fail("Function default type does not match its input declaration.");
		if (const auto Index = Value.GetIndex(); Index
			&& !MatchesMaterialFunctionValueConstraint(
				GetNode(*Index).GetSemantics(), Port->Constraint))
			Fail("Function default does not satisfy its stage or spatial constraint.", PortId);
		PortStack.pop_back();
		if (!Result.Diagnostics.empty()) return MIR::InvalidIndex;
		BoundInputs.emplace(PortId, Value);
		return Value;
	}

	auto MIR::FGraphBuilderImpl::FunctionOutput(FGuid PortId, const FMaterialExpressionInput& Source)
		-> MIR::FValue
	{
		if (!Signature) return Fail("Function output terminal has no owning invocation.");
		const auto Port = std::ranges::find(Signature->Outputs, PortId, &FMaterialFunctionPort::Id);
		if (Port == Signature->Outputs.end()) return Fail("Function output terminal has no matching declaration.");
		const auto Value = TryValidCast(Resolve(Source), Port->Type);
		if (!MatchesType(Value, Port->Type)) return Fail("Function output source does not match its declared type.");
		if (const auto Index = Value.GetIndex(); Index
			&& !MatchesMaterialFunctionValueConstraint(
				GetNode(*Index).GetSemantics(), Port->Constraint))
			return Fail("Function output does not satisfy its stage or spatial constraint.", PortId);
		return Value;
	}

	auto MIR::FGraphBuilderImpl::FunctionCall(const DMaterialExpressionFunctionCall& Call, MIR::FEmitter& Emitter) -> void
	{
#if !DURIN_WITH_EDITORONLY_DATA
		return Emitter.Fail("Function call has no available expression body.");
#else
		if (!Result.Diagnostics.empty()) return;
		if (bValidateAuthoring) return ValidateAuthoringCall(Call, Emitter);
		const auto* Function = Call.Function.Get();
		if (!IsValid(Function) || !Environment.FindFunction) return Emitter.Fail("Function call has no available expression body.");
		if (Shared->ActiveFunctions.size() >= MaterialFunctionMaxCallDepth
			|| std::ranges::contains(Shared->ActiveFunctions, Function))
			return Emitter.Fail("Function Build contains recursion or exceeds the call depth bound.");
		auto Found = Shared->Functions.find(Function);
		if (Found == Shared->Functions.end())
		{
			if (Shared->Functions.size() >= MaterialFunctionMaxDependencies) return Emitter.Fail("Function Build exceeds the dependency bound.");
			auto Body = Environment.FindFunction(*Function);
			if (!Body) return Emitter.Fail("Function expression body is unavailable.");
			if (Body->Expressions.size() > MaterialProgramMaxNodeCount || Body->Signature.Inputs.size() > MaterialFunctionMaxInputs
				|| Body->Signature.Outputs.empty() || Body->Signature.Outputs.size() > MaterialFunctionMaxOutputs)
				return Emitter.Fail("Function expression body exceeds node or signature bounds.");
			uint64 Bytes = Body->AssetPath.size() + Body->Expressions.size() * sizeof(DMaterialExpression*);
			for (const auto& Port : Body->Signature.Inputs) Bytes += sizeof(Port) + Port.Name.size();
			for (const auto& Port : Body->Signature.Outputs) Bytes += sizeof(Port) + Port.Name.size();
			if (Bytes > MaterialFunctionMaxClosureBytes || Shared->ClosureBytes > MaterialFunctionMaxClosureBytes - Bytes)
				return Emitter.Fail("Function expression body metadata exceeds the closure byte bound.");
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
		if (Call.FunctionInputs.size() > MaterialFunctionMaxInputs || Call.FunctionOutputs.empty() || Call.FunctionOutputs.size() > MaterialFunctionMaxOutputs)
			return Emitter.Fail("Function call port bindings exceed their bounds.");
		std::set<FGuid> InputIds, OutputIds;
		for (const auto& Output : Call.FunctionOutputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Outputs, Output.OutputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Outputs.end() || Port->Type != Output.ExpectedType || !OutputIds.insert(Output.OutputId).second)
				return Emitter.Fail("Function call output binding does not match a unique typed port.", Output.OutputId);
		}
		MIR::FGraphBuilderImpl Child(*this, Body, Call.Id);
		if (!Result.Diagnostics.empty()) return;
		for (const auto& Binding : Call.FunctionInputs)
		{
			const auto Port = std::ranges::find(Body.Signature.Inputs, Binding.InputId, &FMaterialFunctionPort::Id);
			if (Port == Body.Signature.Inputs.end() || Port->Type != Binding.ExpectedType || !InputIds.insert(Binding.InputId).second)
				return Emitter.Fail("Function call input binding does not match a unique typed port.", Binding.InputId);
			const auto& Default = Binding.InputDefault;
			if (!Default.empty() && (Default.size() > 4 || static_cast<EMaterialProgramValueType>(Default.size() - 1) != Port->Type
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); })))
				return Emitter.Fail("Retained function binding default has an invalid type or component.", Binding.InputId);
			if (!Binding.Input.IsConnected() && Binding.Input.OutputIndex != 0)
				return Emitter.Fail("Disconnected function binding has an output selector.", Binding.InputId);
			if (Binding.Input.IsConnected() || !Default.empty())
			{
				PortStack.push_back(Binding.InputId);
				const auto Value = Binding.Input.IsConnected()
					? TryValidCast(Resolve(Binding.Input), Port->Type) : MIR::FValue(Literal(Default));
				PortStack.pop_back();
				if (!MatchesType(Value, Port->Type)) return Emitter.Fail("Function binding value does not match its declared type.", Binding.InputId);
				if (const auto Index = Value.GetIndex(); Index
					&& !MatchesMaterialFunctionValueConstraint(
						GetNode(*Index).GetSemantics(), Port->Constraint))
					return Emitter.Fail("Function binding does not satisfy its stage or spatial constraint.", Binding.InputId);
				Child.BoundInputs.emplace(Binding.InputId, Value);
			}
		}
		std::set<FGuid> TerminalIds;
		std::map<FGuid, DMaterialExpression*> OutputTerminals;
		for (const auto& [Id, Expression] : Child.Expressions)
		{
			const auto* Input = Cast<DMaterialExpressionFunctionInput>(Expression);
			const auto* Output = Cast<DMaterialExpressionFunctionOutput>(Expression);
			if (!Input && !Output) continue;
			const auto PortId = Input ? Input->Port.Id : Output->Port.Id;
			const auto& Ports = Input ? Body.Signature.Inputs : Body.Signature.Outputs;
			if (!std::ranges::contains(Ports, PortId, &FMaterialFunctionPort::Id) || !TerminalIds.insert(PortId).second)
				{ Child.Fail("Function terminal does not match a unique declared port."); return; }
			if (Output) OutputTerminals.emplace(PortId, Expression);
		}
		for (const auto& Output : Body.Signature.Outputs)
			if (!OutputTerminals.contains(Output.Id)) { Child.Fail("Function output has no terminal expression."); return; }
		Shared->ActiveFunctions.push_back(Function);
		for (const auto& Input : Body.Signature.Inputs)
			if (Input.bRequired && !Child.BoundInputs.contains(Input.Id)) Child.Fail("Required function input has no binding.", Input.Id);
		// Each invocation owns an independent node/output cache.
		if (!bCompileReachableOnly) Child.BuildAllExpressions();
		for (size_t Index = 0; Index < Call.FunctionOutputs.size(); ++Index)
			Emitter.Output(static_cast<int32>(Index), Child.Resolve({OutputTerminals.at(Call.FunctionOutputs[Index].OutputId)}));
		Shared->ActiveFunctions.pop_back();
		if (!Result.Diagnostics.empty()) return;
#endif
	}
}
