#include "MaterialExpressionGraphBuilder.h"
#include <unordered_set>

#include "Threading/RunnableThread.h"
#include "Materials/MaterialFunctionInterface.h"

namespace Durin::MIR
{
	FGraphBuilder::FGraphBuilder(std::span<DMaterialExpression* const> Expressions,
		FBuildEnvironment Environment)
		: Impl(std::make_unique<FGraphBuilderImpl>(Expressions, std::move(Environment)))
	{
	}

	FGraphBuilder::~FGraphBuilder() = default;

	auto FGraphBuilder::Finish(std::span<const FMaterialExpressionInput> Roots) -> FBuildResult
	{
		return Impl->Finish(Roots);
	}

	auto FGraphBuilder::FinishSurface(const FMaterialExpressionSurfaceOutputs& Outputs) -> FBuildResult
	{
		return Impl->FinishSurface(Outputs);
	}

	auto FGraphBuilder::ValidateSurface(std::span<DMaterialExpression* const> Expressions,
		const FMaterialExpressionSurfaceOutputs& Outputs, FXxHash128* OutCodeFingerprint) -> FMaterialProgramValidationResult
	{
		return FGraphBuilderImpl::ValidateSurface(Expressions, Outputs, OutCodeFingerprint);
	}

	auto FGraphBuilder::ValidateFunction(std::span<DMaterialExpression* const> Expressions) -> FMaterialProgramValidationResult
	{
		return FGraphBuilderImpl::ValidateFunction(Expressions);
	}

