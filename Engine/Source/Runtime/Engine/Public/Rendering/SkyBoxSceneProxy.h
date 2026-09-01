#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"
#include "SceneTypes.h"

namespace Durin
{
	// Captures sky state without retaining or reading reflected objects on the render thread.
	struct FSkyBoxSceneData
	{
		FRHITextureReferenceRef TextureReference;
		FQuat Rotation{1.0, 0.0, 0.0, 0.0};
		FVector3f Tint{1.0f, 1.0f, 1.0f};
		float Intensity = 1.0f;
	};

	class FSkyBoxSceneProxy final
		: public TSceneProxyPublication<FSkyBoxSceneId>
	{
	public:
		FSkyBoxSceneProxy(
			FSceneCandidateIdentity InIdentity,
			FSkyBoxSceneData InData)
			: Identity(std::move(InIdentity)), Data(std::move(InData)) {}

		auto GetIdentity() const -> const FSceneCandidateIdentity& { return Identity; }
		auto GetData() const -> const FSkyBoxSceneData& { return Data; }

	private:
		FSceneCandidateIdentity Identity;
		FSkyBoxSceneData Data;
	};
}
