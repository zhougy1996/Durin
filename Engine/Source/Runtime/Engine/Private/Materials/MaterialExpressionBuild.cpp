#include "Materials/MaterialExpressionBuild.h"
#include <unordered_set>

#include "Threading/RunnableThread.h"
#include "Materials/MaterialFunctionInterface.h"

namespace Durin
{
	FMaterialExpressionBuildContext::FMaterialExpressionBuildContext(std::span<DMaterialExpression* const> InExpressions,
		FMaterialExpressionBuildEnvironment InEnvironment)
		: Shared(std::make_shared<FSharedState>()), Result(Shared->Result), Depths(Shared->Depths),
		  LinkCount(Shared->LinkCount), Environment(std::move(InEnvironment))
	{
		Admit(InExpressions);
		for (uint32 Index = 0; Index < Result.IR.SurfaceRoot.Inputs.size(); ++Index)
			Result.IR.SurfaceRoot.Inputs[Index].Type = GetMaterialSurfaceOutputType(static_cast<EMaterialSurfaceOutput>(Index));
	}

	FMaterialExpressionBuildContext::FMaterialExpressionBuildContext(FMaterialExpressionBuildContext& Parent,
		const FMaterialExpressionFunctionBody& Body, FGuid CallId)
		: Shared(Parent.Shared), Result(Shared->Result), Depths(Shared->Depths), LinkCount(Shared->LinkCount),
		  Environment(Parent.Environment), Signature(&Body.Signature), FunctionPath(Body.AssetPath), CallPath(Parent.CallPath)
	{
		CallPath.push_back(CallId);
		Admit(Body.Expressions);
	}

	auto FMaterialExpressionBuildContext::Admit(std::span<DMaterialExpression* const> InExpressions) -> void
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

	auto FMaterialExpressionBuildContext::Fail(FMaterialError Error, FGuid PortId,
		EMaterialProgramDiagnosticCategory Category) -> uint32
	{
		if (Result.Diagnostics.empty())
		{
			Result.Diagnostics.push_back({.Category = Category,
				.LocationKind = EMaterialProgramDiagnosticLocationKind::Node,
				.NodeId = SourceStack.empty() ? FGuid{} : SourceStack.back(), .Error = std::move(Error),
				.PortId = PortId.IsValid() ? PortId : PortStack.empty() ? FGuid{} : PortStack.back(), .FunctionAssetPath = FunctionPath, .CallPath = CallPath});
		}
		return InvalidMaterialExpressionIndex;
	}

	auto FMaterialExpressionBuildContext::Resolve(const FMaterialExpressionInput& Input) -> FMaterialExpressionBuildValue
	{
		check(IsInGameThread());
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		if (bValidateAuthoring)
		{
			AuthoringCodeHash.UpdateValue(Input.ExpressionId);
			AuthoringCodeHash.UpdateValue(Input.OutputIndex);
			AuthoringCodeHash.UpdateValue(Input.OutputId);
		}
		const FOutputKey Key{Input.ExpressionId, Input.OutputIndex, Input.OutputId};
		if (const auto Found = Values.find(Key); Found != Values.end()) return Found->second;
		const auto Found = Expressions.find(Input.ExpressionId);
		if (Found == Expressions.end()) return Fail(EMaterialExpressionError::InputDisconnectedRefersMissingExpression);
		if (Active.size() >= MaterialProgramMaxDepth || !Active.insert(Key).second)
			return Fail(EMaterialExpressionError::InputsContainCycleExceedTraversalDepthBound);
		SourceStack.push_back(Input.ExpressionId);
		const auto Index = Found->second->Build(*this, Input.OutputIndex, Input.OutputId);
		if (Index.GetIndex() && *Index.GetIndex() >= Result.IR.Nodes.size() && Result.Diagnostics.empty()) Fail(EMaterialExpressionError::BuildReturnedInvalidIRIndex);
		SourceStack.pop_back();
		Active.erase(Key);
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		Values.emplace(Key, Index);
		return Index;
	}

	auto FMaterialExpressionBuildContext::ResolveIndex(const FMaterialExpressionInput& Input) -> uint32
	{
		const auto Value = Resolve(Input);
		return Value.GetIndex() ? *Value.GetIndex() : Fail(EMaterialExpressionError::InvalidTextureDefaultConsumer);
	}

