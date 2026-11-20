#pragma once

#include "CoreMinimal.h"
#include "PipelineStateCache.h"

#include "Renderers/SimpleElement/SimpleElementCollector.h"
#include "Shader/GlobalShader.h"

namespace Durin
{
	class FSimpleLineFragmentShader;
	class FSimpleSpriteFragmentShader;

	class FRHICommandListImmediate;

	struct FSimplePipelineKey
	{
		ESimpleElementShaderClass ShaderClass =
			ESimpleElementShaderClass::Untextured;
		ESimpleElementBlendMode BlendMode = ESimpleElementBlendMode::Opaque;
		ESceneDepthPriorityGroup DepthPriorityGroup =
			ESceneDepthPriorityGroup::World;
		ESceneDepthConvention DepthConvention =
			ESceneDepthConvention::ForwardZ;
		RenderTargetLayouts::EViewportOutput Output =
			RenderTargetLayouts::EViewportOutput::Offscreen;
		EPixelFormat OutputFormat = EPixelFormat::SRGBA8_UNORM;

		auto operator==(const FSimplePipelineKey&) const -> bool = default;
	};

	struct FPreparedSimpleElementPipeline
	{
		FSimplePipelineKey Key;
		FVertexDeclarationRHIRef VertexDeclaration;
		FGraphicsPipelineStateInitializer Initializer;
	};

	struct FPreparedSimpleElementDraw
	{
		FSimpleElementBatchKey Key;
		size_t PipelineIndex = 0;
		TShaderMapRef<FSimpleLineFragmentShader> LineShader;
		TShaderMapRef<FSimpleSpriteFragmentShader> SpriteShader;
		FTextureRHIRef Texture;
		FSamplerRHIRef Sampler;
		uint32 IndexCount = 0;
		uint32 StartIndex = 0;
		int32 VertexOffset = 0;
	};

	struct FPreparedSimpleElementRendering
	{
		std::vector<FPreparedSimpleElementPipeline> Pipelines;
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
		FSimpleElementRenderer();
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
		std::unique_ptr<FState> State;
	};
} // namespace Durin
