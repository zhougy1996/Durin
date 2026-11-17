#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionIO.h"

#include "MaterialExpressionMaterialOutput.gen.h"

namespace Durin
{
	// A graph sink: connections/defaults are authored here, while its owner supplies the domain.
	DCLASS()
	class DMaterialExpressionMaterialOutput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionMaterialOutput(const FObjectInitializer& Initializer) : Super(Initializer) {}
		ENGINE_API auto Serialize(FArchive& Ar) -> void override;
		DPROPERTY()
		FMaterialAttributeRoot Outputs;
		auto GetAuthoredInputCount() const -> uint32 override { return 10; }
		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