	auto FMaterialExpressionBuildContext::MatchesType(const FMaterialExpressionBuildValue& Value, EMaterialProgramValueType Type) const -> bool
	{
		if (Value.GetTexture()) return Type == EMaterialProgramValueType::Texture2D;
		return *Value.GetIndex() < Result.IR.Nodes.size() && Result.IR.Nodes[*Value.GetIndex()].ResultType == Type;
	}

	auto FMaterialExpressionBuildContext::BroadcastScalar(FMaterialExpressionBuildValue Value,
		EMaterialProgramValueType Type) -> FMaterialExpressionBuildValue
	{
		if (Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4
			&& MatchesType(Value, EMaterialProgramValueType::Float))
			return Emit({.Opcode = static_cast<EMaterialProgramOpcode>(static_cast<uint8>(EMaterialProgramOpcode::Splat2)
				+ static_cast<uint8>(Type) - 1), .ResultType = Type, .Inputs = {*Value.GetIndex()}});
		return Value;
	}

	auto FMaterialExpressionBuildContext::Emit(FMaterialIRNode Node) -> uint32
	{
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
		const auto Signature = GetMaterialProgramNodeSignature(Node.Opcode, Node.ResultType);
		if (!Node.HasValidPayload() || !Signature || Node.Inputs.size() != Signature->InputCount)
			return Fail(EMaterialExpressionError::OpcodeResultWidthInputCountInvalid);
		// Adapt authored fixed-width inputs before admitting strictly typed IR.
		// Normalize keeps its requirement for a vector source.
		for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
			if (Signature->Inputs[Slot].size() == 1 && Node.Opcode != EMaterialProgramOpcode::Normalize)
				Node.Inputs[Slot] = *BroadcastScalar(Node.Inputs[Slot], Signature->Inputs[Slot].front()).GetIndex();
		if (!Result.Diagnostics.empty()) return InvalidMaterialExpressionIndex;
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

	auto FMaterialExpressionBuildContext::Literal(std::span<const float> Components) -> uint32
	{
		if (Components.empty() || Components.size() > 4) return Fail(EMaterialExpressionError::NumericInputRequiresDefaultOneFourComponents);
		FMaterialIRNode Node{.Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = static_cast<EMaterialProgramValueType>(Components.size() - 1)};
		FMaterialProgramLiteral Literal;
		const std::array Targets{&Literal.X, &Literal.Y, &Literal.Z, &Literal.W};
		for (size_t Index = 0; Index < Components.size(); ++Index) *Targets[Index] = Components[Index];
		Node.Payload = Literal;
		return Emit(std::move(Node));
	}

	auto FMaterialExpressionBuildContext::Parameter(FGuid Id, EMaterialParameterType Type) -> uint32
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

	auto FMaterialExpressionBuildContext::Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialExpressionInput* const> Inputs,
		std::span<const std::vector<float>* const> Defaults, std::span<const uint8> Swizzle) -> uint32
	{
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
		if (!Signature || Inputs.size() != Signature->InputCount || Defaults.size() != Inputs.size())
			return Fail(EMaterialExpressionError::NumericSignatureMismatch);
		FMaterialIRNode Node{.Opcode = Opcode, .ResultType = Type};
		if (Opcode == EMaterialProgramOpcode::Swizzle)
		{
			if (Swizzle.empty() || Swizzle.size() > 4) return Fail(EMaterialExpressionError::SwizzleSelectOneFourComponents);
			FMaterialIRSwizzle Payload{.Length = static_cast<uint8>(Swizzle.size())};
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
			if (bBroadcast && Index != InvalidMaterialExpressionIndex && GetNode(Index).ResultType == EMaterialProgramValueType::Float)
				Index = Emit({.Opcode = static_cast<EMaterialProgramOpcode>(static_cast<uint8>(EMaterialProgramOpcode::Splat2)
					+ static_cast<uint8>(Type) - 1), .ResultType = Type, .Inputs = {Index}});
			Node.Inputs.push_back(Index);
		}
		return Emit(std::move(Node));
	}

