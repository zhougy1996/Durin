#include "Materials/Material.h"

namespace Durin
{
	DMaterial::DMaterial(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		VectorParameters.emplace(MaterialParameterBaseColor, FVector3(0.95, 0.62, 0.22));
		TextureParameters.emplace(MaterialParameterBaseColorTexture, nullptr);
		ScalarParameters.emplace(MaterialParameterOpacity, 1.0f);
	}

	auto DMaterial::SetScalarParameterValue(std::string_view Name, float Value) -> void
	{
		const std::string Key(Name);
		if (const auto It = ScalarParameters.find(Key); It != ScalarParameters.end() && It->second == Value) return;
		ScalarParameters[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
	}

	auto DMaterial::SetVectorParameterValue(std::string_view Name, const FVector3& Value) -> void
	{
		const std::string Key(Name);
		if (const auto It = VectorParameters.find(Key); It != VectorParameters.end() && It->second == Value) return;
		VectorParameters[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
	}

	auto DMaterial::SetTextureParameterValue(std::string_view Name, DTexture2D* Value) -> void
	{
		const std::string Key(Name);
		if (const auto It = TextureParameters.find(Key); It != TextureParameters.end() && It->second.Get() == Value) return;
		TextureParameters[Key] = Value;
		MarkPackageDirty();
		MarkRenderDataDirty(EMaterialRenderDirtyFlags::ParameterValues);
	}

	auto DMaterial::GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool
	{
		const auto It = ScalarParameters.find(std::string(Name));
		if (It == ScalarParameters.end()) return false;
		OutValue = It->second;
		return true;
	}

	auto DMaterial::GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool
	{
		const auto It = VectorParameters.find(std::string(Name));
		if (It == VectorParameters.end()) return false;
		OutValue = It->second;
		return true;
	}

	auto DMaterial::GetTextureParameterValue(std::string_view Name, DTexture2D*& OutValue) const -> bool
	{
		const auto It = TextureParameters.find(std::string(Name));
		if (It == TextureParameters.end()) return false;
		OutValue = It->second.Get();
		return true;
	}
}
