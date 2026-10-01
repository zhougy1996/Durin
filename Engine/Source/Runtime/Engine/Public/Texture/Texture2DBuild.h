#pragma once
#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA

#include "Texture/TextureBuildOperation.h"

#include "Asset/DerivedDataCacheKeyProxy.h"
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

	// Separates deterministic build/DDC identity from the Engine request serial
	// used to enforce latest-wins result application for one live object.
	struct FTexture2DBuildInputIdentity
	{
		FXxHash128 SourceIdentity;
		FTexture2DBuildSettings Settings;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Invalid;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Invalid;
		uint32 BuilderVersion = 0;

		auto operator==(const FTexture2DBuildInputIdentity&) const -> bool = default;
	};

	// Identifies whether Engine loaded cached data or ran the local build.
	enum class ETexture2DBuildProductOrigin : uint8
	{
		CacheHit,
		Rebuilt
	};

	// This cancellation value is borrowed only for the duration of Build.
	struct FTexture2DBuildExecutionControl
	{
		std::function<bool()> ShouldCancel;
	};

	// Detached Engine-owned CPU product. Applying it remains a separate
	// GameThread operation and does not execute build code.
	struct FTexture2DBuildProduct
	{
		FTexturePlatformData PlatformData;
		FCacheKeyProxy DerivedDataKey;
		uint32 BuilderVersion = 0;
		ETexture2DBuildProductOrigin Origin = ETexture2DBuildProductOrigin::Rebuilt;
	};

	// Operation boundary for detached consumers such as scene import. Engine records
	// build diagnostics; callers receive only failure disposition and input reasons.
	ENGINE_API auto BuildTexture2DDetached(const FTexture2DBuildRequest& Request,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr)
		-> std::expected<FTexture2DBuildProduct, FTextureBuildOperationError>;

	// Diagnostic seam for the Engine compiling manager. Invokes the build module
	// gate. The returned product and identity contain only Engine-owned values.
	// Failure clears OutProduct; OutIdentity retains observed input/builder
	// identity for compilation diagnostics even when the build fails.
	ENGINE_API auto BuildTexture2DPlatformData(const FTexture2DBuildRequest& Request,
		FTexture2DBuildProduct& OutProduct,
		FTexture2DBuildInputIdentity& OutIdentity,
		const FTexture2DBuildExecutionControl* ExecutionControl = nullptr) -> std::expected<void, FTexture2DBuildError>;
}

#endif
