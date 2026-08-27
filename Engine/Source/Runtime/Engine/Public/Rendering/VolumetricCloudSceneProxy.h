#pragma once

#include "EngineAPI.h"
#include "IScene.h"

namespace Durin
{
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
	{
	public:
		explicit FVolumetricCloudSceneProxy(FVolumetricCloudSceneData InData)
			: Data(std::move(InData)) {}

		auto GetData() const -> const FVolumetricCloudSceneData& { return Data; }

	private:
		FVolumetricCloudSceneData Data;
	};
}
