#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Serialization/Archive.h"

namespace Durin
{
	enum class ETextureBuildFailure
	{
		InvalidInput,
		BuildFailed,
		Unavailable,
		InvalidBuilderOutput,
		ApplicationFailed,
		Canceled
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
		std::optional<FArchiveFailure> ArchiveCause;
	};

} // namespace Durin

#endif
