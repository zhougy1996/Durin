#pragma once

#include "Renderers/SimpleElement/SimpleElementCollector.h"
#include "Shader/GlobalShader.h"

namespace Durin
{
	class FRendererResourceCoordinator;
	class FRHICommandListImmediate;

	struct FPreparedSimpleElementDraw
	{
		FSimpleElementBatchKey Key;
		FGraphicsPipelineStateRHIRef Pipeline;
		FGlobalShaderSetRef ShaderSet;
		FTextureRHIRef Texture;
		FSamplerRHIRef Sampler;
		uint32 IndexCount = 0;
		uint32 StartIndex = 0;
		int32 VertexOffset = 0;
	};

	struct FPreparedSimpleElementRendering
	{
		std::vector<FPreparedSimpleElementDraw> Draws;
		FSimpleElementCollectionStatistics Statistics;
		uint32 VertexCapacity = 0;
		uint32 IndexCapacity = 0;

		auto IsEmpty() const -> bool { return Draws.empty(); }
	};

	// Owns global shaders, pipelines, and bounded uploads for simple elements.
	class FSimpleElementRenderer final
	{
	public:
		explicit FSimpleElementRenderer(
			FRendererResourceCoordinator& InCoordinator);
		~FSimpleElementRenderer();

		FSimpleElementRenderer(const FSimpleElementRenderer&) = delete;
		auto operator=(const FSimpleElementRenderer&)
			-> FSimpleElementRenderer& = delete;

		auto Prepare_RenderThread(FRHICommandListImmediate& CommandList,
			const FSceneView& View,
			RenderTargetLayouts::EViewportOutput Output,
			std::span<const FSimpleElement> AdditionalElements = {},
			EPixelFormat OutputFormat = EPixelFormat::SRGBA8_UNORM)
			-> FPreparedSimpleElementRendering;
		auto Draw_RenderThread(FRHICommandListImmediate& CommandList,
			const FPreparedSimpleElementRendering& Prepared,
			ESceneDepthPriorityGroup DepthPriorityGroup) -> void;
		auto ReleaseResources_RenderThread() -> void;

	private:
		struct FState;
		FRendererResourceCoordinator& Coordinator;
		std::unique_ptr<FState> State;
	};
} // namespace Durin
