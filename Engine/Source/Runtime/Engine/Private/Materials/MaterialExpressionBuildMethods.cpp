#include "Materials/MaterialExpressionBuild.h"

namespace Durin
{

	auto DMaterialExpressionScalarConstant::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Components{Value};
		return Emitter.Output(0, Emitter.Literal(Components));
	}

	auto DMaterialExpressionScalarParameter::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Parameter(Metadata.Id, EMaterialParameterType::Scalar));
	}

	auto DMaterialExpressionVector2Constant::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Components{static_cast<float>(Value[0]), static_cast<float>(Value[1])};
		return Emitter.Output(0, Emitter.Literal(Components));
	}

	auto DMaterialExpressionVector3Constant::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Components{static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2])};
		return Emitter.Output(0, Emitter.Literal(Components));
	}

	auto DMaterialExpressionVector4Constant::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Components{static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2]), static_cast<float>(Value[3])};
		return Emitter.Output(0, Emitter.Literal(Components));
	}

	auto DMaterialExpressionVector4Parameter::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Parameter(Metadata.Id, EMaterialParameterType::Vector4));
	}

	auto DMaterialExpressionTextureParameter::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Parameter(Metadata.Id, EMaterialParameterType::Texture));
	}

	auto DMaterialExpressionAdd::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Add, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionSubtract::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Subtract, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionMultiply::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Multiply, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionDivide::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Divide, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionMinimum::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Minimum, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionMaximum::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Maximum, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionNegate::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Negate, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionOneMinus::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::OneMinus, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionAbsolute::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Absolute, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionSaturate::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Saturate, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionNormalize::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Normalize, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionSine::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Sine, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionCosine::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Cosine, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionClamp::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input, &Minimum, &Maximum};
		const std::array Defaults{&InputDefault, &MinimumDefault, &MaximumDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Clamp, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionLerp::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B, &Alpha};
		const std::array Defaults{&ADefault, &BDefault, &AlphaDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Lerp, ResultType, Inputs, Defaults));
	}

	auto DMaterialExpressionMakeVector2::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&X, &Y};
		const std::array Defaults{&XDefault, &YDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2, Inputs, Defaults));
	}

	auto DMaterialExpressionMakeVector3::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&X, &Y, &Z};
		const std::array Defaults{&XDefault, &YDefault, &ZDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3, Inputs, Defaults));
	}

	auto DMaterialExpressionMakeVector4::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&X, &Y, &Z, &W};
		const std::array Defaults{&XDefault, &YDefault, &ZDefault, &WDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeFloat4, EMaterialProgramValueType::Float4, Inputs, Defaults));
	}

	auto DMaterialExpressionSplat2::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Splat2, EMaterialProgramValueType::Float2, Inputs, Defaults));
	}

	auto DMaterialExpressionSplat3::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Splat3, EMaterialProgramValueType::Float3, Inputs, Defaults));
	}

	auto DMaterialExpressionSplat4::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Splat4, EMaterialProgramValueType::Float4, Inputs, Defaults));
	}

	auto DMaterialExpressionBlendNormalsRNM::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Base, &Detail};
		const std::array Defaults{&BaseDefault, &DetailDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::BlendNormalsRNM, EMaterialProgramValueType::Float3, Inputs, Defaults));
	}

	auto DMaterialExpressionUVChannel::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Channel};
		const std::array Defaults{&ChannelDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs, Defaults));
	}

	auto DMaterialExpressionMakeSurface::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&BaseColor, &Normal, &Metallic, &Roughness, &AmbientOcclusion, &Emissive, &Opacity, &OpacityMask};
		const std::array Defaults{&BaseColorDefault, &NormalDefault, &MetallicDefault, &RoughnessDefault, &AmbientOcclusionDefault, &EmissiveDefault, &OpacityDefault, &OpacityMaskDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface, Inputs, Defaults));
	}

	auto DMaterialExpressionAppendVector::Build(MIR::FEmitter& Emitter) const -> void
	{
		std::vector<uint32> Scalars;
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		for (size_t Slot = 0; Slot < Inputs.size(); ++Slot)
		{
			const auto& Input = *Inputs[Slot];
			if (!Input.ExpressionId.IsValid() && (Input.OutputIndex != 0 || Input.OutputId.IsValid()))
				return Emitter.Fail(EMaterialExpressionError::DisconnectedAppendInputOutputSelector);
			if (!Defaults[Slot]->empty() && (Defaults[Slot]->size() > 3
				|| !std::ranges::all_of(*Defaults[Slot], [](float Value) { return std::isfinite(Value); })))
				return Emitter.Fail(EMaterialExpressionError::AppendVectorDefaultContainOneThreeFiniteComponents);
			const auto Index = Input.ExpressionId.IsValid() ? Emitter.ResolveIndex(Input) : Emitter.Literal(*Defaults[Slot]);
			if (Index == MIR::InvalidIndex) return;
			const auto Type = Emitter.GetNode(Index).ResultType;
			if (Type > EMaterialProgramValueType::Float3) return Emitter.Fail(EMaterialExpressionError::AppendVectorRequiresNumericInputsTotalingAtMostFourComponents);
			const auto Width = static_cast<uint8>(Type) + 1;
			if (Scalars.size() + Width > 4) return Emitter.Fail(EMaterialExpressionError::AppendVectorExceedsFourComponents);
			for (uint8 Channel = 0; Channel < Width; ++Channel)
				Scalars.push_back(Width == 1 ? Index : Emitter.Emit({.Opcode = EMaterialProgramOpcode::Swizzle,
					.ResultType = EMaterialProgramValueType::Float, .Inputs = {Index},
					.Payload = MIR::FSwizzle{1, {Channel, 0, 0, 0}}}));
		}
		return Emitter.Output(0, Emitter.Emit({.Opcode = static_cast<EMaterialProgramOpcode>(
			static_cast<uint8>(EMaterialProgramOpcode::MakeFloat2) + Scalars.size() - 2),
			.ResultType = static_cast<EMaterialProgramValueType>(Scalars.size() - 1), .Inputs = std::move(Scalars)}));
	}

	auto DMaterialExpressionSwizzle::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (Components.empty() || Components.size() > 4) return Emitter.Fail(EMaterialExpressionError::SwizzleSelectOneFourComponents);
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Swizzle,
			static_cast<EMaterialProgramValueType>(Components.size() - 1), Inputs, Defaults, Components));
	}

	auto DMaterialExpressionWorldPosition::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::WorldPosition, .ResultType = EMaterialProgramValueType::Float3}));
	}

	auto DMaterialExpressionTime::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::Time, .ResultType = EMaterialProgramValueType::Float}));
	}

	auto DMaterialExpressionTextureCoordinates::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Channel};
		const std::array Defaults{&ChannelDefault};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs, Defaults));
	}

	namespace
	{
		// Node-specific lowering: publish all sampled channels from one sample instruction.
		auto EmitSampleOutputs(MIR::FEmitter& Emitter, uint32 Sample, bool bDecodeNormal) -> void
		{
			if (Sample == MIR::InvalidIndex) return;
			Emitter.Output(static_cast<uint8>(EMaterialSampleOutput::RGBA), Sample);
			for (const auto& Output : GetMaterialSampleOutputs(EMaterialProgramOpcode::TextureSample2D))
			{
				if (Output.Id == EMaterialSampleOutput::RGBA) continue;
				const bool bDecode = bDecodeNormal && Output.Id == EMaterialSampleOutput::RGB;
				const uint8 Width = bDecode ? 2 : static_cast<uint8>(Output.Type) + 1;
				const auto Selected = Emitter.Emit({.Opcode = EMaterialProgramOpcode::Swizzle,
					.ResultType = static_cast<EMaterialProgramValueType>(Width - 1), .Inputs = {Sample},
					.Payload = MIR::FSwizzle{Width, {Output.FirstComponent,
						static_cast<uint8>(Width > 1 ? 1 : 0), static_cast<uint8>(Width > 2 ? 2 : 0)}}});
				Emitter.Output(static_cast<uint8>(Output.Id), bDecode
					? Emitter.Emit({.Opcode = EMaterialProgramOpcode::DecodeNormalRG,
						.ResultType = EMaterialProgramValueType::Float3, .Inputs = {Selected}}) : Selected);
			}
		}
	}

	auto DMaterialExpressionTextureSample2D::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (!UV.ExpressionId.IsValid() && (UV.OutputIndex != 0 || UV.OutputId.IsValid())) return Emitter.Fail(EMaterialExpressionError::DisconnectedUVInputOutputSelector);
		const auto Coordinates = UV.ExpressionId.IsValid() ? Emitter.ResolveIndex(UV) : Emitter.Coordinates();
		const auto ResourceValue = Emitter.Resolve(Texture);
		uint32 Sample;
		if (const auto* Default = ResourceValue.GetTexture())
		{
			const std::array White{1.f, 1.f, 1.f, 1.f};
			const std::array Black{0.f, 0.f, 0.f, 1.f};
			const std::array Normal{.5f, .5f, 1.f, 1.f};
			Sample = Emitter.Literal(Default->Fallback == EMaterialTextureFallback::White ? White
				: Default->Fallback == EMaterialTextureFallback::FlatRGNormal ? Normal : Black);
		}
		else
			Sample = Emitter.Emit({.Opcode = EMaterialProgramOpcode::TextureSample2D,
				.ResultType = EMaterialProgramValueType::Float4, .Inputs = {*ResourceValue.GetIndex(), Coordinates}});
		EmitSampleOutputs(Emitter, Sample, Emitter.IsNormalTexture(ResourceValue));
	}

	auto DMaterialExpressionTextureSampleParameter2D::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (!UV.ExpressionId.IsValid() && (UV.OutputIndex != 0 || UV.OutputId.IsValid())) return Emitter.Fail(EMaterialExpressionError::DisconnectedUVInputOutputSelector);
		const auto Resource = Emitter.Parameter(Metadata.Id, EMaterialParameterType::Texture);
		Emitter.Output(static_cast<uint8>(EMaterialSampleOutput::Texture), Resource);
		const auto Coordinates = UV.ExpressionId.IsValid() ? Emitter.ResolveIndex(UV) : Emitter.Coordinates();
		const auto Sample = Emitter.Emit({.Opcode = EMaterialProgramOpcode::TextureSample2D,
			.ResultType = EMaterialProgramValueType::Float4, .Inputs = {Resource, Coordinates}});
		EmitSampleOutputs(Emitter, Sample, TextureUsage == ETextureUsage::Normal);
	}

	auto DMaterialExpressionGetSurfaceAttributes::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (AttributeMask == 0) return Emitter.Fail(EMaterialExpressionError::SurfaceAttributeOutputSelected);
		const auto Index = Emitter.ResolveIndex(Surface);
		if (Index == MIR::InvalidIndex) return;
		const auto& Base = Emitter.GetNode(Index);
		if (Base.Opcode != EMaterialProgramOpcode::MakeSurface || Base.Inputs.size() != 8)
			return Emitter.Fail(EMaterialExpressionError::SurfaceAttributeInputResolveConstructedSurface);
		for (uint8 Index = 0; Index < 8; ++Index)
			if (AttributeMask & (1u << Index)) Emitter.Output(Index, Base.Inputs[Index]);
	}

	auto DMaterialExpressionSetSurfaceAttributes::Build(MIR::FEmitter& Emitter) const -> void
	{
		const auto Index = Emitter.ResolveIndex(Surface);
		if (Index == MIR::InvalidIndex) return;
		const auto& Base = Emitter.GetNode(Index);
		if (Base.Opcode != EMaterialProgramOpcode::MakeSurface || Base.Inputs.size() != 8)
			return Emitter.Fail(EMaterialExpressionError::SurfaceOverrideInputResolveConstructedSurface);
		MIR::FNode Node{.Opcode = EMaterialProgramOpcode::MakeSurface,
			.ResultType = EMaterialProgramValueType::Surface, .Inputs = Base.Inputs};
		uint8 Seen = 0;
		for (const auto& Attribute : Attributes)
		{
			const auto Slot = static_cast<uint8>(Attribute.Attribute);
			if (Slot >= 8 || (Seen & (1u << Slot))) return Emitter.Fail(EMaterialExpressionError::SurfaceOverrideContainsInvalidDuplicateAttribute);
			Seen |= static_cast<uint8>(1u << Slot);
			Node.Inputs[Slot] = Emitter.ResolveIndex(Attribute.Source);
		}
		return Emitter.Output(0, Emitter.Emit(std::move(Node)));
	}
}
