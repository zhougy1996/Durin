#pragma once

#include "Math/DurinMath.h"
#include "RHIResources.h"
#include "SceneViewState.h"
#include "SimpleElement.h"
#include "VolumetricCloudView.h"

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

	// Selects native or reduced deterministic GTAO generation for one view.
	enum class EGroundTruthAmbientOcclusionQuality : uint8
	{
		HalfResolution,
		FullResolution,
		Count,
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

	// Selects the requested contact-visibility producer for development A/B.
	// Auto preserves the production compute-first policy and fragment fallback.
	enum class EContactShadowRoutePreference : uint8
	{
		Auto,
		Compute,
		Fragment,
		Count,
	};

	struct FSceneViewModeSettings
	{
		ERenderMode RenderMode = ERenderMode::Lit;
		ERasterMode RasterMode = ERasterMode::Solid;
		EViewVisibilityMode VisibilityMode = EViewVisibilityMode::Normal;
		EViewLODMode LODMode = EViewLODMode::Automatic;
		// Renderer-owned material quality policy. Development captures may disable
		// it per submitted view for exact A/B evidence; normal views keep it on.
		bool bEnableSpecularAA = true;
	};

	struct FSceneViewPostProcessSettings
	{
		bool bEnableFXAA = true;
		// Manual display exposure in EV stops. Renderer canonicalizes non-finite
		// values to zero and clamps authored values to the display contract range.
		float ExposureEV = 0.0f;
	};

	struct FSceneViewTerrainSettings
	{
		// Development diagnostic; emits bounded transient terrain patch/LOD lines.
		bool bShowLODOverlay = false;
		// Development comparison mode; emits one eligible Terrain patch per batch.
		bool bDisableBatching = false;
	};

	struct FSceneViewDirectionalShadowSettings
	{
		EDirectionalShadowDiagnosticMode DiagnosticMode =
			EDirectionalShadowDiagnosticMode::Lit;
		EDirectionalShadowFilterQuality FilterQuality =
			EDirectionalShadowFilterQuality::Medium;
		EDirectionalShadowCandidate Candidate =
			EDirectionalShadowCandidate::ThreeCascades;
		// Screen-space contact-shadow supplement for the selected directional
		// light; enabled only when the directional shadow is prepared.
		bool bEnableContactShadows = false;
		EContactShadowRoutePreference ContactRoutePreference =
			EContactShadowRoutePreference::Auto;
		// Development overlay that visualizes the contact-shadow occlusion.
		bool bShowContactDebug = false;
	};

	struct FSceneViewAmbientOcclusionSettings
	{
		// Indirect-only GTAO for solid Lit views using required deferred opaque
		// ownership. Optional resource failure degrades this factor to white.
		bool bEnabled = true;
		EGroundTruthAmbientOcclusionQuality Quality =
			EGroundTruthAmbientOcclusionQuality::HalfResolution;
	};

	struct FSceneViewVolumetricCloudSettings
	{
		EVolumetricCloudQuality Quality = EVolumetricCloudQuality::High;
		EVolumetricCloudDebugMode DebugMode = EVolumetricCloudDebugMode::Lit;
	};

	// Captures rendering behavior with the view submitted to the render thread.
	// Feature-owned groups keep the immutable snapshot cohesive as capabilities grow.
	struct FSceneViewSettings
	{
		FSceneViewModeSettings Mode;
		FSceneViewPostProcessSettings PostProcess;
		FSceneViewTerrainSettings Terrain;
		FSceneViewDirectionalShadowSettings DirectionalShadow;
		FSceneViewAmbientOcclusionSettings AmbientOcclusion;
		FSceneViewVolumetricCloudSettings VolumetricCloud;
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
		Local,
		Environment,
		Emissive,
		Alpha,
		Final,
		Count,
	};

	enum class EGroundTruthAmbientOcclusionDebugMode : uint8
	{
		Disabled,
		Raw,
		Confidence,
		Filtered,
		FinalFactor,
		Count,
	};

	// Carries optional value-owned content overrides for one renderer submission.
	struct FSceneViewRenderOptions
	{
		std::optional<FViewEnvironmentOverride> Environment;
		// Supported development visualization written into HDR Scene Color.
		EGBufferDebugMode GBufferDebugMode = EGBufferDebugMode::Disabled;
		// Supported isolated deferred component visualization.
		EDeferredDirectionalDebugMode DeferredDirectionalDebugMode =
			EDeferredDirectionalDebugMode::Disabled;
		// Supported isolated ambient-occlusion visualization.
		EGroundTruthAmbientOcclusionDebugMode
			GroundTruthAmbientOcclusionDebugMode =
				EGroundTruthAmbientOcclusionDebugMode::Disabled;
	};

	// Identifies a procedural editor-assistance shape rendered over a scene view.
	enum class EViewOverlayShape : uint8
	{
		Arrow,
		Axis,
		Plane,
		Ring,
		Box
	};

	// Describes a transformed procedural overlay primitive in world space.
	struct FViewOverlayPrimitive
	{
		EViewOverlayShape Shape = EViewOverlayShape::Box;
		FMatrix LocalToWorld{1.0};
		FVector4f Color{1.0f};
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
		// Invalid keeps this submission fully stateless.
		FSceneViewStateId ViewStateId;
		// Explicitly rejects continuity for this submission without guessing from motion.
		bool bDiscardHistory = false;
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
		FSimpleElementViewSubmission SimpleElements;
	};
} // namespace Durin
