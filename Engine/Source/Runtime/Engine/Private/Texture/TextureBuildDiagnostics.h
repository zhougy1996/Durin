#pragma once

#include "Texture/TextureBuildOperation.h"
#include "Texture/TextureCubeBuildProvider.h"

namespace Durin::TexturePrivate
{
	// Internal worker seam: the platform-cache coordinator owns reporting.
	auto BuildTextureCubeWithDiagnostic(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildError>;

	inline auto ReportBuildFailure(const FTextureBuildError& Error) -> FTextureBuildOperationError
	{
		DURIN_ERROR_CATEGORY("Texture", "Texture build failed (stage {}, code {}): {}",
			static_cast<int>(Error.Stage), static_cast<int>(Error.Code), Error.Diagnostic);
		if (Error.Code == ETextureBuildFailure::InvalidInput)
			return {ETextureBuildOperationFailure::InvalidInput, Error.Diagnostic};
		return {};
	}
}
