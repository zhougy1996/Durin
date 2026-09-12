#pragma once

#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialTypes.h"
#include "Materials/MaterialFunctionTypes.h"

namespace Durin::Editor::Material
{
	inline constexpr std::array MaterialSurfaceNames{"Base Color", "Normal", "Metallic", "Roughness", "Ambient Occlusion", "Emissive", "Opacity", "Opacity Mask"};
	inline auto DescribeFunctionDefault(const FMaterialFunctionDefault& Value) -> std::string
	{
		switch (Value.Kind)
		{
		case EMaterialFunctionDefaultKind::None: return "No default";
		case EMaterialFunctionDefaultKind::Numeric: return std::format("Default: ({:g}, {:g}, {:g}, {:g})", Value.Numeric.X, Value.Numeric.Y, Value.Numeric.Z, Value.Numeric.W);
		case EMaterialFunctionDefaultKind::Texture: return Value.TextureFallback == EMaterialTextureFallback::White ? "Default: white texture"
			: Value.TextureFallback == EMaterialTextureFallback::Black ? "Default: black texture" : "Default: flat RG normal texture";
		case EMaterialFunctionDefaultKind::Surface: return "Default: declared Surface attributes";
		case EMaterialFunctionDefaultKind::Input: return "Default: another function input";
		case EMaterialFunctionDefaultKind::UV0: return "Default: UV channel 0";
		}
		return "Unknown default";
	}
	inline auto GetProgramType(EMaterialParameterType Type)
		-> EMaterialProgramValueType
	{
		switch (Type)
		{
		case EMaterialParameterType::Scalar: return EMaterialProgramValueType::Float;
		case EMaterialParameterType::Vector2: return EMaterialProgramValueType::Float2;
		case EMaterialParameterType::Vector: return EMaterialProgramValueType::Float3;
		case EMaterialParameterType::Vector4: return EMaterialProgramValueType::Float4;
		case EMaterialParameterType::Texture: return EMaterialProgramValueType::Texture2D;
		}
		return EMaterialProgramValueType::Float;
	}

	inline auto GetProgramTypeName(EMaterialProgramValueType Type) -> const char*
	{
		switch (Type)
		{
		case EMaterialProgramValueType::Float: return "Float";
		case EMaterialProgramValueType::Float2: return "Float2";
		case EMaterialProgramValueType::Float3: return "Float3";
		case EMaterialProgramValueType::Float4: return "Float4";
		case EMaterialProgramValueType::Texture2D: return "Texture2D";
		case EMaterialProgramValueType::Surface: return "Surface";
		}
		return "Unknown";
	}

	inline auto MakeParameterValue(
		EMaterialProgramValueType Type,
		const FMaterialProgramLiteral& Literal) -> FMaterialParameterValue
	{
		switch (Type)
		{
		case EMaterialProgramValueType::Float:
			return FMaterialParameterValue::MakeScalar(Literal.X);
		case EMaterialProgramValueType::Float2:
			return FMaterialParameterValue::MakeVector2({Literal.X, Literal.Y});
		case EMaterialProgramValueType::Float3:
			return FMaterialParameterValue::MakeVector(
				{Literal.X, Literal.Y, Literal.Z});
		case EMaterialProgramValueType::Float4:
			return FMaterialParameterValue::MakeVector4(
				{Literal.X, Literal.Y, Literal.Z, Literal.W});
		default: return {};
		}
	}

	inline auto GetParameterType(EMaterialProgramValueType Type) -> std::optional<EMaterialParameterType>
	{
		switch (Type)
		{
		case EMaterialProgramValueType::Float: return EMaterialParameterType::Scalar;
		case EMaterialProgramValueType::Float2: return EMaterialParameterType::Vector2;
		case EMaterialProgramValueType::Float3: return EMaterialParameterType::Vector;
		case EMaterialProgramValueType::Float4: return EMaterialParameterType::Vector4;
		case EMaterialProgramValueType::Texture2D: return EMaterialParameterType::Texture;
		default: return std::nullopt;
		}
	}

	inline auto ReadParameterLiteral(EMaterialProgramValueType Type,
		const FMaterialParameterValue& Value) -> FMaterialProgramLiteral
	{
		switch (Type)
		{
		case EMaterialProgramValueType::Float: return {Value.ScalarValue};
		case EMaterialProgramValueType::Float2:
			return {static_cast<float>(Value.Vector2Value.x), static_cast<float>(Value.Vector2Value.y)};
		case EMaterialProgramValueType::Float3:
			return {static_cast<float>(Value.VectorValue.x), static_cast<float>(Value.VectorValue.y),
				static_cast<float>(Value.VectorValue.z)};
		case EMaterialProgramValueType::Float4:
			return {static_cast<float>(Value.Vector4Value.x), static_cast<float>(Value.Vector4Value.y),
				static_cast<float>(Value.Vector4Value.z), static_cast<float>(Value.Vector4Value.w)};
		default: return {};
		}
	}
}
