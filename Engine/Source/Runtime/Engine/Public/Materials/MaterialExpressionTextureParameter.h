#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialTypes.h"

#include "MaterialExpressionTextureParameter.gen.h"

namespace Durin
{
	// A resource parameter owns texture sampling policy and its applicable usage hint.
	DCLASS()
	class DMaterialExpressionTextureParameter : public DMaterialExpressionParameter
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureParameter(const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY()
		FMaterialTextureValue DefaultValue;

		DPROPERTY()
		ETextureUsage TextureUsage = ETextureUsage::Color;

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
