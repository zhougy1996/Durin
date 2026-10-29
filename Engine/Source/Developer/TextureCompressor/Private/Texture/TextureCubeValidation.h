#pragma once
#include "CoreMinimal.h"

namespace Durin
{
	enum class ETextureCubeValidationError
	{
		EmptyDimensions, AspectRatio, DimensionLimit, PixelLimit, Exposure,
		LDRExposure, FaceDimensionLimit, AllocationLimit, PixelStorage,
		InvalidRadiance, OutputMode, RadianceLimit
	};

	struct FTextureCompressorDiagnostic
	{
		std::string Diagnostic;
		std::optional<ETextureCubeValidationError> CubeInputCause;
	};
}
