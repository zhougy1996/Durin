#pragma once

#include "EngineAPI.h"
#include "IScene.h"

namespace Durin
{
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
