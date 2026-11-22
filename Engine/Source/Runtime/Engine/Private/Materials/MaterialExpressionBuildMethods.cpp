#include "Materials/MaterialExpressionBuild.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionArithmetic.h"
#include "Materials/MaterialExpressionVector.h"
#include "Materials/MaterialExpressionMaterialAttributes.h"
#include "Materials/MaterialExpressionScene.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionSwitch.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialExpressionCustomData.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinates.h"
#include "Materials/MaterialExpressionTextureParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVector4Parameter.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"

namespace Durin
{

	auto DMaterialExpressionVertexInterpolator::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::VertexInterpolator, ResultType, Inputs));
	}

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
		return Emitter.Output(0, Emitter.Literal(Components,
			bTangentSpaceNormal ? EMaterialSpatialKind::Normal : EMaterialSpatialKind::None,
			bTangentSpaceNormal ? EMaterialCoordinateSpace::Tangent : EMaterialCoordinateSpace::None));
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
		if (!DefaultValue.IsValid()) return Emitter.Fail("Texture parameter resource, dimension or sampling policy is invalid.");
		return Emitter.Output(0, Emitter.Parameter(Metadata.Id, EMaterialParameterType::Texture, DefaultValue.Dimension));
	}

	auto DMaterialExpressionCollectionParameter::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (!Collection)
			return Emitter.Fail("Collection parameter expression has no collection asset.");
		Emitter.Output(0, Emitter.CollectionParameter(*Collection, ParameterId));
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

	auto DMaterialExpressionDot::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Dot, EMaterialProgramValueType::Float, Inputs));
	}

	auto DMaterialExpressionCross::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Cross, EMaterialProgramValueType::Float3, Inputs));
	}

	auto DMaterialExpressionLength::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Length, EMaterialProgramValueType::Float, Inputs));
	}

	auto DMaterialExpressionDistance::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Distance, EMaterialProgramValueType::Float, Inputs));
	}

	auto DMaterialExpressionPow::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Base, &Exponent};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Pow, ResultType, Inputs));
	}

#define DURIN_BUILD_MATERIAL_UNARY(ClassName, OpcodeName) \
	auto ClassName::Build(MIR::FEmitter& Emitter) const -> void \
	{ \
		const std::array Inputs{&Input}; \
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::OpcodeName, ResultType, Inputs)); \
	}
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionSqrt, Sqrt)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionExp, Exp)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionLog, Log)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionFloor, Floor)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionCeil, Ceil)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionRound, Round)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionFrac, Frac)
	DURIN_BUILD_MATERIAL_UNARY(DMaterialExpressionSign, Sign)
