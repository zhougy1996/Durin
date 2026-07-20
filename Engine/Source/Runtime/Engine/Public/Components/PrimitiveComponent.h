#pragma once
#include "Components/SceneComponent.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IScene.h"

#include "PrimitiveComponent.gen.h"

namespace Durin
{
	enum class EPrimitiveRenderStateDirtyFlags : uint8
	{
		None = 0,
		Proxy = 1 << 0,
		Transform = 1 << 1,
		MaterialData = 1 << 2
	};
	ENUM_CLASS_FLAGS(EPrimitiveRenderStateDirtyFlags);

	DCLASS()
	class DPrimitiveComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;

		ENGINE_API virtual auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy>;
		ENGINE_API auto GetRenderMatrix() const -> FMatrix;
		ENGINE_API auto GetPrimitiveSceneId() const -> FPrimitiveSceneId { return PrimitiveSceneId; }
		ENGINE_API auto MarkRenderStateDirty(EPrimitiveRenderStateDirtyFlags DirtyFlags = EPrimitiveRenderStateDirtyFlags::Proxy) -> void;

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;
		ENGINE_API virtual auto BuildMaterialRenderUpdate(EMaterialRenderDirtyFlags DirtyFlags, FMaterialRenderUpdate& OutUpdate) -> bool;

	private:
		auto EnsurePrimitiveSceneId() -> FPrimitiveSceneId;

		FPrimitiveSceneId PrimitiveSceneId = InvalidPrimitiveSceneId;
	};
}
