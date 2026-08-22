#include "Engine/VolumetricCloudSceneProxy.h"

#include <cmath>

namespace Durin
{
	namespace
	{
		template<typename TVector>
		auto IsFiniteVector(const TVector& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y);
		}

		template<>
		auto IsFiniteVector(const FVector3f& Value) -> bool
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y)
				&& std::isfinite(Value.z);
		}

		template<typename TVector>
		auto IsPositiveVector(const TVector& Value) -> bool
		{
			return IsFiniteVector(Value) && Value.x > 0.0f && Value.y > 0.0f;
		}

		template<>
		auto IsPositiveVector(const FVector3f& Value) -> bool
		{
			return IsFiniteVector(Value) && Value.x > 0.0f
				&& Value.y > 0.0f && Value.z > 0.0f;
		}

		auto MakeDiagnostic(EVolumetricCloudEligibilityReason Reason,
			std::string Message, bool bEligible = false)
			-> FVolumetricCloudEligibilityDiagnostic
		{
			return {.Reason = Reason, .bEligible = bEligible,
				.Message = std::move(Message)};
		}

		auto IsLayerValid(const FVolumetricCloudSceneData& Data) -> bool
		{
			return std::isfinite(Data.MinimumZ) && std::isfinite(Data.MaximumZ)
				&& Data.MinimumZ < Data.MaximumZ;
		}

		auto IsDistanceValid(const FVolumetricCloudSceneData& Data) -> bool
		{
			return std::isfinite(Data.MaximumDistance) && Data.MaximumDistance > 0.0;
		}

		auto IsDensityMappingValid(const FVolumetricCloudSceneData& Data) -> bool
		{
			return IsPositiveVector(Data.BaseFrequency)
				&& IsPositiveVector(Data.DetailFrequency)
				&& IsFiniteVector(Data.WindOffset)
				&& IsPositiveVector(Data.WeatherFrequency)
				&& IsFiniteVector(Data.WeatherOffset);
		}

		auto AreOpticalParametersValid(const FVolumetricCloudSceneData& Data) -> bool
		{
			return std::isfinite(Data.Coverage) && Data.Coverage >= 0.0f
				&& Data.Coverage <= 1.0f
				&& std::isfinite(Data.DetailErosion) && Data.DetailErosion >= 0.0f
				&& Data.DetailErosion <= 1.0f
				&& std::isfinite(Data.Extinction) && Data.Extinction > 0.0f
				&& Data.Extinction <= 1.0f
				&& std::isfinite(Data.LightExtinction)
				&& Data.LightExtinction >= 0.0f && Data.LightExtinction <= 1.0f
				&& std::isfinite(Data.Ambient) && Data.Ambient >= 0.0f
				&& Data.Ambient <= 1.0f;
		}
	}

	auto DiagnoseVolumetricCloudEligibility(const FVolumetricCloudSceneData& Data,
		const FVolumetricCloudEligibilityContext& Context)
		-> FVolumetricCloudEligibilityDiagnostic
	{
		if (!Data.bEnabled)
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::Disabled,
				"Disabled: enable the volumetric cloud component.");
		if (Context.bOwnerHidden)
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::OwnerHidden,
				"Hidden: make the owning actor visible.");
		if (!Context.bBaseDensityTextureAssigned)
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::MissingBaseDensityTexture,
				"Missing Base Density Texture: assign a built Volume Texture.");
		if (!Context.bBaseDensityTextureReady)
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::InvalidBaseDensityTexture,
				"Invalid Base Density Texture: rebuild or reimport the assigned asset.");
		if (!Context.bDetailDensityTextureAssigned)
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::MissingDetailDensityTexture,
				"Missing Detail Density Texture: assign a built Volume Texture.");
		if (!Context.bDetailDensityTextureReady)
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::InvalidDetailDensityTexture,
				"Invalid Detail Density Texture: rebuild or reimport the assigned asset.");
		if (!IsLayerValid(Data))
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::InvalidLayer,
				"Invalid Layer: Minimum Z must be lower than Maximum Z.");
		if (!IsDistanceValid(Data))
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::InvalidMaximumDistance,
				"Invalid Maximum Distance: enter a finite value greater than zero.");
		if (!IsDensityMappingValid(Data))
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::InvalidDensityMapping,
				"Invalid Density Mapping: frequencies must be positive and all offsets finite.");
		if (!AreOpticalParametersValid(Data))
			return MakeDiagnostic(EVolumetricCloudEligibilityReason::InvalidOpticalParameters,
				"Invalid Optical Parameters: use finite normalized values and positive Extinction.");
		return MakeDiagnostic(EVolumetricCloudEligibilityReason::Ready,
			"Ready: eligible for volumetric cloud rendering.", true);
	}

	auto AreVolumetricCloudParametersValid(
		const FVolumetricCloudSceneData& Data) -> bool
	{
		return IsLayerValid(Data) && IsDistanceValid(Data)
			&& IsDensityMappingValid(Data) && AreOpticalParametersValid(Data);
	}

	auto IsVolumetricCloudCandidateEligible(
		const FVolumetricCloudSceneData& Data) -> bool
	{
		return Data.PersistentId.IsValid()
			&& Data.InstanceId != 0 && Data.PublicationRevision != 0
			&& DiagnoseVolumetricCloudEligibility(Data, {
				.bBaseDensityTextureAssigned = Data.BaseDensityTexture != nullptr,
				.bBaseDensityTextureReady = Data.BaseDensityTexture != nullptr,
				.bDetailDensityTextureAssigned = Data.DetailDensityTexture != nullptr,
				.bDetailDensityTextureReady = Data.DetailDensityTexture != nullptr}).bEligible;
	}
}
