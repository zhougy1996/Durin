#pragma once

#include "EngineAPI.h"
#include "DObject/CoreDObject.h"
#include "RHIResources.h"

#include "Texture.gen.h"

namespace Durin
{
	class FTextureAssetResource;
	class FTextureReference;
	class FTextureResourceCompletion;

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

	// Common reflected boundary and render-resource lifecycle owner for texture assets.
	DCLASS(Abstract)
	class DTexture : public DObject
	{
		GENERATED_BODY()

	public:
		ENGINE_API ~DTexture() override;
		ENGINE_API auto BeginDestroy() -> void override;

		ENGINE_API auto GetTextureReferenceRHI() const
			-> FRHITextureReferenceRef;
		ENGINE_API auto GetRenderResourceState() const
			-> ERenderResourceState;
		ENGINE_API auto GetAppliedRenderRevision() const -> uint64;
		auto GetBuildRevision() const -> uint64 { return BuildRevision; }

	protected:
		ENGINE_API explicit DTexture(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto QueueRenderResourceBuild() -> void;
		ENGINE_API auto InvalidateRenderResource() -> void;
		auto GetRenderCompletion() const
			-> const std::shared_ptr<FTextureResourceCompletion>&
		{
			return RenderCompletion;
		}

		virtual auto CreateRenderResourceCandidate(
			FTextureReference* TextureReference,
			uint64 Revision,
			const std::shared_ptr<FTextureResourceCompletion>& Completion)
			-> std::unique_ptr<FTextureAssetResource> = 0;

	private:
		auto ReleaseRenderResources() -> void;

		std::unique_ptr<FTextureReference> TextureReference;
		std::unique_ptr<FTextureAssetResource> RenderResource;
		std::shared_ptr<FTextureResourceCompletion> RenderCompletion;
		bool bTextureReferenceInitializationQueued = false;
		bool bAcceptingRenderResourceBuilds = true;
		uint64 BuildRevision = 0;
	};
}
