#pragma once

#include "Materials/MaterialInterface.h"
#include "Texture/Texture2D.h"

#include "Material.gen.h"

namespace Durin
{
	DCLASS()
	class ENGINE_API DMaterial : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		explicit DMaterial(const FObjectInitializer& ObjectInitializer);

		auto GetParameterDefinitions() const -> std::span<const FMaterialParameterDefinition> override;
		auto ResolveParameterValue(const FGuid& Id, FResolvedMaterialParameter& OutParameter) const -> bool override;

		auto SetScalarParameterValue(FName Name, float Value) -> bool;
		auto SetVectorParameterValue(FName Name, const FVector3& Value) -> bool;
		auto SetTextureParameterValue(FName Name, DTexture2D* Value) -> bool;
		auto GetScalarParameterValue(FName Name, float& OutValue) const -> bool override;
		auto GetVectorParameterValue(FName Name, FVector3& OutValue) const -> bool override;
		auto GetTextureParameterValue(FName Name, DTexture2D*& OutValue) const -> bool override;
		auto PostLoad(std::string& OutError) -> bool override;

	private:
		// Definition identity and metadata are canonical; only the nested Value fields are editable.
		DPROPERTY()
		std::vector<FMaterialParameterDefinition> ParameterDefinitions;
	};
}
