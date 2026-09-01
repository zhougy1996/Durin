#pragma once

#include "EngineAPI.h"
#include "RHIResources.h"
#include "SceneTypes.h"

namespace Durin
{
	class FSkyBoxSceneInfo;

	// Captures sky state without retaining or reading reflected objects on the render thread.
	struct FSkyBoxSceneData
	{
		FRHITextureReferenceRef TextureReference;
		FQuat Rotation{1.0, 0.0, 0.0, 0.0};
		FVector3f Tint{1.0f, 1.0f, 1.0f};
		float Intensity = 1.0f;
	};

	// Contains every renderer-facing sky value before ownership crosses threads.
	struct FSkyBoxSceneProxyDesc
	{
		FGuid PersistentId;
		std::string SelectionKey;
		FSkyBoxSceneId RuntimeId = InvalidSkyBoxSceneId;
		FSkyBoxSceneData Data;

		auto IsValid() const -> bool
		{
			return PersistentId.IsValid() && RuntimeId != InvalidSkyBoxSceneId;
		}
	};

	class FSkyBoxSceneProxy final
	{
	public:
		explicit FSkyBoxSceneProxy(FSkyBoxSceneProxyDesc InDesc)
			: Desc(std::move(InDesc)) {}

		auto GetDesc() const -> const FSkyBoxSceneProxyDesc& { return Desc; }
		auto GetData() const -> const FSkyBoxSceneData& { return Desc.Data; }

	private:
		auto AttachToSceneInfo(FSkyBoxSceneInfo* InSceneInfo) -> void
		{
			check(SceneInfo == nullptr && InSceneInfo != nullptr);
			SceneInfo = InSceneInfo;
		}
		auto DetachFromSceneInfo(FSkyBoxSceneInfo* InSceneInfo) -> void
		{
			check(SceneInfo == InSceneInfo);
			SceneInfo = nullptr;
		}

		FSkyBoxSceneProxyDesc Desc;
		FSkyBoxSceneInfo* SceneInfo = nullptr;

		friend class FSkyBoxSceneInfo;
	};
}
