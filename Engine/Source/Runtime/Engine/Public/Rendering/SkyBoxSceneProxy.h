#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"

namespace Durin
{
	// Captures sky state without retaining or reading reflected objects on the render thread.
	struct FSkyBoxSceneData
	{
		FGuid SceneId;
		std::string SelectionKey;
		uint64 InstanceId = 0;
		FRHITextureReferenceRef TextureReference;
		FQuat Rotation{1.0, 0.0, 0.0, 0.0};
		FVector3f Tint{1.0f, 1.0f, 1.0f};
		float Intensity = 1.0f;
	};

	class FSkyBoxSceneProxy final
	{
	public:
		explicit FSkyBoxSceneProxy(FSkyBoxSceneData InData)
			: Data(std::move(InData)) {}

		auto GetData() const -> const FSkyBoxSceneData& { return Data; }

	private:
		FSkyBoxSceneData Data;
	};
}
