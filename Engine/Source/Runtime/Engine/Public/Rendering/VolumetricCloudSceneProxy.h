#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"
#include "SceneTypes.h"

namespace Durin
{
	// Captures one immutable global-cloud candidate without retaining reflected objects.
	struct FVolumetricCloudSceneData
	{
		int32 Priority = 0;
		bool bEnabled = true;
		bool bEligible = false;

		FRHITextureReferenceRef BaseDensityTexture;
		FRHITextureReferenceRef DetailDensityTexture;
		FRHITextureReferenceRef WeatherTexture;

		double MinimumZ = 1'500.0;
		double MaximumZ = 3'500.0;
		double MaximumDistance = 100'000.0;
		FVector3f BaseFrequency{0.00008f};
		FVector3f DetailFrequency{0.00032f};
		FVector3f WindOffset{0.0f};
		FVector2f WeatherFrequency{0.00004f};
		FVector2f WeatherOffset{0.0f};
		float Coverage = 0.55f;
		float DetailErosion = 0.30f;
		float Extinction = 0.0015f;
		float LightExtinction = 0.0020f;
		float Ambient = 0.12f;
	};

	enum class EVolumetricCloudEligibilityReason : uint8
	{
		Disabled,
		OwnerHidden,
		MissingBaseDensityTexture,
		InvalidBaseDensityTexture,
		MissingDetailDensityTexture,
		InvalidDetailDensityTexture,
		InvalidLayer,
		InvalidMaximumDistance,
		InvalidDensityMapping,
		InvalidOpticalParameters,
		Ready
	};

	struct FVolumetricCloudEligibilityDiagnostic
	{
		EVolumetricCloudEligibilityReason Reason =
			EVolumetricCloudEligibilityReason::Disabled;
		bool bEligible = false;
		std::string Message;
	};

	struct FVolumetricCloudEligibilityContext
	{
		bool bOwnerHidden = false;
		bool bBaseDensityTextureAssigned = false;
		bool bBaseDensityTextureReady = false;
		bool bDetailDensityTextureAssigned = false;
		bool bDetailDensityTextureReady = false;
	};

	[[nodiscard]] ENGINE_API auto DiagnoseVolumetricCloudEligibility(
		const FVolumetricCloudSceneData& Data,
		const FVolumetricCloudEligibilityContext& Context)
		-> FVolumetricCloudEligibilityDiagnostic;
	[[nodiscard]] ENGINE_API auto AreVolumetricCloudParametersValid(
		const FVolumetricCloudSceneData& Data) -> bool;
	[[nodiscard]] ENGINE_API auto IsVolumetricCloudCandidateEligible(
		const FVolumetricCloudSceneData& Data) -> bool;

	class FVolumetricCloudSceneProxy final
		: public TSceneProxyPublication<FVolumetricCloudSceneId>
	{
	public:
		FVolumetricCloudSceneProxy(
			FSceneCandidateIdentity InIdentity,
			FVolumetricCloudSceneData InData)
			: Identity(std::move(InIdentity)), Data(std::move(InData)) {}

		auto GetIdentity() const -> const FSceneCandidateIdentity& { return Identity; }
		auto GetData() const -> const FVolumetricCloudSceneData& { return Data; }

	private:
		FSceneCandidateIdentity Identity;
		FVolumetricCloudSceneData Data;
	};
}
