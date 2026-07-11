#pragma once

#include "Materials/MaterialInterface.h"

#include "Material.gen.h"

namespace Durin
{
	DCLASS()
	class ENGINE_API DMaterial : public DMaterialInterface
	{
		GENERATED_BODY()
	public:
		explicit DMaterial(const FObjectInitializer& ObjectInitializer);

		auto SetScalarParameterValue(std::string_view Name, float Value) -> void;
		auto SetVectorParameterValue(std::string_view Name, const FVector3& Value) -> void;
		auto GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool override;
		auto GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool override;

	private:
		DPROPERTY(Edit)
		std::unordered_map<std::string, float> ScalarParameters;

		DPROPERTY(Edit)
		std::unordered_map<std::string, FVector3> VectorParameters;
	};
}
