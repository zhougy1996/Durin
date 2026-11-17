#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"
#include "Materials/MaterialParameterCollection.h"

#include "MaterialExpressionCollectionParameter.gen.h"

namespace Durin
{
	// Reads one numeric declaration from the current world's collection snapshot.
	DCLASS()
	class DMaterialExpressionCollectionParameter : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCollectionParameter(
			const FObjectInitializer& Initializer) : Super(Initializer) {}

		DPROPERTY(Edit)
		TObjectPtr<DMaterialParameterCollection> Collection;

		DPROPERTY(Edit)
		FGuid ParameterId;

		ENGINE_API auto Build(MIR::FEmitter& Emitter) const -> void override;
	};
}
