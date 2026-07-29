#pragma once

#include "EditorGridRendering.h"
#include "Misc/CoreTypes.h"
#include "RHIResources.h"
#include "RendererAPI.h"
#include "RendererRenderTargetLayouts.h"
#include "SceneView.h"

namespace Durin
{
	class FRHICommandListImmediate;
}

namespace Durin::RendererEditorAssistance
{
	// Identifies the independently initialized assistance feature.
	enum class EFeature : uint8
	{
		EditorGrid,
		Gizmo,
		OverlayLine,
		OverlayIcon,
	};

	// Selects depth-independent X-Ray drawing or preserved-depth drawing.
	enum class EDepthMode : uint8
	{
		XRay,
		Visible,
	};

	// Distinguishes pipeline topology only where Gizmo compatibility requires it.
	enum class EGizmoTopology : uint8
	{
		NotApplicable,
		Solid,
		Wire,
	};

	// Tracks the independent invalidation domains used by lazy assistance resources.
	struct FResourceGeneration
	{
		uint64 Shader = 0;
		uint64 Device = 0;
		uint64 Manual = 0;

		auto operator==(const FResourceGeneration&) const -> bool = default;
	};

	// Selects which generation counters make one resource attempt stale.
	struct FResourceGenerationDependencies
	{
		bool bShader = false;
		bool bDevice = false;
		bool bManual = false;
	};

	// Separates payload availability from the latest generation-scoped attempt.
	enum class EResourceAvailability : uint8
	{
		Uninitialized,
		Ready,
		Failed,
	};

	struct FGenerationScopedAttempt
	{
		EResourceAvailability Availability =
			EResourceAvailability::Uninitialized;
		FResourceGeneration PayloadGeneration;
		FResourceGeneration AttemptedGeneration;
		bool bHasPayload = false;
		bool bHasAttempt = false;
		bool bLastAttemptFailed = false;
		std::string FailureDetail;
	};

	// Identifies one independently cached assistance pipeline variant.
	struct FPipelineKey
	{
		EFeature Feature = EFeature::EditorGrid;
		RendererRenderTargetLayouts::EViewportOutput Output =
			RendererRenderTargetLayouts::EViewportOutput::Offscreen;
		EDepthMode DepthMode = EDepthMode::Visible;
		EGizmoTopology GizmoTopology = EGizmoTopology::NotApplicable;

		auto operator==(const FPipelineKey&) const -> bool = default;
	};

	// Records assistance demand derived from one immutable scene view.
	struct FRequest
	{
		RendererRenderTargetLayouts::EViewportOutput Output =
			RendererRenderTargetLayouts::EViewportOutput::Offscreen;
		bool bEditorGrid = false;
		bool bSolidGizmos = false;
		bool bWireGizmos = false;
		bool bOverlayLines = false;
		bool bOverlayIcons = false;

		auto IsEmpty() const -> bool
		{
			return !bEditorGrid && !bSolidGizmos && !bWireGizmos
				&& !bOverlayLines && !bOverlayIcons;
		}
	};

	// Selects the procedural editor-assistance geometry emitted by a draw helper.
	enum class EDrawOperation : uint8
	{
		EditorGrid,
		XRayGizmos,
		XRayOverlayLines,
		XRayOverlayIcons,
		VisibleGizmos,
		VisibleOverlayLines,
		VisibleOverlayIcons,
	};

	// Binds one available pipeline to the per-view operation that requested it.
	struct FPreparedPipeline
	{
		FPipelineKey Key;
		FGraphicsPipelineStateRHIRef Pipeline;
	};

	// Owns all view-dependent assistance data prepared for one RenderView call.
	struct FPrepared
	{
		std::optional<EditorGridRendering::FEditorGridUniform> EditorGridUniform;
		uint32 OverlayLineIndexCount = 0;
		uint32 OverlayIconIndexCount = 0;
		bool bSolidGizmos = false;
		bool bWireGizmos = false;
		std::vector<FPreparedPipeline> Pipelines;

		auto HasDrawableOperation() const -> bool
		{
			return !Pipelines.empty();
		}
	};

	RENDERER_API auto AnalyzeRequest(
		const FSceneView& View,
		RendererRenderTargetLayouts::EViewportOutput Output) -> FRequest;
	RENDERER_API auto GetRequiredPipelineKeys(const FRequest& Request)
		-> std::vector<FPipelineKey>;
	RENDERER_API auto BuildDrawableOperations(
		const FRequest& Request,
		std::span<const FPipelineKey> AvailablePipelines)
		-> std::vector<EDrawOperation>;
	RENDERER_API auto GetDrawOrder() -> std::span<const EDrawOperation>;
	RENDERER_API auto ShouldAttemptResource(
		const FGenerationScopedAttempt& Attempt,
		const FResourceGeneration& Generation,
		const FResourceGenerationDependencies& Dependencies) -> bool;
	RENDERER_API auto RecordResourceAttemptSuccess(
		FGenerationScopedAttempt& Attempt,
		const FResourceGeneration& Generation) -> void;
	RENDERER_API auto RecordResourceAttemptFailure(
		FGenerationScopedAttempt& Attempt,
		const FResourceGeneration& Generation,
		std::string_view Detail = {}) -> void;

	// Prepares only resources and operations demanded by this view.
	auto Prepare(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRequest& Request) -> FPrepared;
	auto Draw(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FPrepared& Prepared) -> void;
	// Advances lazy retry generations; integration with development commands is
	// owned by the recoverable renderer-resource plan.
	auto InvalidateShaderResources() -> void;
	auto InvalidateDeviceResources() -> void;
	auto RetryFailedResources() -> void;
	// Resets all render-thread-owned base, pipeline, dynamic, and diagnostic state.
	auto ReleaseResources() -> void;
} // namespace Durin::RendererEditorAssistance
