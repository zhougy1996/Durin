#include "Materials/MaterialExpressionBuild.h"

namespace Durin
{

	auto DMaterialExpressionScalarConstant::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Components{Value};
		return Context.Literal(Components);
	}

	auto DMaterialExpressionScalarParameter::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		return Context.Parameter(Metadata.Id, EMaterialParameterType::Scalar);
	}

	auto DMaterialExpressionVector2Constant::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Components{static_cast<float>(Value[0]), static_cast<float>(Value[1])};
		return Context.Literal(Components);
	}

	auto DMaterialExpressionVector2Parameter::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		return Context.Parameter(Metadata.Id, EMaterialParameterType::Vector2);
	}

	auto DMaterialExpressionVector3Constant::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Components{static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2])};
		return Context.Literal(Components);
	}

	auto DMaterialExpressionVector3Parameter::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		return Context.Parameter(Metadata.Id, EMaterialParameterType::Vector);
	}

	auto DMaterialExpressionVector4Constant::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Components{static_cast<float>(Value[0]), static_cast<float>(Value[1]), static_cast<float>(Value[2]), static_cast<float>(Value[3])};
		return Context.Literal(Components);
	}

	auto DMaterialExpressionVector4Parameter::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		return Context.Parameter(Metadata.Id, EMaterialParameterType::Vector4);
	}

	auto DMaterialExpressionTextureParameter::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		return Context.Parameter(Metadata.Id, EMaterialParameterType::Texture);
	}

	auto DMaterialExpressionAdd::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Context.Numeric(EMaterialProgramOpcode::Add, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionSubtract::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Context.Numeric(EMaterialProgramOpcode::Subtract, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionMultiply::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Context.Numeric(EMaterialProgramOpcode::Multiply, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionDivide::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Context.Numeric(EMaterialProgramOpcode::Divide, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionMinimum::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Context.Numeric(EMaterialProgramOpcode::Minimum, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionMaximum::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		return Context.Numeric(EMaterialProgramOpcode::Maximum, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionNegate::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Negate, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionOneMinus::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::OneMinus, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionAbsolute::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Absolute, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionSaturate::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Saturate, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionNormalize::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Normalize, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionSine::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Sine, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionCosine::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Cosine, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionClamp::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input, &Minimum, &Maximum};
		const std::array Defaults{&InputDefault, &MinimumDefault, &MaximumDefault};
		return Context.Numeric(EMaterialProgramOpcode::Clamp, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionLerp::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&A, &B, &Alpha};
		const std::array Defaults{&ADefault, &BDefault, &AlphaDefault};
		return Context.Numeric(EMaterialProgramOpcode::Lerp, ResultType, Inputs, Defaults);
	}

	auto DMaterialExpressionMakeVector2::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&X, &Y};
		const std::array Defaults{&XDefault, &YDefault};
		return Context.Numeric(EMaterialProgramOpcode::MakeFloat2, EMaterialProgramValueType::Float2, Inputs, Defaults);
	}

	auto DMaterialExpressionMakeVector3::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&X, &Y, &Z};
		const std::array Defaults{&XDefault, &YDefault, &ZDefault};
		return Context.Numeric(EMaterialProgramOpcode::MakeFloat3, EMaterialProgramValueType::Float3, Inputs, Defaults);
	}

	auto DMaterialExpressionMakeVector4::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&X, &Y, &Z, &W};
		const std::array Defaults{&XDefault, &YDefault, &ZDefault, &WDefault};
		return Context.Numeric(EMaterialProgramOpcode::MakeFloat4, EMaterialProgramValueType::Float4, Inputs, Defaults);
	}

	auto DMaterialExpressionSplat2::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Splat2, EMaterialProgramValueType::Float2, Inputs, Defaults);
	}

	auto DMaterialExpressionSplat3::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Splat3, EMaterialProgramValueType::Float3, Inputs, Defaults);
	}

	auto DMaterialExpressionSplat4::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Splat4, EMaterialProgramValueType::Float4, Inputs, Defaults);
	}

	auto DMaterialExpressionDecodeNormalRG::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::DecodeNormalRG, EMaterialProgramValueType::Float3, Inputs, Defaults);
	}

	auto DMaterialExpressionBlendNormalsRNM::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Base, &Detail};
		const std::array Defaults{&BaseDefault, &DetailDefault};
		return Context.Numeric(EMaterialProgramOpcode::BlendNormalsRNM, EMaterialProgramValueType::Float3, Inputs, Defaults);
	}

	auto DMaterialExpressionUVChannel::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Channel};
		const std::array Defaults{&ChannelDefault};
		return Context.Numeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs, Defaults);
	}

	auto DMaterialExpressionMakeSurface::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&BaseColor, &Normal, &Metallic, &Roughness, &AmbientOcclusion, &Emissive, &Opacity, &OpacityMask};
		const std::array Defaults{&BaseColorDefault, &NormalDefault, &MetallicDefault, &RoughnessDefault, &AmbientOcclusionDefault, &EmissiveDefault, &OpacityDefault, &OpacityMaskDefault};
		return Context.Numeric(EMaterialProgramOpcode::MakeSurface, EMaterialProgramValueType::Surface, Inputs, Defaults);
	}

	auto DMaterialExpressionAppendVector::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		std::vector<uint32> Scalars;
		const std::array Inputs{&A, &B};
		const std::array Defaults{&ADefault, &BDefault};
		for (size_t Slot = 0; Slot < Inputs.size(); ++Slot)
		{
			const auto& Input = *Inputs[Slot];
			if (!Input.ExpressionId.IsValid() && (Input.OutputIndex != 0 || Input.OutputId.IsValid()))
				return Context.Fail("Disconnected append input has an output selector.");
			if (!Defaults[Slot]->empty() && (Defaults[Slot]->size() > 3
				|| !std::ranges::all_of(*Defaults[Slot], [](float Value) { return std::isfinite(Value); })))
				return Context.Fail("Append Vector default must contain one to three finite components.");
			const auto Index = Input.ExpressionId.IsValid() ? Context.ResolveIndex(Input) : Context.Literal(*Defaults[Slot]);
			if (Index == InvalidMaterialExpressionIndex) return Index;
			const auto Type = Context.GetNode(Index).ResultType;
			if (Type > EMaterialProgramValueType::Float3) return Context.Fail("Append Vector requires numeric inputs totaling at most four components.");
			const auto Width = static_cast<uint8>(Type) + 1;
			if (Scalars.size() + Width > 4) return Context.Fail("Append Vector exceeds four components.");
			for (uint8 Channel = 0; Channel < Width; ++Channel)
				Scalars.push_back(Width == 1 ? Index : Context.Emit({.Opcode = EMaterialProgramOpcode::Swizzle,
					.ResultType = EMaterialProgramValueType::Float, .Inputs = {Index},
					.Payload = FMaterialIRSwizzle{1, {Channel, 0, 0, 0}}}));
		}
		return Context.Emit({.Opcode = static_cast<EMaterialProgramOpcode>(
			static_cast<uint8>(EMaterialProgramOpcode::MakeFloat2) + Scalars.size() - 2),
			.ResultType = static_cast<EMaterialProgramValueType>(Scalars.size() - 1), .Inputs = std::move(Scalars)});
	}

	auto DMaterialExpressionSwizzle::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		if (Components.empty() || Components.size() > 4) return Context.Fail("Swizzle must select one to four components.");
		const std::array Inputs{&Input};
		const std::array Defaults{&InputDefault};
		return Context.Numeric(EMaterialProgramOpcode::Swizzle,
			static_cast<EMaterialProgramValueType>(Components.size() - 1), Inputs, Defaults, Components);
	}

	auto DMaterialExpressionTextureCoordinates::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const std::array Inputs{&Channel};
		const std::array Defaults{&ChannelDefault};
		return Context.Numeric(EMaterialProgramOpcode::UVChannel, EMaterialProgramValueType::Float2, Inputs, Defaults);
	}

	auto DMaterialExpressionTextureSample2D::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputId.IsValid()) return Context.Fail("Sample output requires an index, not an output GUID.");
		if (OutputIndex != 0) return Context.SampleOutput(*this, OutputIndex);
		if (!UV.ExpressionId.IsValid() && (UV.OutputIndex != 0 || UV.OutputId.IsValid())) return Context.Fail("Disconnected UV input has an output selector.");
		const auto Coordinates = UV.ExpressionId.IsValid() ? Context.ResolveIndex(UV) : Context.Coordinates();
		const auto ResourceValue = Context.Resolve(Texture);
		if (const auto* Default = ResourceValue.GetTexture())
		{
			const std::array White{1.f, 1.f, 1.f, 1.f};
			const std::array Black{0.f, 0.f, 0.f, 1.f};
			const std::array Normal{.5f, .5f, 1.f, 1.f};
			return Context.Literal(Default->Fallback == EMaterialTextureFallback::White ? White
				: Default->Fallback == EMaterialTextureFallback::FlatRGNormal ? Normal : Black);
		}
		const auto Resource = *ResourceValue.GetIndex();
		return Context.Emit({.Opcode = EMaterialProgramOpcode::TextureSample2D,
			.ResultType = EMaterialProgramValueType::Float4, .Inputs = {Resource, Coordinates}});
	}

	auto DMaterialExpressionTextureSampleParameter2D::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputId.IsValid()) return Context.Fail("Sample output requires an index, not an output GUID.");
		if (OutputIndex == 7) return Context.Parameter(Metadata.Id, EMaterialParameterType::Texture);
		if (OutputIndex != 0) return Context.SampleOutput(*this, OutputIndex);
		if (!UV.ExpressionId.IsValid() && (UV.OutputIndex != 0 || UV.OutputId.IsValid())) return Context.Fail("Disconnected UV input has an output selector.");
		const auto Resource = Context.Parameter(Metadata.Id, EMaterialParameterType::Texture);
		const auto Coordinates = UV.ExpressionId.IsValid() ? Context.ResolveIndex(UV) : Context.Coordinates();
		return Context.Emit({.Opcode = EMaterialProgramOpcode::TextureSample2D,
			.ResultType = EMaterialProgramValueType::Float4, .Inputs = {Resource, Coordinates}});
	}

	auto DMaterialExpressionGetSurfaceAttributes::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputId.IsValid() || OutputIndex >= 8 || !(AttributeMask & (1u << OutputIndex)))
			return Context.Fail("Surface attribute output is not selected.");
		const auto Index = Context.ResolveIndex(Surface);
		if (Index == InvalidMaterialExpressionIndex) return Index;
		const auto& Base = Context.GetNode(Index);
		if (Base.Opcode != EMaterialProgramOpcode::MakeSurface || Base.Inputs.size() != 8)
			return Context.Fail("Surface attribute input must resolve to a constructed Surface.");
		return Base.Inputs[OutputIndex];
	}

	auto DMaterialExpressionSetSurfaceAttributes::Build(FMaterialExpressionBuildContext& Context, uint8 OutputIndex, FGuid OutputId) const -> FMaterialExpressionBuildValue
	{
		if (OutputIndex != 0 || OutputId.IsValid()) return Context.Fail("Expression has only its primary output.");
		const auto Index = Context.ResolveIndex(Surface);
		if (Index == InvalidMaterialExpressionIndex) return Index;
		const auto& Base = Context.GetNode(Index);
		if (Base.Opcode != EMaterialProgramOpcode::MakeSurface || Base.Inputs.size() != 8)
			return Context.Fail("Surface override input must resolve to a constructed Surface.");
		FMaterialIRNode Node{.Opcode = EMaterialProgramOpcode::MakeSurface,
			.ResultType = EMaterialProgramValueType::Surface, .Inputs = Base.Inputs};
		uint8 Seen = 0;
		for (const auto& Attribute : Attributes)
		{
			const auto Slot = static_cast<uint8>(Attribute.Attribute);
			if (Slot >= 8 || (Seen & (1u << Slot))) return Context.Fail("Surface override contains an invalid or duplicate attribute.");
			Seen |= static_cast<uint8>(1u << Slot);
			Node.Inputs[Slot] = Context.ResolveIndex(Attribute.Source);
		}
		return Context.Emit(std::move(Node));
	}
}
