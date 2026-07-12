#pragma once

#include "Math/DurinMath.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class IScene;

	enum class ERenderMode : uint8
	{
		Lit,
		Unlit
	};

	enum class ERasterMode : uint8
	{
		Solid,
		Wireframe
	};

	struct FRendererViewSettings
	{
		bool bEnableFXAA = true;
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
	};

	struct FSceneView
	{
		FMatrix ViewMatrix{1.0};
		FMatrix ProjectionMatrix{1.0};
		FMatrix ViewProjectionMatrix{1.0};
		FVector3 ViewLocation{0.0};
		uint32 ViewportWidth = 0;
		uint32 ViewportHeight = 0;
	};

	class IRendererModule : public IModuleInterface
	{
	public:
		virtual auto CreateScene() -> std::unique_ptr<IScene> = 0;
		virtual auto GetViewSettings() const -> FRendererViewSettings = 0;
		virtual auto SetViewSettings(const FRendererViewSettings& InSettings) -> void = 0;
		virtual auto SetFXAAEnabled(bool bInEnabled) -> void = 0;
		virtual auto IsFXAAEnabled() const -> bool = 0;
		virtual auto SetRenderMode(ERenderMode Mode) -> void = 0;
		virtual auto GetRenderMode() const -> ERenderMode = 0;
		virtual auto SetRasterMode(ERasterMode Mode) -> void = 0;
		virtual auto GetRasterMode() const -> ERasterMode = 0;
		virtual auto PrepareSceneResources(FRHICommandListImmediate& CommandList, IScene* Scene) -> void = 0;
		virtual auto RenderView(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* OutputTarget, bool bPresentOutput) -> void = 0;
		virtual auto RenderScene(FRHICommandListImmediate& CommandList, IScene* Scene, const FSceneView& View, FRHITexture* RenderTarget) -> void = 0;
	};
}