	FGraphBuilderImpl::FGraphBuilderImpl(std::span<DMaterialExpression* const> InExpressions,
		FBuildEnvironment InEnvironment)
		: Shared(std::make_shared<FSharedState>()), Result(Shared->Result), Depths(Shared->Depths),
		  LinkCount(Shared->LinkCount), Environment(std::move(InEnvironment))
	{
		Admit(InExpressions);
		for (uint32 Index = 0; Index < Result.IR.SurfaceRoot.Inputs.size(); ++Index)
			Result.IR.SurfaceRoot.Inputs[Index].Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index));
	}

	FGraphBuilderImpl::FGraphBuilderImpl(FGraphBuilderImpl& Parent,
		const FFunctionBody& Body, FGuid CallId)
		: Shared(Parent.Shared), Result(Shared->Result), Depths(Shared->Depths), LinkCount(Shared->LinkCount),
		  Environment(Parent.Environment), Signature(&Body.Signature), FunctionPath(Body.AssetPath), CallPath(Parent.CallPath)
	{
		CallPath.push_back(CallId);
		Admit(Body.Expressions);
	}

	auto FGraphBuilderImpl::Admit(std::span<DMaterialExpression* const> InExpressions) -> void
	{
		check(IsInGameThread());
		if (InExpressions.size() > MaterialProgramMaxNodeCount)
		{
			Fail(EMaterialExpressionError::CollectionExceedsAuthoredNodeBound);
			return;
		}
		std::map<FGuid, FMaterialParameterDefinition> ParameterOwners;
		std::unordered_set<FName> ParameterNames;
		for (auto* Expression : InExpressions)
		{
			if (!IsValid(Expression) || !Expression->Id.IsValid()
				|| !Expressions.emplace(Expression->Id, Expression).second)
			{
				Fail(EMaterialExpressionError::CollectionContainsNullOwnerInvalidGUIDDuplicateGUID);
				return;
			}
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression);
				Parameter && (Signature || !Parameter->Metadata.Id.IsValid()))
			{
				Fail(EMaterialExpressionError::ParameterExpressionsRequireValidParameterGUIDsMaterialOwner);
				return;
			}
			if (Signature && Cast<DMaterialExpressionMaterialOutput>(Expression))
			{
				Fail(EMaterialFunctionError::UnsupportedMaterialExpression);
				return;
			}
			if (!Cast<DMaterialExpressionMaterialOutput>(Expression)) AuthoredLinks += Expression->GetAuthoredInputCount();
			if (AuthoredLinks > MaterialProgramMaxLinkCount)
			{
				Fail(EMaterialExpressionError::CollectionExceedsAuthoredInputLinkBound);
				return;
			}
			if (const auto* Parameter = Cast<DMaterialExpressionParameter>(Expression))
			{
				const auto Definition = Parameter->GetParameterDefinition();
				if (Definition.Type == EMaterialParameterType::Texture)
					Shared->TextureUsages[Definition.Id] = Definition.TextureUsage;
				const auto [Owner, bInserted] = ParameterOwners.emplace(Definition.Id, Definition);
				if ((!bInserted && Owner->second != Definition)
					|| (bInserted && !ParameterNames.insert(Definition.Name).second)
					|| !ValidateMaterialParameterDefinitions(std::span(&Definition, 1)))
				{
					Fail(EMaterialExpressionError::ParameterExpressionMetadataDefaultInvalid);
					return;
				}
			}
		}
		if (ParameterOwners.size() > MaterialMaxParameterDefinitionCount)
		{
			Fail(EMaterialExpressionError::CollectionExceedsParameterDeclarationBound);
			return;
		}
		for (const auto& [Id, Definition] : ParameterOwners)
		{
			if (Definition.Type == EMaterialParameterType::Texture)
			{
				// Usage affects RGB sampling even through opaque authoring function calls.
				AuthoringCodeHash.UpdateValue(Id);
				AuthoringCodeHash.UpdateValue(Definition.TextureUsage);
			}
			if (Expressions.contains(Id))
			{
				Fail(EMaterialExpressionError::ParameterIdentityDistinctExpressionIdentity);
				return;
			}
		}
	}

	auto FGraphBuilderImpl::Fail(FMaterialError Error, FGuid PortId,
		EMaterialProgramDiagnosticCategory Category) -> uint32
	{
		if (Result.Diagnostics.empty())
		{
			Result.Diagnostics.push_back({.Category = Category,
				.LocationKind = EMaterialProgramDiagnosticLocationKind::Node,
				.NodeId = SourceStack.empty() ? FGuid{} : SourceStack.back(), .Error = std::move(Error),
				.PortId = PortId.IsValid() ? PortId : PortStack.empty() ? FGuid{} : PortStack.back(), .FunctionAssetPath = FunctionPath, .CallPath = CallPath});
		}
		return InvalidIndex;
	}

	auto FGraphBuilderImpl::ReportMissingOutput(const DMaterialExpression& Expression,
		const FMaterialExpressionInput& Input) -> void
	{
		if (Cast<DMaterialExpressionMaterialOutput>(&Expression))
			Fail(EMaterialExpressionError::MaterialOutputSinkUsedAsExpressionSource);
		else if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(&Expression))
		{
			if (Input.OutputIndex != 0 || !Input.OutputId.IsValid())
				Fail(EMaterialFunctionError::CallConnectionsRequireOutputGUIDZeroOutputIndex);
			else if (std::ranges::find(Call->Outputs, Input.OutputId, &FMaterialFunctionOutputBinding::OutputId) == Call->Outputs.end())
				Fail(EMaterialFunctionError::CallOutputGUIDBound, Input.OutputId);
		}
		else if (Cast<DMaterialExpressionTextureSample2D>(&Expression) || Cast<DMaterialExpressionTextureSampleParameter2D>(&Expression))
		{
			const auto Opcode = Cast<DMaterialExpressionTextureSampleParameter2D>(&Expression)
				? EMaterialProgramOpcode::TextureSampleParameter2D : EMaterialProgramOpcode::TextureSample2D;
			if (Input.OutputId.IsValid()) Fail(EMaterialExpressionError::SampleOutputRequiresIndexOutputGUID);
			else if (!FindMaterialSampleOutput(Opcode, Input.OutputIndex)) Fail(EMaterialExpressionError::SampleExpressionOutputSelectorInvalid);
		}
		else if (const auto* Attributes = Cast<DMaterialExpressionGetSurfaceAttributes>(&Expression))
		{
			if (Input.OutputId.IsValid() || Input.OutputIndex >= 8 || !(Attributes->AttributeMask & (1u << Input.OutputIndex)))
				Fail(EMaterialExpressionError::SurfaceAttributeOutputSelected);
		}
		else if (Input.OutputIndex != 0 || Input.OutputId.IsValid())
		{
			if (Cast<DMaterialExpressionFunctionInput>(&Expression)) Fail(EMaterialFunctionError::InputTerminalPrimaryOutput);
			else if (Cast<DMaterialExpressionFunctionOutput>(&Expression)) Fail(EMaterialFunctionError::OutputTerminalPrimaryOutput);
			else Fail(EMaterialExpressionError::PrimaryOutput);
		}
		// Keep existing selector diagnostics for known nodes. Registered outputs, including
		// those of new expression classes, are accepted without a central node-type schema.
		if (Result.Diagnostics.empty()) Fail(EMaterialExpressionError::BuildOutputNotRegistered, Input.OutputId);
	}

	auto FGraphBuilderImpl::BuildExpression(const DMaterialExpression& Expression) -> void
	{
		if (!Result.Diagnostics.empty() || Built.contains(Expression.Id)) return;
		if (Active.size() >= MaterialProgramMaxDepth || !Active.insert(Expression.Id).second)
		{
			Fail(EMaterialExpressionError::InputsContainCycleExceedTraversalDepthBound);
			return;
		}
		SourceStack.push_back(Expression.Id);
		// Each recursive invocation has its own emitter; upstream builds cannot change its output owner.
		FEmitter Emitter(*this, Expression.Id);
		Expression.Build(Emitter);
		SourceStack.pop_back();
		Active.erase(Expression.Id);
		if (Result.Diagnostics.empty()) Built.insert(Expression.Id);
	}

	auto FGraphBuilderImpl::Resolve(const FMaterialExpressionInput& Input) -> FValue
	{
		check(IsInGameThread());
		if (!Result.Diagnostics.empty()) return InvalidIndex;
		if (bValidateAuthoring)
		{
			AuthoringCodeHash.UpdateValue(Input.ExpressionId);
			AuthoringCodeHash.UpdateValue(Input.OutputIndex);
			AuthoringCodeHash.UpdateValue(Input.OutputId);
		}
		const auto Found = Expressions.find(Input.ExpressionId);
		if (Found == Expressions.end()) return Fail(EMaterialExpressionError::InputDisconnectedRefersMissingExpression);
		// Do not expose a partially registered output while its expression is still building.
		BuildExpression(*Found->second);
		if (!Result.Diagnostics.empty()) return InvalidIndex;
		const FOutputKey Key{Input.ExpressionId, Input.OutputIndex, Input.OutputId};
		if (const auto Value = Values.find(Key); Value != Values.end()) return Value->second;
		SourceStack.push_back(Input.ExpressionId);
		ReportMissingOutput(*Found->second, Input);
		SourceStack.pop_back();
		return InvalidIndex;
	}

	auto FGraphBuilderImpl::ResolveIndex(const FMaterialExpressionInput& Input) -> uint32
	{
		const auto Value = Resolve(Input);
		return Value.GetIndex() ? *Value.GetIndex() : Fail(EMaterialExpressionError::InvalidTextureDefaultConsumer);
	}

	auto FGraphBuilderImpl::MatchesType(const FValue& Value, EMaterialProgramValueType Type) const -> bool
	{
		if (Value.GetTexture()) return Type == EMaterialProgramValueType::Texture2D;
		return *Value.GetIndex() < Result.IR.Nodes.size() && Result.IR.Nodes[*Value.GetIndex()].ResultType == Type;
	}

	auto FGraphBuilderImpl::BroadcastScalar(FValue Value,
		EMaterialProgramValueType Type) -> FValue
	{
		if (Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4
			&& MatchesType(Value, EMaterialProgramValueType::Float))
			return Emit({.Opcode = static_cast<EMaterialProgramOpcode>(static_cast<uint8>(EMaterialProgramOpcode::Splat2)
				+ static_cast<uint8>(Type) - 1), .ResultType = Type, .Inputs = {*Value.GetIndex()}});
		return Value;
	}

	auto FGraphBuilderImpl::Emit(FNode Node) -> uint32
	{
		if (!Result.Diagnostics.empty()) return InvalidIndex;
		const auto Signature = GetMaterialProgramNodeSignature(Node.Opcode, Node.ResultType);
		if (!Node.HasValidPayload() || !Signature || Node.Inputs.size() != Signature->InputCount)
			return Fail(EMaterialExpressionError::OpcodeResultWidthInputCountInvalid);
		// Adapt authored fixed-width inputs before admitting strictly typed IR.
		// Normalize keeps its requirement for a vector source.
		for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
			if (Signature->Inputs[Slot].size() == 1 && Node.Opcode != EMaterialProgramOpcode::Normalize)
				Node.Inputs[Slot] = *BroadcastScalar(Node.Inputs[Slot], Signature->Inputs[Slot].front()).GetIndex();
		if (!Result.Diagnostics.empty()) return InvalidIndex;
		if (Result.IR.Nodes.size() >= MaterialFunctionMaxExpandedNodes
			|| LinkCount + Node.Inputs.size() > MaterialFunctionMaxExpandedLinks)
			return Fail(EMaterialExpressionError::BuildExceedsExpandedIRNodeLinkBound, {}, EMaterialProgramDiagnosticCategory::Bounds);
		uint32 Depth = 1;
		for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
		{
			const auto Input = Node.Inputs[Slot];
			const auto Accepted = Signature->Inputs[Slot];
			if (Input >= Result.IR.Nodes.size()
				|| std::ranges::find(Accepted, Result.IR.Nodes[Input].ResultType) == Accepted.end())
				return Fail(EMaterialExpressionError::InputIncompatibleType);
			Depth = std::max(Depth, Depths[Input] + 1);
		}
		if (Depth > MaterialProgramMaxDepth) return Fail(EMaterialExpressionError::BuildExceedsIRDepthBound);
		if (Node.Opcode == EMaterialProgramOpcode::Constant)
		{
			const std::array Values{Node.GetLiteral().X, Node.GetLiteral().Y, Node.GetLiteral().Z, Node.GetLiteral().W};
			for (uint32 Index = 0; Index <= static_cast<uint32>(Node.ResultType); ++Index)
				if (!std::isfinite(Values[Index])) return Fail(EMaterialExpressionError::NonFiniteConstant);
		}
		if (Node.Opcode == EMaterialProgramOpcode::Swizzle)
		{
			if (Node.GetSwizzle().Length != static_cast<uint32>(Node.ResultType) + 1)
				return Fail(EMaterialExpressionError::SwizzleWidthMismatch);
			const std::array Mask{Node.GetSwizzle().Components[0], Node.GetSwizzle().Components[1], Node.GetSwizzle().Components[2], Node.GetSwizzle().Components[3]};
			for (uint8 Index = 0; Index < Node.GetSwizzle().Length; ++Index)
				if (Mask[Index] > static_cast<uint32>(Result.IR.Nodes[Node.Inputs[0]].ResultType))
					return Fail(EMaterialExpressionError::SwizzleSelectionExceedsSourceWidth);
		}
		const auto Index = static_cast<uint32>(Result.IR.Nodes.size());
		LinkCount += static_cast<uint32>(Node.Inputs.size());
		Result.IR.Nodes.push_back(std::move(Node));
		Depths.push_back(Depth);
		Result.Sources.push_back({.ExpressionIndex = Index, .NodeId = SourceStack.empty() ? FGuid{} : SourceStack.back(),
			.PortId = PortStack.empty() ? FGuid{} : PortStack.back(), .FunctionAssetPath = FunctionPath, .CallPath = CallPath});
		return Index;
	}

	auto FGraphBuilderImpl::Literal(std::span<const float> Components) -> uint32
	{
		if (Components.empty() || Components.size() > 4) return Fail(EMaterialExpressionError::NumericInputRequiresDefaultOneFourComponents);
		FNode Node{.Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = static_cast<EMaterialProgramValueType>(Components.size() - 1)};
		FMaterialProgramLiteral Literal;
		const std::array Targets{&Literal.X, &Literal.Y, &Literal.Z, &Literal.W};
		for (size_t Index = 0; Index < Components.size(); ++Index) *Targets[Index] = Components[Index];
		Node.Payload = Literal;
		return Emit(std::move(Node));
	}

	auto FGraphBuilderImpl::Parameter(FGuid Id, EMaterialParameterType Type) -> uint32
	{
		if (!Id.IsValid()) return Fail(EMaterialExpressionError::ParameterExpressionRequiresValidParameterGUID);
		EMaterialProgramValueType ValueType;
		switch (Type)
		{
		case EMaterialParameterType::Scalar: ValueType = EMaterialProgramValueType::Float; break;
		case EMaterialParameterType::Vector2: ValueType = EMaterialProgramValueType::Float2; break;
		case EMaterialParameterType::Vector: ValueType = EMaterialProgramValueType::Float3; break;
		case EMaterialParameterType::Vector4: ValueType = EMaterialProgramValueType::Float4; break;
		case EMaterialParameterType::Texture: ValueType = EMaterialProgramValueType::Texture2D; break;
		default: return Fail(EMaterialExpressionError::ParameterExpressionUnsupportedType);
		}
		const auto Existing = std::ranges::find(Result.Parameters, Id, &FMaterialCompilerParameterDeclaration::Id);
		if (Existing != Result.Parameters.end() && Existing->Type != Type) return Fail(EMaterialExpressionError::ParameterGUIDConflictingTypes);
		if (Existing == Result.Parameters.end()) Result.Parameters.push_back({Id, Type});
		if (Result.Parameters.size() > MaterialProgramMaxReferencedParameterCount) return Fail(EMaterialExpressionError::ParameterCountExceedsBound);
		return Emit({.Opcode = Type == EMaterialParameterType::Texture ? EMaterialProgramOpcode::TextureParameter : EMaterialProgramOpcode::Parameter,
			.ResultType = ValueType, .Payload = Id});
	}

	auto FGraphBuilderImpl::Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialExpressionInput* const> Inputs,
		std::span<const std::vector<float>* const> Defaults, std::span<const uint8> Swizzle) -> uint32
	{
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
		if (!Signature || Inputs.size() != Signature->InputCount || Defaults.size() != Inputs.size())
			return Fail(EMaterialExpressionError::NumericSignatureMismatch);
		FNode Node{.Opcode = Opcode, .ResultType = Type};
		if (Opcode == EMaterialProgramOpcode::Swizzle)
		{
			if (Swizzle.empty() || Swizzle.size() > 4) return Fail(EMaterialExpressionError::SwizzleSelectOneFourComponents);
			FSwizzle Payload{.Length = static_cast<uint8>(Swizzle.size())};
			std::ranges::copy(Swizzle, Payload.Components.begin());
			Node.Payload = Payload;
		}
		for (size_t Slot = 0; Slot < Inputs.size(); ++Slot)
		{
			const bool bScalarDefault = IsMaterialAdaptiveNumeric(Opcode)
				&& Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4
				&& !(Opcode == EMaterialProgramOpcode::Lerp && Slot == 2);
			const bool bBroadcast = bScalarDefault && (Inputs.size() > 1 || !Inputs[Slot]->ExpressionId.IsValid());
			const auto& Default = *Defaults[Slot];
			if (!Default.empty())
			{
				const auto Accepted = Signature->Inputs[Slot];
				if (Default.size() > 4 || (std::ranges::find(Accepted,
					static_cast<EMaterialProgramValueType>(Default.size() - 1)) == Accepted.end() && !(bScalarDefault && Default.size() == 1))
					|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); }))
					return Fail(EMaterialExpressionError::RetainedNumericDefaultInvalidWidthNonFiniteComponent);
			}
			const auto& Input = *Inputs[Slot];
			if (!Input.ExpressionId.IsValid() && (Input.OutputIndex != 0 || Input.OutputId.IsValid()))
				return Fail(EMaterialExpressionError::DisconnectedNumericInputOutputSelector);
			auto Index = Input.ExpressionId.IsValid() ? ResolveIndex(Input) : Literal(Default);
			if (bBroadcast && Index != InvalidIndex && GetNode(Index).ResultType == EMaterialProgramValueType::Float)
				Index = Emit({.Opcode = static_cast<EMaterialProgramOpcode>(static_cast<uint8>(EMaterialProgramOpcode::Splat2)
					+ static_cast<uint8>(Type) - 1), .ResultType = Type, .Inputs = {Index}});
			Node.Inputs.push_back(Index);
		}
		return Emit(std::move(Node));
	}

	auto FGraphBuilderImpl::Coordinates() -> uint32
	{
		const std::array Channel{0.f};
		return Emit({.Opcode = EMaterialProgramOpcode::UVChannel,
			.ResultType = EMaterialProgramValueType::Float2, .Inputs = {Literal(Channel)}});
	}

	auto FGraphBuilderImpl::BuildAllExpressions() -> void
	{
		for (const auto& [Id, Expression] : Expressions)
		{
			if (!Result.Diagnostics.empty()) return;
			if (Cast<DMaterialExpressionMaterialOutput>(Expression)) continue;
			BuildExpression(*Expression);
		}
	}

	auto FGraphBuilderImpl::Finish(std::span<const FMaterialExpressionInput> Roots) -> FBuildResult
	{
		if (Roots.size() > MaterialFunctionMaxOutputs) Fail(EMaterialExpressionError::RootCountExceedsBound);
		BuildAllExpressions();
		if (Result.Diagnostics.empty()) for (const auto& Root : Roots) Result.Roots.push_back(ResolveIndex(Root));
		if (!Result.Diagnostics.empty())
		{
			Result.IR = {};
			Result.Roots.clear();
			Result.Parameters.clear();
			Result.Sources.clear();
			Result.Dependencies.clear();
		}
		else
		{
			std::ranges::sort(Result.Parameters, {}, &FMaterialCompilerParameterDeclaration::Id);
			std::ranges::sort(Result.Dependencies, {}, &FFunctionDependency::AssetPath);
		}
		return std::move(Result);
	}

	auto BuildGraph(std::span<DMaterialExpression* const> Expressions,
		std::span<const FMaterialExpressionInput> Roots, FBuildEnvironment Environment) -> FBuildResult
	{
		FGraphBuilderImpl Context(Expressions, std::move(Environment));
		return Context.Finish(Roots);
	}
}
