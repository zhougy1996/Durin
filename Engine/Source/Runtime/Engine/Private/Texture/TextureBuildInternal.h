#pragma once

#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/Texture2DBuild.h"
#include "Texture/TextureCubeBuild.h"
#include "Texture/ITextureCompressorModule.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataRequestOwner.h"
#include "DerivedDataStatus.h"
#endif

namespace Durin::TexturePrivate
{
	struct FPreparedTextureCubeBuild
	{
		FTextureCubeCanonicalBuildInput CanonicalInput;
		FTextureSource Source;
	};
	auto PrepareTextureCubeBuild(const FTextureCubeBuildRequest& Request)
		-> std::expected<FPreparedTextureCubeBuild, FTextureBuildError>;
	// Internal worker seam: the platform-cache coordinator owns reporting.
	auto BuildTextureCubeWithDiagnostic(const FTextureCubeBuildRequest& Request)
		-> std::expected<FTextureCubeBuildValue, FTextureBuildError>;

	ENGINE_API auto BuildTextureCubeSource(const FTextureSource& Source, bool bSRGB,
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

#if DURIN_WITH_EDITOR
	// The caller retains Owner through callback retirement. All outcomes invoke Completion once.
	auto BuildTexture2DAsync(FTexture2DBuildRequest Request,
		DerivedData::IRequestOwner& Owner, std::function<bool()> ShouldCancel,
		std::function<void(DerivedData::EStatus, std::unique_ptr<FTexturePlatformData>, std::string)> Completion) -> void;

	// All outcomes complete once; caller retains Owner until callback retirement.
	auto BuildTextureCubeSourceAsync(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, DerivedData::IRequestOwner& Owner,
		std::function<bool()> ShouldCancel,
		std::function<void(DerivedData::EStatus, std::unique_ptr<FTextureCubePlatformData>, std::string)> Completion,
		const FTextureCubeCanonicalBuildInput* PreparedInput = nullptr) -> void;
#endif
}
#endif
