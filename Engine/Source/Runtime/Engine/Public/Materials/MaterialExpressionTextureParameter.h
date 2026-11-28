#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpressionParameter.h"
#include "Materials/MaterialProgramTypes.h"
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

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::TextureParameter, .Name = "Texture Object Parameter",
				.Description = "A texture resource for function inputs or multiple samples. For ordinary texture mapping, use Texture Sample Parameter 2D.",
				.Category = "Parameters"};
		}

		DPROPERTY()
		FMaterialTextureValue DefaultValue;

		DPROPERTY()
		ETextureUsage TextureUsage = ETextureUsage::Color;

		ENGINE_API auto GetParameterDefinition() const -> FMaterialParameterDefinition override;
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
		ENGINE_API auto SetParameterValue(const FName& Name, const FMaterialParameterMetadata& Meta,
			EMaterialExpressionSetParameterValueFlags Flags = EMaterialExpressionSetParameterValueFlags::None) -> bool override;
#endif

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
