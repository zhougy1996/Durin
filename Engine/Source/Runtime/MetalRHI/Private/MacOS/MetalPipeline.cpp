#include "MetalPipeline.h"
#include "MetalResourceDescriptors.h"
#include "RHIShaderParameters.h"

namespace Durin
{
	auto ToMetalVertexFormat(EVertexElementType Type) -> MTL::VertexFormat
	{
		switch (Type)
		{
		case EVertexElementType::Float1: return MTL::VertexFormatFloat;
		case EVertexElementType::Float2: return MTL::VertexFormatFloat2;
		case EVertexElementType::Float3: return MTL::VertexFormatFloat3;
		case EVertexElementType::Float4: return MTL::VertexFormatFloat4;
		case EVertexElementType::Color:
		case EVertexElementType::UByte4N: return MTL::VertexFormatUChar4Normalized;
		case EVertexElementType::Half2: return MTL::VertexFormatHalf2;
		case EVertexElementType::Half4: return MTL::VertexFormatHalf4;
		case EVertexElementType::Short4N: return MTL::VertexFormatShort4Normalized;
		default: return MTL::VertexFormatInvalid;
		}
	}

	static auto ToMetalDepthCompare(ERHIDepthCompareOp Op)
		-> std::optional<MTL::CompareFunction>
	{
		switch (Op)
		{
		case ERHIDepthCompareOp::Never: return MTL::CompareFunctionNever;
		case ERHIDepthCompareOp::Less: return MTL::CompareFunctionLess;
		case ERHIDepthCompareOp::Equal: return MTL::CompareFunctionEqual;
		case ERHIDepthCompareOp::LessOrEqual: return MTL::CompareFunctionLessEqual;
		case ERHIDepthCompareOp::Greater: return MTL::CompareFunctionGreater;
		case ERHIDepthCompareOp::NotEqual: return MTL::CompareFunctionNotEqual;
		case ERHIDepthCompareOp::GreaterOrEqual: return MTL::CompareFunctionGreaterEqual;
		case ERHIDepthCompareOp::Always: return MTL::CompareFunctionAlways;
		default: return std::nullopt;
		}
	}

	static auto ToMetalBlendFactor(ERHIBlendFactor Factor)
		-> std::optional<MTL::BlendFactor>
	{
		switch (Factor)
		{
		case ERHIBlendFactor::Zero: return MTL::BlendFactorZero;
		case ERHIBlendFactor::One: return MTL::BlendFactorOne;
		case ERHIBlendFactor::SrcColor: return MTL::BlendFactorSourceColor;
		case ERHIBlendFactor::OneMinusSrcColor:
			return MTL::BlendFactorOneMinusSourceColor;
		case ERHIBlendFactor::DstColor: return MTL::BlendFactorDestinationColor;
		case ERHIBlendFactor::OneMinusDstColor:
			return MTL::BlendFactorOneMinusDestinationColor;
		case ERHIBlendFactor::SrcAlpha: return MTL::BlendFactorSourceAlpha;
		case ERHIBlendFactor::OneMinusSrcAlpha:
			return MTL::BlendFactorOneMinusSourceAlpha;
		case ERHIBlendFactor::DstAlpha: return MTL::BlendFactorDestinationAlpha;
		case ERHIBlendFactor::OneMinusDstAlpha:
			return MTL::BlendFactorOneMinusDestinationAlpha;
		case ERHIBlendFactor::ConstantColor: return MTL::BlendFactorBlendColor;
		case ERHIBlendFactor::OneMinusConstantColor:
			return MTL::BlendFactorOneMinusBlendColor;
		case ERHIBlendFactor::ConstantAlpha: return MTL::BlendFactorBlendAlpha;
		case ERHIBlendFactor::OneMinusConstantAlpha:
			return MTL::BlendFactorOneMinusBlendAlpha;
		case ERHIBlendFactor::SrcAlphaSaturate:
			return MTL::BlendFactorSourceAlphaSaturated;
		default: return std::nullopt;
		}
	}