	auto FMaterialExpressionBuildContext::Coordinates() -> uint32
	{
		const std::array Channel{0.f};
		return Emit({.Opcode = EMaterialProgramOpcode::UVChannel,
			.ResultType = EMaterialProgramValueType::Float2, .Inputs = {Literal(Channel)}});
	}

	auto FMaterialExpressionBuildContext::SampleOutput(const DMaterialExpression& Expression, uint8 OutputIndex) -> uint32
	{
		const auto* Output = FindMaterialSampleOutput(EMaterialProgramOpcode::TextureSample2D, OutputIndex);
		if (!Output) return Fail(EMaterialExpressionError::SampleExpressionOutputSelectorInvalid);
		const auto Sample = ResolveIndex({Expression.Id});
		if (Sample == InvalidMaterialExpressionIndex) return Sample;
		if (Output->Id == EMaterialSampleOutput::RGBA) return Sample;
		bool bDecodeNormal = false;
		if (Output->Id == EMaterialSampleOutput::RGB)
		{
			if (const auto* Parameter = Cast<DMaterialExpressionTextureParameter>(&Expression))
				bDecodeNormal = Parameter->TextureUsage == ETextureUsage::Normal;
			else if (const auto* TextureSample = Cast<DMaterialExpressionTextureSample2D>(&Expression))
			{
				const auto Resource = Resolve(TextureSample->Texture);
				if (const auto* Default = Resource.GetTexture())
					bDecodeNormal = Default->Fallback == EMaterialTextureFallback::FlatRGNormal;
				else if (const auto* Index = Resource.GetIndex(); Index && *Index != InvalidMaterialExpressionIndex)
				{
					const auto Usage = Shared->TextureUsages.find(GetNode(*Index).GetParameterId());
					bDecodeNormal = Usage != Shared->TextureUsages.end() && Usage->second == ETextureUsage::Normal;
				}
			}
		}
		// RGB is the ready-to-use normal; raw RGBA and scalar channels retain
		// their encoded values for channel processing.
		const uint8 Width = bDecodeNormal ? 2 : static_cast<uint8>(Output->Type) + 1;
		const auto Selected = Emit({.Opcode = EMaterialProgramOpcode::Swizzle,
			.ResultType = static_cast<EMaterialProgramValueType>(Width - 1), .Inputs = {Sample},
			.Payload = FMaterialIRSwizzle{Width, {Output->FirstComponent,
				static_cast<uint8>(Width > 1 ? 1 : 0), static_cast<uint8>(Width > 2 ? 2 : 0)}}});
		return bDecodeNormal ? Emit({.Opcode = EMaterialProgramOpcode::DecodeNormalRG,
			.ResultType = EMaterialProgramValueType::Float3, .Inputs = {Selected}}) : Selected;
	}

	auto FMaterialExpressionBuildContext::BuildAllExpressions() -> void
	{
		for (const auto& [Id, Expression] : Expressions)
		{
			if (!Result.Diagnostics.empty()) return;
			if (Cast<DMaterialExpressionMaterialOutput>(Expression)) continue;
			FMaterialExpressionInput Probe{Id};
			if (const auto* Call = Cast<DMaterialExpressionFunctionCall>(Expression); Call && !Call->Outputs.empty())
				Probe.OutputId = Call->Outputs.front().OutputId;
			if (const auto* Attributes = Cast<DMaterialExpressionGetSurfaceAttributes>(Expression))
				while (Probe.OutputIndex < 8 && !(Attributes->AttributeMask & (1u << Probe.OutputIndex))) ++Probe.OutputIndex;
			Resolve(Probe);
		}
	}

	auto FMaterialExpressionBuildContext::Finish(std::span<const FMaterialExpressionInput> Roots) -> FMaterialExpressionBuildResult
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
			std::ranges::sort(Result.Dependencies, {}, &FMaterialExpressionFunctionDependency::AssetPath);
		}
		return std::move(Result);
	}

	auto BuildMaterialExpressionGraph(std::span<DMaterialExpression* const> Expressions,
		std::span<const FMaterialExpressionInput> Roots, FMaterialExpressionBuildEnvironment Environment) -> FMaterialExpressionBuildResult
	{
		FMaterialExpressionBuildContext Context(Expressions, std::move(Environment));
		return Context.Finish(Roots);
	}
}
