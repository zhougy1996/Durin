#pragma once

#include "CoreMinimal.h"
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/TextureBuildOperation.h"

#include "Texture/Texture2D.h"
#include "Texture/Texture2DBuildTypes.h"

namespace Durin
{
	// Detached source snapshot and settings; mip acquisition belongs to execution.
	struct FTexture2DBuildRequest
	{
		FTextureSource Source;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	// Captures metadata and retained storage without reading or decoding source bytes.
	ENGINE_API auto MakeTexture2DBuildRequest(const FTextureSource& Source,
		const FTexture2DBuildSettings& Settings = {})
		-> std::expected<FTexture2DBuildRequest, FTexture2DInputError>;
	ENGINE_API auto ValidateTexture2DBuildSource(const FTextureSource& Source)
		-> std::expected<void, FTexture2DInputError>;

	// Operation boundary for detached consumers such as scene import. Engine records
	// build diagnostics; callers receive only failure disposition and input reasons.
	ENGINE_API auto BuildTexture2DDetached(const FTexture2DBuildRequest& Request)
		-> std::expected<FTexturePlatformData, FTextureBuildOperationError>;


}

#endif
