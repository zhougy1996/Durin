#pragma once

#include "Materials/MaterialTypes.h"

namespace Durin
{
	enum class EMaterialParameterValueType : uint8
	{
		Scalar,
		Vector,
		Texture,
	};

	enum class EMaterialParameterPresentation : uint8
	{
		Drag,
		Color,
		AssetPicker,
	};

	struct FMaterialParameterDescriptor
	{
		std::string_view Name;
		const char* Label;
		EMaterialParameterValueType ValueType;
		EMaterialParameterPresentation Presentation;
		std::array<float, 3> DefaultValue{};
		float Minimum = 0.0f;
		float Maximum = 0.0f;
	};

	inline constexpr std::array MaterialParameterDescriptors{
		FMaterialParameterDescriptor{
			MaterialParameterBaseColor, "Base Color", EMaterialParameterValueType::Vector,
			EMaterialParameterPresentation::Color, {0.95f, 0.62f, 0.22f}, 0.0f, 1.0f},
		FMaterialParameterDescriptor{
			MaterialParameterBaseColorTexture, "Base Color Texture", EMaterialParameterValueType::Texture,
			EMaterialParameterPresentation::AssetPicker},
		FMaterialParameterDescriptor{
			MaterialParameterOpacity, "Opacity", EMaterialParameterValueType::Scalar,
			EMaterialParameterPresentation::Drag, {1.0f}, 0.0f, 1.0f},
		FMaterialParameterDescriptor{
			MaterialParameterSpecularStrength, "Specular Strength", EMaterialParameterValueType::Scalar,
			EMaterialParameterPresentation::Drag, {0.35f}, 0.0f, 1.0f},
		FMaterialParameterDescriptor{
			MaterialParameterShininess, "Shininess", EMaterialParameterValueType::Scalar,
			EMaterialParameterPresentation::Drag, {32.0f}, 1.0f, 256.0f},
	};

	constexpr auto GetMaterialParameterMapName(EMaterialParameterValueType Type, bool bOverrides) -> const char*
	{
		switch (Type)
		{
		case EMaterialParameterValueType::Scalar: return bOverrides ? "ScalarParameterOverrides" : "ScalarParameters";
		case EMaterialParameterValueType::Vector: return bOverrides ? "VectorParameterOverrides" : "VectorParameters";
		case EMaterialParameterValueType::Texture: return bOverrides ? "TextureParameterOverrides" : "TextureParameters";
		default: return "";
		}
	}
}
