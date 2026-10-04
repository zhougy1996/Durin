#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"
#include "PipelineStateCache.h"
#include <Metal/Metal.hpp>

namespace Durin
{
	class FMetalShader final : public FRHIShader
	{
	public:
		FMetalShader(const FRHIShaderCreateDesc& Desc,
			NS::SharedPtr<MTL::Library> InLibrary, NS::SharedPtr<MTL::Function> InFunction)
			: FRHIShader(Desc), Library(std::move(InLibrary)), Function(std::move(InFunction)) {}

		~FMetalShader() override
		{
			const FMetalAutoreleasePool Pool;
			Function.reset();
			Library.reset();
		}

		auto GetFunction() const -> MTL::Function* { return Function.get(); }

	private:
		NS::SharedPtr<MTL::Library> Library;
		NS::SharedPtr<MTL::Function> Function;
	};

	class FMetalComputePipelineState final : public FRHIComputePipelineState
	{
	public:
		FMetalComputePipelineState(FRHIShader* InShader,
			FPipelineLayoutDesc InLayout, NS::SharedPtr<MTL::ComputePipelineState> InPipeline)
			: Shader(InShader), Layout(std::move(InLayout)), Pipeline(std::move(InPipeline)) {}

		~FMetalComputePipelineState() override
		{
			const FMetalAutoreleasePool Pool;
			Pipeline.reset();
		}

		auto GetPipeline() const -> MTL::ComputePipelineState* { return Pipeline.get(); }
		auto GetLayout() const -> const FPipelineLayoutDesc& { return Layout; }
		auto GetShader() const -> FRHIShader* { return Shader.GetReference(); }

	private:
		FShaderRHIRef Shader;
		FPipelineLayoutDesc Layout;
		NS::SharedPtr<MTL::ComputePipelineState> Pipeline;
	};

	class FMetalVertexDeclaration final : public FRHIVertexDeclaration
	{
	public:
		explicit FMetalVertexDeclaration(FVertexDeclarationElementList InElements)
			: Elements(std::move(InElements)) {}
		auto GetElements() const -> const FVertexDeclarationElementList& override
		{ return Elements; }
	private:
		FVertexDeclarationElementList Elements;
	};

	class FMetalGraphicsPipelineState final : public FRHIGraphicsPipelineState
	{
	public:
		FMetalGraphicsPipelineState(FRHIShader* InVertex,
			FRHIShader* InFragment, FRHIVertexDeclaration* InDeclaration,
			NS::SharedPtr<MTL::RenderPipelineState> InPipeline,
			FRHIRenderTargetLayout InRenderTargets,
			FRHIRasterizerState InRasterizer,
			MTL::PrimitiveType InPrimitiveType,
			FPipelineLayoutDesc InLayout,
			NS::SharedPtr<MTL::DepthStencilState> InDepthStencil)
			: Vertex(InVertex), Fragment(InFragment), Declaration(InDeclaration),
				Pipeline(std::move(InPipeline)), RenderTargets(std::move(InRenderTargets)),
				Rasterizer(InRasterizer), PrimitiveType(InPrimitiveType),
				Layout(std::move(InLayout)),
				DepthStencil(std::move(InDepthStencil)) {}
		~FMetalGraphicsPipelineState() override
		{
			const FMetalAutoreleasePool Pool;
			DepthStencil.reset();
			Pipeline.reset();
		}

		auto GetPipeline() const -> MTL::RenderPipelineState* { return Pipeline.get(); }
		auto GetRenderTargets() const -> const FRHIRenderTargetLayout&
			{ return RenderTargets; }
		auto GetRasterizer() const -> const FRHIRasterizerState&
			{ return Rasterizer; }
		auto GetPrimitiveType() const -> MTL::PrimitiveType { return PrimitiveType; }
		auto GetLayout() const -> const FPipelineLayoutDesc& { return Layout; }
		auto GetVertexShader() const -> FRHIShader* { return Vertex.GetReference(); }
		auto GetFragmentShader() const -> FRHIShader* { return Fragment.GetReference(); }
		auto GetDepthStencil() const -> MTL::DepthStencilState*
			{ return DepthStencil.get(); }
		auto GetRequiredVertexStreams() const -> uint16
		{
			uint16 Streams = 0;
			for (const auto& Element : Declaration->GetElements())
			{
				if (Element.Type == EVertexElementType::None) break;
				Streams |= uint16(1u << Element.StreamIndex);
			}
			return Streams;
		}
	private:
		FShaderRHIRef Vertex;
		FShaderRHIRef Fragment;
		FVertexDeclarationRHIRef Declaration;
		NS::SharedPtr<MTL::RenderPipelineState> Pipeline;
		FRHIRenderTargetLayout RenderTargets;
		FRHIRasterizerState Rasterizer;
		MTL::PrimitiveType PrimitiveType;
		FPipelineLayoutDesc Layout;
		NS::SharedPtr<MTL::DepthStencilState> DepthStencil;
	};

	auto ToMetalVertexFormat(EVertexElementType Type) -> MTL::VertexFormat;
	auto CreateMetalGraphicsPipeline(MTL::Device* Device, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>;
	auto CreateMetalComputePipeline(MTL::Device* Device, const FComputePipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIComputePipelineState>;
	auto CreateMetalShader(MTL::Device* Device, const FRHIShaderCreateDesc& Desc) -> TRefCountPtr<FRHIShader>;
}