	static auto ToMetalBlendOperation(ERHIBlendOp Op)
		-> std::optional<MTL::BlendOperation>
	{
		switch (Op)
		{
		case ERHIBlendOp::Add: return MTL::BlendOperationAdd;
		case ERHIBlendOp::Subtract: return MTL::BlendOperationSubtract;
		case ERHIBlendOp::ReverseSubtract:
			return MTL::BlendOperationReverseSubtract;
		case ERHIBlendOp::Min: return MTL::BlendOperationMin;
		case ERHIBlendOp::Max: return MTL::BlendOperationMax;
		default: return std::nullopt;
		}
	}


	static auto CopyMetalError(const NS::Error* Error) -> std::string
	{
		const auto* Description = Error ? Error->localizedDescription() : nullptr;
		const char* Text = Description ? Description->utf8String() : nullptr;
		return Text ? Text : "unknown error";
	}
	auto CreateMetalGraphicsPipeline(MTL::Device* Device, const FGraphicsPipelineStateInitializer& Initializer)
		-> TRefCountPtr<FRHIGraphicsPipelineState>
	{
		const FMetalAutoreleasePool Pool;
		if (!Device || !Initializer.BoundShaders.VertexShader
			|| !Initializer.BoundShaders.FragmentShader
			|| !Initializer.VertexDeclaration
			|| (Initializer.PrimitiveTopology !=
				FGraphicsPipelineStateInitializer::EPrimitiveTopology::TriangleList
			&& Initializer.PrimitiveTopology !=
				FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList)
			|| Initializer.RenderTargetLayout.NumColorRenderTargets > 4
			|| (Initializer.RenderTargetLayout.NumColorRenderTargets == 0
				&& !Initializer.RenderTargetLayout.bHasDepthStencil)
			|| (Initializer.RenderTargetLayout.bHasDepthStencil
				&& Initializer.RenderTargetLayout.DepthStencilAttachment.Format
					!= EPixelFormat::D32)
			|| Initializer.MultisampleState != FRHIMultisampleState{}
			|| Initializer.DepthStencilState.bEnableStencil
			|| (!Initializer.RenderTargetLayout.bHasDepthStencil
				&& Initializer.DepthStencilState != FRHIDepthStencilState{})
			|| Initializer.RasterizerState.PolygonMode != ERHIPolygonMode::Fill
			|| Initializer.RasterizerState.bEnableDepthClamp
			|| Initializer.RasterizerState.LineWidth != 1.0f)
			return nullptr;
		auto Key = BuildGraphicsPipelineStateKey(Initializer, nullptr);
		if (!Key || Key->Target != MetalShaderTarget) return nullptr;
		auto* Vertex = dynamic_cast<FMetalShader*>(
			Initializer.BoundShaders.VertexShader);
		auto* Fragment = dynamic_cast<FMetalShader*>(
			Initializer.BoundShaders.FragmentShader);
		if (!Vertex || !Fragment)
			return nullptr;
		const auto MatchesStage = [&](FMetalShader* Shader,
		EShaderStageFlags Stage) {
			const auto Map = Shader->GetMetalBindings();
			size_t Count = 0;
			for (uint32 SetIndex = 0;
				SetIndex < Key->PipelineLayout.BindingLayouts.size(); ++SetIndex)
				for (const auto& Binding : Key->PipelineLayout
					.BindingLayouts[SetIndex].BindingLayouts)
				{
					if (!EnumHasAnyFlags(Binding.StageFlags, Stage)) continue;
					if (Count >= Map.size()
						|| Map[Count].SetIndex != SetIndex
						|| Map[Count].BindingIndex != Binding.Slot
						|| Map[Count].Type != (Binding.Type == ERHIBindingType::UniformBufferDynamic
							? ERHIBindingType::UniformBuffer : Binding.Type)
						|| Map[Count].Count != Binding.ArraySize)
						return false;
					++Count;
				}
			return Count == Map.size();
		};
		if (!MatchesStage(Vertex, EShaderStageFlags::Vertex)
			|| !MatchesStage(Fragment, EShaderStageFlags::Fragment))
			return nullptr;
		for (const auto Stage : {EShaderStageFlags::Vertex,
			EShaderStageFlags::Fragment})
		{
			const bool bPush = std::ranges::any_of(
				Key->PipelineLayout.PushConstantRanges,
				[&](const auto& Range) {
					return EnumHasAnyFlags(Range.StageFlags, Stage);
				});
			auto* Shader = Stage == EShaderStageFlags::Vertex
				? Vertex : Fragment;
			if (bPush != (Shader->GetMetalPushConstantBufferSlot()
				!= UINT32_MAX)) return nullptr;
		}
		for (const auto& Range : Key->PipelineLayout.PushConstantRanges)
			if (uint64(Range.Offset) + Range.Size > 65536)
				return nullptr;
		auto Desc = NS::TransferPtr(MTL::RenderPipelineDescriptor::alloc()->init());
		Desc->setVertexFunction(Vertex->GetFunction());
		Desc->setFragmentFunction(Fragment->GetFunction());
		Desc->setSampleCount(1);
		const bool bLineList = Initializer.PrimitiveTopology
			== FGraphicsPipelineStateInitializer::EPrimitiveTopology::LineList;
		Desc->setInputPrimitiveTopology(bLineList
			? MTL::PrimitiveTopologyClassLine : MTL::PrimitiveTopologyClassTriangle);
		for (uint32 Index = 0;
			Index < Initializer.RenderTargetLayout.NumColorRenderTargets; ++Index)
		{
			const auto Format = Initializer.RenderTargetLayout.ColorAttachments[Index]
				.RenderTarget.Format;
			if (!IsMetalColorRenderFormat(Format))
				return nullptr;
			Desc->colorAttachments()->object(Index)->setPixelFormat(ToMetalPixelFormat(Format));
			const auto& Blend = Initializer.ColorBlendStates[Index];
			auto* NativeBlend = Desc->colorAttachments()->object(Index);
			NativeBlend->setBlendingEnabled(Blend.bEnable);
			MTL::ColorWriteMask WriteMask = MTL::ColorWriteMaskNone;
			if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Red))
				WriteMask |= MTL::ColorWriteMaskRed;
			if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Green))
				WriteMask |= MTL::ColorWriteMaskGreen;
			if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Blue))
				WriteMask |= MTL::ColorWriteMaskBlue;
			if (EnumHasAnyFlags(Blend.ColorWriteMask, ERHIColorWriteMask::Alpha))
				WriteMask |= MTL::ColorWriteMaskAlpha;
			NativeBlend->setWriteMask(WriteMask);
			if (Blend.bEnable)
			{
				const auto SrcColor = ToMetalBlendFactor(Blend.SrcColorFactor);
				const auto DstColor = ToMetalBlendFactor(Blend.DstColorFactor);
				const auto SrcAlpha = ToMetalBlendFactor(Blend.SrcAlphaFactor);
				const auto DstAlpha = ToMetalBlendFactor(Blend.DstAlphaFactor);
				const auto ColorOp = ToMetalBlendOperation(Blend.ColorOp);
				const auto AlphaOp = ToMetalBlendOperation(Blend.AlphaOp);
				if (!SrcColor || !DstColor || !SrcAlpha || !DstAlpha
					|| !ColorOp || !AlphaOp) return nullptr;
				NativeBlend->setSourceRGBBlendFactor(*SrcColor);
				NativeBlend->setDestinationRGBBlendFactor(*DstColor);
				NativeBlend->setRgbBlendOperation(*ColorOp);
				NativeBlend->setSourceAlphaBlendFactor(*SrcAlpha);
				NativeBlend->setDestinationAlphaBlendFactor(*DstAlpha);
				NativeBlend->setAlphaBlendOperation(*AlphaOp);
			}
		}
		if (Initializer.RenderTargetLayout.bHasDepthStencil)
			Desc->setDepthAttachmentPixelFormat(MTL::PixelFormatDepth32Float);
		auto VertexDesc = NS::RetainPtr(MTL::VertexDescriptor::vertexDescriptor());
		for (const auto& Element : Initializer.VertexDeclaration->GetElements())
		{
			if (Element.Type == EVertexElementType::None) break;
			const MTL::VertexFormat Format = ToMetalVertexFormat(Element.Type);
			if (Format == MTL::VertexFormatInvalid
				|| Element.StreamIndex >= 16 || Element.AttributeIndex >= 31)
				return nullptr;
			auto* Attribute = VertexDesc->attributes()->object(Element.AttributeIndex);
			Attribute->setFormat(Format);
			Attribute->setOffset(Element.Offset);
			Attribute->setBufferIndex(Element.StreamIndex);
			auto* Layout = VertexDesc->layouts()->object(Element.StreamIndex);
			Layout->setStride(Element.Stride);
			Layout->setStepFunction(Element.InputRate
				== FRHIVertexElementIdentity::EInputRate::Instance
				? MTL::VertexStepFunctionPerInstance
				: MTL::VertexStepFunctionPerVertex);
		}
		Desc->setVertexDescriptor(VertexDesc.get());
		NS::Error* Error = nullptr;
		auto Pipeline = NS::TransferPtr(Device->newRenderPipelineState(Desc.get(), &Error));
		if (!Pipeline)
		{
			DURIN_ERROR("Metal graphics pipeline creation failed: {}",
				CopyMetalError(Error));
			return nullptr;
		}
		const auto Compare = ToMetalDepthCompare(Initializer.DepthStencilState.CompareOp);
		if (!Compare) return nullptr;
		auto DepthDesc = NS::TransferPtr(MTL::DepthStencilDescriptor::alloc()->init());
		// Metal validation requires an explicit disabled state for color-only passes.
		DepthDesc->setDepthCompareFunction(Initializer.RenderTargetLayout.bHasDepthStencil
			&& Initializer.DepthStencilState.bEnableTest ? *Compare : MTL::CompareFunctionAlways);
		DepthDesc->setDepthWriteEnabled(Initializer.RenderTargetLayout.bHasDepthStencil
			&& Initializer.DepthStencilState.bEnableWrite);
		auto DepthStencil = NS::TransferPtr(Device->newDepthStencilState(DepthDesc.get()));
		if (!DepthStencil) return nullptr;
		return new FMetalGraphicsPipelineState(Vertex, Fragment,
			Initializer.VertexDeclaration, std::move(Pipeline),
			Initializer.RenderTargetLayout, Initializer.RasterizerState,
			bLineList ? MTL::PrimitiveTypeLine : MTL::PrimitiveTypeTriangle,
			std::move(Key->PipelineLayout), std::move(DepthStencil));
	}

	auto CreateMetalComputePipeline(MTL::Device* Device, const FComputePipelineStateInitializer& Initializer)
		-> TRefCountPtr<FRHIComputePipelineState>
	{
		const FMetalAutoreleasePool Pool;
		if (!Device || !Initializer.ComputeShader
			|| Initializer.ComputeShader->GetTarget() != MetalShaderTarget)
			return nullptr;
		auto Key = BuildComputePipelineStateKey(Initializer, nullptr);
		if (!Key) return nullptr;
		auto* Shader = dynamic_cast<FMetalShader*>(Initializer.ComputeShader);
		if (!Shader) return nullptr;
		const auto Bindings = Shader->GetMetalBindings();
		size_t Count = 0;
		for (uint32 SetIndex = 0;
			SetIndex < Key->PipelineLayout.BindingLayouts.size(); ++SetIndex)
			for (const auto& Binding : Key->PipelineLayout.BindingLayouts[SetIndex].BindingLayouts)
			{
				if (Count >= Bindings.size()
					|| Bindings[Count].SetIndex != SetIndex
					|| Bindings[Count].BindingIndex != Binding.Slot
					|| Bindings[Count].Type != (Binding.Type == ERHIBindingType::UniformBufferDynamic
						? ERHIBindingType::UniformBuffer : Binding.Type)
					|| Bindings[Count].Count != Binding.ArraySize)
					return nullptr;
				++Count;
			}
		if (Count != Bindings.size()
			|| Key->PipelineLayout.PushConstantRanges.empty()
				!= (Shader->GetMetalPushConstantBufferSlot() == UINT32_MAX))
			return nullptr;
		for (const auto& Range : Key->PipelineLayout.PushConstantRanges)
			if (uint64(Range.Offset) + Range.Size > 65536)
				return nullptr;
		NS::Error* Error = nullptr;
		auto Pipeline = NS::TransferPtr(Device->newComputePipelineState(Shader->GetFunction(), &Error));
		if (!Pipeline)
		{
			DURIN_ERROR("Metal compute pipeline creation failed: {}",
				CopyMetalError(Error));
			return nullptr;
		}
		const auto Group = Shader->GetComputeThreadGroupSize();
		if (uint64(Group[0]) * Group[1] * Group[2]
			> Pipeline->maxTotalThreadsPerThreadgroup()) return nullptr;
		return new FMetalComputePipelineState(Shader,
			std::move(Key->PipelineLayout), std::move(Pipeline));
	}

	auto CreateMetalShader(MTL::Device* Device, const FRHIShaderCreateDesc& Desc)
		-> TRefCountPtr<FRHIShader>
	{
		const FMetalAutoreleasePool Pool;
		if (!Device || Desc.Target != MetalShaderTarget
			|| Desc.CodeFormat != EShaderCodeFormat::Msl20Source
			|| !Desc.EntryPoint || !*Desc.EntryPoint
			|| Desc.Code.size() < 32
			|| std::ranges::find(Desc.Code, std::byte{0}) != Desc.Code.end()
			|| FXxHash128::HashBuffer(Desc.Code) != Desc.Hash
			|| (Desc.Frequency == EShaderFrequency::Compute
				&& !IsValidComputeThreadGroupSize(Desc.ComputeThreadGroupSize))
			|| (Desc.Frequency != EShaderFrequency::Compute
				&& Desc.ComputeThreadGroupSize != std::array<uint32, 3>{})
			|| !ValidateMetalBindingRemap(Desc.Frequency,
				Desc.MetalBindings, Desc.MetalPushConstantBufferSlot,
				Desc.BindingRemapIdentity)) return nullptr;
		const std::string_view Source(
			reinterpret_cast<const char*>(Desc.Code.data()), Desc.Code.size());
		if (!Source.substr(0, 256).contains("#include <metal_stdlib>"))
			return nullptr;
		const std::string SourceText(Source);
		auto Text = NS::TransferPtr(NS::String::alloc()->init(SourceText.c_str(), NS::UTF8StringEncoding));
		if (!Text) return nullptr;
		auto Options = NS::TransferPtr(MTL::CompileOptions::alloc()->init());
		Options->setLanguageVersion(MTL::LanguageVersion2_0);
		NS::Error* Error = nullptr;
		auto Library = NS::TransferPtr(Device->newLibrary(Text.get(), Options.get(), &Error));
		if (!Library)
		{
			DURIN_ERROR("Metal shader library compilation failed: {}",
				CopyMetalError(Error));
			return nullptr;
		}
		auto Entry = NS::RetainPtr(NS::String::string(Desc.EntryPoint, NS::UTF8StringEncoding));
		if (!Entry) return nullptr;
		auto Function = NS::TransferPtr(Library->newFunction(Entry.get()));
		if (!Function) return nullptr;
		const MTL::FunctionType Expected = Desc.Frequency == EShaderFrequency::Vertex
			? MTL::FunctionTypeVertex
			: Desc.Frequency == EShaderFrequency::Fragment
				? MTL::FunctionTypeFragment : MTL::FunctionTypeKernel;
		if (Function->functionType() != Expected) return nullptr;
		return new FMetalShader(Desc, std::move(Library), std::move(Function));
	}
}
