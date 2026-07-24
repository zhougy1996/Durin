#pragma once

#include "Math/DurinMath.h"

namespace Durin
{
	class FRHICommandListImmediate;
	class FRHITexture;
	class IScene;

	// Selects whether scene shading evaluates material lighting.
	enum class ERenderMode : uint8
	{
		Lit,
		Unlit
	};

	// Selects filled or wireframe primitive rasterization.
	enum class ERasterMode : uint8
	{
		Solid,
		Wireframe
	};

	// Carries runtime-selectable rendering behavior shared by all scene views.
	struct FRendererViewSettings
	{
		bool bEnableFXAA = true;
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
	};

	// Identifies a procedural editor-assistance shape rendered over a scene view.
	enum class EViewOverlayShape : uint8
	{
		Arrow,
		Axis,
		Plane,
		Ring,
		Box,
		WireBox
	};

	// Identifies a screen-sized editor icon anchored in world space.
	enum class EViewOverlayIcon : uint8
	{
		Camera,
		DirectionalLight
	};

	// Selects solid or distance-patterned rendering for an overlay line.
	enum class EViewOverlayLinePattern : uint8
	{
		Solid,
		Dashed
	};

	// Describes a transformed procedural overlay primitive in world space.
	struct FViewOverlayPrimitive
	{
		EViewOverlayShape Shape = EViewOverlayShape::Box;
		FMatrix LocalToWorld{1.0};
		FVector4f Color{1.0f};
	};

	// Describes a world-space overlay segment with screen-space width and pattern.
	struct FViewOverlayLine
	{
		FVector3 Start{0.0};
		FVector3 End{0.0};
		FVector4f Color{1.0f};
		float WidthPixels = 1.0f;
		EViewOverlayLinePattern Pattern = EViewOverlayLinePattern::Solid;
		float PatternPeriodPixels = 12.0f;
	};

	// Describes a world-space icon whose visual size is fixed in pixels.
	struct FViewOverlayIcon
	{
		EViewOverlayIcon Icon = EViewOverlayIcon::Camera;
		FVector3 WorldPosition{0.0};
		FVector4f Color{1.0f};
		float SizePixels = 30.0f;
	};

	// Configures the editor grid plane and its distance-based presentation.
	struct FViewEditorGrid
	{
		bool bVisible = false;
		double Height = 0.0;
		float FadeDistance = 10000.0f;
		FVector4f MinorColor{0.45f, 0.45f, 0.45f, 0.12f};
		FVector4f MajorColor{0.55f, 0.55f, 0.55f, 0.28f};
		FVector4f AxisXColor{1.0f, 0.28f, 0.28f, 0.8f};
		FVector4f AxisYColor{0.28f, 0.9f, 0.38f, 0.8f};
	};

	// Captures the matrices, viewport, and editor overlays required to render one view.
	struct FSceneView
	{
		FMatrix ViewMatrix{1.0};
		FMatrix ProjectionMatrix{1.0};
		FMatrix ViewProjectionMatrix{1.0};
		FVector3 ViewLocation{0.0};
		uint32 ViewportX = 0;
		uint32 ViewportY = 0;
		uint32 ViewportWidth = 0;
		uint32 ViewportHeight = 0;
		// Zero means fill the output target; positive values are fitted and centered by the renderer.
		float AspectRatioConstraint = 0.0f;
		FViewEditorGrid EditorGrid;
		std::vector<FViewOverlayPrimitive> OverlayPrimitives;
		std::vector<FViewOverlayLine> OverlayLines;
		std::vector<FViewOverlayIcon> OverlayIcons;
	};

	// Defines scene ownership and frame rendering services exposed by the renderer module.
	class IRendererModule : public IModuleInterface
	{
	public:
		// Queues destruction of renderer-owned RHI state before the rendering thread stops.
		virtual auto ReleaseResources() -> void = 0;
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
