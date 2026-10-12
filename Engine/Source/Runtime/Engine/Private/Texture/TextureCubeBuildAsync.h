#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITOR
#include "TextureBuildDiagnostics.h"
#include "DerivedDataBuildSession.h"

namespace Durin::TexturePrivate
{
	using FCubeSourceCompletion = std::function<void(std::expected<std::unique_ptr<FTextureCubePlatformData>, FTextureBuildError>)>;
	// All outcomes complete once; caller retains Owner until callback retirement.
	auto BuildTextureCubeSourceAsync(const FTextureSource& Source, bool bSRGB,
		uint32 FaceDimension, float Exposure, DerivedData::IRequestOwner& Owner,
		std::function<bool()> ShouldCancel, FCubeSourceCompletion Completion,
		const FTextureCubeCanonicalBuildInput* PreparedInput = nullptr) -> void;
}
#endif
