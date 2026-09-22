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
		Ambiguous,
		InvocationFailed,
		InvalidProviderOutput,
		ApplicationFailed
	};
	enum class ETextureBuildStage
	{
		Provider,
		Normalize,
		Recipe,
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
		ETextureBuildStage Stage = ETextureBuildStage::Provider;
		std::string Diagnostic;
		std::optional<ETextureCubeInputError> CubeInputCause;
	};

} // namespace Durin
