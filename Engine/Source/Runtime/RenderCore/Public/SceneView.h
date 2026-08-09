#pragma once

#include "Math/DurinMath.h"

namespace Durin
{
	// Selects whether one scene view evaluates material lighting.
	enum class ERenderMode : uint8
	{
		Lit,
		Unlit
	};

	// Selects filled or wireframe primitive rasterization for one scene view.
	enum class ERasterMode : uint8
	{
		Solid,
		Wireframe
	};

	// Selects conservative CPU frustum classification for one submitted view.
	enum class EViewVisibilityMode : uint8
	{
		Normal,
		FrustumCullingDisabled
	};

	// Selects automatic projected-size LODs or the qualified LOD-0 comparison path.
	enum class EViewLODMode : uint8
	{
		Automatic,
		ForceLOD0
	};

	// Captures rendering behavior with the view submitted to the render thread.
	struct FSceneViewSettings
	{
		bool bEnableFXAA = true;
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		EViewVisibilityMode VisibilityMode = EViewVisibilityMode::Normal;
		EViewLODMode LODMode = EViewLODMode::Automatic;
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

	// Captures the matrices, viewport policy, and editor overlays required to render one view.
	struct FSceneView
	{
		FMatrix ViewMatrix{1.0};
		FMatrix ProjectionMatrix{1.0};
		FMatrix ViewProjectionMatrix{1.0};
		FVector3 ViewLocation{0.0};
		FVector4f ClearColor{0.0f, 0.0f, 0.0f, 1.0f};
		uint32 ViewportX = 0;
		uint32 ViewportY = 0;
		uint32 ViewportWidth = 0;
		uint32 ViewportHeight = 0;
		// Zero means fill the output target; positive values are fitted and centered by the renderer.
		float AspectRatioConstraint = 0.0f;
		FSceneViewSettings Settings;
		FViewEditorGrid EditorGrid;
		std::vector<FViewOverlayPrimitive> OverlayPrimitives;
		std::vector<FViewOverlayLine> OverlayLines;
		std::vector<FViewOverlayIcon> OverlayIcons;
	};
}
