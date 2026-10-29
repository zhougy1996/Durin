#pragma once

#include "CoreMinimal.h"

#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

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

	// Build failure context. Diagnostics are presented once by the owning operation.
	struct [[nodiscard]] FTextureBuildError
	{
		ETextureBuildFailure Code = ETextureBuildFailure::BuildFailed;
		ETextureBuildStage Stage = ETextureBuildStage::Module;
		std::string Diagnostic;
	};

} // namespace Durin

#endif
