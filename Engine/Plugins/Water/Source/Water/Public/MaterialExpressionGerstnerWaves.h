#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "WaterAPI.h"
#include "MaterialExpressionGerstnerWaves.gen.h"

namespace Durin
{
	DCLASS(DisplayName = "Evaluate Gerstner Waves")
	class DMaterialExpressionGerstnerWaves : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionGerstnerWaves(const FObjectInitializer& Initializer) : Super(Initializer) {}
#if DURIN_WITH_EDITORONLY_DATA
		WATER_API auto GetOutputs() const -> std::span<const FMaterialExpressionOutput> override;
#endif
		WATER_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
