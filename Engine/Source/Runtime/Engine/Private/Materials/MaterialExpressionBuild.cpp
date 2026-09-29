#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "MaterialExpressionGraphBuilder.h"
#include <cmath>
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
		{
			const auto Semantics = GetMaterialSurfaceOutputSemantics(
				static_cast<EMaterialSurfaceOutput>(Index));
			auto& Input = Result.IR.SurfaceRoot.Inputs[Index];
			Input.Type = Semantics.Type;
			Input.LegalStages = Semantics.Stages;
			Input.SpatialKind = Semantics.Kind;
			Input.CoordinateSpace = Semantics.Space;
		}
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
			const auto HashInput = [&](const FMaterialNumericInput& Input) {
				AuthoringCodeHash.UpdateValue(Input.UseConstant);
				AuthoringCodeHash.UpdateValue(static_cast<uint32>(Input.Constant.size()));
				for (float Value : Input.Constant) AuthoringCodeHash.UpdateValue(Value);
			};
			Expression->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Struct
					&& static_cast<FStructProperty*>(Property)->GetStruct() == FMaterialNumericInput::StaticStruct())
					HashInput(*static_cast<const FMaterialNumericInput*>(Property->GetValuePtr(Expression)));
			});
			if (const auto* Surface = Cast<DMaterialExpressionSetSurfaceAttributes>(Expression))
				for (const auto& Attribute : Surface->Attributes) HashInput(Attribute.Source);

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
			else if (!std::ranges::contains(Call->Outputs, Input.OutputId, &FMaterialFunctionOutputBinding::OutputId))
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
		{
			if (Signature->Inputs[Slot].size() == 1
				&& MaterialNumericInputAllowsScalarBroadcast(Node.Opcode, static_cast<uint32>(Slot)))
			{
				const auto Before = Node.Inputs[Slot];
				Node.Inputs[Slot] = *BroadcastScalar(Before, Signature->Inputs[Slot].front()).GetIndex();
				if (Node.Inputs[Slot] != Before
					&& (Node.Opcode == EMaterialProgramOpcode::Multiply
						|| Node.Opcode == EMaterialProgramOpcode::Divide))
					Node.ScalarBroadcastMask |= static_cast<uint8>(1u << Slot);
			}
		}
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
				|| !std::ranges::contains(Accepted, Result.IR.Nodes[Input].ResultType))
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
		std::vector<FMaterialValueSemantics> InputSemantics;
		InputSemantics.reserve(Node.Inputs.size());
		for (const auto Input : Node.Inputs)
			InputSemantics.push_back(Result.IR.Nodes[Input].GetSemantics());
		for (size_t Slot = 0; Slot < InputSemantics.size(); ++Slot)
			if (Node.ScalarBroadcastMask & (1u << Slot))
				InputSemantics[Slot].Type = EMaterialProgramValueType::Float;
		const auto Transform = Node.Opcode == EMaterialProgramOpcode::TransformPosition
			|| Node.Opcode == EMaterialProgramOpcode::TransformDirection
			|| Node.Opcode == EMaterialProgramOpcode::TransformNormal
			? &std::get<FMaterialTransformPayload>(Node.Payload) : nullptr;
		const auto RequestedKind = Node.SpatialKind;
		const auto RequestedSpace = Node.CoordinateSpace;
		auto Semantics = ResolveMaterialProgramNodeSemantics(
			Node.Opcode, Node.ResultType, InputSemantics, Transform);
		if (Node.Opcode == EMaterialProgramOpcode::Constant
			&& Node.SpatialKind != EMaterialSpatialKind::None)
			Semantics = Node.GetSemantics();
		if (Semantics && Node.Opcode == EMaterialProgramOpcode::Swizzle)
		{
			const auto& Input = InputSemantics.front();
			const auto Swizzle = Node.GetSwizzle();
			const bool bIdentity3 = Input.Type == EMaterialProgramValueType::Float3
				&& Swizzle.Length == 3 && Swizzle.Components[0] == 0
				&& Swizzle.Components[1] == 1 && Swizzle.Components[2] == 2;
			const bool bIdentity2 = Input.Type == EMaterialProgramValueType::Float2
				&& Swizzle.Length == 2 && Swizzle.Components[0] == 0
				&& Swizzle.Components[1] == 1;
			const bool bSelectXYZ = Input.Type >= EMaterialProgramValueType::Float3
				&& Input.Type <= EMaterialProgramValueType::Float4
				&& Swizzle.Length == 3 && Swizzle.Components[0] == 0
				&& Swizzle.Components[1] == 1 && Swizzle.Components[2] == 2;
			const bool bSelectXY = Input.Type >= EMaterialProgramValueType::Float2
				&& Input.Type <= EMaterialProgramValueType::Float4
				&& Swizzle.Length == 2 && Swizzle.Components[0] == 0
				&& Swizzle.Components[1] == 1;
			if ((bIdentity3 && Input.Kind != EMaterialSpatialKind::ScreenCoordinate)
				|| (bIdentity2 && Input.Kind == EMaterialSpatialKind::ScreenCoordinate))
				Semantics = FMaterialValueSemantics{Node.ResultType, Input.Stages,
					Input.Kind, Input.Space};
			if (RequestedKind != EMaterialSpatialKind::None
				|| RequestedSpace != EMaterialCoordinateSpace::None)
			{
				const bool bIdentityWidth = (bSelectXYZ && RequestedKind != EMaterialSpatialKind::ScreenCoordinate)
					|| (bSelectXY && RequestedKind == EMaterialSpatialKind::ScreenCoordinate);
				if (Input.Kind != EMaterialSpatialKind::None || !bIdentityWidth)
					return Fail(EMaterialExpressionError::InputIncompatibleSemantics);
				Semantics = FMaterialValueSemantics{Node.ResultType, Input.Stages,
					RequestedKind, RequestedSpace};
			}
		}
		if (!Semantics || !IsValidMaterialValueSemantics(*Semantics))
			return Fail(EMaterialExpressionError::InputIncompatibleSemantics);
		Node.LegalStages = Semantics->Stages;
		Node.SpatialKind = Semantics->Kind;
		Node.CoordinateSpace = Semantics->Space;
		const auto IsFoldable = [](EMaterialProgramOpcode Opcode) {
			return Opcode >= EMaterialProgramOpcode::Dot
				&& Opcode <= EMaterialProgramOpcode::Reflect;
		};
		if (IsFoldable(Node.Opcode) && !bValidateAuthoring)
		{
			std::array<FMaterialProgramLiteral, 3> Values{};
			bool bAllConstant = true;
			for (size_t Slot = 0; Slot < Node.Inputs.size(); ++Slot)
			{
				const auto& InputNode = Result.IR.Nodes[Node.Inputs[Slot]];
				if (InputNode.Opcode == EMaterialProgramOpcode::Constant)
					Values[Slot] = InputNode.GetLiteral();
				else if (InputNode.Opcode >= EMaterialProgramOpcode::Splat2
					&& InputNode.Opcode <= EMaterialProgramOpcode::Splat4
					&& InputNode.Inputs.size() == 1
					&& Result.IR.Nodes[InputNode.Inputs[0]].Opcode == EMaterialProgramOpcode::Constant)
				{
					const float Scalar = Result.IR.Nodes[InputNode.Inputs[0]].GetLiteral().X;
					Values[Slot] = {Scalar, Scalar, Scalar, Scalar};
				}
				else { bAllConstant = false; break; }
			}
			if (bAllConstant)
			{
				FMaterialProgramLiteral Folded;
				const std::array<float*, 4> Output{&Folded.X, &Folded.Y, &Folded.Z, &Folded.W};
				const auto Lane = [&](size_t Input, uint32 Component) -> float {
					const auto& V = Values[Input];
					return Component == 0 ? V.X : Component == 1 ? V.Y : Component == 2 ? V.Z : V.W;
				};
				const uint32 Width = static_cast<uint32>(Node.ResultType) + 1;
				const uint32 InputWidth = Node.Opcode == EMaterialProgramOpcode::Dot
					|| Node.Opcode == EMaterialProgramOpcode::Length
					|| Node.Opcode == EMaterialProgramOpcode::Distance
					? static_cast<uint32>(Result.IR.Nodes[Node.Inputs[0]].ResultType) + 1 : Width;
				for (uint32 Component = 0; Component < Width; ++Component)
				{
					const float A = Lane(0, Component), B = Lane(1, Component), C = Lane(2, Component);
					switch (Node.Opcode)
					{
					case EMaterialProgramOpcode::Pow: *Output[Component] = std::pow(A, B); break;
					case EMaterialProgramOpcode::Sqrt: *Output[Component] = std::sqrt(A); break;
					case EMaterialProgramOpcode::Exp: *Output[Component] = std::exp(A); break;
					case EMaterialProgramOpcode::Log: *Output[Component] = std::log(A); break;
					case EMaterialProgramOpcode::Floor: *Output[Component] = std::floor(A); break;
					case EMaterialProgramOpcode::Ceil: *Output[Component] = std::ceil(A); break;
					case EMaterialProgramOpcode::Round: *Output[Component] = std::round(A); break;
					case EMaterialProgramOpcode::Frac: *Output[Component] = A - std::floor(A); break;
					case EMaterialProgramOpcode::Fmod: *Output[Component] = std::fmod(A, B); break;
					case EMaterialProgramOpcode::Step: *Output[Component] = A <= B ? 1.f : 0.f; break;
					case EMaterialProgramOpcode::SmoothStep:
					{
						const float T = std::clamp((C - A) / (B - A), 0.f, 1.f);
						*Output[Component] = T * T * (3.f - 2.f * T);
						break;
					}
					case EMaterialProgramOpcode::Sign: *Output[Component] = A > 0.f ? 1.f : A < 0.f ? -1.f : 0.f; break;
					default: break;
					}
				}
				const auto Dot = [&](size_t Left, size_t Right) {
					float Sum = 0.f;
					for (uint32 Component = 0; Component < InputWidth; ++Component)
						Sum += Lane(Left, Component) * Lane(Right, Component);
					return Sum;
				};
				if (Node.Opcode == EMaterialProgramOpcode::Dot) Folded.X = Dot(0, 1);
				if (Node.Opcode == EMaterialProgramOpcode::Length) Folded.X = std::sqrt(Dot(0, 0));
				if (Node.Opcode == EMaterialProgramOpcode::Distance)
				{
					float Sum = 0.f;
					for (uint32 Component = 0; Component < InputWidth; ++Component)
					{
						const float Delta = Lane(0, Component) - Lane(1, Component);
						Sum += Delta * Delta;
					}
					Folded.X = std::sqrt(Sum);
				}
				if (Node.Opcode == EMaterialProgramOpcode::Cross)
					Folded = {Lane(0, 1) * Lane(1, 2) - Lane(0, 2) * Lane(1, 1),
						Lane(0, 2) * Lane(1, 0) - Lane(0, 0) * Lane(1, 2),
						Lane(0, 0) * Lane(1, 1) - Lane(0, 1) * Lane(1, 0)};
				if (Node.Opcode == EMaterialProgramOpcode::Reflect)
				{
					const float Scale = 2.f * Dot(1, 0);
					for (uint32 Component = 0; Component < Width; ++Component)
						*Output[Component] = Lane(0, Component) - Scale * Lane(1, Component);
				}
				const std::array FoldedComponents{Folded.X, Folded.Y, Folded.Z, Folded.W};
				for (uint32 Component = 0; Component < Width; ++Component)
					if (!std::isfinite(FoldedComponents[Component]))
						return Fail(EMaterialExpressionError::ConstantFoldInvalidDomainNonFinite);
				Node.Opcode = EMaterialProgramOpcode::Constant;
				Node.Inputs.clear();
				Node.ScalarBroadcastMask = 0;
				Node.Payload = Folded;
				Depth = 1;
			}
		}
		const auto Index = static_cast<uint32>(Result.IR.Nodes.size());
		LinkCount += static_cast<uint32>(Node.Inputs.size());
		Result.IR.Nodes.push_back(std::move(Node));
		Depths.push_back(Depth);
		Result.Sources.push_back({.ExpressionIndex = Index, .NodeId = SourceStack.empty() ? FGuid{} : SourceStack.back(),
			.PortId = PortStack.empty() ? FGuid{} : PortStack.back(), .FunctionAssetPath = FunctionPath, .CallPath = CallPath});
		return Index;
	}

	auto FGraphBuilderImpl::Literal(std::span<const float> Components,
		EMaterialSpatialKind Kind, EMaterialCoordinateSpace Space) -> uint32
	{
		if (Components.empty() || Components.size() > 4) return Fail(EMaterialExpressionError::NumericInputRequiresDefaultOneFourComponents);
		FNode Node{.Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = static_cast<EMaterialProgramValueType>(Components.size() - 1),
			.SpatialKind = Kind, .CoordinateSpace = Space};
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

	auto FGraphBuilderImpl::CollectionParameter(
		const DMaterialParameterCollection& Collection, FGuid ParameterId) -> uint32
	{
		if (!ParameterId.IsValid())
			return Fail(EMaterialExpressionError::CollectionParameterMissingDeclaration);
		const auto* Declaration = Collection.FindDeclaration(ParameterId);
		if (!Declaration)
			return Fail(EMaterialExpressionError::CollectionParameterMissingDeclaration);
		auto Layout = Collection.BuildLayout();
		if (!Layout)
			return Fail(EMaterialExpressionError::CollectionParameterInvalidSchema);
		const auto Existing = std::ranges::find(Result.Collections,
			Layout->CollectionId, &FMaterialParameterCollectionLayout::CollectionId);
		if (Existing != Result.Collections.end() && !Existing->HasCompatibleSchema(*Layout))
			return Fail(EMaterialExpressionError::CollectionParameterInvalidSchema);
		if (Existing == Result.Collections.end())
		{
			if (Result.Collections.size() >= MaterialParameterCollectionMaxPerMaterial)
				return Fail(EMaterialExpressionError::CollectionCountExceedsBound);
			Result.Collections.push_back(std::move(*Layout));
		}
		EMaterialProgramValueType ValueType;
		switch (Declaration->Type)
		{
		case EMaterialParameterType::Scalar: ValueType = EMaterialProgramValueType::Float; break;
		case EMaterialParameterType::Vector2: ValueType = EMaterialProgramValueType::Float2; break;
		case EMaterialParameterType::Vector: ValueType = EMaterialProgramValueType::Float3; break;
		case EMaterialParameterType::Vector4: ValueType = EMaterialProgramValueType::Float4; break;
		default: return Fail(EMaterialExpressionError::CollectionParameterInvalidSchema);
		}
		return Emit({.Opcode = EMaterialProgramOpcode::CollectionParameter,
			.ResultType = ValueType,
			.Payload = FCollectionParameter{Collection.GetCollectionId(), ParameterId}});
	}

	auto FGraphBuilderImpl::Numeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialNumericInput* const> Inputs,
		std::span<const uint8> Swizzle, EMaterialSpatialKind OutputKind,
		EMaterialCoordinateSpace OutputSpace) -> uint32
	{
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
		if (!Signature || Inputs.size() != Signature->InputCount)
			return Fail(EMaterialExpressionError::NumericSignatureMismatch);
		FNode Node{.Opcode = Opcode, .ResultType = Type};
		if (Opcode == EMaterialProgramOpcode::Swizzle)
		{
			if (Swizzle.empty() || Swizzle.size() > 4) return Fail(EMaterialExpressionError::SwizzleSelectOneFourComponents);
			FSwizzle Payload{.Length = static_cast<uint8>(Swizzle.size())};
			std::ranges::copy(Swizzle, Payload.Components.begin());
			Node.Payload = Payload;
			Node.SpatialKind = OutputKind;
			Node.CoordinateSpace = OutputSpace;
		}
		for (size_t Slot = 0; Slot < Inputs.size(); ++Slot)
		{
			const bool bScalarDefault = IsMaterialAdaptiveNumeric(Opcode)
				&& Type > EMaterialProgramValueType::Float && Type <= EMaterialProgramValueType::Float4
				&& !(Opcode == EMaterialProgramOpcode::Lerp && Slot == 2);
			const bool bBroadcast = bScalarDefault
				&& (!Inputs[Slot]->Connection.ExpressionId.IsValid()
					|| (Inputs.size() > 1 && MaterialNumericInputAllowsScalarBroadcast(
						Opcode, static_cast<uint32>(Slot))));
			const auto& Stored = *Inputs[Slot];
			const auto& Default = Stored.Constant;
			if (Default.empty()) return Fail(EMaterialExpressionError::RetainedNumericDefaultInvalidWidthNonFiniteComponent);
			{
				const auto Accepted = Signature->Inputs[Slot];
				if (Default.size() > 4 || (!std::ranges::contains(Accepted,
					static_cast<EMaterialProgramValueType>(Default.size() - 1)) && !(bScalarDefault && Default.size() == 1))
					|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); }))
					return Fail(EMaterialExpressionError::RetainedNumericDefaultInvalidWidthNonFiniteComponent);
			}
			const auto& Input = Stored.Connection;
			if (!Input.ExpressionId.IsValid() && (Input.OutputIndex != 0 || Input.OutputId.IsValid()))
				return Fail(EMaterialExpressionError::DisconnectedNumericInputOutputSelector);
			const bool bSurfaceNormalDefault = Opcode == EMaterialProgramOpcode::MakeSurface
				&& Slot == static_cast<size_t>(EMaterialSurfaceOutput::Normal)
				&& !Input.ExpressionId.IsValid();
			auto Index = Input.ExpressionId.IsValid() ? ResolveIndex(Input)
				: Literal(Stored.UseConstant ? Default
					: GetMaterialNumericInputFallback(Opcode, Type, Slot),
					bSurfaceNormalDefault ? EMaterialSpatialKind::Normal
						: EMaterialSpatialKind::None,
					bSurfaceNormalDefault ? EMaterialCoordinateSpace::Tangent
						: EMaterialCoordinateSpace::None);
			if (bBroadcast && Index != InvalidIndex && GetNode(Index).ResultType == EMaterialProgramValueType::Float)
			{
				Index = Emit({.Opcode = static_cast<EMaterialProgramOpcode>(static_cast<uint8>(EMaterialProgramOpcode::Splat2)
					+ static_cast<uint8>(Type) - 1), .ResultType = Type, .Inputs = {Index}});
				if (Opcode == EMaterialProgramOpcode::Multiply
					|| Opcode == EMaterialProgramOpcode::Divide)
					Node.ScalarBroadcastMask |= static_cast<uint8>(1u << Slot);
			}
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
			Result.Collections.clear();
			Result.Sources.clear();
			Result.Dependencies.clear();
		}
		else
		{
			std::ranges::sort(Result.Parameters, {}, &FMaterialCompilerParameterDeclaration::Id);
			std::ranges::sort(Result.Collections, {}, &FMaterialParameterCollectionLayout::CollectionId);
			std::ranges::sort(Result.Dependencies, {}, &FFunctionDependency::AssetPath);
			Result.bSucceeded = true;
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
