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
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Add, ResultType, Inputs));
	}

	auto DMaterialExpressionSubtract::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Subtract, ResultType, Inputs));
	}

	auto DMaterialExpressionMultiply::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Multiply, ResultType, Inputs));
	}

	auto DMaterialExpressionDivide::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Divide, ResultType, Inputs));
	}

	auto DMaterialExpressionMinimum::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Minimum, ResultType, Inputs));
	}

	auto DMaterialExpressionMaximum::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Maximum, ResultType, Inputs));
	}

	auto DMaterialExpressionNegate::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Negate, ResultType, Inputs));
	}

	auto DMaterialExpressionOneMinus::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::OneMinus, ResultType, Inputs));
	}

	auto DMaterialExpressionAbsolute::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Absolute, ResultType, Inputs));
	}

	auto DMaterialExpressionSaturate::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Saturate, ResultType, Inputs));
	}

	auto DMaterialExpressionNormalize::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Normalize, ResultType, Inputs));
	}

	auto DMaterialExpressionSine::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Sine, ResultType, Inputs));
	}

	auto DMaterialExpressionCosine::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Cosine, ResultType, Inputs));
	}

	auto DMaterialExpressionClamp::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input, &Minimum, &Maximum};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Clamp, ResultType, Inputs));
	}

	auto DMaterialExpressionLerp::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B, &Alpha};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Lerp, ResultType, Inputs));
	}

	auto DMaterialExpressionMakeVector2::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&X, &Y};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2, Inputs));
	}

	auto DMaterialExpressionMakeVector3::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&X, &Y, &Z};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3, Inputs));
	}

	auto DMaterialExpressionMakeVector4::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&X, &Y, &Z, &W};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeFloat4, EMaterialProgramValueType::Float4, Inputs));
	}

	auto DMaterialExpressionSplat2::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Splat2, EMaterialProgramValueType::Float2, Inputs));
	}

	auto DMaterialExpressionSplat3::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Splat3, EMaterialProgramValueType::Float3, Inputs));
	}

	auto DMaterialExpressionSplat4::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Splat4, EMaterialProgramValueType::Float4, Inputs));
	}

	auto DMaterialExpressionBlendNormalsRNM::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Base, &Detail};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::BlendNormalsRNM, EMaterialProgramValueType::Float3, Inputs));
	}

	auto DMaterialExpressionUVChannel::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Channel};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs));
	}

	auto DMaterialExpressionMakeSurface::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&BaseColor, &Normal, &Metallic, &Roughness, &AmbientOcclusion, &Emissive, &Opacity, &OpacityMask};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface, Inputs));
	}

	auto DMaterialExpressionAppendVector::Build(MIR::FEmitter& Emitter) const -> void
	{
		std::vector<uint32> Scalars;
		const std::array Inputs{&A, &B};
		for (size_t Slot = 0; Slot < Inputs.size(); ++Slot)
		{
			const auto& Input = Inputs[Slot]->Connection;
			const auto& Default = Inputs[Slot]->Constant;
			if (!Input.ExpressionId.IsValid() && (Input.OutputIndex != 0 || Input.OutputId.IsValid()))
				return Emitter.Fail(EMaterialExpressionError::DisconnectedAppendInputOutputSelector);
			if (Default.empty() || Default.size() > 3
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); }))
				return Emitter.Fail(EMaterialExpressionError::AppendVectorDefaultContainOneThreeFiniteComponents);
			const auto Index = Input.ExpressionId.IsValid() ? Emitter.ResolveIndex(Input) : Emitter.Literal(Inputs[Slot]->UseConstant ? Default : std::vector<float>{0.f});
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
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Swizzle,
			static_cast<EMaterialProgramValueType>(Components.size() - 1), Inputs, Components));
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
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs));
	}

	namespace
	{
		auto ResolveUV(MIR::FEmitter& Emitter, const FMaterialNumericInput& UV) -> uint32
		{
			const auto& Input = UV.Connection;
			if (!Input.ExpressionId.IsValid() && (Input.OutputIndex != 0 || Input.OutputId.IsValid()))
			{
				Emitter.Fail(EMaterialExpressionError::DisconnectedUVInputOutputSelector);
				return MIR::InvalidIndex;
			}
			if (UV.Constant.empty() || UV.Constant.size() > 2
				|| !std::ranges::all_of(UV.Constant, [](float V) { return std::isfinite(V); }))
			{
				Emitter.Fail(EMaterialExpressionError::RetainedNumericDefaultInvalidWidthNonFiniteComponent);
				return MIR::InvalidIndex;
			}
			auto Index = Input.ExpressionId.IsValid() ? Emitter.ResolveIndex(Input)
				: UV.UseConstant ? Emitter.Literal(UV.Constant) : Emitter.Coordinates();
			if (Index != MIR::InvalidIndex && Emitter.GetNode(Index).ResultType == EMaterialProgramValueType::Float)
				Index = Emitter.Emit({.Opcode = EMaterialProgramOpcode::Splat2,
					.ResultType = EMaterialProgramValueType::Float2, .Inputs = {Index}});
			return Index;
		}
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
		const auto Coordinates = ResolveUV(Emitter, UV);
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
		const auto Resource = Emitter.Parameter(Metadata.Id, EMaterialParameterType::Texture);
		Emitter.Output(static_cast<uint8>(EMaterialSampleOutput::Texture), Resource);
		const auto Coordinates = ResolveUV(Emitter, UV);
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
			const auto& Input = Attribute.Source;
			const auto Width = static_cast<uint32>(GetMaterialSurfaceOutputType(Attribute.Attribute)) + 1;
			if (Input.Constant.empty() || (Input.Constant.size() != 1 && Input.Constant.size() != Width)
				|| !std::ranges::all_of(Input.Constant, [](float V) { return std::isfinite(V); }))
				return Emitter.Fail(EMaterialExpressionError::RetainedNumericDefaultInvalidWidthNonFiniteComponent);
			if (Input.Connection.ExpressionId.IsValid()) Node.Inputs[Slot] = Emitter.ResolveIndex(Input.Connection);
			else if (Input.Connection.OutputIndex != 0 || Input.Connection.OutputId.IsValid())
				return Emitter.Fail(EMaterialExpressionError::DisconnectedNumericInputOutputSelector);
			else if (Input.UseConstant) Node.Inputs[Slot] = Emitter.Literal(Input.Constant);
			const auto Selected = Node.Inputs[Slot];
			if (Selected != MIR::InvalidIndex && Width == 3 && Emitter.GetNode(Selected).ResultType == EMaterialProgramValueType::Float)
				Node.Inputs[Slot] = Emitter.Emit({.Opcode = EMaterialProgramOpcode::Splat3,
					.ResultType = EMaterialProgramValueType::Float3, .Inputs = {Selected}});
		}
		return Emitter.Output(0, Emitter.Emit(std::move(Node)));
	}
}
