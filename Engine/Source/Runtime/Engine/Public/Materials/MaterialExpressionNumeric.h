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

		ENGINE_API auto GetInputInfo(int32 Index) const -> FMaterialExpressionInputInfo override;
#if DURIN_WITH_EDITORONLY_DATA
		ENGINE_API auto GetOutput(int32 Index) const -> std::optional<FMaterialExpressionOutput> override;
#endif

	protected:
		ENGINE_API virtual auto GetNumericResultType(EMaterialProgramOpcode Opcode) const -> std::optional<EMaterialProgramValueType>;
	};
}
