#pragma once

#include "Math/Vector.h"
#include "RendererAPI.h"

#include <functional>
#include <limits>

namespace Durin
{
	// Owns the frozen P1 spatial contracts before GPU resource integration.
	class RENDERER_API FVolumetricCloudSpatialRenderer final
	{
	public:
		enum class EQualityTier : uint8
		{
			Performance,
			High,
			Epic,
			Reference,
		};

		struct FQualityPolicy
		{
			uint32 LinearScaleNumerator = 1;
			uint32 LinearScaleDenominator = 2;
			uint32 PrimarySampleCount = 24;
			uint32 LightSampleCount = 2;
			uint32 TemporalPatternLength = 8;
			float HistoryWeight = 0.90f;

			[[nodiscard]] auto IsFullResolution() const -> bool
			{
				return LinearScaleNumerator == LinearScaleDenominator;
			}
		};

		struct FExtent
		{
			uint32 Width = 0;
			uint32 Height = 0;

			auto operator==(const FExtent&) const -> bool = default;
		};

		struct FViewportRect
		{
			uint32 X = 0;
			uint32 Y = 0;
			uint32 Width = 0;
			uint32 Height = 0;

			auto operator==(const FViewportRect&) const -> bool = default;
		};
		struct FParameters
		{
			double MinimumZ = 1'500.0;
			double MaximumZ = 3'500.0;
			double MaximumDistance = 100'000.0;
			FVector3f BaseFrequency{0.00008f};
			FVector3f DetailFrequency{0.00032f};
			FVector3f WindOffset{0.0f};
			FVector2f WeatherFrequency{0.00004f};
			FVector2f WeatherOffset{0.0f};
			FVector3f LightDirection{0.0f, 0.0f, 1.0f};
			FVector3f LightColor{1.0f};
			float Coverage = 0.55f;
			float DetailErosion = 0.30f;
			float Extinction = 0.0015f;
			float LightExtinction = 0.0020f;
			float Ambient = 0.12f;
			float TransmittanceCutoff = 0.01f;
			uint32 PrimarySampleCount = 32;
			uint32 LightSampleCount = 4;

			[[nodiscard]] RENDERER_API auto IsValid() const -> bool;
		};

		struct FTextureBindings
		{
			class FRHITexture* BaseDensity = nullptr;
			FRHITexture* DetailDensity = nullptr;
			FRHITexture* Weather = nullptr;
			FRHITexture* SceneDepth = nullptr;
			class FRHISampler* DensitySampler = nullptr;

			[[nodiscard]] auto HasRequiredInputs() const -> bool
			{
				return BaseDensity != nullptr && DetailDensity != nullptr
					&& SceneDepth != nullptr && DensitySampler != nullptr;
			}
		};

		enum class ERoute : uint8
		{
			Disabled,
			Compute,
			Fragment
		};

		enum class ERouteReason : uint8
		{
			DisabledOrUnneeded,
			InvalidInputs,
			InvalidExtent,
			Compute,
			ComputePayloadUnavailable,
			ComputeTargetUnavailable,
			ComputeExtentUnsupported,
			FragmentPayloadUnavailable,
			FragmentTargetUnavailable,
			Count
		};

		struct FRouteInputs
		{
			bool bRequested = false;
			bool bRequiredInputsValid = false;
			bool bComputePayloadReady = false;
			bool bComputeTargetReady = false;
			bool bFragmentPayloadReady = false;
			bool bFragmentTargetReady = false;
			uint32 Width = 0;
			uint32 Height = 0;
			uint32 MaxGroupCountX = 0;
			uint32 MaxGroupCountY = 0;
		};

		struct FRouteDecision
		{
			ERoute Route = ERoute::Disabled;
			ERouteReason Reason = ERouteReason::DisabledOrUnneeded;
		};

		struct FSlabRay
		{
			FVector3 Origin{0.0};
			FVector3 Direction{0.0};
			double MinimumZ = 1'500.0;
			double MaximumZ = 3'500.0;
			double MaximumDistance = 100'000.0;
		};

