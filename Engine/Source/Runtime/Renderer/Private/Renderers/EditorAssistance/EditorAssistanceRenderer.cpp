#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"

#include "Renderers/EditorAssistance/EditorGridRenderer.h"
#include "Renderers/EditorAssistance/GizmoRenderer.h"
#include "Renderers/EditorAssistance/OverlayIconRenderer.h"
#include "Renderers/EditorAssistance/OverlayLineRenderer.h"
#include "Misc/AssertionMacros.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	using namespace RendererEditorAssistance;

	namespace
	{
		auto ContainsPipelineKey(
			std::span<const FPipelineKey> Keys,
			const FPipelineKey& Expected) -> bool
		{
			return std::ranges::find(Keys, Expected) != Keys.end();
		}

		auto MakePipelineKey(
			const FRequest& Request,
			EFeature Feature,
			EDepthMode DepthMode,
			EGizmoTopology GizmoTopology = EGizmoTopology::NotApplicable)
			-> FPipelineKey
		{
			return {
				.Feature = Feature,
				.Output = Request.Output,
				.DepthMode = DepthMode,
				.GizmoTopology = GizmoTopology,
			};
		}
	} // namespace

	FEditorAssistanceRenderer::FEditorAssistanceRenderer(
		FRendererResourceCoordinator& InCoordinator,
		FFullscreenGeometryResources& InFullscreenGeometry)
		: EditorGridRenderer(std::make_unique<FEditorGridRenderer>(
			InCoordinator, InFullscreenGeometry))
		, GizmoRenderer(std::make_unique<FGizmoRenderer>(InCoordinator))
		, OverlayLineRenderer(
			std::make_unique<FOverlayLineRenderer>(InCoordinator))
		, OverlayIconRenderer(
			std::make_unique<FOverlayIconRenderer>(InCoordinator))
	{
	}

	FEditorAssistanceRenderer::~FEditorAssistanceRenderer() = default;

	auto FEditorAssistanceRenderer::AnalyzeRequest(
		const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output) -> FRequest
	{
		FRequest Request;
		Request.Output = Output;
		Request.bEditorGrid = View.EditorGrid.bVisible;
		Request.bOverlayLines = !View.OverlayLines.empty();
		Request.bOverlayIcons = !View.OverlayIcons.empty();
		for (const FViewOverlayPrimitive& Primitive : View.OverlayPrimitives)
		{
			if (Primitive.Shape == EViewOverlayShape::WireBox)
				Request.bWireGizmos = true;
			else
				Request.bSolidGizmos = true;
		}
		return Request;
	}

	auto FEditorAssistanceRenderer::GetRequiredPipelineKeys(
		const FRequest& Request) -> std::vector<FPipelineKey>
	{
		std::vector<FPipelineKey> Keys;
		if (Request.bEditorGrid)
		{
			Keys.push_back(MakePipelineKey(
				Request, EFeature::EditorGrid, EDepthMode::Visible));
		}
		auto AddDepthVariants = [&Keys, &Request](
			EFeature Feature,
			EGizmoTopology GizmoTopology = EGizmoTopology::NotApplicable) {
			Keys.push_back(MakePipelineKey(
				Request, Feature, EDepthMode::XRay, GizmoTopology));
			Keys.push_back(MakePipelineKey(
				Request, Feature, EDepthMode::Visible, GizmoTopology));
		};
		if (Request.bSolidGizmos)
			AddDepthVariants(EFeature::Gizmo, EGizmoTopology::Solid);
		if (Request.bWireGizmos)
			AddDepthVariants(EFeature::Gizmo, EGizmoTopology::Wire);
		if (Request.bOverlayLines)
			AddDepthVariants(EFeature::OverlayLine);
		if (Request.bOverlayIcons)
			AddDepthVariants(EFeature::OverlayIcon);
		return Keys;
	}

	auto FEditorAssistanceRenderer::BuildDrawableOperations(
		const FRequest& Request,
		std::span<const FPipelineKey> AvailablePipelines)
		-> std::vector<EDrawOperation>
	{
		auto HasFeaturePipeline = [&Request, AvailablePipelines](
			EFeature Feature,
			EDepthMode DepthMode,
			EGizmoTopology GizmoTopology = EGizmoTopology::NotApplicable) {
			return ContainsPipelineKey(
				AvailablePipelines,
				MakePipelineKey(Request, Feature, DepthMode, GizmoTopology));
		};
		auto HasGizmoPipeline = [&Request, &HasFeaturePipeline](
			EDepthMode DepthMode) {
			return (Request.bSolidGizmos
					&& HasFeaturePipeline(
						EFeature::Gizmo, DepthMode, EGizmoTopology::Solid))
				|| (Request.bWireGizmos
					&& HasFeaturePipeline(
						EFeature::Gizmo, DepthMode, EGizmoTopology::Wire));
		};

		std::vector<EDrawOperation> Operations;
		for (const EDrawOperation Operation : GetDrawOrder())
		{
			bool bDrawable = false;
			switch (Operation)
			{
			case EDrawOperation::EditorGrid:
				bDrawable = Request.bEditorGrid
					&& HasFeaturePipeline(
						EFeature::EditorGrid, EDepthMode::Visible);
				break;
			case EDrawOperation::XRayGizmos:
				bDrawable = HasGizmoPipeline(EDepthMode::XRay);
				break;
			case EDrawOperation::XRayOverlayLines:
				bDrawable = Request.bOverlayLines
					&& HasFeaturePipeline(
						EFeature::OverlayLine, EDepthMode::XRay);
				break;
			case EDrawOperation::XRayOverlayIcons:
				bDrawable = Request.bOverlayIcons
					&& HasFeaturePipeline(
						EFeature::OverlayIcon, EDepthMode::XRay);
				break;
			case EDrawOperation::VisibleGizmos:
				bDrawable = HasGizmoPipeline(EDepthMode::Visible);
				break;
			case EDrawOperation::VisibleOverlayLines:
				bDrawable = Request.bOverlayLines
					&& HasFeaturePipeline(
						EFeature::OverlayLine, EDepthMode::Visible);
				break;
			case EDrawOperation::VisibleOverlayIcons:
				bDrawable = Request.bOverlayIcons
					&& HasFeaturePipeline(
						EFeature::OverlayIcon, EDepthMode::Visible);
				break;
			}
			if (bDrawable)
				Operations.push_back(Operation);
		}
		return Operations;
	}

	auto FEditorAssistanceRenderer::GetDrawOrder()
		-> std::span<const EDrawOperation>
	{
		static constexpr std::array DrawOrder{
			EDrawOperation::EditorGrid,
			EDrawOperation::XRayGizmos,
			EDrawOperation::XRayOverlayLines,
			EDrawOperation::XRayOverlayIcons,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::VisibleOverlayLines,
			EDrawOperation::VisibleOverlayIcons,
		};
		return DrawOrder;
	}

	auto FEditorAssistanceRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRequest& Request) -> FPrepared
	{
		check(IsInRenderingThread());
		FPrepared Prepared;
		if (Request.bEditorGrid)
		{
			EditorGridRenderer->Prepare_RenderThread(
				CommandList, View, Request.Output, Prepared);
		}
		if (Request.bSolidGizmos || Request.bWireGizmos)
		{
			GizmoRenderer->Prepare_RenderThread(
				CommandList, Request, Prepared);
		}
		if (Request.bOverlayLines)
		{
			OverlayLineRenderer->Prepare_RenderThread(
				CommandList, View, Request.Output, Prepared);
		}
		if (Request.bOverlayIcons)
		{
			OverlayIconRenderer->Prepare_RenderThread(
				CommandList, View, Request.Output, Prepared);
		}
		return Prepared;
	}

	auto FEditorAssistanceRenderer::Draw_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FPrepared& Prepared) -> void
	{
		check(IsInRenderingThread());
		CommandList.SetViewport(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			0.0f,
			static_cast<float>(View.ViewportX + View.ViewportWidth),
			static_cast<float>(View.ViewportY + View.ViewportHeight),
			1.0f);
		CommandList.SetScissor(
			static_cast<float>(View.ViewportX),
			static_cast<float>(View.ViewportY),
			static_cast<float>(View.ViewportWidth),
			static_cast<float>(View.ViewportHeight));

		for (const EDrawOperation Operation : GetDrawOrder())
		{
			switch (Operation)
			{
			case EDrawOperation::EditorGrid:
				EditorGridRenderer->Draw_RenderThread(CommandList, Prepared);
				break;
			case EDrawOperation::XRayGizmos:
				GizmoRenderer->Draw_RenderThread(
					CommandList, View, Prepared, EDepthMode::XRay);
				break;
			case EDrawOperation::XRayOverlayLines:
				OverlayLineRenderer->Draw_RenderThread(
					CommandList, Prepared, EDepthMode::XRay);
				break;
			case EDrawOperation::XRayOverlayIcons:
				OverlayIconRenderer->Draw_RenderThread(
					CommandList, Prepared, EDepthMode::XRay);
				break;
			case EDrawOperation::VisibleGizmos:
				GizmoRenderer->Draw_RenderThread(
					CommandList, View, Prepared, EDepthMode::Visible);
				break;
			case EDrawOperation::VisibleOverlayLines:
				OverlayLineRenderer->Draw_RenderThread(
					CommandList, Prepared, EDepthMode::Visible);
				break;
			case EDrawOperation::VisibleOverlayIcons:
				OverlayIconRenderer->Draw_RenderThread(
					CommandList, Prepared, EDepthMode::Visible);
				break;
			}
		}
	}

	auto FEditorAssistanceRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		EditorGridRenderer->ReleaseResources_RenderThread();
		GizmoRenderer->ReleaseResources_RenderThread();
		OverlayLineRenderer->ReleaseResources_RenderThread();
		OverlayIconRenderer->ReleaseResources_RenderThread();
	}
} // namespace Durin
