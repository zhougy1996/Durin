#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/TextureBuildOperation.h"
#include "Texture/TextureCubeBuild.h"
#include "Texture/ITextureBuildModule.h"

namespace Durin::TexturePrivate
{
	// Internal worker seam: the platform-cache coordinator owns reporting.
	auto BuildTextureCubeWithDiagnostic(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildError>;

	auto BuildTextureCubeSource(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, ECookTargetPlatform Platform, ECookTargetProfile Profile, bool bPersist,
		const FTextureCubeCanonicalBuildInput* PreparedInput = nullptr)
		-> std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError>;

	inline auto ReportBuildFailure(const FTextureBuildError& Error) -> FTextureBuildOperationError
	{
		if (Error.Code == ETextureBuildFailure::Canceled) return {ETextureBuildOperationFailure::Canceled, {}};
		DURIN_ERROR_CATEGORY("Texture", "Texture build failed (stage {}, code {}): {}",
			static_cast<int>(Error.Stage), static_cast<int>(Error.Code), Error.Diagnostic);
		if (Error.Code == ETextureBuildFailure::InvalidInput)
			return {ETextureBuildOperationFailure::InvalidInput, Error.Diagnostic};
		return {};
	}
}

#endif
