#include "Renderers/VolumetricCloudSpatialRenderer.h"

#include "Math/Operations.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Durin
{
	namespace
	{
		template<typename T>
		auto IsFiniteVector(const T& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		template<>
		auto IsFiniteVector(const FVector3f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				   && std::isfinite(Value.z);
		}

		auto Saturate(float Value) -> float
		{
			return std::clamp(Value, 0.0f, 1.0f);
		}

		auto Fract(const FVector3f& Value) -> FVector3f
		{
			return Value - Math::Floor(Value);
		}

		auto Fract(const FVector2f& Value) -> FVector2f
		{
			return Value - Math::Floor(Value);
		}
	} // namespace

	auto FVolumetricCloudSpatialRenderer::FParameters::IsValid() const -> bool
	{
		const float LightLengthSquared = Math::Dot(LightDirection, LightDirection);
		return std::isfinite(MinimumZ) && std::isfinite(MaximumZ)
			   && std::isfinite(MaximumDistance) && MinimumZ < MaximumZ
			   && MaximumDistance > 0.0 && IsFiniteVector(BaseFrequency)
			   && IsFiniteVector(DetailFrequency) && IsFiniteVector(WindOffset)
			   && IsFiniteVector(WeatherFrequency) && IsFiniteVector(WeatherOffset)
			   && IsFiniteVector(LightDirection) && IsFiniteVector(LightColor)
			   && IsFiniteVector(AmbientColor)
			   && std::isfinite(LightLengthSquared) && LightLengthSquared > 1.0e-12f
			   && Coverage >= 0.0f && Coverage <= 1.0f
			   && DetailErosion >= 0.0f && DetailErosion <= 1.0f
			   && std::isfinite(Extinction) && Extinction > 0.0f
			   && std::isfinite(LightExtinction) && LightExtinction >= 0.0f
			   && std::isfinite(Ambient) && Ambient >= 0.0f
			   && std::isfinite(TransmittanceCutoff)
			   && TransmittanceCutoff >= 0.0f && TransmittanceCutoff <= 1.0f
			   && PrimarySampleCount > 0
			   && PrimarySampleCount <= MaximumPrimarySamples
			   && LightSampleCount <= MaximumLightSamples;
	}

	auto FVolumetricCloudSpatialRenderer::CalculateTargetBytes(
		uint32 Width, uint32 Height
	) -> uint64
	{
		constexpr uint64 Maximum = std::numeric_limits<uint64>::max();
		const uint64 Pixels = static_cast<uint64>(Width) * Height;
		return Pixels > Maximum / BytesPerPixel ? Maximum : Pixels * BytesPerPixel;
	}

	auto FVolumetricCloudSpatialRenderer::ResolveQualityPolicy(
		EQualityTier Tier
	) -> FQualityPolicy
	{
		switch (Tier)
		{
		case EQualityTier::Performance:
			return {.PrimarySampleCount = 16, .LightSampleCount = 1, .TemporalPatternLength = 8, .HistoryWeight = 0.88f};
		case EQualityTier::High:
		case EQualityTier::Count:
			return {.PrimarySampleCount = 24, .LightSampleCount = 2, .TemporalPatternLength = 8, .HistoryWeight = 0.90f};
		case EQualityTier::Epic:
			return {.PrimarySampleCount = 32, .LightSampleCount = 4, .TemporalPatternLength = 8, .HistoryWeight = 0.92f};
		case EQualityTier::Reference:
			return {.LinearScaleNumerator = 1, .LinearScaleDenominator = 1, .PrimarySampleCount = 32, .LightSampleCount = 4, .TemporalPatternLength = 1, .HistoryWeight = 0.0f};
		}
		return ResolveQualityPolicy(DefaultQualityTier);
	}

	auto FVolumetricCloudSpatialRenderer::CalculateScaledExtent(
		uint32 Width, uint32 Height, const FQualityPolicy& Policy
	) -> FExtent
	{
		if (Width == 0 || Height == 0 || Policy.LinearScaleNumerator == 0
			|| Policy.LinearScaleDenominator == 0)
		{
			return {};
		}
		auto Scale = [&Policy](uint32 Extent) {
			const uint64 Product = static_cast<uint64>(Extent)
								   * Policy.LinearScaleNumerator;
			const uint64 Rounded = Product / Policy.LinearScaleDenominator
								   + (Product % Policy.LinearScaleDenominator != 0 ? 1ull : 0ull);
			return static_cast<uint32>(std::clamp<uint64>(Rounded, 1, std::numeric_limits<uint32>::max()));
		};
		return {Scale(Width), Scale(Height)};
	}

	auto FVolumetricCloudSpatialRenderer::CalculateScaledViewport(
		const FViewportRect& Viewport, const FExtent& Output, const FExtent& Target
	) -> FViewportRect
	{
		if (Output.Width == 0 || Output.Height == 0 || Target.Width == 0
			|| Target.Height == 0 || Viewport.X > Output.Width
			|| Viewport.Y > Output.Height
			|| Viewport.Width > Output.Width - Viewport.X
			|| Viewport.Height > Output.Height - Viewport.Y)
		{
			return {};
		}
		auto Floor = [](uint32 Value, uint32 TargetSize, uint32 OutputSize) {
			return static_cast<uint32>(
				static_cast<uint64>(Value) * TargetSize / OutputSize
			);
		};
		auto Ceil = [](uint32 Value, uint32 TargetSize, uint32 OutputSize) {
			const uint64 Product = static_cast<uint64>(Value) * TargetSize;
			return static_cast<uint32>(Product / OutputSize + (Product % OutputSize != 0 ? 1ull : 0ull));
		};
		FViewportRect Result;
		Result.X = Floor(Viewport.X, Target.Width, Output.Width);
		Result.Y = Floor(Viewport.Y, Target.Height, Output.Height);
		const uint32 Right = Ceil(
			Viewport.X + Viewport.Width, Target.Width, Output.Width
		);
		const uint32 Bottom = Ceil(
			Viewport.Y + Viewport.Height, Target.Height, Output.Height
		);
		Result.Width = Right - Result.X;
		Result.Height = Bottom - Result.Y;
		return Result;
	}

	auto FVolumetricCloudSpatialRenderer::CalculatePolicyKey(
		EQualityTier Tier
	) -> uint64
	{
		const FQualityPolicy Policy = ResolveQualityPolicy(Tier);
		return static_cast<uint64>(Tier)
			   | (static_cast<uint64>(Policy.LinearScaleNumerator) << 8u)
			   | (static_cast<uint64>(Policy.LinearScaleDenominator) << 16u)
			   | (static_cast<uint64>(Policy.PrimarySampleCount) << 24u)
			   | (static_cast<uint64>(Policy.LightSampleCount) << 32u)
			   | (static_cast<uint64>(Policy.TemporalPatternLength) << 40u);
	}

	auto FVolumetricCloudSpatialRenderer::CalculateJitter(
		uint64 SuccessfulSequence, const FQualityPolicy& Policy
	) -> FVector2f
	{
		if (Policy.TemporalPatternLength <= 1)
			return FVector2f(0.0f);
		auto RadicalInverse = [](uint64 Index, uint32 Base) {
			float Result = 0.0f;
			float Fraction = 1.0f / static_cast<float>(Base);
			while (Index != 0)
			{
				Result += static_cast<float>(Index % Base) * Fraction;
				Index /= Base;
				Fraction /= static_cast<float>(Base);
			}
			return Result;
		};
		const uint64 Index = SuccessfulSequence % Policy.TemporalPatternLength + 1;
		return {RadicalInverse(Index, 2) - 0.5f, RadicalInverse(Index, 3) - 0.5f};
	}

	auto FVolumetricCloudSpatialRenderer::SelectRoute(
		const FRouteInputs& Inputs
	) -> FRouteDecision
	{
		if (!Inputs.bRequested)
			return {ERoute::Disabled, ERouteReason::DisabledOrUnneeded};
		if (!Inputs.bRequiredInputsValid)
			return {ERoute::Disabled, ERouteReason::InvalidInputs};
		if (Inputs.Width == 0 || Inputs.Height == 0)
			return {ERoute::Disabled, ERouteReason::InvalidExtent};

		const bool bComputeExtentSupported = Inputs.MaxGroupCountX != 0
											 && Inputs.MaxGroupCountY != 0
											 && CalculateGroupCount(Inputs.Width) <= Inputs.MaxGroupCountX
											 && CalculateGroupCount(Inputs.Height) <= Inputs.MaxGroupCountY;
		if (Inputs.bComputePayloadReady && Inputs.bComputeTargetReady
			&& bComputeExtentSupported)
		{
			return {ERoute::Compute, ERouteReason::Compute};
		}

		ERouteReason ComputeFailure = ERouteReason::ComputePayloadUnavailable;
		if (Inputs.bComputePayloadReady && !Inputs.bComputeTargetReady)
			ComputeFailure = ERouteReason::ComputeTargetUnavailable;
		else if (Inputs.bComputePayloadReady && Inputs.bComputeTargetReady
				 && !bComputeExtentSupported)
			ComputeFailure = ERouteReason::ComputeExtentUnsupported;
		if (Inputs.bFragmentPayloadReady && Inputs.bFragmentTargetReady)
			return {ERoute::Fragment, ComputeFailure};
		if (!Inputs.bFragmentPayloadReady)
			return {ERoute::Disabled, ERouteReason::FragmentPayloadUnavailable};
		return {ERoute::Disabled, ERouteReason::FragmentTargetUnavailable};
	}

	auto FVolumetricCloudSpatialRenderer::IntersectHeightSlab(
		const FSlabRay& Ray
	) -> FSlabInterval
	{
		auto IsFiniteVector = [](const FVector3& Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				   && std::isfinite(Value.z);
		};
		if (!IsFiniteVector(Ray.Origin) || !IsFiniteVector(Ray.Direction)
			|| !std::isfinite(Ray.MinimumZ) || !std::isfinite(Ray.MaximumZ)
			|| !std::isfinite(Ray.MaximumDistance)
			|| Ray.MinimumZ >= Ray.MaximumZ || Ray.MaximumDistance <= 0.0)
		{
			return {};
		}
		const double DirectionLengthSquared =
			Ray.Direction.x * Ray.Direction.x
			+ Ray.Direction.y * Ray.Direction.y
			+ Ray.Direction.z * Ray.Direction.z;
		if (!std::isfinite(DirectionLengthSquared)
			|| DirectionLengthSquared <= 1.0e-24)
		{
			return {};
		}

		constexpr double ParallelThreshold = 1.0e-12;
		if (std::abs(Ray.Direction.z) <= ParallelThreshold)
		{
			if (Ray.Origin.z < Ray.MinimumZ || Ray.Origin.z > Ray.MaximumZ)
				return {};
			return {0.0, Ray.MaximumDistance, true};
		}

		double First = (Ray.MinimumZ - Ray.Origin.z) / Ray.Direction.z;
		double Second = (Ray.MaximumZ - Ray.Origin.z) / Ray.Direction.z;
		if (First > Second) std::swap(First, Second);
		const double Near = std::max(0.0, First);
		const double Far = std::min(Ray.MaximumDistance, Second);
		if (!std::isfinite(Near) || !std::isfinite(Far) || Far <= Near)
			return {};
		return {Near, Far, true};
	}

	auto FVolumetricCloudSpatialRenderer::IntegrateReference(
		const FReferenceInput& Input
	) -> FReferenceResult
	{
		FReferenceResult Result;
		if (!Input.bInsideFittedViewport || !Input.Parameters.IsValid()
			|| !Input.Samplers.HasRequiredInputs())
			return Result;
		FSlabRay Ray = Input.Ray;
		Ray.MinimumZ = Input.Parameters.MinimumZ;
		Ray.MaximumZ = Input.Parameters.MaximumZ;
		Ray.MaximumDistance = Input.Parameters.MaximumDistance;
		FSlabInterval Interval = IntersectHeightSlab(Ray);
		if (!Interval.bIntersects) return Result;
		if (std::isfinite(Input.OpaqueDistance))
			Interval.FarDistance = std::min(
				Interval.FarDistance, Input.OpaqueDistance
			);
		if (Interval.FarDistance <= Interval.NearDistance) return Result;

		const FParameters& Parameters = Input.Parameters;
		const double StepDistance =
			(Interval.FarDistance - Interval.NearDistance)
			/ static_cast<double>(Parameters.PrimarySampleCount);
		const FVector3f Direction = Math::Normalize(
			FVector3f(Ray.Direction)
		);
		const FVector3f Origin = FVector3f(Ray.Origin);
		const FVector3f ToLight = Math::Normalize(Parameters.LightDirection);
		const float LayerThickness = static_cast<float>(
			Parameters.MaximumZ - Parameters.MinimumZ
		);
		const float LightStep = LayerThickness
								/ static_cast<float>(std::max(Parameters.LightSampleCount, 1u));
		Result.bIntegrated = true;

		for (uint32 SampleIndex = 0;
			 SampleIndex < Parameters.PrimarySampleCount; ++SampleIndex)
		{
			const double Distance = Interval.NearDistance
									+ (static_cast<double>(SampleIndex) + 0.5) * StepDistance;
			const FVector3f Position = Origin
									   + Direction * static_cast<float>(Distance);
			const float Height = Saturate(
				(Position.z - static_cast<float>(Parameters.MinimumZ))
				/ LayerThickness
			);
			const float HeightProfile = Saturate(
				std::min(Height / 0.15f, (1.0f - Height) / 0.20f)
			);
			const float Base = Saturate(Input.Samplers.BaseDensity(Fract(
				Position * Parameters.BaseFrequency + Parameters.WindOffset
			)));
			const float Detail = Saturate(Input.Samplers.DetailDensity(Fract(
				Position * Parameters.DetailFrequency
				+ Parameters.WindOffset * 3.7f
			)));
			const float Weather = Input.Samplers.Weather ? Saturate(Input.Samplers.Weather(Fract(
															   FVector2f(Position) * Parameters.WeatherFrequency
															   + Parameters.WeatherOffset
														   ))) :
														   1.0f;
			const float Coverage = Saturate(Parameters.Coverage * Weather);
			const float Shape = Coverage > 0.0f ? Saturate((Base - (1.0f - Coverage)) / std::max(Coverage, 1.0e-4f)) : 0.0f;
			const float Density = Saturate(
									  Shape - Detail * Parameters.DetailErosion
								  )
								  * HeightProfile;
			++Result.PrimarySamples;
			if (Density <= 0.0f) continue;

			float LightOpticalDepth = 0.0f;
			for (uint32 LightIndex = 0;
				 LightIndex < Parameters.LightSampleCount; ++LightIndex)
			{
				const FVector3f LightPosition = Position + ToLight * (static_cast<float>(LightIndex) + 0.5f) * LightStep;
				const float LightBase = Saturate(Input.Samplers.BaseDensity(
					Fract(LightPosition * Parameters.BaseFrequency + Parameters.WindOffset)
				));
				const float LightDetail = Saturate(Input.Samplers.DetailDensity(
					Fract(LightPosition * Parameters.DetailFrequency + Parameters.WindOffset * 3.7f)
				));
				const float LightWeather = Input.Samplers.Weather ? Saturate(Input.Samplers.Weather(Fract(
																		FVector2f(LightPosition) * Parameters.WeatherFrequency
																		+ Parameters.WeatherOffset
																	))) :
																	1.0f;
				const float LightCoverage = Saturate(
					Parameters.Coverage * LightWeather
				);
				const float LightShape = LightCoverage > 0.0f ? Saturate((LightBase - (1.0f - LightCoverage)) / std::max(LightCoverage, 1.0e-4f)) : 0.0f;
				const float LightHeight = Saturate(
					(LightPosition.z - static_cast<float>(Parameters.MinimumZ))
					/ LayerThickness
				);
				const float LightHeightProfile = Saturate(std::min(
					LightHeight / 0.15f, (1.0f - LightHeight) / 0.20f
				));
				const float LightDensity = Saturate(
											   LightShape - LightDetail * Parameters.DetailErosion
										   )
										   * LightHeightProfile;
				LightOpticalDepth += LightDensity
									 * Parameters.LightExtinction * LightStep;
				++Result.LightSamples;
			}
			const float LightTransmittance = std::exp(-LightOpticalDepth);
			const float StepOpticalDepth = Density * Parameters.Extinction
										   * static_cast<float>(StepDistance);
			const float StepTransmittance = std::exp(-StepOpticalDepth);
			const float Scattered = 1.0f - StepTransmittance;
			constexpr float Anisotropy = 0.35f;
			const float CosTheta = std::clamp(Math::Dot(Direction, ToLight), -1.0f, 1.0f);
			const float PhaseDenominator = std::pow(
				1.0f + Anisotropy * Anisotropy
					- 2.0f * Anisotropy * CosTheta,
				1.5f
			);
			const float Phase = (1.0f - Anisotropy * Anisotropy)
								/ std::max(PhaseDenominator, 1.0e-4f);
			const FVector3f Lighting = Parameters.Ambient
										   * Parameters.AmbientColor
									   + Phase * LightTransmittance * Parameters.LightColor;
			Result.Radiance += Result.Transmittance * Scattered * Lighting;
			Result.Transmittance *= StepTransmittance;
			if (Result.Transmittance <= Parameters.TransmittanceCutoff)
			{
				Result.Transmittance = 0.0f;
				break;
			}
		}
		return Result;
	}

	auto FVolumetricCloudSpatialRenderer::MakeExecutionCounters(
		const FRouteInputs& Inputs, const FRouteDecision& Decision, uint64 PrimarySamples, uint64 LightSamples
	) -> FExecutionCounters
	{
		FExecutionCounters Counters{
			.Route = Decision.Route,
			.Reason = Decision.Reason,
			.PrimarySamples = PrimarySamples,
			.LightSamples = LightSamples,
			.TargetBytes = Decision.Route == ERoute::Disabled ? 0 : CalculateTargetBytes(Inputs.Width, Inputs.Height)
		};
		if (Decision.Route == ERoute::Compute)
		{
			Counters.GroupCountX = CalculateGroupCount(Inputs.Width);
			Counters.GroupCountY = CalculateGroupCount(Inputs.Height);
			Counters.Dispatches = 1;
		}
		else if (Decision.Route == ERoute::Fragment)
		{
			Counters.Draws = 1;
		}
		return Counters;
	}
} // namespace Durin
