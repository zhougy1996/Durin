#pragma once

#include "CoreMinimal.h"

#include "Materials/MaterialExpression.h"

#include "MaterialExpressionNumeric.gen.h"

namespace Durin
{
	// Groups numeric expressions; concrete families own their applicable pins and defaults.
	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialExpressionNumeric : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionNumeric(const FObjectInitializer& Initializer) : Super(Initializer) {}
	};
}
