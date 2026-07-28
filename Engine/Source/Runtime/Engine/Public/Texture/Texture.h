#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"

#include "Texture.gen.h"

namespace Durin
{
	// Reports the persistent result of source decoding, platform build, and GPU upload.
	DENUM(DisplayName = "Texture Build Status")
	enum class ETextureBuildStatus : uint8
	{
		Unbuilt DMETA(DisplayName = "Not Built"), // No valid platform data is installed.
		Ready,             // Platform data is valid and its render build is queued.
		MissingSource,     // The copied source path is empty or missing.
		DecodeFailure,     // Source bytes could not be decoded.
		BuildFailure,      // Platform-data construction failed.
		UploadFailure,     // The current render-resource revision failed.
		UnsupportedFormat, // The selected platform format is unavailable.
	};

	// Tracks the revisioned render-thread lifecycle of a texture resource.
	DENUM()
	enum class ERenderResourceState : uint8
	{
		Idle,
		Pending,
		Building,
		Ready,
		Failed,
		Released,
	};

	// Identifies the current render-resource revision's actionable failure boundary.
	enum class ETextureRenderFailure : uint8
	{
		None,
		UnsupportedFormat,
		CreateOrUpload,
	};

	enum class ETextureDerivedDataStatus : uint8
	{
		None,
		Hit,
		Missing,
		Corrupt,
		Incompatible,
		Rebuilt,
		WriteFailure,
		SourceUnavailableCached,
		SourceUnavailable,
		CookedLoaded,
		CookedFailure
	};

	struct FTextureDerivedDataDiagnostic
	{
		ETextureDerivedDataStatus Status = ETextureDerivedDataStatus::None;
		std::string Key;
		std::string Message;
		bool bSourceDecoderInvoked = false;
	};

	enum class ETextureSourceStatus : uint8
	{
		NoSource,
		Available,
		Changed,
		Missing,
		Invalid
	};

	struct FTextureSourceDiagnostic
	{
		ETextureSourceStatus Status = ETextureSourceStatus::NoSource;
		std::string PhysicalPath;
		std::string Message;
	};

	// Common reflected boundary for texture assets. Resource ownership remains
	// in the concrete leaves until the lifecycle migration stage.
	DCLASS(Abstract)
	class DTexture : public DObject
	{
		GENERATED_BODY()

	protected:
		ENGINE_API explicit DTexture(const FObjectInitializer& ObjectInitializer);
	};
}
