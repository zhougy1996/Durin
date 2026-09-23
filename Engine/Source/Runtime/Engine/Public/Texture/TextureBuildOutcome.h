#pragma once

#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace Durin
{
	enum class ETextureBuildFailure
	{
		InvalidInput,
		BuildFailed,
		Unavailable,
		InvalidBuilderOutput,
		ApplicationFailed
	};
	enum class ETextureBuildStage
	{
		Module,
		Normalize,
		Build,
		Apply
	};

	enum class ETextureCubeInputError
	{
		EmptyDimensions, AspectRatio, DimensionLimit, PixelLimit, Exposure,
		LDRExposure, FaceDimensionLimit, AllocationLimit, PixelStorage,
		InvalidRadiance, OutputMode, RadianceLimit
	};

	// Build failure context. Diagnostics are presented once by the owning operation.
	struct [[nodiscard]] FTextureBuildError
	{
		ETextureBuildFailure Code = ETextureBuildFailure::BuildFailed;
		ETextureBuildStage Stage = ETextureBuildStage::Module;
		std::string Diagnostic;
		std::optional<ETextureCubeInputError> CubeInputCause;
	};

} // namespace Durin
