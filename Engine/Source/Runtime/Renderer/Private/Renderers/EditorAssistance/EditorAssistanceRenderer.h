#pragma once

#include "EditorGridRendering.h"
#include "Misc/CoreTypes.h"
#include "RHIResources.h"
#include "RendererAPI.h"
#include "Resources/RenderTargetLayouts.h"
#include "SceneView.h"
#include "Shader/GlobalShader.h"

namespace Durin
{
	class FEditorGridRenderer;
	class FGizmoRenderer;
	class FOverlayIconRenderer;
	class FOverlayLineRenderer;
	class FRendererResourceCoordinator;
	class FFullscreenGeometryResources;
	class FRHICommandListImmediate;
}

namespace Durin::RendererEditorAssistance
{
	enum class EFeature : uint8
	{
		EditorGrid,
		Gizmo,
		OverlayLine,
		OverlayIcon,
	};

	enum class EDepthMode : uint8
	{
		XRay,
		Visible,
	};

	enum class EGizmoTopology : uint8
	{
		NotApplicable,
		Solid,
		Wire,
	};

	struct FPipelineKey
	{
		EFeature Feature = EFeature::EditorGrid;
		RenderTargetLayouts::EViewportOutput Output =
			RenderTargetLayouts::EViewportOutput::Offscreen;
		EDepthMode DepthMode = EDepthMode::Visible;
		ESceneDepthConvention DepthConvention =
			ESceneDepthConvention::ForwardZ;
		EGizmoTopology GizmoTopology = EGizmoTopology::NotApplicable;

		auto operator==(const FPipelineKey&) const -> bool = default;
	};

	struct FRequest
	{
		RenderTargetLayouts::EViewportOutput Output =
			RenderTargetLayouts::EViewportOutput::Offscreen;
		ESceneDepthConvention DepthConvention =
			ESceneDepthConvention::ForwardZ;
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

	inline auto GetVisibleDepthCompareOp(
		ESceneDepthConvention DepthConvention) -> ERHIDepthCompareOp
	{
		return DepthConvention == ESceneDepthConvention::ReversedZ
			? ERHIDepthCompareOp::GreaterOrEqual
			: ERHIDepthCompareOp::Less;
	}

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

	struct FPreparedPipeline
	{
		FPipelineKey Key;
		FGraphicsPipelineStateRHIRef Pipeline;
		FGlobalShaderSetRef ShaderSet;
	};

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
} // namespace Durin::RendererEditorAssistance

namespace Durin
{
	// Owns editor-assistance request preparation, sub-renderer composition, and
	// draw ordering while concrete renderers own their independent GPU resources.
	class FEditorAssistanceRenderer final
	{
	public:
		using FRequest = RendererEditorAssistance::FRequest;
		using FPipelineKey = RendererEditorAssistance::FPipelineKey;
		using FPrepared = RendererEditorAssistance::FPrepared;
		using EDrawOperation = RendererEditorAssistance::EDrawOperation;

		FEditorAssistanceRenderer(
			FRendererResourceCoordinator& InCoordinator,
			FFullscreenGeometryResources& InFullscreenGeometry);
		~FEditorAssistanceRenderer();

		FEditorAssistanceRenderer(const FEditorAssistanceRenderer&) = delete;
		auto operator=(const FEditorAssistanceRenderer&)
			-> FEditorAssistanceRenderer& = delete;

		RENDERER_API static auto AnalyzeRequest(
			const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output) -> FRequest;
		RENDERER_API static auto GetRequiredPipelineKeys(
			const FRequest& Request) -> std::vector<FPipelineKey>;
		RENDERER_API static auto BuildDrawableOperations(
			const FRequest& Request,
			std::span<const FPipelineKey> AvailablePipelines)
			-> std::vector<EDrawOperation>;
		RENDERER_API static auto GetDrawOrder()
			-> std::span<const EDrawOperation>;

		auto Prepare_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FRequest& Request) -> FPrepared;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FPrepared& Prepared) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		std::unique_ptr<FEditorGridRenderer> EditorGridRenderer;
		std::unique_ptr<FGizmoRenderer> GizmoRenderer;
		std::unique_ptr<FOverlayLineRenderer> OverlayLineRenderer;
		std::unique_ptr<FOverlayIconRenderer> OverlayIconRenderer;
	};
} // namespace Durin
