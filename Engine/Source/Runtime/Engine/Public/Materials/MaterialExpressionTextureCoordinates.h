#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"
#include "Materials/MaterialProgramTypes.h"

#include "MaterialExpressionTextureCoordinates.gen.h"

namespace Durin
{
	// Reads one mesh UV channel as a Float2; transforms belong to upstream math nodes.
	DCLASS()
	class DMaterialExpressionTextureCoordinates : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionTextureCoordinates(const FObjectInitializer& Initializer) : Super(Initializer) {}

		auto GetCreationInfo() const -> FMaterialExpressionCreationInfo override
		{
			return {.Opcode = EMaterialProgramOpcode::TextureCoordinates, .Name = "Texture Coordinates",
				.Description = "Reads a mesh UV channel as Float2. Apply transforms with upstream math nodes.",
				.Category = "Inputs", .FixedOutputType = EMaterialProgramValueType::Float2,
				.SearchKeywords = "UV Channel UVChannel"};
		}

		auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override
		{
			if (Index == 0) return {.Name = "Channel", .Type = EMaterialProgramValueType::Float};
			return {};
		}

		DPROPERTY()
		FMaterialNumericInput Channel;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