#undef DURIN_BUILD_MATERIAL_UNARY

	auto DMaterialExpressionFmod::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&A, &B};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Fmod, ResultType, Inputs));
	}

	auto DMaterialExpressionStep::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Edge, &Value};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Step, ResultType, Inputs));
	}

	auto DMaterialExpressionSmoothStep::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Minimum, &Maximum, &Value};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::SmoothStep, ResultType, Inputs));
	}

	auto DMaterialExpressionReflect::Build(MIR::FEmitter& Emitter) const -> void
	{
		const std::array Inputs{&Incident, &Normal};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Reflect, ResultType, Inputs));
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

	static auto AttributeAssignment(MIR::FEmitter& Emitter, const FMaterialExpressionInput* Input) -> std::optional<uint32>
	{
		if (!Input) return {};
		if (Input->IsConnected()) return Emitter.ResolveIndex(*Input);
		if (Input->OutputIndex == 0) return {};
		Emitter.Fail("Disconnected attribute input has an output selector.");
		return MIR::InvalidIndex;
	}

	auto DMaterialExpressionMakeMaterialAttributes::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.Aggregate(MIR::InvalidIndex, [&](EMaterialProperty Property) {
			return AttributeAssignment(Emitter, GetExpressionInputForProperty(Property));
		}));
	}

	auto DMaterialExpressionBlendMaterialAttributes::Build(MIR::FEmitter& Emitter) const -> void
	{
		const auto Left = Emitter.ResolveIndex(A), Right = Emitter.ResolveIndex(B);
		if (Left == MIR::InvalidIndex || Right == MIR::InvalidIndex) return;
		if (Alpha.Constant.size() != 1 || !std::isfinite(Alpha.Constant[0])
			|| (!Alpha.Connection.IsConnected() && Alpha.Connection.OutputIndex != 0))
			return Emitter.Fail("Attribute blend alpha has an invalid retained default or selector.");
		const auto Weight = Alpha.Connection.IsConnected() ? Emitter.ResolveIndex(Alpha.Connection)
			: Emitter.Literal(Alpha.UseConstant ? std::span<const float>(Alpha.Constant) : std::span<const float>(std::array{.5f}));
		if (Weight == MIR::InvalidIndex || Emitter.GetNode(Weight).ResultType != EMaterialProgramValueType::Float)
			return Emitter.Fail("Attribute blend alpha must be a scalar.");
		Emitter.Output(0, Emitter.Aggregate(MIR::InvalidIndex, [&](EMaterialProperty Property) -> std::optional<uint32> {
			const auto* Field = FMaterialAttributeDefinitionMap::Find(Property);
			const auto AValue = Emitter.Attribute(Left, Property);
			const auto BValue = Emitter.Attribute(Right, Property);
			if (AValue == MIR::InvalidIndex || BValue == MIR::InvalidIndex) return MIR::InvalidIndex;
			auto Value = Emitter.Emit({.Opcode = EMaterialProgramOpcode::Lerp, .ResultType = Field->Semantics.Type,
				.Inputs = {AValue, BValue, Weight}});
			if (Field->Blend == EMaterialAttributeBlend::NormalizedNormal && Value != MIR::InvalidIndex)
				Value = Emitter.Emit({.Opcode = EMaterialProgramOpcode::Normalize, .ResultType = Field->Semantics.Type, .Inputs = {Value}});
			return Value;
		}));
	}

	auto DMaterialExpressionAppendVector::Build(MIR::FEmitter& Emitter) const -> void
	{
		std::vector<uint32> Scalars;
		const std::array Inputs{&A, &B};
		for (size_t Slot = 0; Slot < Inputs.size(); ++Slot)
		{
			const auto& Input = Inputs[Slot]->Connection;
			const auto& Default = Inputs[Slot]->Constant;
			if (!Input.IsConnected() && Input.OutputIndex != 0)
				return Emitter.Fail("Disconnected append input has an output selector.");
			if (Default.empty() || Default.size() > 3
				|| !std::ranges::all_of(Default, [](float Value) { return std::isfinite(Value); }))
				return Emitter.Fail("Append Vector default must contain one to three finite components.");
			const auto Index = Input.IsConnected() ? Emitter.ResolveIndex(Input) : Emitter.Literal(Inputs[Slot]->UseConstant ? Default : std::vector<float>{0.f});
			if (Index == MIR::InvalidIndex) return;
			const auto Type = Emitter.GetNode(Index).ResultType;
			if (Type > EMaterialProgramValueType::Float3) return Emitter.Fail("Append Vector requires numeric inputs totaling at most four components.");
			const auto Width = static_cast<uint8>(Type) + 1;
			if (Scalars.size() + Width > 4) return Emitter.Fail("Append Vector exceeds four components.");
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
		if (Components.empty() || Components.size() > 4) return Emitter.Fail("Swizzle must select one to four components.");
		const std::array Inputs{&Input};
		return Emitter.Output(0, Emitter.Numeric(EMaterialProgramOpcode::Swizzle,
			static_cast<EMaterialProgramValueType>(Components.size() - 1), Inputs, Components,
			OutputSpatialKind, OutputCoordinateSpace));
	}

	auto DMaterialExpressionWorldPosition::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::WorldPosition, .ResultType = EMaterialProgramValueType::Float3}));
	}

	auto DMaterialExpressionPrimitiveCustomData::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::PrimitiveCustomData,
			.ResultType = ResultType, .Payload = MIR::FCustomDataRead{DataIndex}}));
	}

	auto DMaterialExpressionInstanceCustomData::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::InstanceCustomData,
			.ResultType = ResultType, .Payload = MIR::FCustomDataRead{DataIndex}}));
	}

	auto DMaterialExpressionTime::Build(MIR::FEmitter& Emitter) const -> void
	{
		return Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::Time, .ResultType = EMaterialProgramValueType::Float}));
	}

