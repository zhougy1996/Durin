#pragma once

#include "Shader/GlobalShader.h"
#include "Resources/RenderTargetLayouts.h"
#include "DynamicRHI.h"
#include "RHICommandList.h"

namespace Durin::Tests
{
	class FDepthCaptureVertexShader final : public FGlobalShader
	{
	public:
		DURIN_DECLARE_GLOBAL_SHADER(FDepthCaptureVertexShader, FGlobalShader,
			"/GeometryQualification/Depth", EShaderFrequency::Vertex, "VertexMain");
	};
	class FDepthCaptureFragmentShader final : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FDepthCaptureFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(SceneDepth);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FDepthCaptureFragmentShader, FGlobalShader,
			"/GeometryQualification/Depth", EShaderFrequency::Fragment, "FragmentMain");
	};
	class FShadowCaptureFragmentShader final : public FGlobalShader
	{
	public:
		DURIN_BEGIN_SHADER_PARAMETERS(FShadowCaptureFragmentShader)
			DURIN_SHADER_PARAMETER_TEXTURE(ShadowDepth);
		DURIN_END_SHADER_PARAMETERS();
		DURIN_DECLARE_GLOBAL_SHADER(FShadowCaptureFragmentShader, FGlobalShader,
			"/GeometryQualification/Depth", EShaderFrequency::Fragment, "ShadowMain");
	};
	inline constexpr std::string_view DepthCaptureSource = R"SLANG(
[[vk::binding(0,0)]] Texture2D<float> SceneDepth;
[[vk::binding(1,0)]] Texture2DArray<float> ShadowDepth;
[shader("vertex")]
float4 VertexMain(uint id : SV_VertexID) : SV_Position {
 float2 p = float2((id << 1) & 2, id & 2);
 return float4(p * 2.0 - 1.0, 0, 1);
}
[shader("fragment")]
float FragmentMain(float4 p : SV_Position) : SV_Target0 {
 return SceneDepth.Load(int3(int2(p.xy), 0));
}
[shader("fragment")]
float ShadowMain(float4 p : SV_Position) : SV_Target0 {
 return ShadowDepth.Load(int4(int2(p.xy), 0, 0));
}
)SLANG";
	inline auto ReadGeometryTexture(FRHICommandListImmediate& CommandList, FRHITexture* Source, FByteBuffer& Pixels, bool bDepth = false, uint32 ArrayLayer = 0) -> void
	{
		if (bDepth)
		{
			FShaderMapBase Map;
			const std::array<const FShaderType*, 2> Types{&FDepthCaptureVertexShader::StaticType(), Source->GetArraySize() > 1 ? static_cast<const FShaderType*>(&FShadowCaptureFragmentShader::StaticType()) : &FDepthCaptureFragmentShader::StaticType()};
			FShaderOperationResult Error;
			check((Error = Map.InitializeFromShaderTypes(Types, {})));
			const auto FloatTexture = GDynamicRHI->RHICreateTexture(CommandList,
				FRHITextureCreateDesc::Create2D("GeometryDepthReadback", Source->GetSizeX(), Source->GetSizeY(), EPixelFormat::R32_FLOAT)
					.SetFlags(ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ShaderResource));
			FGraphicsPipelineStateInitializer Initializer;
			Initializer.RenderTargetLayout = RenderTargetLayouts::MakeGBufferDebugOutput();
			Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.Format = EPixelFormat::R32_FLOAT;
			Initializer.BoundShaders.VertexShader = Map.GetOrCreateShaderRHI(Types[0]);
			Initializer.BoundShaders.FragmentShader = Map.GetOrCreateShaderRHI(Types[1]);
			const auto Declaration = GDynamicRHI->RHICreateVertexDeclaration({});
			Initializer.VertexDeclaration = Declaration;
			Initializer.RasterizerState.CullMode = ERHICullMode::None;
			Initializer.PipelineLayout = Map.GetMergedPipelineLayout();
			const auto Pipeline = GDynamicRHI->RHICreateGraphicsPipelineState("GeometryDepthCapture", Initializer);
			FTextureViewRHIRef ShadowView;
			if (Source->GetArraySize() > 1)
			{
				auto ViewDesc = MakeDefaultTextureViewDesc(*Source, ERHITextureViewUsage::Sampled);
				ViewDesc.Range.FirstArrayLayer = ArrayLayer;
				ViewDesc.Range.NumArrayLayers = 1;
				ShadowView = GDynamicRHI->RHICreateTextureView(Source, ViewDesc);
				check(ShadowView);
			}
			FRHIRenderPassInfo Pass;
			Pass.RenderTargetLayout = Initializer.RenderTargetLayout;
			Pass.ColorRenderTargets[0] = FloatTexture;
			CommandList.BeginRenderPass(Pass, "GeometryDepthCapture");
			CommandList.SetGraphicsPipelineState(*Pipeline);
			CommandList.SetViewport(0, 0, 0, Source->GetSizeX(), Source->GetSizeY(), 1);
			CommandList.SetScissor(0, 0, Source->GetSizeX(), Source->GetSizeY());
			if (Source->GetArraySize() > 1)
			{
				const std::array Parameters{FRHIShaderParameterResource{.Resource = ShadowView.GetReference(),
					.SetIndex = 0, .BindingIndex = 1, .Type = ERHIBindingType::Texture}};
				CommandList.SetShaderParameters(Map.GetOrCreateShaderRHI(Types[1]), Parameters);
			}
			else
			{
				TShaderRef<FDepthCaptureFragmentShader> Fragment(static_cast<FDepthCaptureFragmentShader*>(Map.GetShader(Types[1])), &Map);
				FDepthCaptureFragmentShader::FParameters Parameters;
				Parameters.SceneDepth = Source;
				SetShaderParameters(CommandList, Fragment, Parameters);
			}
			CommandList.Draw(FRHIDrawArguments{.VertexCount = 3});
			CommandList.EndRenderPass();
			check(GDynamicRHI->RHIReadTexture2D(CommandList, FloatTexture, 0, 0, Pixels));
			return;
		}

		const auto Desc = FRHITextureCreateDesc::Create2D("GeometryReadback", Source->GetSizeX(), Source->GetSizeY(), Source->GetFormat())
			.SetFlags(ETextureCreateFlags::DestinationCopy | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ShaderResource);
		const auto Readback = GDynamicRHI->RHICreateTexture(CommandList, Desc);
		const FRHITextureSubresourceRange Whole{bDepth ? ERHITextureAspect::Depth : ERHITextureAspect::Color, 0, 1, 0, 1};
		CommandList.TransitionTextures(std::array{
			FRHITextureTransition{Source, Whole, ERHIAccess::GraphicsShaderRead, ERHIAccess::TransferRead},
			FRHITextureTransition{Readback, Whole, ERHIAccess::Discard, ERHIAccess::TransferWrite}});
		FRHITextureCopyRegion Region;
		Region.Extent = {Source->GetSizeX(), Source->GetSizeY(), 1};
		CommandList.CopyTexture(Source, Readback, std::array{Region});
		CommandList.TransitionTextures(std::array{
			FRHITextureTransition{Source, Whole, ERHIAccess::TransferRead, ERHIAccess::GraphicsShaderRead},
			FRHITextureTransition{Readback, Whole, ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});
		check(GDynamicRHI->RHIReadTexture2D(CommandList, Readback, 0, 0, Pixels));
	}
}
