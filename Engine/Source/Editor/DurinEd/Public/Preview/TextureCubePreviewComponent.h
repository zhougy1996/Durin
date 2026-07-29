#pragma once

#include "Components/StaticMeshComponent.h"
#include "DurinEdAPI.h"

#include "TextureCubePreviewComponent.gen.h"

namespace Durin
{
	class DTextureCube;

	// Owns the specialized cube-sampling proxy used by preview worlds.
	DCLASS()
	class DTextureCubePreviewComponent final : public DStaticMeshComponent
	{
		GENERATED_BODY()
	public:
		DURINED_API explicit DTextureCubePreviewComponent(const FObjectInitializer& ObjectInitializer);

		DURINED_API auto SetTextureCube(DTextureCube* InTextureCube) -> void;
		auto GetTextureCube() const -> DTextureCube* { return TextureCube.Get(); }
		DURINED_API auto CreateSceneProxy() -> std::unique_ptr<PrimitiveSceneProxy> override;

	private:
		DPROPERTY(Transient)
		TObjectPtr<DTextureCube> TextureCube;
	};
} // namespace Durin
