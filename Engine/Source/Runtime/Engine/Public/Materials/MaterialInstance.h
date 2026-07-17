#pragma once

#include "Materials/MaterialInterface.h"

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
		auto ClearScalarParameterValue(std::string_view Name) -> bool;
		auto ClearVectorParameterValue(std::string_view Name) -> bool;
		auto GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool override;
		auto GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool override;
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

		friend class DMaterialInterface;
	};
}
