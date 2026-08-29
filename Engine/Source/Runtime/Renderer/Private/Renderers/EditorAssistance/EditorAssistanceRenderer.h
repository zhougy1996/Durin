#pragma once

#include "EditorGridRendering.h"
#include "Misc/CoreTypes.h"
#include "RHIResources.h"
#include "RendererAPI.h"
#include "Resources/RenderTargetLayouts.h"
#include "Renderers/SimpleElement/SimpleElementRenderer.h"
#include "SceneView.h"
#include "Shader/GlobalShader.h"

namespace Durin
{
	class FEditorGridRenderer;
	class FGizmoRenderer;
	class FSimpleElementRenderer;
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
		SimpleElement,
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
		bool bSimpleElements = false;

		auto IsEmpty() const -> bool
		{
			return !bEditorGrid && !bSolidGizmos
				&& !bSimpleElements;
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
		ForegroundSimpleElements,
		VisibleGizmos,
		WorldSimpleElements,
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
		bool bSolidGizmos = false;
		std::vector<FPreparedPipeline> Pipelines;
		FPreparedSimpleElementRendering SimpleElements;

		auto HasDrawableOperation() const -> bool
		{
			return !Pipelines.empty() || !SimpleElements.IsEmpty();
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
			RenderTargetLayouts::EViewportOutput Output,
			std::span<const FSimpleElement> AdditionalElements = {}) -> FRequest;
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
			const FRequest& Request,
			std::span<const FSimpleElement> AdditionalElements = {}) -> FPrepared;
		auto Draw_RenderThread(
			FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			const FPrepared& Prepared) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		std::unique_ptr<FEditorGridRenderer> EditorGridRenderer;
		std::unique_ptr<FGizmoRenderer> GizmoRenderer;
		std::unique_ptr<FSimpleElementRenderer> SimpleElementRenderer;
	};
} // namespace Durin
