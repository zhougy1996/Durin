#include "RendererEditorAssistance.h"

namespace Durin::RendererEditorAssistance
{
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
	}

	auto AnalyzeRequest(
		const FSceneView& View,
		RendererRenderTargetLayouts::EViewportOutput Output) -> FRequest
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

	auto GetRequiredPipelineKeys(const FRequest& Request)
		-> std::vector<FPipelineKey>
	{
		std::vector<FPipelineKey> Keys;
		if (Request.bEditorGrid)
			Keys.push_back(MakePipelineKey(
				Request, EFeature::EditorGrid, EDepthMode::Visible));
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

	auto BuildDrawableOperations(
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

	auto GetDrawOrder() -> std::span<const EDrawOperation>
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
} // namespace Durin::RendererEditorAssistance
