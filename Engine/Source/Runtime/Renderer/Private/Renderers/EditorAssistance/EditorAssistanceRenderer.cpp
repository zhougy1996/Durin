#include "Renderers/EditorAssistance/EditorAssistanceRenderer.h"

#include "Renderers/EditorAssistance/EditorGridRenderer.h"
#include "Renderers/EditorAssistance/GizmoRenderer.h"
#include "Renderers/SimpleElement/SimpleElementRenderer.h"
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
				.DepthConvention = Request.DepthConvention,
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
		, SimpleElementRenderer(
			std::make_unique<FSimpleElementRenderer>(InCoordinator))
	{
	}

	FEditorAssistanceRenderer::~FEditorAssistanceRenderer() = default;

	auto FEditorAssistanceRenderer::AnalyzeRequest(
		const FSceneView& View,
		RenderTargetLayouts::EViewportOutput Output,
		std::span<const FSimpleElement> AdditionalElements) -> FRequest
	{
		FRequest Request;
		Request.Output = Output;
		Request.DepthConvention = View.DepthConvention;
		Request.bEditorGrid = View.EditorGrid.bVisible;
		Request.bSimpleElements = !View.SimpleElements.GetElements().empty()
			|| !AdditionalElements.empty();
		for (const FViewOverlayPrimitive& Primitive : View.OverlayPrimitives)
			Request.bSolidGizmos = true;
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
			return Request.bSolidGizmos
					&& HasFeaturePipeline(
						EFeature::Gizmo, DepthMode, EGizmoTopology::Solid);
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
			case EDrawOperation::ForegroundSimpleElements:
				bDrawable = Request.bSimpleElements;
				break;
			case EDrawOperation::VisibleGizmos:
				bDrawable = HasGizmoPipeline(EDepthMode::Visible);
				break;
			case EDrawOperation::WorldSimpleElements:
				bDrawable = Request.bSimpleElements;
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
			EDrawOperation::ForegroundSimpleElements,
			EDrawOperation::VisibleGizmos,
			EDrawOperation::WorldSimpleElements,
		};
		return DrawOrder;
	}

	auto FEditorAssistanceRenderer::Prepare_RenderThread(
		FRHICommandListImmediate& CommandList,
		const FSceneView& View,
		const FRequest& Request,
		std::span<const FSimpleElement> AdditionalElements) -> FPrepared
	{
		check(IsInRenderingThread());
		FPrepared Prepared;
		if (Request.bEditorGrid)
		{
			EditorGridRenderer->Prepare_RenderThread(
				CommandList, View, Request.Output, Prepared);
		}
		if (Request.bSolidGizmos)
		{
			GizmoRenderer->Prepare_RenderThread(
				CommandList, Request, Prepared);
		}
		if (Request.bSimpleElements)
		{
			Prepared.SimpleElements = SimpleElementRenderer->Prepare_RenderThread(
				CommandList, View, Request.Output, AdditionalElements);
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
			case EDrawOperation::ForegroundSimpleElements:
				SimpleElementRenderer->Draw_RenderThread(CommandList,
					Prepared.SimpleElements,
					ESceneDepthPriorityGroup::Foreground);
				break;
			case EDrawOperation::VisibleGizmos:
				GizmoRenderer->Draw_RenderThread(
					CommandList, View, Prepared, EDepthMode::Visible);
				break;
			case EDrawOperation::WorldSimpleElements:
				SimpleElementRenderer->Draw_RenderThread(CommandList,
					Prepared.SimpleElements,
					ESceneDepthPriorityGroup::World);
				break;
			}
		}
	}

	auto FEditorAssistanceRenderer::ReleaseResources_RenderThread() -> void
	{
		check(IsInRenderingThread());
		EditorGridRenderer->ReleaseResources_RenderThread();
		GizmoRenderer->ReleaseResources_RenderThread();
		SimpleElementRenderer->ReleaseResources_RenderThread();
	}
} // namespace Durin
