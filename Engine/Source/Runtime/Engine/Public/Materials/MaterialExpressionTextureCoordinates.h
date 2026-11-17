#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

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

		DPROPERTY()
		FMaterialNumericInput Channel;

		auto GetAuthoredInputCount() const -> uint32 override { return 1; }

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;

	};
}
