#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialProgramTypes.h"
#include "Materials/MaterialExpression.h"

#include "MaterialExpressionCustomOutput.gen.h"

namespace Durin
{
	// Shader output terminals publish typed roots instead of graph value pins.
	DCLASS(Abstract, NoClassDefaultObject)
	class DMaterialExpressionCustomOutput : public DMaterialExpression
	{
		GENERATED_BODY()
	public:
		explicit DMaterialExpressionCustomOutput(const FObjectInitializer& Initializer) : Super(Initializer) {}
		virtual auto GetFunctionName() const -> std::string_view = 0;
		virtual auto GetNumOutputs() const -> int32 = 0;
		virtual auto GetDisplayName() const -> std::string_view = 0;

#if DURIN_WITH_EDITORONLY_DATA
		// The active shading model validates these connections during compilation.
		auto IsInputConnectionRequired(int32 Index) const -> bool override { return false; }
#endif
	};
}