		struct FSlabInterval
		{
			double NearDistance = 0.0;
			double FarDistance = 0.0;
			bool bIntersects = false;
		};

		struct FReferenceSamplers
		{
			std::function<float(const FVector3f&)> BaseDensity;
			std::function<float(const FVector3f&)> DetailDensity;
			std::function<float(const FVector2f&)> Weather;

			[[nodiscard]] auto HasRequiredInputs() const -> bool
			{
				return static_cast<bool>(BaseDensity)
					&& static_cast<bool>(DetailDensity);
			}
		};

		struct FReferenceInput
		{
			FSlabRay Ray;
			bool bInsideFittedViewport = true;
			// Opaque depth is reconstructed before the CPU reference, so the
			// distance is independent of forward/reversed device-depth encoding.
			double OpaqueDistance = std::numeric_limits<double>::infinity();
			FParameters Parameters;
			FReferenceSamplers Samplers;
		};

		struct FReferenceResult
		{
			FVector3f Radiance{0.0f};
			float Transmittance = 1.0f;
			uint32 PrimarySamples = 0;
			uint32 LightSamples = 0;
			bool bIntegrated = false;
		};

		struct FExecutionCounters
		{
			ERoute Route = ERoute::Disabled;
			ERouteReason Reason = ERouteReason::DisabledOrUnneeded;
			uint32 GroupCountX = 0;
			uint32 GroupCountY = 0;
			uint64 PrimarySamples = 0;
			uint64 LightSamples = 0;
			uint64 TargetBytes = 0;
			uint32 Dispatches = 0;
			uint32 Draws = 0;
			uint32 Copies = 0;
			uint32 TargetWidth = 0;
			uint32 TargetHeight = 0;
			uint32 OutputWidth = 0;
			uint32 OutputHeight = 0;
			EQualityTier QualityTier = EQualityTier::High;
		};

		static constexpr uint32 ThreadGroupSize = 8;
		static constexpr uint32 MaximumPrimarySamples = 32;
		static constexpr uint32 MaximumLightSamples = 4;
		static constexpr uint64 BytesPerPixel = 8;
		static constexpr uint64 MaximumRetainedTargetBytes =
			192ull * 1024ull * 1024ull;
		static constexpr uint64 MaximumRetainedTargetBytesPerFamily =
			MaximumRetainedTargetBytes / 3;
		static constexpr EQualityTier DefaultQualityTier = EQualityTier::High;

		static constexpr auto CalculateGroupCount(uint32 Extent) -> uint32
		{
			return Extent / ThreadGroupSize
				+ (Extent % ThreadGroupSize != 0 ? 1u : 0u);
		}

		static auto CalculateTargetBytes(uint32 Width, uint32 Height) -> uint64;
		static auto ResolveQualityPolicy(EQualityTier Tier) -> FQualityPolicy;
		static auto CalculateScaledExtent(
			uint32 Width, uint32 Height, const FQualityPolicy& Policy) -> FExtent;
		static auto CalculateScaledViewport(const FViewportRect& Viewport,
			const FExtent& Output, const FExtent& Target) -> FViewportRect;
		static auto CalculatePolicyKey(EQualityTier Tier) -> uint64;
		static auto CalculateJitter(
			uint64 SuccessfulSequence, const FQualityPolicy& Policy) -> FVector2f;
		static auto SelectRoute(const FRouteInputs& Inputs) -> FRouteDecision;
		static auto IntersectHeightSlab(const FSlabRay& Ray) -> FSlabInterval;
		static auto IntegrateReference(const FReferenceInput& Input)
			-> FReferenceResult;
		static auto MakeExecutionCounters(
			const FRouteInputs& Inputs, const FRouteDecision& Decision,
			uint64 PrimarySamples, uint64 LightSamples) -> FExecutionCounters;
	};
} // namespace Durin
