#include "Materials/MaterialExpressions.h"

#include "Threading/RunnableThread.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialFunction.h"
#include "Materials/Material.h"
#include "DObject/Property.h"

namespace Durin
{
	auto DMaterialExpression::PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void
	{
		Super::PostEditChangeProperty(Event);
		auto* Owner = GetOuter();
		if (Owner && (Owner->IsA(DMaterial::StaticClass()) || Owner->IsA(DMaterialFunction::StaticClass())))
		{
			auto OwnerEvent = Event;
			OwnerEvent.MemberProperty = Owner->GetClass()->FindPropertyByName("ExpressionCollection");
			Owner->PostEditChangeProperty(OwnerEvent);
		}
	}

	namespace
	{
		auto LowerInput(const FMaterialExpressionInput& Input) -> FMaterialProgramLink
		{
			return {Input.ExpressionId, Input.OutputIndex, Input.OutputId};
		}
	}

	auto DMaterialExpressionParameter::MakeDefinition(EMaterialParameterType Type, FMaterialParameterValue Value) const
		-> FMaterialParameterDefinition
	{
		return {.Id = Metadata.Id, .Name = Metadata.Name, .Type = Type, .Value = std::move(Value),
			.DisplayName = Metadata.DisplayName, .GroupName = Metadata.GroupName,
			.SortOrder = Metadata.SortOrder, .Presentation = Metadata.Presentation};
	}

