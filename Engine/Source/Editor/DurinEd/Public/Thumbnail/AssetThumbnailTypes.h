#pragma once

#include "CoreMinimal.h"

#include "CoreGlobals.h"
#include "DObject/AssetPath.h"
#include "DurinEdAPI.h"
#include "Preview/PreviewMeshPaths.h"

namespace Durin
{
	class DClass;
	class DThumbnailRenderer;

	// Describes one exact-class renderer registration, owned by the manager.
	struct FThumbnailRenderingInfo
	{
		std::string AssetClassName;
		std::string RendererClassName;
		// Borrowed registration objects, populated by the manager's lookup.
		DClass* ClassNeedingThumbnail = nullptr;
		DClass* RendererClass = nullptr;
		DThumbnailRenderer* Renderer = nullptr;
	};

	class FRHITexture;

	// Identifies the public lifecycle state of one renderer-neutral thumbnail request.
	enum class EAssetThumbnailState : uint8
	{
		None,
		Pending,
		Ready,
		Failed
	};

	// Selects scheduler ordering without changing thumbnail cache identity.
	enum class EAssetThumbnailPriority : uint8
	{
		Prefetch,
		Visible
	};

	// Selects the fixed TextureCube direction convention used by rendered previews.
	enum class EAssetThumbnailCubeDirectionConvention : uint8
	{
		WorldSpaceReflectionVector = 1
	};

	// Defines output settings that participate directly in persistent cache identity.
	struct FAssetThumbnailOutputSettings
	{
		uint32 Width = 256;
		uint32 Height = 256;
		uint32 ColorSpaceVersion = 1;
		uint32 EncodingVersion = 1;

		auto operator==(const FAssetThumbnailOutputSettings&) const -> bool = default;
	};

	// Defines the shared rendered-thumbnail visual fixture.
	struct FThumbnailVisualContract
	{
		static constexpr std::string_view SphereAssetPath = PreviewMeshPaths::SphereAssetPath;
		static constexpr std::string_view OutputEncoding = "PNG";
		static constexpr std::string_view WorkingColorSpace = "Linear-sRGB";
		static constexpr std::string_view OutputColorSpace = "sRGB";

		FAssetThumbnailOutputSettings Output;
		float BackgroundRed = 0.18f;
		float BackgroundGreen = 0.18f;
		float BackgroundBlue = 0.18f;
		float BackgroundAlpha = 1.0f;
		float CameraDirectionX = 2.6f;
		float CameraDirectionY = -2.6f;
		float CameraDirectionZ = 1.8f;
		float CameraDistance = 4.1f;
		float VerticalFieldOfViewDegrees = 42.0f;
		float NearClipDistance = 0.1f;
		float FarClipDistance = 100.0f;
		float KeyLightDirectionX = -2.6f;
		float KeyLightDirectionY = 2.6f;
		float KeyLightDirectionZ = -2.4f;
		float KeyLightIntensity = 1.0f;
		float FillLightIntensity = 0.15f;
		float Exposure = 1.0f;
		float SphereUniformScale = 1.0f;
		float SphereRotationPitchDegrees = 0.0f;
		float SphereRotationYawDegrees = 0.0f;
		float SphereRotationRollDegrees = 0.0f;
		uint32 PostProcessVersion = 1;
		bool bEditorAssistanceEnabled = false;
		bool bOutputOpaque = true;
		EAssetThumbnailCubeDirectionConvention CubeDirectionConvention =
			EAssetThumbnailCubeDirectionConvention::WorldSpaceReflectionVector;
	};

	// Bounds scheduler work and retained CPU, GPU, and persistent cache resources.
	struct FAssetThumbnailBudgets
	{
		uint32 MaximumQueuedJobs = 512;
		uint32 MaximumRendersPerFrame = 1;
		uint32 MaximumParkedRenderedJobs = 64;
		uint32 MaximumRetainedEntries = 4096;
		uint32 ResourcePollIntervalFrames = 4;
		uint32 MaximumResourceWaitFrames = 600;
		// Bound game-thread admission independently of render and resource readiness.
		uint32 MaximumAdmissionsPerFrame = 8;
		double FrameTimeAllowanceSeconds = 0.002;
		// Enabled by the process pool; isolated tools do not subscribe to catalog changes.
		bool bPreGenerateOnAssetChanges = false;
		uint64 CpuPixelBudgetBytes = 64ull * 1024ull * 1024ull;
		uint64 GpuTextureBudgetBytes = 64ull * 1024ull * 1024ull;
	};

	// Exposes one service-owned result without transferring UI texture ownership.
	struct FAssetThumbnailView
	{
		EAssetThumbnailState State = EAssetThumbnailState::None;
		FRHITexture* Texture = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		bool bHasTransparency = false;
		bool bShowTransparencyGrid = true;
		std::string Diagnostic;
	};

} // namespace Durin