#define DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(ClassName, OpcodeValue, TypeValue) \
	auto ClassName::Build(MIR::FEmitter& Emitter) const -> void \
	{ \
		return Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::OpcodeValue, \
			.ResultType = EMaterialProgramValueType::TypeValue})); \
	}

	namespace
	{
		auto SceneCoordinates(MIR::FEmitter& Emitter, const FMaterialNumericInput& Coordinates) -> uint32
		{
			if (!Coordinates.Connection.IsConnected() && Coordinates.Connection.OutputIndex != 0)
			{ Emitter.Fail("Disconnected scene coordinates have an output selector."); return MIR::InvalidIndex; }
			if (Coordinates.Constant.size() != 2 || !std::ranges::all_of(Coordinates.Constant, [](float V) { return std::isfinite(V); }))
			{ Emitter.Fail("Scene texture coordinates must be finite Float2."); return MIR::InvalidIndex; }
			if (Coordinates.Connection.IsConnected()) return Emitter.ResolveIndex(Coordinates.Connection);
			return Coordinates.UseConstant ? Emitter.Literal(Coordinates.Constant)
				: Emitter.Emit({.Opcode = EMaterialProgramOpcode::ScreenPosition, .ResultType = EMaterialProgramValueType::Float2});
		}
	}
	auto DMaterialExpressionSceneTexture::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::SceneTexture, .ResultType = EMaterialProgramValueType::Float4,
			.Inputs = {SceneCoordinates(Emitter, Coordinates)}, .Payload = MIR::FSceneTextureRead{SceneTextureId,
				bFiltered ? EMaterialSceneTextureFilter::Linear : EMaterialSceneTextureFilter::Point}}));
	}
	auto DMaterialExpressionSceneDepth::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.Emit({.Opcode = EMaterialProgramOpcode::SceneDepth, .ResultType = EMaterialProgramValueType::Float3,
			.Inputs = {SceneCoordinates(Emitter, Coordinates)}, .Payload = MIR::FSceneTextureRead{EMaterialSceneTextureId::SceneDepth, EMaterialSceneTextureFilter::Point}}));
	}
	DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(DMaterialExpressionCameraPosition, CameraPosition, Float3)
	DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(DMaterialExpressionCameraVector, CameraVector, Float3)
	DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(DMaterialExpressionObjectPosition, ObjectPosition, Float3)
	DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(DMaterialExpressionVertexNormal, VertexNormal, Float3)
	DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(DMaterialExpressionScreenPosition, ScreenPosition, Float2)
	DURIN_DEFINE_MATERIAL_CONTEXT_BUILD(DMaterialExpressionViewSize, ViewSize, Float2)