	auto DMaterialExpressionScalarConstant::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float};
		Node.Literal = {Value};
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionScalarParameter::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Parameter,
			.ResultType = EMaterialProgramValueType::Float};
		Node.Parameter = GetParameterDefinition();
		if (!ValidateMaterialParameterDefinitions(std::span(&Node.Parameter, 1))) return false;
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionVector2Constant::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float2};
		Node.Literal = {static_cast<float>(Value[0]), static_cast<float>(Value[1])};
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionVector2Parameter::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Parameter,
			.ResultType = EMaterialProgramValueType::Float2};
		Node.Parameter = GetParameterDefinition();
		if (!ValidateMaterialParameterDefinitions(std::span(&Node.Parameter, 1))) return false;
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionVector3Constant::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float3};
		Node.Literal = {static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2])};
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionVector3Parameter::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Parameter,
			.ResultType = EMaterialProgramValueType::Float3};
		Node.Parameter = GetParameterDefinition();
		if (!ValidateMaterialParameterDefinitions(std::span(&Node.Parameter, 1))) return false;
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionVector4Constant::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Constant,
			.ResultType = EMaterialProgramValueType::Float4};
		Node.Literal = {static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2]), static_cast<float>(Value[3])};
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionVector4Parameter::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::Parameter,
			.ResultType = EMaterialProgramValueType::Float4};
		Node.Parameter = GetParameterDefinition();
		if (!ValidateMaterialParameterDefinitions(std::span(&Node.Parameter, 1))) return false;
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionTextureParameter::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::TextureParameter,
			.ResultType = EMaterialProgramValueType::Texture2D};
		Node.Parameter = GetParameterDefinition();
		if (!ValidateMaterialParameterDefinitions(std::span(&Node.Parameter, 1))) return false;
		OutNode = std::move(Node);
		return true;
	}
	auto DMaterialExpressionNumeric::LowerNumeric(EMaterialProgramOpcode Opcode, EMaterialProgramValueType Type,
		std::span<const FMaterialExpressionInput* const> Inputs,
		std::span<const std::vector<float>* const> Defaults, FMaterialProgramNode& OutNode) const -> bool
	{
		check(IsInGameThread());
		const auto Signature = GetMaterialProgramNodeSignature(Opcode, Type);
		if (!Signature || Inputs.size() != Signature->InputCount || Defaults.size() != Inputs.size()) return false;
		FMaterialProgramNode Node{.Id = Id, .Opcode = Opcode, .ResultType = Type};
		for (size_t Index = 0; Index < Inputs.size(); ++Index)
		{
			const auto& Input = *Inputs[Index];
			const auto& DefaultComponents = *Defaults[Index];
			const auto Count = DefaultComponents.size();
			if (Count > 4) return false;
			FMaterialInputDefault Default;
			if (Count != 0)
			{
				Default.Kind = EMaterialInputDefaultKind::Literal;
				Default.Type = static_cast<EMaterialProgramValueType>(Count - 1);
				if (std::ranges::find(Signature->Inputs[Index], Default.Type) == Signature->Inputs[Index].end()) return false;
				std::array<float*, 4> Components{&Default.Literal.X, &Default.Literal.Y, &Default.Literal.Z, &Default.Literal.W};
				for (size_t Component = 0; Component < Count; ++Component)
				{
					if (!std::isfinite(DefaultComponents[Component])) return false;
					*Components[Component] = DefaultComponents[Component];
				}
			}
			Node.Inputs.push_back(LowerInput(Input));
			Node.InputDefaults.push_back(Default);
		}
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionAdd::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return LowerNumeric(EMaterialProgramOpcode::Add, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSubtract::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return LowerNumeric(EMaterialProgramOpcode::Subtract, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionMultiply::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return LowerNumeric(EMaterialProgramOpcode::Multiply, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionDivide::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return LowerNumeric(EMaterialProgramOpcode::Divide, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionMinimum::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return LowerNumeric(EMaterialProgramOpcode::Minimum, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionMaximum::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return LowerNumeric(EMaterialProgramOpcode::Maximum, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionNegate::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Negate, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionOneMinus::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::OneMinus, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionAbsolute::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Absolute, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSaturate::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Saturate, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionNormalize::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Normalize, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSine::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Sine, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionCosine::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Cosine, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionClamp::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input, &Minimum, &Maximum};
		const std::array Defaults{&InputDefault, &MinimumDefault, &MaximumDefault};
		return LowerNumeric(EMaterialProgramOpcode::Clamp, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionLerp::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&A, &B, &Alpha};
		const std::array Defaults{&ADefault, &BDefault, &AlphaDefault};
		return LowerNumeric(EMaterialProgramOpcode::Lerp, ResultType, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionMakeVector2::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&X, &Y};
		const std::array Defaults{&XDefault, &YDefault};
		return LowerNumeric(EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionMakeVector3::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&X, &Y, &Z};
		const std::array Defaults{&XDefault, &YDefault, &ZDefault};
		return LowerNumeric(EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionMakeVector4::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&X, &Y, &Z, &W};
		const std::array Defaults{&XDefault, &YDefault, &ZDefault, &WDefault};
		return LowerNumeric(EMaterialProgramOpcode::MakeFloat4, EMaterialProgramValueType::Float4, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSplat2::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Splat2, EMaterialProgramValueType::Float2, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSplat3::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Splat3, EMaterialProgramValueType::Float3, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSplat4::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::Splat4, EMaterialProgramValueType::Float4, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionTruncateToScalar::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::TruncateToFloat, EMaterialProgramValueType::Float, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionTruncateToVector2::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::TruncateToFloat2, EMaterialProgramValueType::Float2, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionTruncateToVector3::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::TruncateToFloat3, EMaterialProgramValueType::Float3, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionDecodeNormalRG::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return LowerNumeric(EMaterialProgramOpcode::DecodeNormalRG, EMaterialProgramValueType::Float3, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionBlendNormalsRNM::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Base, &Detail};
		const std::array Defaults{&BaseDefault, &DetailDefault};
		return LowerNumeric(EMaterialProgramOpcode::BlendNormalsRNM, EMaterialProgramValueType::Float3, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionUVChannel::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		const std::array Inputs{&Channel};
		const std::array Defaults{&ChannelDefault};
		return LowerNumeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionSwizzle::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext&) const -> bool
	{
		if (Components.empty() || Components.size() > 4
			|| std::ranges::any_of(Components, [](uint8 Component) { return Component > 3; })) return false;
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		FMaterialProgramNode Node;
		if (!LowerNumeric(EMaterialProgramOpcode::Swizzle,
			static_cast<EMaterialProgramValueType>(Components.size() - 1), Inputs, Defaults, Node)) return false;
		Node.SwizzleLength = static_cast<uint8>(Components.size());
		const std::array<uint8*, 4> Targets{&Node.SwizzleX, &Node.SwizzleY, &Node.SwizzleZ, &Node.SwizzleW};
		for (size_t Index = 0; Index < Components.size(); ++Index) *Targets[Index] = Components[Index];
		OutNode = std::move(Node);
		return true;
	}
	namespace
	{
		auto LowerDefault(const FMaterialScalarExpressionDefault& Value) -> FMaterialInputDefault
		{
			return Value.bPresent ? FMaterialInputDefault{EMaterialInputDefaultKind::Literal,
				EMaterialProgramValueType::Float, {Value.Value}} : FMaterialInputDefault{};
		}
		auto LowerDefault(const FMaterialVector2ExpressionDefault& Value) -> FMaterialInputDefault
		{
			return Value.bPresent ? FMaterialInputDefault{EMaterialInputDefaultKind::Literal,
				EMaterialProgramValueType::Float2, {static_cast<float>(Value.Value.x), static_cast<float>(Value.Value.y)}} : FMaterialInputDefault{};
		}
		auto LowerUV(const FMaterialExpressionUVSettings& Value) -> FMaterialUVSettings
		{
			return {LowerDefault(Value.Channel), LowerDefault(Value.Scale), LowerDefault(Value.Offset), LowerDefault(Value.Rotation)};
		}
	}

	auto DMaterialExpressionTextureSample2D::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		OutNode = {.Id = Id, .Opcode = EMaterialProgramOpcode::TextureSample2D,
			.ResultType = EMaterialProgramValueType::Float4, .Inputs = {LowerInput(Texture), LowerInput(UV)}, .UVSettings = LowerUV(UVSettings)};
		return true;
	}

	auto DMaterialExpressionTextureSampleParameter2D::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		FMaterialProgramNode Node;
		if (!Super::Lower(Node, Context)) return false;
		Node.Opcode = EMaterialProgramOpcode::TextureSampleParameter2D;
		Node.ResultType = EMaterialProgramValueType::Float4;
		Node.Inputs = {LowerInput(UV)};
		Node.UVSettings = LowerUV(UVSettings);
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionTextureCoordinates::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		OutNode = {.Id = Id, .Opcode = EMaterialProgramOpcode::TextureCoordinates,
			.ResultType = EMaterialProgramValueType::Float2, .Inputs = {LowerInput(Channel), LowerInput(Scale), LowerInput(Offset), LowerInput(Rotation)}, .UVSettings = LowerUV(Defaults)};
		return true;
	}

	auto DMaterialExpressionMakeSurface::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		const std::array Inputs{&BaseColor, &Normal, &Metallic, &Roughness, &AmbientOcclusion, &Emissive, &Opacity, &OpacityMask};
		const std::array Defaults{&BaseColorDefault, &NormalDefault, &MetallicDefault, &RoughnessDefault, &AmbientOcclusionDefault, &EmissiveDefault, &OpacityDefault, &OpacityMaskDefault};
		return LowerNumeric(EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface, Inputs, Defaults, OutNode);
	}

	auto DMaterialExpressionGetSurfaceAttributes::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		if (AttributeMask == 0) return false;
		OutNode = {.Id = Id, .Opcode = EMaterialProgramOpcode::GetSurfaceAttributes,
			.ResultType = EMaterialProgramValueType::Surface, .Inputs = {LowerInput(Surface)}, .SurfaceAttributeMask = AttributeMask};
		return true;
	}

	auto DMaterialExpressionSetSurfaceAttributes::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		uint8 Seen = 0;
		std::vector<FMaterialSurfaceAttributeBinding> LoweredAttributes;
		for (const auto& Attribute : Attributes)
		{
			const auto Index = static_cast<uint8>(Attribute.Attribute);
			if (Index >= 8 || (Seen & (1u << Index))) return false;
			Seen |= static_cast<uint8>(1u << Index);
			LoweredAttributes.push_back({Attribute.Attribute, LowerInput(Attribute.Source)});
		}
		OutNode = {.Id = Id, .Opcode = EMaterialProgramOpcode::SetSurfaceAttributes,
			.ResultType = EMaterialProgramValueType::Surface, .Inputs = {LowerInput(Surface)}, .SurfaceAttributes = std::move(LoweredAttributes)};
		return true;
	}

	auto DMaterialExpressionFunctionInput::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		if (!Context.Signature) return false;
		const auto& Ports = Context.Signature->Inputs;
		const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
		if (Port == Ports.end()) return false;
		OutNode = {.Id = Id, .Opcode = EMaterialProgramOpcode::FunctionInput, .ResultType = Port->Type,
			.FunctionPortId = PortId};
		return true;
	}

	auto DMaterialExpressionFunctionOutput::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		if (!Context.Signature) return false;
		const auto& Ports = Context.Signature->Outputs;
		const auto Port = std::ranges::find(Ports, PortId, &FMaterialFunctionPort::Id);
		if (Port == Ports.end()) return false;
		OutNode = {.Id = Id, .Opcode = EMaterialProgramOpcode::FunctionOutput, .ResultType = Port->Type,
			.Inputs = {LowerInput(Source)},
			.FunctionPortId = PortId};
		return true;
	}

	auto DMaterialExpressionFunctionCall::Lower(FMaterialProgramNode& OutNode,
		const FMaterialExpressionLoweringContext& Context) const -> bool
	{
		check(IsInGameThread());
		if (!Context.Calls || Outputs.empty()) return false;
		FMaterialFunctionCall Call{.NodeId = Id, .Function = Function, .Outputs = Outputs};
		for (const auto& Binding : Inputs)
		{
			FMaterialFunctionInputBinding Input{.InputId = Binding.InputId,
				.ExpectedType = Binding.ExpectedType, .Source = LowerInput(Binding.Input)};
			const auto Count = Binding.InputDefault.size();
			if (Count > 4) return false;
			if (Count != 0)
			{
				Input.Default.Kind = EMaterialInputDefaultKind::Literal;
				Input.Default.Type = static_cast<EMaterialProgramValueType>(Count - 1);
				if (Input.Default.Type != Binding.ExpectedType) return false;
				const std::array<float*, 4> Components{&Input.Default.Literal.X, &Input.Default.Literal.Y,
					&Input.Default.Literal.Z, &Input.Default.Literal.W};
				for (size_t Index = 0; Index < Count; ++Index)
				{
					if (!std::isfinite(Binding.InputDefault[Index])) return false;
					*Components[Index] = Binding.InputDefault[Index];
				}
			}
			Call.Inputs.push_back(Input);
		}
		if (Context.bValidateFunctionReferences)
		{
			if (!IsValid(Function.Get())) return false;
			const FMaterialFunctionCallSnapshot Snapshot{Call.NodeId, Function->GetObjectPath(), Call.Inputs, Call.Outputs};
			if (!ValidateMaterialFunctionCallSignature(Snapshot, Function->GetFunctionSignature())) return false;
		}
		FMaterialProgramNode Node{.Id = Id, .Opcode = EMaterialProgramOpcode::FunctionCall, .ResultType = Outputs.front().ExpectedType};
		Context.Calls->push_back(std::move(Call));
		OutNode = std::move(Node);
		return true;
	}

	auto DMaterialExpressionScalarParameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Scalar, FMaterialParameterValue::MakeScalar(DefaultValue));
		Definition.bHasRange = bHasRange;
		Definition.MinimumValue = MinimumValue;
		Definition.MaximumValue = MaximumValue;
		return Definition;
	}

	auto DMaterialExpressionVector2Parameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Vector2, FMaterialParameterValue::MakeVector2(DefaultValue));
		return Definition;
	}

	auto DMaterialExpressionVector3Parameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Vector, FMaterialParameterValue::MakeVector(DefaultValue));
		return Definition;
	}

	auto DMaterialExpressionVector4Parameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Vector4, FMaterialParameterValue::MakeVector4(DefaultValue));
		return Definition;
	}

	auto DMaterialExpressionTextureParameter::GetParameterDefinition() const -> FMaterialParameterDefinition
	{
		auto Definition = MakeDefinition(EMaterialParameterType::Texture,
			FMaterialParameterValue::MakeTexture(DefaultValue.Texture.Get(), DefaultValue.SamplerState, DefaultValue.TextureFallback));
		Definition.TextureUsage = TextureUsage;
		return Definition;
	}

}
