#pragma once

#include "DObject/CoreDObject.h"
#include "EngineAPI.h"
#include "Materials/MaterialTypes.h"

#include "MaterialInterface.gen.h"

namespace Durin
{
	DCLASS()
	class ENGINE_API DMaterialInterface : public DObject
	{
		GENERATED_BODY()
	public:
		explicit DMaterialInterface(const FObjectInitializer& ObjectInitializer);

		virtual auto GetScalarParameterValue(std::string_view Name, float& OutValue) const -> bool;
		virtual auto GetVectorParameterValue(std::string_view Name, FVector3& OutValue) const -> bool;
		virtual auto GetParent() const -> DMaterialInterface*;

		auto GetRenderData() const -> FMaterialRenderData;
	};
}
