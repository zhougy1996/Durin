#pragma once

#include "CoreMinimal.h"
#include "MetalAutoreleasePool.h"
#include "RHIResources.h"
#include "MetalShaderFormat.h"
#include "MetalBindingPlan.h"
#include "PipelineStateCache.h"
#include <Metal/Metal.hpp>

namespace Durin
{
	class FMetalShader final : public FRHIShader
	{
	public:
		FMetalShader(const FRHIShaderCreateDesc& Desc, ShaderFormat::FMetalBindingMap InBindings,
			NS::SharedPtr<MTL::Library> InLibrary, NS::SharedPtr<MTL::Function> InFunction)
			: FRHIShader(Desc), Bindings(std::move(InBindings)), Library(std::move(InLibrary)), Function(std::move(InFunction)) {}

		~FMetalShader() override
		{
			const FMetalAutoreleasePool Pool;
			Function.reset();
			Library.reset();
		}

		auto GetMetalBindings() const -> std::span<const FMetalShaderBinding> { return Bindings.Bindings; }
		auto GetMetalPushConstantBufferSlot() const -> uint32 { return Bindings.PushConstantBufferSlot; }
		auto GetFunction() const -> MTL::Function* { return Function.get(); }

	private:
		ShaderFormat::FMetalBindingMap Bindings;
		NS::SharedPtr<MTL::Library> Library;
		NS::SharedPtr<MTL::Function> Function;
	};

	class FMetalComputePipelineState final : public FRHIComputePipelineState
	{
	public:
		FMetalComputePipelineState(FRHIShader* InShader,
			std::shared_ptr<const FPipelineLayoutDesc> InLayout, NS::SharedPtr<MTL::ComputePipelineState> InPipeline,
			std::shared_ptr<const FMetalBindingPlan> InBindings)
			: Shader(InShader), Layout(std::move(InLayout)), Pipeline(std::move(InPipeline)), Bindings(std::move(InBindings)) {}

		~FMetalComputePipelineState() override
		{
			const FMetalAutoreleasePool Pool;
			Pipeline.reset();
		}

		auto GetPipeline() const -> MTL::ComputePipelineState* { return Pipeline.get(); }
		auto GetBindingPlan() const -> const std::shared_ptr<const FMetalBindingPlan>& { return Bindings; }
		auto GetLayout() const -> const FPipelineLayoutDesc& { return *Layout; }
		auto GetShader() const -> FMetalShader* { return static_cast<FMetalShader*>(Shader.GetReference()); }

	private:
		FShaderRHIRef Shader;
		std::shared_ptr<const FPipelineLayoutDesc> Layout;
		NS::SharedPtr<MTL::ComputePipelineState> Pipeline;
		std::shared_ptr<const FMetalBindingPlan> Bindings;
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
			std::shared_ptr<const FPipelineLayoutDesc> InLayout,
			NS::SharedPtr<MTL::DepthStencilState> InDepthStencil,
			std::shared_ptr<const FMetalBindingPlan> InBindings)
			: Vertex(InVertex), Fragment(InFragment), Declaration(InDeclaration),
				Pipeline(std::move(InPipeline)), RenderTargets(std::move(InRenderTargets)),
				Rasterizer(InRasterizer), PrimitiveType(InPrimitiveType),
				Layout(std::move(InLayout)),
				DepthStencil(std::move(InDepthStencil)), Bindings(std::move(InBindings)) {}
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
		auto GetBindingPlan() const -> const std::shared_ptr<const FMetalBindingPlan>& { return Bindings; }
		auto GetLayout() const -> const FPipelineLayoutDesc& { return *Layout; }
		auto GetVertexShader() const -> FMetalShader* { return static_cast<FMetalShader*>(Vertex.GetReference()); }
		auto GetFragmentShader() const -> FMetalShader* { return static_cast<FMetalShader*>(Fragment.GetReference()); }
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
		std::shared_ptr<const FPipelineLayoutDesc> Layout;
		NS::SharedPtr<MTL::DepthStencilState> DepthStencil;
		std::shared_ptr<const FMetalBindingPlan> Bindings;
	};

	auto ToMetalVertexFormat(EVertexElementType Type) -> MTL::VertexFormat;
	auto CreateMetalGraphicsPipeline(MTL::Device* Device, const FGraphicsPipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIGraphicsPipelineState>;
	auto CreateMetalComputePipeline(MTL::Device* Device, const FComputePipelineStateInitializer& Initializer) -> TRefCountPtr<FRHIComputePipelineState>;
	auto CreateMetalShader(MTL::Device* Device, const FRHIShaderCreateDesc& Desc) -> TRefCountPtr<FRHIShader>;
}
