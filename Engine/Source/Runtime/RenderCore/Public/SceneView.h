#pragma once

#include "Math/DurinMath.h"
#include "RHIResources.h"

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

	// Defines how normalized device depth maps the validated view clip interval.
	enum class ESceneDepthConvention : uint8
	{
		ForwardZ,
		ReversedZ
	};

	// Development-only directional-shadow evidence selected per submitted view.
	enum class EDirectionalShadowDiagnosticMode : uint8
	{
		Lit,
		ShadowDepthCoverage,
		ReceiverUnbiased,
		ReceiverBiased,
		ReceiverNormalOffset,
		TexelGrid,
		BiasContributions,
		Classification,
		FilterFootprint,
		FilterTapValidity,
		FilterDifference,
		CascadeIndex,
		CascadeTransition,
		CascadeCoverage,
		CascadeDifference,
		Count,
	};

	// Selects one deterministic directional-shadow comparison kernel per view.
	enum class EDirectionalShadowFilterQuality : uint8
	{
		Low,
		Medium,
		High,
		Count,
	};

	// Selects the bounded directional-shadow representation prepared for one view.
	enum class EDirectionalShadowCandidate : uint8
	{
		SingleMap,
		ThreeCascades,
		Count,
	};

	// Captures rendering behavior with the view submitted to the render thread.
	struct FSceneViewSettings
	{
		bool bEnableFXAA = true;
		// Manual display exposure in EV stops. Renderer canonicalizes non-finite
		// values to zero and clamps authored values to the display contract range.
		float ExposureEV = 0.0f;
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		EViewVisibilityMode VisibilityMode = EViewVisibilityMode::Normal;
		EViewLODMode LODMode = EViewLODMode::Automatic;
		// Development diagnostic; emits bounded transient terrain patch/LOD lines.
		bool bShowTerrainLODOverlay = false;
		// Development comparison mode; emits one eligible Terrain patch per batch.
		bool bDisableTerrainBatching = false;
		EDirectionalShadowDiagnosticMode DirectionalShadowDiagnosticMode =
			EDirectionalShadowDiagnosticMode::Lit;
		EDirectionalShadowFilterQuality DirectionalShadowFilterQuality =
			EDirectionalShadowFilterQuality::Medium;
		EDirectionalShadowCandidate DirectionalShadowCandidate =
			EDirectionalShadowCandidate::ThreeCascades;
		// Screen-space contact-shadow supplement for the selected directional
		// light; enabled only when the directional shadow is prepared.
		bool bEnableContactShadows = false;
		// Development overlay that visualizes the contact-shadow occlusion.
		bool bShowContactShadowDebug = false;
	};

	// Supplies one submission-local cube environment without publishing scene state.
	struct FViewEnvironmentOverride
	{
		FRHITextureReferenceRef TextureReference;
		FQuat Rotation{1.0, 0.0, 0.0, 0.0};
		FVector3f Tint{1.0f};
		float Intensity = 1.0f;
	};

	// Development-only visualization sourced from the qualified GBuffer and
	// written into HDR Scene Color before the normal display transform.
	enum class EGBufferDebugMode : uint8
	{
		Disabled,
		Material,
		ShadingNormal,
		GeometricNormal,
		Surface,
		Emissive,
		Flags,
		Depth,
		ViewPosition,
		ReconstructionError,
		MaterialInputs,
		Count,
	};

	// Development-only component views written into the isolated deferred HDR
	// qualification target. They never replace production Scene Color.
	enum class EDeferredDirectionalDebugMode : uint8
	{
		Disabled,
		DecodedMaterial,
		Directional,
		Environment,
		Emissive,
		Alpha,
		Final,
		Count,
	};

	// Carries optional value-owned content overrides for one renderer submission.
	struct FSceneViewRenderOptions
	{
		std::optional<FViewEnvironmentOverride> Environment;
		// Development-only A/B path. Writes opaque and masked geometry into the
		// minimal GBuffer before the unchanged forward scene pass.
		bool bEnableGBufferQualification = false;
		EGBufferDebugMode GBufferDebugMode = EGBufferDebugMode::Disabled;
		// Development-only M3 route. Produces an isolated deferred directional,
		// shadow, environment, and emissive result before unchanged forward Scene Color.
		bool bEnableDeferredDirectionalQualification = false;
		EDeferredDirectionalDebugMode DeferredDirectionalDebugMode =
			EDeferredDirectionalDebugMode::Disabled;
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
		DirectionalLight,
		PlayerStart
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
		ESceneDepthConvention DepthConvention = ESceneDepthConvention::ForwardZ;
		double NearClipDistance = 0.1;
		double FarClipDistance = 500000.0;
		// Horizontal radial distance policy; the transition ends before the projection far plane.
		double ViewFadeStart = 180000.0;
		double ViewRenderDistance = 200000.0;
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
