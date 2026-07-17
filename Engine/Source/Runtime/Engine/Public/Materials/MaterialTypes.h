#pragma once

#include "EngineAPI.h"
#include "Misc/EnumClassFlags.h"

namespace Durin
{
	inline constexpr std::string_view MaterialParameterBaseColor = "BaseColor";
	inline constexpr std::string_view MaterialParameterOpacity = "Opacity";
	inline constexpr std::string_view MaterialParameterSpecularStrength = "SpecularStrength";
	inline constexpr std::string_view MaterialParameterShininess = "Shininess";

	struct FMaterialRenderData
	{
		FVector4f BaseColor{0.95f, 0.62f, 0.22f, 1.0f};
		float SpecularStrength = 0.35f;
		float Shininess = 32.0f;
	};

	enum class EMaterialRenderDirtyFlags : uint8
	{
		None = 0,
		ParameterValues = 1 << 0,
		ParentChain = 1 << 1
	};
	ENUM_CLASS_FLAGS(EMaterialRenderDirtyFlags);

	struct FMaterialRenderUpdate
	{
		uint32 SlotIndex = 0;
		FMaterialRenderData RenderData;
		uint64 MaterialVersion = 0;
		uint64 ComponentRevision = 0;
		EMaterialRenderDirtyFlags DirtyFlags = EMaterialRenderDirtyFlags::None;
	};
}
