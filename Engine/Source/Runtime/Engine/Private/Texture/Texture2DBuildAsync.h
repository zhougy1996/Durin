#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITOR
#include "Texture/Texture2DBuild.h"
#include "DerivedDataBuildSession.h"

namespace Durin::TexturePrivate
{
	// The caller retains Owner through callback retirement. All outcomes invoke Completion once.
	auto BuildTexture2DAsync(FTexture2DBuildRequest Request,
		DerivedData::IRequestOwner& Owner, std::function<bool()> ShouldCancel,
		std::function<void(std::expected<FTexturePlatformData, FTexture2DBuildError>,
			FTexture2DBuildInputIdentity)> Completion) -> void;
}
#endif
