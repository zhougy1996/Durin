#pragma once

#include "Materials/MaterialInterface.h"
#include "Texture/Texture2D.h"

#include "MaterialInstance.gen.h"

namespace Durin
{
	DCLASS()
	class ENGINE_API DMaterialInstance : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		explicit DMaterialInstance(const FObjectInitializer& ObjectInitializer);

		auto SetParent(DMaterialInterface* InParent) -> bool;
		auto GetParent() const -> DMaterialInterface* override;
		auto SetScalarParameterValue(std::string_view Name, float Value) -> void;
		auto SetVectorParameterValue(std::string_view Name, const FVector3& Value) -> void;
		auto SetTextureParameterValue(std::string_view Name, DTexture2D* Value) -> void;
		auto ClearScalarParameterValue(std::string_view Name) -> bool;
		auto ClearVectorParameterValue(std::string_view Name) -> bool;
		auto ClearTextureParameterValue(std::string_view Name) -> bool;
		auto HasScalarParameterOverride(std::string_view Name) const -> bool;
		auto HasVectorParameterOverride(std::string_view Name) const -> bool;
		auto HasTextureParameterOverride(std::string_view Name) const -> bool;
		auto GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool override;
		auto GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool override;
		auto GetTextureParameterValue(std::string_view Name, DTexture2D*& OutValue) const -> bool override;
		auto BeginDestroy() -> void override;
		auto PostLoad(std::string& OutError) -> bool override;

	private:
		auto OnParentRenderDataDirty(EMaterialRenderDirtyFlags DirtyFlags) -> void;

		DPROPERTY(Edit)
		TObjectPtr<DMaterialInterface> Parent;

		DPROPERTY(Edit)
		std::unordered_map<std::string, float> ScalarParameterOverrides;

		DPROPERTY(Edit)
		std::unordered_map<std::string, FVector3> VectorParameterOverrides;

		DPROPERTY(Edit)
		std::unordered_map<std::string, TObjectPtr<DTexture2D>> TextureParameterOverrides;

		friend class DMaterialInterface;
	};
}
