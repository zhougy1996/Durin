#include "Renderers/VolumetricCloudSpatialRenderer.h"

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
			return Value - glm::floor(Value);
		}

		auto Fract(const FVector2f& Value) -> FVector2f
		{
			return Value - glm::floor(Value);
		}
	}

	auto FVolumetricCloudSpatialRenderer::FParameters::IsValid() const -> bool
	{
		const float LightLengthSquared = glm::dot(LightDirection, LightDirection);
		return std::isfinite(MinimumZ) && std::isfinite(MaximumZ)
			&& std::isfinite(MaximumDistance) && MinimumZ < MaximumZ
			&& MaximumDistance > 0.0 && IsFiniteVector(BaseFrequency)
			&& IsFiniteVector(DetailFrequency) && IsFiniteVector(WindOffset)
			&& IsFiniteVector(WeatherFrequency) && IsFiniteVector(WeatherOffset)
			&& IsFiniteVector(LightDirection) && IsFiniteVector(LightColor)
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
		uint32 Width, uint32 Height) -> uint64
	{
		constexpr uint64 Maximum = std::numeric_limits<uint64>::max();
		const uint64 Pixels = static_cast<uint64>(Width) * Height;
		return Pixels > Maximum / BytesPerPixel
			? Maximum
			: Pixels * BytesPerPixel;
	}

	auto FVolumetricCloudSpatialRenderer::SelectRoute(
		const FRouteInputs& Inputs) -> FRouteDecision
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
			return {ERoute::Disabled,
				ERouteReason::FragmentPayloadUnavailable};
		return {ERoute::Disabled, ERouteReason::FragmentTargetUnavailable};
	}

	auto FVolumetricCloudSpatialRenderer::IntersectHeightSlab(
		const FSlabRay& Ray) -> FSlabInterval
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
		const FReferenceInput& Input) -> FReferenceResult
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
				Interval.FarDistance, Input.OpaqueDistance);
		if (Interval.FarDistance <= Interval.NearDistance) return Result;

		const FParameters& Parameters = Input.Parameters;
		const double StepDistance =
			(Interval.FarDistance - Interval.NearDistance)
			/ static_cast<double>(Parameters.PrimarySampleCount);
		const FVector3f Direction = glm::normalize(
			FVector3f(Ray.Direction));
		const FVector3f Origin = FVector3f(Ray.Origin);
		const FVector3f ToLight = glm::normalize(Parameters.LightDirection);
		const float LayerThickness = static_cast<float>(
			Parameters.MaximumZ - Parameters.MinimumZ);
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
				/ LayerThickness);
			const float HeightProfile = Saturate(
				std::min(Height / 0.15f, (1.0f - Height) / 0.20f));
			const float Base = Saturate(Input.Samplers.BaseDensity(Fract(
				Position * Parameters.BaseFrequency + Parameters.WindOffset)));
			const float Detail = Saturate(Input.Samplers.DetailDensity(Fract(
				Position * Parameters.DetailFrequency
				+ Parameters.WindOffset * 3.7f)));
			const float Weather = Input.Samplers.Weather
				? Saturate(Input.Samplers.Weather(Fract(
					FVector2f(Position) * Parameters.WeatherFrequency
					+ Parameters.WeatherOffset)))
				: 1.0f;
			const float Coverage = Saturate(Parameters.Coverage * Weather);
			const float Shape = Coverage > 0.0f
				? Saturate((Base - (1.0f - Coverage))
					/ std::max(Coverage, 1.0e-4f))
				: 0.0f;
			const float Density = Saturate(
				Shape - Detail * Parameters.DetailErosion) * HeightProfile;
			++Result.PrimarySamples;
			if (Density <= 0.0f) continue;

			float LightOpticalDepth = 0.0f;
			for (uint32 LightIndex = 0;
				LightIndex < Parameters.LightSampleCount; ++LightIndex)
			{
				const FVector3f LightPosition = Position + ToLight
					* (static_cast<float>(LightIndex) + 0.5f) * LightStep;
				const float LightBase = Saturate(Input.Samplers.BaseDensity(
					Fract(LightPosition * Parameters.BaseFrequency
						+ Parameters.WindOffset)));
				LightOpticalDepth += LightBase
					* Parameters.LightExtinction * LightStep;
				++Result.LightSamples;
			}
			const float LightTransmittance = std::exp(-LightOpticalDepth);
			const float StepOpticalDepth = Density * Parameters.Extinction
				* static_cast<float>(StepDistance);
			const float StepTransmittance = std::exp(-StepOpticalDepth);
			const float Scattered = 1.0f - StepTransmittance;
			const float Lighting = Parameters.Ambient + LightTransmittance;
			Result.Radiance += Result.Transmittance * Scattered * Lighting
				* Parameters.LightColor;
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
		const FRouteInputs& Inputs, const FRouteDecision& Decision,
		uint64 PrimarySamples, uint64 LightSamples) -> FExecutionCounters
	{
		FExecutionCounters Counters{
			.Route = Decision.Route,
			.Reason = Decision.Reason,
			.PrimarySamples = PrimarySamples,
			.LightSamples = LightSamples,
			.TargetBytes = Decision.Route == ERoute::Disabled
				? 0 : CalculateTargetBytes(Inputs.Width, Inputs.Height)};
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