#undef DURIN_DEFINE_MATERIAL_CONTEXT_BUILD

	auto DMaterialExpressionStaticBool::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.StaticBool(DeclarationId, DefaultValue));
	}

	auto DMaterialExpressionStaticSwitch::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.StaticSwitch(Condition, ResultType, FalseValue, TrueValue));
	}

	auto DMaterialExpressionQualitySwitch::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.QualitySwitch(ResultType, DefaultValue, Low, High));
	}

	auto DMaterialExpressionFeatureLevelSwitch::Build(MIR::FEmitter& Emitter) const -> void
	{
		Emitter.Output(0, Emitter.FeatureLevelSwitch(ResultType, DefaultValue, ES3_1, SM5, SM6));
	}

	namespace
	{
		auto BuildTransform(MIR::FEmitter& Emitter, EMaterialProgramOpcode Opcode,
			const FMaterialNumericInput& Input, EMaterialCoordinateSpace Source,
			EMaterialCoordinateSpace Destination) -> void
		{
			Emitter.Output(0, Emitter.Transform(Opcode, Input, Source, Destination));
		}
	}

	auto DMaterialExpressionTransformPosition::Build(MIR::FEmitter& Emitter) const -> void
	{
		BuildTransform(Emitter, EMaterialProgramOpcode::TransformPosition, Input, Source, Destination);
	}

	auto DMaterialExpressionTransformDirection::Build(MIR::FEmitter& Emitter) const -> void
	{
		BuildTransform(Emitter, EMaterialProgramOpcode::TransformDirection, Input, Source, Destination);
	}

	auto DMaterialExpressionTransformNormal::Build(MIR::FEmitter& Emitter) const -> void
	{
		BuildTransform(Emitter, EMaterialProgramOpcode::TransformNormal, Input, Source, Destination);
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
			if (!Input.IsConnected() && Input.OutputIndex != 0)
			{
				Emitter.Fail("Disconnected UV input has an output selector.");
				return MIR::InvalidIndex;
			}
			if (UV.Constant.size() != 2
				|| !std::ranges::all_of(UV.Constant, [](float V) { return std::isfinite(V); }))
			{
				Emitter.Fail("Retained numeric default has an invalid width or non-finite component.");
				return MIR::InvalidIndex;
			}
			auto Index = Input.IsConnected() ? Emitter.ResolveIndex(Input)
				: UV.UseConstant ? Emitter.Literal(UV.Constant) : Emitter.Coordinates();
			if (Index != MIR::InvalidIndex && Emitter.GetNode(Index).ResultType != EMaterialProgramValueType::Float2)
			{
				Emitter.Fail("Texture2D coordinates require Float2.");
				return MIR::InvalidIndex;
			}
			return Index;
		}
		auto ResolveCubeDirection(MIR::FEmitter& Emitter, const FMaterialNumericInput& Direction) -> uint32
		{
			if (Direction.Constant.size() != 3 || !std::ranges::all_of(Direction.Constant, [](float V) { return std::isfinite(V); }))
			{
				Emitter.Fail("TextureCube direction requires a finite Float3 default.");
				return MIR::InvalidIndex;
			}
			if (!Direction.Connection.IsConnected() && Direction.Connection.OutputIndex)
			{
				Emitter.Fail("Disconnected Cube direction has an output selector.");
				return MIR::InvalidIndex;
			}
			return Direction.Connection.IsConnected() ? Emitter.ResolveIndex(Direction.Connection)
				: Emitter.Literal(Direction.UseConstant ? Direction.Constant : std::vector<float>{1,0,0});
		}
		template<class SampleExpression>
		auto EmitTextureRead(MIR::FEmitter& Emitter, const SampleExpression& Expression,
			uint32 Resource, uint32 Coordinates, std::span<const float> Fallback = {}) -> uint32
		{
			const auto Mode = Expression.ReadMode;
			if (Mode > EMaterialTextureReadMode::Grad)
			{
				Emitter.Fail("Texture read mode is unsupported.");
				return MIR::InvalidIndex;
			}
			constexpr bool bCube = requires { Expression.Direction; };
			MIR::FNode Node{.Opcode = bCube ? EMaterialProgramOpcode::TextureSampleCube : EMaterialProgramOpcode::TextureSample2D,
				.ResultType = EMaterialProgramValueType::Float4, .Inputs = {Resource, Coordinates},
				.Payload = MIR::FTextureRead{Mode}};
			const auto ResolveOperand = [&](const FMaterialNumericInput& Operand, uint32 Width) {
				if (!Operand.Connection.IsConnected()
					&& Operand.Connection.OutputIndex != 0)
				{
					Emitter.Fail("Disconnected texture read operand has an output selector.");
					return MIR::InvalidIndex;
				}
				if (Operand.Constant.size() != Width || !std::ranges::all_of(Operand.Constant, [](float V) { return std::isfinite(V); }))
				{
					Emitter.Fail("Texture read operand has an invalid width or non-finite default.");
					return MIR::InvalidIndex;
				}
				return Operand.Connection.IsConnected() ? Emitter.ResolveIndex(Operand.Connection)
					: Emitter.Literal(Operand.UseConstant ? Operand.Constant : std::vector<float>(Width, 0.f));
			};
			if (Mode == EMaterialTextureReadMode::Level || Mode == EMaterialTextureReadMode::Bias)
				Node.Inputs.push_back(ResolveOperand(Expression.MipValue, 1));
			else if (Mode == EMaterialTextureReadMode::Grad)
			{
				Node.Inputs.push_back(ResolveOperand(Expression.DDX, bCube ? 3 : 2));
				Node.Inputs.push_back(ResolveOperand(Expression.DDY, bCube ? 3 : 2));
			}
			if (!Fallback.empty())
			{
				// A function's unbound texture folds to its constant fallback only after
				// validating the same active operands as a real resource read.
				std::vector<FMaterialValueSemantics> Semantics{{bCube ? EMaterialProgramValueType::TextureCube : EMaterialProgramValueType::Texture2D}};
				for (size_t Slot = 1; Slot < Node.Inputs.size(); ++Slot)
				{
					if (Node.Inputs[Slot] == MIR::InvalidIndex) return MIR::InvalidIndex;
					Semantics.push_back(Emitter.GetNode(Node.Inputs[Slot]).GetSemantics());
				}
				if (!ResolveMaterialProgramNodeSemantics(Node.Opcode, Node.ResultType, Semantics, nullptr, Mode))
				{
					Emitter.Fail("Texture read operands have incompatible types or evaluation stages.");
					return MIR::InvalidIndex;
				}
				if constexpr (bCube)
				{
					const auto& Direction = Emitter.GetNode(Coordinates);
					const auto V = Direction.GetLiteral();
					if (Direction.Opcode == EMaterialProgramOpcode::Constant && V.X == 0 && V.Y == 0 && V.Z == 0)
					{
						Emitter.Fail("TextureCube direction must be nonzero.");
						return MIR::InvalidIndex;
					}
				}
				return Emitter.Literal(Fallback);
			}
			return Emitter.Emit(std::move(Node));
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
		if (ReadMode > EMaterialTextureReadMode::Grad) return Emitter.Fail("Texture read mode is unsupported.");
		const auto Coordinates = ResolveUV(Emitter, UV);
		const auto ResourceValue = Emitter.Resolve(Texture);
		uint32 Sample;
		if (const auto* Default = ResourceValue.GetTexture())
		{
			if (Default->Dimension != EMaterialTextureDimension::Texture2D) return Emitter.Fail("Texture2D sample requires a 2D resource.");
			const std::array White{1.f, 1.f, 1.f, 1.f};
			const std::array Black{0.f, 0.f, 0.f, 1.f};
			const std::array Normal{.5f, .5f, 1.f, 1.f};
			Sample = EmitTextureRead(Emitter, *this, MIR::InvalidIndex, Coordinates, Default->Fallback == EMaterialTextureFallback::White ? White
				: Default->Fallback == EMaterialTextureFallback::FlatRGNormal ? Normal : Black);
		}
		else
			Sample = EmitTextureRead(Emitter, *this, *ResourceValue.GetIndex(), Coordinates);
		EmitSampleOutputs(Emitter, Sample, Emitter.IsNormalTexture(ResourceValue));
	}

	auto DMaterialExpressionTextureSampleParameter2D::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (DefaultValue.Dimension != EMaterialTextureDimension::Texture2D || !DefaultValue.IsValid()) return Emitter.Fail("Texture sample parameter 2D requires a 2D declaration.");
		const auto Resource = Emitter.Parameter(Metadata.Id, EMaterialParameterType::Texture);
		Emitter.Output(static_cast<uint8>(EMaterialSampleOutput::Texture), Resource);
		if (ReadMode > EMaterialTextureReadMode::Grad) return Emitter.Fail("Texture read mode is unsupported.");
		const auto Coordinates = ResolveUV(Emitter, UV);
		const auto Sample = EmitTextureRead(Emitter, *this, Resource, Coordinates);
		EmitSampleOutputs(Emitter, Sample, TextureUsage == ETextureUsage::Normal);
	}

	auto DMaterialExpressionTextureSampleCube::Build(MIR::FEmitter& Emitter) const -> void
	{
		const auto Coordinates = ResolveCubeDirection(Emitter, Direction);
		const auto Resource = Emitter.Resolve(Texture);
		uint32 Sample;
		if (const auto* Default = Resource.GetTexture())
		{
			if (Default->Dimension != EMaterialTextureDimension::TextureCube || Default->Fallback == EMaterialTextureFallback::FlatRGNormal)
				return Emitter.Fail("Cube sample requires a Cube resource and white/black fallback.");
			const std::array White{1.f,1.f,1.f,1.f}, Black{0.f,0.f,0.f,1.f};
			Sample = EmitTextureRead(Emitter, *this, MIR::InvalidIndex, Coordinates, Default->Fallback == EMaterialTextureFallback::White ? White : Black);
		}
		else Sample = EmitTextureRead(Emitter, *this, *Resource.GetIndex(), Coordinates);
		EmitSampleOutputs(Emitter, Sample, false);
	}

	auto DMaterialExpressionTextureSampleParameterCube::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (DefaultValue.Dimension != EMaterialTextureDimension::TextureCube || !DefaultValue.IsValid() || TextureUsage == ETextureUsage::Normal)
			return Emitter.Fail("Cube sample parameter requires a Cube declaration and white/black fallback.");
		const auto Resource = Emitter.Parameter(Metadata.Id, EMaterialParameterType::Texture, EMaterialTextureDimension::TextureCube);
		Emitter.Output(static_cast<uint8>(EMaterialSampleOutput::Texture), Resource);
		EmitSampleOutputs(Emitter, EmitTextureRead(Emitter, *this, Resource, ResolveCubeDirection(Emitter, Direction)), false);
	}

	auto DMaterialExpressionGetMaterialAttributes::Build(MIR::FEmitter& Emitter) const -> void
	{
		if (AttributeIDs.empty() || AttributeIDs.size() > 64) return Emitter.Fail("Surface attribute output is not selected or exceeds its bound.");
		std::unordered_set<FGuid> Seen;
		for (const auto ID : AttributeIDs)
			if (!FMaterialAttributeDefinitionMap::Find(ID) || !Seen.insert(ID).second)
				return Emitter.Fail("Surface attribute output identity is unknown or duplicated.");
		const auto Index = Emitter.ResolveIndex(Surface);
		if (Index == MIR::InvalidIndex) return;
		for (const auto& Field : FMaterialAttributeDefinitionMap::GetDefinitions())
			if (IsSelected(Field.Property))
				Emitter.Output(uint32(Field.Property), Emitter.Attribute(Index, Field.Property));
	}

	auto DMaterialExpressionSetMaterialAttributes::Build(MIR::FEmitter& Emitter) const -> void
	{
		std::map<EMaterialProperty, const FMaterialExpressionInput*> Assignments;
		for (const auto& Attribute : Attributes)
		{
			const auto Property = Attribute.GetProperty();
			if (!FMaterialAttributeDefinitionMap::Find(Property) || !Assignments.emplace(Property, &Attribute.Input).second)
				return Emitter.Fail("Surface override contains an invalid or duplicate attribute.");
		}
		const auto Prototype = AttributeAssignment(Emitter, &Surface);
		if (Prototype == MIR::InvalidIndex) return;
		Emitter.Output(0, Emitter.Aggregate(Prototype.value_or(MIR::InvalidIndex), [&](EMaterialProperty Property) {
			const auto It = Assignments.find(Property);
			return AttributeAssignment(Emitter, It == Assignments.end() ? nullptr : It->second);
		}));
	}
}
