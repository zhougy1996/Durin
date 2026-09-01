#pragma once

#include "Components/SceneComponent.h"

#ifdef _DHT_PARSER
namespace Durin
{
	struct FLinearColor
	{
		float R, G, B, A;
	};
}
#else
#include "Math/Color.h"
#endif

#include "SkyBoxComponent.gen.h"

namespace Durin
{
	class DTextureCube;
	class FSkyBoxSceneProxy;

	// Publishes a persistent cube-texture reference and rotation-only sky snapshot.
	DCLASS()
	class DSkyBoxComponent : public DSceneComponent
	{
		GENERATED_BODY()
	public:
		ENGINE_API explicit DSkyBoxComponent(const FObjectInitializer& ObjectInitializer);

		ENGINE_API auto OnRegister() -> void override;
		ENGINE_API auto OnUnregister() -> void override;
		ENGINE_API auto OnOwnerVisibilityChanged() -> void override;
		ENGINE_API auto PreEditChangeProperty(FPropertyEditProposal& Proposal, std::string& OutError) -> bool override;
		ENGINE_API auto PostEditChangeProperty(const FPropertyChangedEvent& Event) -> void override;

		auto GetTextureCube() const -> DTextureCube* { return TextureCube.Get(); }
		auto GetTint() const -> const FLinearColor& { return Tint; }
		auto GetIntensity() const -> float { return Intensity; }
		auto GetSkyBoxSceneId() const -> const FGuid& { return SkyBoxSceneId; }
		auto GetSkyBoxInstanceId() const -> uint64 { return SkyBoxInstanceId; }

		ENGINE_API auto SetTextureCube(DTextureCube* InTextureCube) -> void;
		ENGINE_API auto SetTint(const FLinearColor& InTint) -> void;
		ENGINE_API auto SetIntensity(float InIntensity) -> void;

	protected:
		ENGINE_API auto OnUpdateTransform() -> void override;

	private:
		auto EnsureSkyBoxInstanceId() -> uint64;
		auto CreateRenderState() -> void;
		auto DestroyRenderState() -> void;
		auto MarkRenderStateDirty() -> void;

		DPROPERTY(Edit)
		TObjectPtr<DTextureCube> TextureCube;

		DPROPERTY(Edit, MetaData="HideAlpha")
		FLinearColor Tint{1.0f, 1.0f, 1.0f, 1.0f};

		DPROPERTY(Edit)
		float Intensity = 1.0f;

		// Serialized identity makes active selection independent of registration order.
		DPROPERTY()
		FGuid SkyBoxSceneId;

		// Nonserialized identity separates duplicates that intentionally share persistent state.
		uint64 SkyBoxInstanceId = 0;
		// Non-owning token used only to retire the exact published proxy.
		FSkyBoxSceneProxy* SceneProxy = nullptr;
	};
}
