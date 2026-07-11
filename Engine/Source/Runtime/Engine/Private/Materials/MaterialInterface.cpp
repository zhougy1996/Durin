#include "Materials/MaterialInterface.h"

namespace Durin
{
	DMaterialInterface::DMaterialInterface(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
	}

	auto DMaterialInterface::GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool
	{
		return false;
	}

	auto DMaterialInterface::GetParent() const -> DMaterialInterface*
	{
		return nullptr;
	}

	auto DMaterialInterface::GetRenderData() const -> FMaterialRenderData
	{
		FMaterialRenderData Result;
		FVector3 BaseColor;
		if (GetVectorParameterValue(MaterialParameterBaseColor, BaseColor))
		{
			Result.BaseColor.r = static_cast<float>(std::clamp(BaseColor.x, 0.0, 1.0));
			Result.BaseColor.g = static_cast<float>(std::clamp(BaseColor.y, 0.0, 1.0));
			Result.BaseColor.b = static_cast<float>(std::clamp(BaseColor.z, 0.0, 1.0));
		}

		float Opacity = Result.BaseColor.a;
		if (GetScalarParameterValue(MaterialParameterOpacity, Opacity))
		{
			Result.BaseColor.a = std::clamp(Opacity, 0.0f, 1.0f);
		}
		return Result;
	}
}
