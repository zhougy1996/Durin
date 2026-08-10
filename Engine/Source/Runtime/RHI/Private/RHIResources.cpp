#include "RHIResources.h"

#include "RHI.h"
#include "RHICapabilities.h"

#include "Math/Operations.h"

#include "Math/Vector.h"

namespace Durin
{
	namespace
	{
		auto IsValidSampleCount(uint8 Samples) -> bool
		{
			return Samples == 1 || Samples == 2 || Samples == 4
				|| Samples == 8 || Samples == 16;
		}

		auto SampleCountFlag(uint8 Samples) -> ERHISampleCountFlags
		{
			switch (Samples)
			{
			case 1: return ERHISampleCountFlags::Samples1;
			case 2: return ERHISampleCountFlags::Samples2;
			case 4: return ERHISampleCountFlags::Samples4;
			case 8: return ERHISampleCountFlags::Samples8;
			case 16: return ERHISampleCountFlags::Samples16;
			default: return ERHISampleCountFlags::None;
			}
		}

		auto GetVertexElementSize(EVertexElementType Type) -> uint32
		{
			switch (Type)
			{
			case EVertexElementType::Float1:
			case EVertexElementType::PackedNormal:
			case EVertexElementType::UByte4:
			case EVertexElementType::UByte4N:
			case EVertexElementType::Color:
			case EVertexElementType::Short2:
			case EVertexElementType::Short2N:
			case EVertexElementType::Half2:
			case EVertexElementType::UShort2:
			case EVertexElementType::UShort2N:
			case EVertexElementType::URGB10A2N:
			case EVertexElementType::UInt: return 4;
			case EVertexElementType::Float2:
			case EVertexElementType::Short4:
			case EVertexElementType::Short4N:
			case EVertexElementType::Half4:
			case EVertexElementType::UShort4:
			case EVertexElementType::UShort4N: return 8;
			case EVertexElementType::Float3: return 12;
			case EVertexElementType::Float4: return 16;
			default: return 0;
			}
		}

		template<typename TValue>
		auto HashValue(FXxHash64Builder& Builder, const TValue& Value) -> void
		{
			Builder.UpdateValue(Value);
		}

		auto HashAttachment(FXxHash64Builder& Builder,
			const FRHIAttachmentLayout& Attachment) -> void
		{
			HashValue(Builder, Attachment.Format);
			HashValue(Builder, Attachment.NumSamples);
			HashValue(Builder, Attachment.LoadAction);
			HashValue(Builder, Attachment.StoreAction);
			HashValue(Builder, Attachment.StencilLoadAction);
			HashValue(Builder, Attachment.StencilStoreAction);
			HashValue(Builder, Attachment.InitialLayout);
			HashValue(Builder, Attachment.FinalLayout);
			HashValue(Builder, Attachment.InitialAccess);
			HashValue(Builder, Attachment.FinalAccess);
		}

		constexpr ERHIAccess ReadAccessMask = ERHIAccess::VertexBufferRead
			| ERHIAccess::IndexBufferRead
			| ERHIAccess::GraphicsUniformRead
			| ERHIAccess::ComputeUniformRead
			| ERHIAccess::GraphicsShaderRead
			| ERHIAccess::ComputeShaderRead
			| ERHIAccess::TransferRead
			| ERHIAccess::HostRead;

		constexpr ERHIAccess ExclusiveAccessMask = ERHIAccess::ColorAttachmentReadWrite
			| ERHIAccess::DepthStencilReadWrite
			| ERHIAccess::GraphicsShaderReadWrite
			| ERHIAccess::ComputeShaderReadWrite
			| ERHIAccess::TransferWrite
			| ERHIAccess::HostWrite
			| ERHIAccess::Present;

		auto IsSingleBit(ERHIAccess Access) -> bool
		{
			const uint32 Value = static_cast<uint32>(Access);
			return Value != 0 && (Value & (Value - 1)) == 0;
		}

		auto IsValidAccessShape(ERHIAccess Access, bool bExpected, std::string& OutError) -> bool
		{
			auto Fail = [&OutError](const char* Message) {
				OutError = Message;
				return false;
			};
			if (Access == ERHIAccess::Discard)
			{
				return bExpected || Fail("Discard access is valid only as expected-before state.");
			}
			if (Access == ERHIAccess::None)
			{
				return bExpected || Fail("Required-after access must not be None.");
			}
			if (EnumHasAnyFlags(Access, ERHIAccess::Discard))
			{
				return Fail("Discard access must not be combined with another state.");
			}
			const ERHIAccess KnownMask = ReadAccessMask | ExclusiveAccessMask;
			if ((static_cast<uint32>(Access) & ~static_cast<uint32>(KnownMask)) != 0)
			{
				return Fail("Access contains an unknown state bit.");
			}
			if (EnumHasAnyFlags(Access, ExclusiveAccessMask) && !IsSingleBit(Access))
			{
				return Fail("Write-capable and presentation access states are exclusive.");
			}
			return true;
		}

		auto BufferUsageAdmits(EBufferUsageFlags Usage, ERHIAccess Access) -> bool
		{
			if (Access == ERHIAccess::None || Access == ERHIAccess::Discard) return true;
			if (EnumHasAnyFlags(Access, ERHIAccess::VertexBufferRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::VertexBuffer)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::IndexBufferRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::IndexBuffer)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsUniformRead | ERHIAccess::ComputeUniformRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsShaderRead | ERHIAccess::ComputeShaderRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer
					| EBufferUsageFlags::ByteAddressBuffer | EBufferUsageFlags::UnorderedAccess)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::TransferRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::SourceCopy)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::HostRead)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::KeepCPUAccessible)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::GraphicsShaderReadWrite | ERHIAccess::ComputeShaderReadWrite)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::UnorderedAccess)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::TransferWrite)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::DestinationCopy
					| EBufferUsageFlags::Static | EBufferUsageFlags::Dynamic)) return false;
			if (EnumHasAnyFlags(Access, ERHIAccess::HostWrite)
				&& !EnumHasAnyFlags(Usage, EBufferUsageFlags::Dynamic | EBufferUsageFlags::KeepCPUAccessible)) return false;
			return !EnumHasAnyFlags(Access, ERHIAccess::ColorAttachmentReadWrite
				| ERHIAccess::DepthStencilReadWrite | ERHIAccess::Present);
		}

		auto TextureUsageAdmits(const FRHITexture& Texture, ERHIAccess Access) -> bool
		{
			if (Access == ERHIAccess::None || Access == ERHIAccess::Discard) return true;
			const ETextureCreateFlags Usage = Texture.GetFlags();
			constexpr ERHIAccess TextureReadMask = ERHIAccess::GraphicsShaderRead
				| ERHIAccess::ComputeShaderRead;
			if (EnumHasAnyFlags(Access, ReadAccessMask & ~TextureReadMask)
				&& Access != ERHIAccess::TransferRead && Access != ERHIAccess::HostRead) return false;
			if (EnumHasAnyFlags(Access, TextureReadMask)
				&& !EnumHasAnyFlags(Usage, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::Storage)) return false;
			if (Access == ERHIAccess::TransferRead)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback);
			if (Access == ERHIAccess::HostRead)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::CPUReadback);
			if (Access == ERHIAccess::ColorAttachmentReadWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable);
			if (Access == ERHIAccess::DepthStencilReadWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::DepthStencilTargetable);
			if (Access == ERHIAccess::GraphicsShaderReadWrite || Access == ERHIAccess::ComputeShaderReadWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::Storage);
			if (Access == ERHIAccess::TransferWrite)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::DestinationCopy
					| ETextureCreateFlags::ShaderResource | ETextureCreateFlags::Storage);
			if (Access == ERHIAccess::HostWrite) return false;
			if (Access == ERHIAccess::Present)
				return EnumHasAnyFlags(Usage, ETextureCreateFlags::RenderTargetable);
			return true;
		}

		auto RangesOverlap(uint64 FirstOffset, uint64 FirstSize, uint64 SecondOffset, uint64 SecondSize) -> bool
		{
			return FirstOffset < SecondOffset + SecondSize && SecondOffset < FirstOffset + FirstSize;
		}

		auto TextureAspectsAdmit(ERHITextureAspect Aspects, ERHIAccess Access) -> bool
		{
			if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Color)
				&& Access == ERHIAccess::DepthStencilReadWrite) return false;
			if (EnumHasAnyFlags(Aspects, ERHITextureAspect::Depth | ERHITextureAspect::Stencil)
				&& (Access == ERHIAccess::ColorAttachmentReadWrite || Access == ERHIAccess::Present)) return false;
			return true;
		}
	}

	auto FGraphicsPipelineStateInitializer::IsValid() const -> bool
	{
		FGraphicsPipelineStateKey IgnoredKey;
		std::string IgnoredError;
		return BuildGraphicsPipelineStateKey(*this, nullptr, IgnoredKey,
			IgnoredError);
	}

	auto ValidateShaderParameterUpdate(const FPipelineLayoutDesc& Layout,
		EShaderStageFlags ShaderStage,
		std::span<const FRHIShaderParameterResource> Resources,
		std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) {
			OutError = Message;
			return false;
		};
		if (ShaderStage != EShaderStageFlags::Vertex
			&& ShaderStage != EShaderStageFlags::Fragment)
			return Fail("Shader parameter update stage is invalid.");
		for (const FRHIShaderParameterResource& Resource : Resources)
		{
			if (Resource.SetIndex >= Layout.BindingLayouts.size())
				return Fail("Shader parameter set is outside the active pipeline layout.");
			const FBindingLayout& Set = Layout.BindingLayouts[Resource.SetIndex];
			const auto BindingIt = std::ranges::find(Set.BindingLayouts,
				Resource.BindingIndex, &FBindingLayoutItem::Slot);
			if (BindingIt == Set.BindingLayouts.end()
				|| BindingIt->Type != Resource.Type
				|| Resource.ArrayElement >= BindingIt->ArraySize
				|| !EnumHasAnyFlags(BindingIt->StageFlags, ShaderStage))
				return Fail("Shader parameter location, element, type, or stage is incompatible with the active pipeline layout.");
		}
		OutError.clear();
		return true;
	}

	auto ValidateShaderBindingCompleteness(const FPipelineLayoutDesc& Layout,
		std::span<const FRHIShaderParameterResource> Resources,
		std::string& OutError) -> bool
	{
		for (uint32 SetIndex = 0; SetIndex < Layout.BindingLayouts.size(); ++SetIndex)
		{
			for (const FBindingLayoutItem& Binding :
				Layout.BindingLayouts[SetIndex].BindingLayouts)
			{
				for (uint32 ArrayElement = 0; ArrayElement < Binding.ArraySize;
					++ArrayElement)
				{
					const auto It = std::ranges::find_if(Resources,
						[=](const FRHIShaderParameterResource& Resource) {
							return Resource.SetIndex == SetIndex
								&& Resource.BindingIndex == Binding.Slot
								&& Resource.ArrayElement == ArrayElement;
						});
					if (It == Resources.end() || !It->Resource
						|| It->Type != Binding.Type)
					{
						OutError = "Draw is missing a required shader binding element.";
						return false;
					}
				}
			}
		}
		OutError.clear();
		return true;
	}

	auto BuildGraphicsPipelineStateKey(
		const FGraphicsPipelineStateInitializer& Initializer,
		const FRHICapabilities* Capabilities,
		FGraphicsPipelineStateKey& OutKey,
		std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) {
			OutError = Message;
			return false;
		};
		const auto IsCompareValid = [](ERHIDepthCompareOp Op) {
			return Op < ERHIDepthCompareOp::Count;
		};
		const auto IsStencilFaceValid = [&](const FRHIStencilFaceState& Face) {
			return IsCompareValid(Face.CompareOp)
				&& Face.FailOp < ERHIStencilOp::Count
				&& Face.PassOp < ERHIStencilOp::Count
				&& Face.DepthFailOp < ERHIStencilOp::Count;
		};
		const auto IsBlendValid = [](const FRHIColorBlendState& Blend) {
			const uint8 Mask = static_cast<uint8>(Blend.ColorWriteMask);
			return Blend.SrcColorFactor < ERHIBlendFactor::Count
				&& Blend.DstColorFactor < ERHIBlendFactor::Count
				&& Blend.ColorOp < ERHIBlendOp::Count
				&& Blend.SrcAlphaFactor < ERHIBlendFactor::Count
				&& Blend.DstAlphaFactor < ERHIBlendFactor::Count
				&& Blend.AlphaOp < ERHIBlendOp::Count
				&& (Mask & ~static_cast<uint8>(ERHIColorWriteMask::All)) == 0;
		};

		if (Initializer.RasterizerState.PolygonMode >= ERHIPolygonMode::Count
			|| Initializer.RasterizerState.CullMode >= ERHICullMode::Count
			|| Initializer.RasterizerState.FrontFace >= ERHIFrontFace::Count
			|| !std::isfinite(Initializer.RasterizerState.DepthBiasConstantFactor)
			|| !std::isfinite(Initializer.RasterizerState.DepthBiasClamp)
			|| !std::isfinite(Initializer.RasterizerState.DepthBiasSlopeFactor)
			|| !std::isfinite(Initializer.RasterizerState.LineWidth)
			|| Initializer.RasterizerState.LineWidth <= 0.0f
			|| Initializer.PrimitiveTopology >= FGraphicsPipelineStateInitializer::EPrimitiveTopology::Count
			|| !IsValidSampleCount(Initializer.MultisampleState.RasterSamples)
			|| !IsCompareValid(Initializer.DepthStencilState.CompareOp)
			|| !IsStencilFaceValid(Initializer.DepthStencilState.FrontFace)
			|| !IsStencilFaceValid(Initializer.DepthStencilState.BackFace))
			return Fail("Graphics pipeline contains an invalid fixed-state value.");
		for (const FRHIColorBlendState& Blend : Initializer.ColorBlendStates)
			if (!IsBlendValid(Blend))
				return Fail("Graphics pipeline contains an invalid blend-state value.");

		if (!Initializer.BoundShaders.VertexShader
			|| !Initializer.BoundShaders.FragmentShader)
			return Fail("Graphics pipeline requires vertex and fragment shaders.");
		if (Initializer.BoundShaders.VertexShader->GetFrequency() != EShaderFrequency::Vertex
			|| Initializer.BoundShaders.FragmentShader->GetFrequency() != EShaderFrequency::Fragment)
			return Fail("Graphics pipeline shader stages do not match their slots.");
		for (const FBindingLayout& Set : Initializer.PipelineLayout.BindingLayouts)
		{
			std::unordered_set<uint32> Slots;
			for (const FBindingLayoutItem& Binding : Set.BindingLayouts)
			{
				const auto KnownStages = EShaderStageFlags::Vertex | EShaderStageFlags::Fragment;
				if (Binding.StageFlags == EShaderStageFlags::None
					|| (static_cast<uint32>(Binding.StageFlags)
						& ~static_cast<uint32>(KnownStages)) != 0
					|| Binding.Type > ERHIBindingType::StorageImage
					|| Binding.ArraySize == 0 || !Slots.insert(Binding.Slot).second)
					return Fail("Graphics pipeline reflected layout is structurally invalid.");
			}
		}
		for (const FPushConstantRange& Range : Initializer.PipelineLayout.PushConstantRanges)
		{
			const auto KnownStages = EShaderStageFlags::Vertex | EShaderStageFlags::Fragment;
			if (Range.StageFlags == EShaderStageFlags::None || Range.Size == 0
				|| (Range.Offset % 4) != 0 || (Range.Size % 4) != 0
				|| (static_cast<uint32>(Range.StageFlags)
					& ~static_cast<uint32>(KnownStages)) != 0)
				return Fail("Graphics pipeline push-constant layout is structurally invalid.");
		}

		if (!Initializer.RenderTargetLayout.IsValid())
			return Fail("Graphics pipeline render-target layout is invalid.");
		const uint8 RasterSamples = Initializer.RenderTargetLayout.NumColorRenderTargets > 0
			? Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.NumSamples
			: Initializer.RenderTargetLayout.DepthStencilAttachment.NumSamples;
		if (Initializer.MultisampleState.RasterSamples != RasterSamples)
			return Fail("Graphics pipeline raster samples do not match the render-target layout.");
		if ((Initializer.DepthStencilState.bEnableTest
			|| Initializer.DepthStencilState.bEnableWrite
			|| Initializer.DepthStencilState.bEnableStencil)
			&& !Initializer.RenderTargetLayout.bHasDepthStencil)
			return Fail("Graphics pipeline depth/stencil state requires a depth attachment.");
		if (Initializer.DepthStencilState.bEnableStencil
			&& !GetPixelFormatInfo(Initializer.RenderTargetLayout.DepthStencilAttachment.Format).bHasStencil)
			return Fail("Graphics pipeline stencil state requires a stencil-capable attachment.");

		if (!Initializer.VertexDeclaration)
			return Fail("Graphics pipeline requires a vertex declaration.");
		std::vector<FRHIVertexElementIdentity> VertexElements;
		std::unordered_set<uint32> Attributes;
		std::unordered_map<uint8, std::pair<uint16,
			FRHIVertexElementIdentity::EInputRate>> Streams;
		for (const FVertexElement& Element : Initializer.VertexDeclaration->GetElements())
		{
			if (Element.Type == EVertexElementType::None) break;
			const uint32 ElementSize = GetVertexElementSize(Element.Type);
			if (Element.Type >= EVertexElementType::Count || Element.Stride == 0
				|| Element.InputRate >= FRHIVertexElementIdentity::EInputRate::Count
				|| ElementSize == 0
				|| static_cast<uint32>(Element.Offset) + ElementSize > Element.Stride
				|| !Attributes.insert(Element.AttributeIndex).second)
				return Fail("Graphics pipeline vertex declaration is structurally invalid.");
			const auto [StreamIt, bInserted] = Streams.emplace(Element.StreamIndex,
				std::pair{Element.Stride, Element.InputRate});
			if (!bInserted && StreamIt->second != std::pair{Element.Stride, Element.InputRate})
				return Fail("Graphics pipeline vertex stream stride or input rate is inconsistent.");
			for (const FRHIVertexElementIdentity& Existing : VertexElements)
			{
				const uint32 ExistingSize = GetVertexElementSize(Existing.Type);
				if (Existing.StreamIndex == Element.StreamIndex
					&& Element.Offset < Existing.Offset + ExistingSize
					&& Existing.Offset < Element.Offset + ElementSize)
					return Fail("Graphics pipeline vertex elements overlap within a stream.");
			}
			VertexElements.push_back({Element.StreamIndex, Element.Offset,
				Element.Type, Element.AttributeIndex, Element.Stride,
				Element.InputRate});
		}

		if (Capabilities)
		{
			if (Initializer.RenderTargetLayout.NumColorRenderTargets > Capabilities->MaxColorAttachments)
				return Fail("Graphics pipeline color attachment count exceeds the device limit.");
			const ERHISampleCountFlags SampleFlag = SampleCountFlag(RasterSamples);
			if (!EnumHasAllFlags(Capabilities->ColorSampleCounts, SampleFlag)
				|| (Initializer.RenderTargetLayout.bHasDepthStencil
					&& !EnumHasAllFlags(Capabilities->DepthSampleCounts, SampleFlag)))
				return Fail("Graphics pipeline sample count is unsupported by the device.");
			if (Initializer.RasterizerState.PolygonMode != ERHIPolygonMode::Fill
				&& !Capabilities->bSupportsNonSolidFill)
				return Fail("Graphics pipeline non-solid fill is unsupported by the device.");
			if (Initializer.RasterizerState.bEnableDepthClamp
				&& !Capabilities->bSupportsDepthClamp)
				return Fail("Graphics pipeline depth clamp is unsupported by the device.");
			if (Initializer.RasterizerState.LineWidth != 1.0f
				&& !Capabilities->bSupportsWideLines)
				return Fail("Graphics pipeline wide lines are unsupported by the device.");
		}

		FGraphicsPipelineStateKey Key;
		Key.VertexShaderHash = Initializer.BoundShaders.VertexShader->GetHash();
		Key.FragmentShaderHash = Initializer.BoundShaders.FragmentShader->GetHash();
		Key.RenderTargetLayout.NumColorRenderTargets =
			Initializer.RenderTargetLayout.NumColorRenderTargets;
		for (uint32 Index = 0;
			Index < Initializer.RenderTargetLayout.NumColorRenderTargets; ++Index)
			Key.RenderTargetLayout.ColorAttachments[Index] =
				Initializer.RenderTargetLayout.ColorAttachments[Index];
		Key.RenderTargetLayout.bHasDepthStencil =
			Initializer.RenderTargetLayout.bHasDepthStencil;
		if (Key.RenderTargetLayout.bHasDepthStencil)
			Key.RenderTargetLayout.DepthStencilAttachment =
				Initializer.RenderTargetLayout.DepthStencilAttachment;
		Key.VertexElements = std::move(VertexElements);
		Key.PipelineLayout = Initializer.PipelineLayout;
		for (FBindingLayout& Set : Key.PipelineLayout.BindingLayouts)
			std::ranges::sort(Set.BindingLayouts, {}, &FBindingLayoutItem::Slot);
		std::ranges::sort(Key.PipelineLayout.PushConstantRanges,
			[](const FPushConstantRange& A, const FPushConstantRange& B) {
				return std::tie(A.Offset, A.Size, A.StageFlags)
					< std::tie(B.Offset, B.Size, B.StageFlags);
			});
		Key.RasterizerState = Initializer.RasterizerState;
		if (!Key.RasterizerState.bEnableDepthBias)
		{
			Key.RasterizerState.DepthBiasConstantFactor = 0.0f;
			Key.RasterizerState.DepthBiasClamp = 0.0f;
			Key.RasterizerState.DepthBiasSlopeFactor = 0.0f;
		}
		if (Key.RasterizerState.PolygonMode != ERHIPolygonMode::Line)
			Key.RasterizerState.LineWidth = 1.0f;
		Key.MultisampleState = Initializer.MultisampleState;
		Key.DepthStencilState = Initializer.DepthStencilState;
		if (!Key.DepthStencilState.bEnableTest)
			Key.DepthStencilState.CompareOp = ERHIDepthCompareOp::Less;
		if (!Key.DepthStencilState.bEnableStencil)
		{
			Key.DepthStencilState.FrontFace = {};
			Key.DepthStencilState.BackFace = {};
			Key.DepthStencilState.StencilCompareMask = 0xff;
			Key.DepthStencilState.StencilWriteMask = 0xff;
			Key.DepthStencilState.StencilReference = 0;
		}
		Key.ColorBlendStates.assign(Initializer.ColorBlendStates.begin(),
			Initializer.ColorBlendStates.begin()
				+ Initializer.RenderTargetLayout.NumColorRenderTargets);
		for (FRHIColorBlendState& Blend : Key.ColorBlendStates)
			if (!Blend.bEnable)
			{
				const ERHIColorWriteMask Mask = Blend.ColorWriteMask;
				Blend = {};
				Blend.ColorWriteMask = Mask;
			}
		Key.PrimitiveTopology = Initializer.PrimitiveTopology;
		OutKey = std::move(Key);
		OutError.clear();
		return true;
	}

	auto FGraphicsPipelineStateKeyHasher::operator()(
		const FGraphicsPipelineStateKey& Key) const -> size_t
	{
		FXxHash64Builder Builder;
		HashValue(Builder, Key.VertexShaderHash);
		HashValue(Builder, Key.FragmentShaderHash);
		HashValue(Builder, Key.RenderTargetLayout.NumColorRenderTargets);
		for (uint32 Index = 0; Index < Key.RenderTargetLayout.NumColorRenderTargets; ++Index)
		{
			const FRHIColorAttachmentLayout& Color = Key.RenderTargetLayout.ColorAttachments[Index];
			HashAttachment(Builder, Color.RenderTarget);
			HashValue(Builder, Color.bHasResolveTarget);
			if (Color.bHasResolveTarget) HashAttachment(Builder, Color.ResolveTarget);
		}
		HashValue(Builder, Key.RenderTargetLayout.bHasDepthStencil);
		if (Key.RenderTargetLayout.bHasDepthStencil)
			HashAttachment(Builder, Key.RenderTargetLayout.DepthStencilAttachment);
		for (const FRHIVertexElementIdentity& Element : Key.VertexElements)
		{
			HashValue(Builder, Element.StreamIndex);
			HashValue(Builder, Element.Offset);
			HashValue(Builder, Element.Type);
			HashValue(Builder, Element.AttributeIndex);
			HashValue(Builder, Element.Stride);
			HashValue(Builder, Element.InputRate);
		}
		HashValue(Builder, Key.VertexElements.size());
		HashValue(Builder, Key.PipelineLayout.BindingLayouts.size());
		for (const FBindingLayout& Set : Key.PipelineLayout.BindingLayouts)
		{
			HashValue(Builder, Set.BindingLayouts.size());
			for (const FBindingLayoutItem& Binding : Set.BindingLayouts)
			{
				HashValue(Builder, Binding.StageFlags);
				HashValue(Builder, Binding.Slot);
				HashValue(Builder, Binding.Type);
				HashValue(Builder, Binding.ArraySize);
			}
		}
		HashValue(Builder, Key.PipelineLayout.PushConstantRanges.size());
		for (const FPushConstantRange& Range : Key.PipelineLayout.PushConstantRanges)
		{
			HashValue(Builder, Range.StageFlags);
			HashValue(Builder, Range.Offset);
			HashValue(Builder, Range.Size);
		}
		HashValue(Builder, Key.RasterizerState.PolygonMode);
		HashValue(Builder, Key.RasterizerState.CullMode);
		HashValue(Builder, Key.RasterizerState.FrontFace);
		HashValue(Builder, Key.RasterizerState.bEnableDepthClamp);
		HashValue(Builder, Key.RasterizerState.bEnableDepthBias);
		HashValue(Builder, Key.RasterizerState.DepthBiasConstantFactor);
		HashValue(Builder, Key.RasterizerState.DepthBiasClamp);
		HashValue(Builder, Key.RasterizerState.DepthBiasSlopeFactor);
		HashValue(Builder, Key.RasterizerState.LineWidth);
		HashValue(Builder, Key.MultisampleState.RasterSamples);
		HashValue(Builder, Key.MultisampleState.bEnableAlphaToCoverage);
		HashValue(Builder, Key.DepthStencilState.bEnableTest);
		HashValue(Builder, Key.DepthStencilState.bEnableWrite);
		HashValue(Builder, Key.DepthStencilState.CompareOp);
		HashValue(Builder, Key.DepthStencilState.bEnableStencil);
		const auto HashStencilFace = [&Builder](const FRHIStencilFaceState& Face) {
			HashValue(Builder, Face.CompareOp);
			HashValue(Builder, Face.FailOp);
			HashValue(Builder, Face.PassOp);
			HashValue(Builder, Face.DepthFailOp);
		};
		HashStencilFace(Key.DepthStencilState.FrontFace);
		HashStencilFace(Key.DepthStencilState.BackFace);
		HashValue(Builder, Key.DepthStencilState.StencilCompareMask);
		HashValue(Builder, Key.DepthStencilState.StencilWriteMask);
		HashValue(Builder, Key.DepthStencilState.StencilReference);
		for (const FRHIColorBlendState& Blend : Key.ColorBlendStates)
		{
			HashValue(Builder, Blend.bEnable);
			HashValue(Builder, Blend.SrcColorFactor);
			HashValue(Builder, Blend.DstColorFactor);
			HashValue(Builder, Blend.ColorOp);
			HashValue(Builder, Blend.SrcAlphaFactor);
			HashValue(Builder, Blend.DstAlphaFactor);
			HashValue(Builder, Blend.AlphaOp);
			HashValue(Builder, Blend.ColorWriteMask);
		}
		HashValue(Builder, Key.PrimitiveTopology);
		return static_cast<size_t>(Builder.Finalize().HashValue);
	}

	auto GetTextureAspects(EPixelFormat Format) -> ERHITextureAspect
	{
		const FPixelFormatInfo& Info = GetPixelFormatInfo(Format);
		ERHITextureAspect Result = ERHITextureAspect::None;
		if (Info.bHasDepth) Result |= ERHITextureAspect::Depth;
		if (Info.bHasStencil) Result |= ERHITextureAspect::Stencil;
		return Result == ERHITextureAspect::None ? ERHITextureAspect::Color : Result;
	}

	auto GetTextureLayoutForAccess(ERHIAccess Access, ERHITextureLayout& OutLayout) -> bool
	{
		if (Access == ERHIAccess::None || Access == ERHIAccess::Discard) OutLayout = ERHITextureLayout::Undefined;
		else if (Access == ERHIAccess::ColorAttachmentReadWrite) OutLayout = ERHITextureLayout::ColorAttachment;
		else if (Access == ERHIAccess::DepthStencilReadWrite) OutLayout = ERHITextureLayout::DepthStencilAttachment;
		else if (Access == ERHIAccess::GraphicsShaderRead || Access == ERHIAccess::ComputeShaderRead
			|| Access == (ERHIAccess::GraphicsShaderRead | ERHIAccess::ComputeShaderRead)) OutLayout = ERHITextureLayout::ShaderReadOnly;
		else if (Access == ERHIAccess::TransferRead) OutLayout = ERHITextureLayout::TransferSource;
		else if (Access == ERHIAccess::TransferWrite) OutLayout = ERHITextureLayout::TransferDestination;
		else if (Access == ERHIAccess::HostRead || Access == ERHIAccess::HostWrite
			|| Access == ERHIAccess::GraphicsShaderReadWrite || Access == ERHIAccess::ComputeShaderReadWrite) OutLayout = ERHITextureLayout::General;
		else if (Access == ERHIAccess::Present) OutLayout = ERHITextureLayout::Present;
		else return false;
		return true;
	}

	auto FRHIBufferTransition::Whole(FRHIBuffer* Buffer, ERHIAccess ExpectedBefore,
		ERHIAccess RequiredAfter) -> FRHIBufferTransition
	{
		return {.Buffer = Buffer, .Offset = 0, .Size = Buffer ? Buffer->GetSize() : 0,
			.ExpectedBefore = ExpectedBefore, .RequiredAfter = RequiredAfter};
	}

	auto FRHITextureTransition::Whole(FRHITexture* Texture, ERHIAccess ExpectedBefore,
		ERHIAccess RequiredAfter) -> FRHITextureTransition
	{
		return {.Texture = Texture,
			.Range = {.Aspects = Texture ? GetTextureAspects(Texture->GetFormat()) : ERHITextureAspect::None,
				.FirstMip = 0, .NumMips = Texture ? static_cast<uint32>(Texture->GetNumMips()) : 0u,
				.FirstArrayLayer = 0, .NumArrayLayers = Texture ? static_cast<uint32>(Texture->GetArraySize()) : 0u},
			.ExpectedBefore = ExpectedBefore, .RequiredAfter = RequiredAfter};
	}

	auto ValidateBufferTransition(const FRHIBufferTransition& Transition, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (Transition.Buffer == nullptr) return Fail("Buffer transition resource is null.");
		if (Transition.Buffer->GetResourceType() != ERHIResourceType::Buffer) return Fail("Buffer transition resource type is invalid.");
		if (Transition.Size == 0) return Fail("Buffer transition size must be nonzero.");
		const uint64 ResourceSize = Transition.Buffer->GetSize();
		if (Transition.Offset > ResourceSize || Transition.Size > ResourceSize - Transition.Offset)
			return Fail("Buffer transition range exceeds the resource size.");
		if (!IsValidAccessShape(Transition.ExpectedBefore, true, OutError)
			|| !IsValidAccessShape(Transition.RequiredAfter, false, OutError)) return false;
		if (!BufferUsageAdmits(Transition.Buffer->GetUsage(), Transition.ExpectedBefore)
			|| !BufferUsageAdmits(Transition.Buffer->GetUsage(), Transition.RequiredAfter))
			return Fail("Buffer transition access is incompatible with resource usage.");
		OutError.clear();
		return true;
	}

	auto ValidateTextureTransition(const FRHITextureTransition& Transition, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (Transition.Texture == nullptr) return Fail("Texture transition resource is null.");
		if (Transition.Texture->GetResourceType() != ERHIResourceType::Texture) return Fail("Texture transition resource type is invalid.");
		if (Transition.Range.Aspects == ERHITextureAspect::None) return Fail("Texture transition aspects must be nonempty.");
		constexpr ERHITextureAspect KnownAspects = ERHITextureAspect::Color | ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		if ((static_cast<uint8>(Transition.Range.Aspects) & ~static_cast<uint8>(KnownAspects)) != 0
			|| !EnumHasAllFlags(GetTextureAspects(Transition.Texture->GetFormat()), Transition.Range.Aspects))
			return Fail("Texture transition aspects are unsupported by the pixel format.");
		if (Transition.Range.NumMips == 0 || Transition.Range.NumArrayLayers == 0)
			return Fail("Texture transition mip and layer counts must be nonzero.");
		if (Transition.Range.FirstMip > Transition.Texture->GetNumMips()
			|| Transition.Range.NumMips > Transition.Texture->GetNumMips() - Transition.Range.FirstMip)
			return Fail("Texture transition mip range exceeds the resource.");
		if (Transition.Range.FirstArrayLayer > Transition.Texture->GetArraySize()
			|| Transition.Range.NumArrayLayers > Transition.Texture->GetArraySize() - Transition.Range.FirstArrayLayer)
			return Fail("Texture transition layer range exceeds the resource.");
		if (!IsValidAccessShape(Transition.ExpectedBefore, true, OutError)
			|| !IsValidAccessShape(Transition.RequiredAfter, false, OutError)) return false;
		ERHITextureLayout IgnoredLayout;
		if (!GetTextureLayoutForAccess(Transition.ExpectedBefore, IgnoredLayout)
			|| !GetTextureLayoutForAccess(Transition.RequiredAfter, IgnoredLayout))
			return Fail("Texture transition access has no deterministic texture layout.");
		if (!TextureUsageAdmits(*Transition.Texture, Transition.ExpectedBefore)
			|| !TextureUsageAdmits(*Transition.Texture, Transition.RequiredAfter))
			return Fail("Texture transition access is incompatible with resource usage.");
		if (!TextureAspectsAdmit(Transition.Range.Aspects, Transition.ExpectedBefore)
			|| !TextureAspectsAdmit(Transition.Range.Aspects, Transition.RequiredAfter))
			return Fail("Texture transition access is incompatible with the selected aspects.");
		OutError.clear();
		return true;
	}

	auto ValidateBufferTransitions(std::span<const FRHIBufferTransition> Transitions, std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < Transitions.size(); ++Index)
		{
			if (!ValidateBufferTransition(Transitions[Index], OutError)) return false;
			for (size_t OtherIndex = 0; OtherIndex < Index; ++OtherIndex)
			{
				if (Transitions[Index].Buffer == Transitions[OtherIndex].Buffer
					&& RangesOverlap(Transitions[Index].Offset, Transitions[Index].Size,
						Transitions[OtherIndex].Offset, Transitions[OtherIndex].Size))
				{
					OutError = "Buffer transition batch contains overlapping ranges.";
					return false;
				}
			}
		}
		OutError.clear();
		return true;
	}

	auto ValidateTextureTransitions(std::span<const FRHITextureTransition> Transitions, std::string& OutError) -> bool
	{
		for (size_t Index = 0; Index < Transitions.size(); ++Index)
		{
			if (!ValidateTextureTransition(Transitions[Index], OutError)) return false;
			for (size_t OtherIndex = 0; OtherIndex < Index; ++OtherIndex)
			{
				const auto& A = Transitions[Index];
				const auto& B = Transitions[OtherIndex];
				const bool bAspectOverlap = EnumHasAnyFlags(A.Range.Aspects, B.Range.Aspects);
				const bool bMipOverlap = RangesOverlap(A.Range.FirstMip, A.Range.NumMips, B.Range.FirstMip, B.Range.NumMips);
				const bool bLayerOverlap = RangesOverlap(A.Range.FirstArrayLayer, A.Range.NumArrayLayers,
					B.Range.FirstArrayLayer, B.Range.NumArrayLayers);
				if (A.Texture == B.Texture && bAspectOverlap && bMipOverlap && bLayerOverlap)
				{
					OutError = "Texture transition batch contains overlapping subresource ranges.";
					return false;
				}
			}
		}
		OutError.clear();
		return true;
	}

	auto MakeDefaultBufferViewDesc(
		const FRHIBuffer& Buffer,
		ERHIBufferViewType Type,
		EPixelFormat Format) -> FRHIBufferViewDesc
	{
		return {.Offset = 0, .Size = Buffer.GetSize(), .Type = Type, .Format = Format};
	}

	auto MakeDefaultTextureViewDesc(
		const FRHITexture& Texture,
		ERHITextureViewUsage Usage) -> FRHITextureViewDesc
	{
		const bool bAttachment = Usage == ERHITextureViewUsage::ColorAttachment
			|| Usage == ERHITextureViewUsage::DepthStencilAttachment;
		return {
			.Usage = Usage,
			.Dimension = !bAttachment && Texture.GetDimension() == ETextureDimension::TextureCube
				? ERHITextureViewDimension::TextureCube
				: ERHITextureViewDimension::Texture2D,
			.Format = Texture.GetFormat(),
			.Range = {
				.Aspects = GetTextureAspects(Texture.GetFormat()),
				.FirstMip = 0,
				.NumMips = bAttachment ? 1u : static_cast<uint32>(Texture.GetNumMips()),
				.FirstArrayLayer = 0,
				.NumArrayLayers = bAttachment ? 1u : static_cast<uint32>(Texture.GetArraySize())}};
	}

	auto ValidateBufferViewDesc(
		const FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc,
		std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (Buffer == nullptr) return Fail("Buffer view parent is null.");
		if (Buffer->GetResourceType() != ERHIResourceType::Buffer) return Fail("Buffer view parent type is invalid.");
		if (Desc.Size == 0) return Fail("Buffer view size must be nonzero.");
		if (Desc.Offset > Buffer->GetSize() || Desc.Size > Buffer->GetSize() - Desc.Offset)
			return Fail("Buffer view range exceeds the parent buffer.");

		const EBufferUsageFlags Usage = Buffer->GetUsage();
		switch (Desc.Type)
		{
		case ERHIBufferViewType::Uniform:
			if (Desc.Format != EPixelFormat::Unknown) return Fail("Uniform buffer views cannot specify a format.");
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer))
				return Fail("Uniform buffer view requires UniformBuffer usage.");
			if ((Desc.Offset % 16) != 0 || (Desc.Size % 16) != 0)
				return Fail("Uniform buffer view offset and size must be 16-byte aligned.");
			break;
		case ERHIBufferViewType::StructuredStorage:
			if (Desc.Format != EPixelFormat::Unknown) return Fail("Structured buffer views cannot specify a format.");
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess))
				return Fail("Structured storage view requires StructuredBuffer or UnorderedAccess usage.");
			if (Buffer->GetStride() == 0 || (Desc.Offset % Buffer->GetStride()) != 0
				|| (Desc.Size % Buffer->GetStride()) != 0)
				return Fail("Structured buffer view range must align to the parent stride.");
			break;
		case ERHIBufferViewType::ByteAddressStorage:
			if (Desc.Format != EPixelFormat::Unknown) return Fail("Byte-address buffer views cannot specify a format.");
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::ByteAddressBuffer))
				return Fail("Byte-address storage view requires ByteAddressBuffer usage.");
			if ((Desc.Offset % 4) != 0 || (Desc.Size % 4) != 0)
				return Fail("Byte-address buffer view range must be four-byte aligned.");
			break;
		case ERHIBufferViewType::Formatted:
		{
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::FormattedBuffer))
				return Fail("Formatted buffer view requires FormattedBuffer usage.");
			const FPixelFormatInfo& Format = GetPixelFormatInfo(Desc.Format);
			if (Desc.Format == EPixelFormat::Unknown || Format.BlockSize != 1
				|| Format.BytesPerBlock == 0 || Format.Kind == EPixelFormatKind::DepthStencil)
				return Fail("Formatted buffer view requires an uncompressed color format.");
			if ((Desc.Offset % Format.BytesPerBlock) != 0 || (Desc.Size % Format.BytesPerBlock) != 0)
				return Fail("Formatted buffer view range must align to its texel size.");
			break;
		}
		default:
			return Fail("Buffer view type is invalid.");
		}
		OutError.clear();
		return true;
	}

	auto ValidateTextureViewDesc(
		const FRHITexture* Texture,
		const FRHITextureViewDesc& Desc,
		std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (Texture == nullptr) return Fail("Texture view parent is null.");
		if (Texture->GetResourceType() != ERHIResourceType::Texture) return Fail("Texture view parent type is invalid.");
		if (Desc.Format != Texture->GetFormat()) return Fail("Texture view format must exactly match its parent.");
		if (Desc.Range.Aspects == ERHITextureAspect::None)
			return Fail("Texture view aspects must be nonempty.");
		constexpr ERHITextureAspect KnownAspects = ERHITextureAspect::Color
			| ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		if ((static_cast<uint8>(Desc.Range.Aspects) & ~static_cast<uint8>(KnownAspects)) != 0
			|| !EnumHasAllFlags(GetTextureAspects(Texture->GetFormat()), Desc.Range.Aspects))
			return Fail("Texture view aspects are unsupported by the parent format.");
		if (Desc.Range.NumMips == 0 || Desc.Range.NumArrayLayers == 0)
			return Fail("Texture view mip and layer counts must be nonzero.");
		if (Desc.Range.FirstMip > Texture->GetNumMips()
			|| Desc.Range.NumMips > Texture->GetNumMips() - Desc.Range.FirstMip)
			return Fail("Texture view mip range exceeds the parent texture.");
		if (Desc.Range.FirstArrayLayer > Texture->GetArraySize()
			|| Desc.Range.NumArrayLayers > Texture->GetArraySize() - Desc.Range.FirstArrayLayer)
			return Fail("Texture view layer range exceeds the parent texture.");

		if (Desc.Dimension == ERHITextureViewDimension::TextureCube)
		{
			if (Texture->GetDimension() != ETextureDimension::TextureCube
				|| Desc.Range.FirstArrayLayer != 0
				|| Desc.Range.NumArrayLayers != TextureCubeFaceCount)
				return Fail("Cube texture views require all six faces of a cube parent.");
		}
		else if (Desc.Dimension == ERHITextureViewDimension::Texture2D)
		{
			if ((Texture->GetDimension() != ETextureDimension::Texture2D
				&& Texture->GetDimension() != ETextureDimension::TextureCube)
				|| Desc.Range.NumArrayLayers != 1)
				return Fail("Texture2D views require one layer of a 2D or cube parent.");
		}
		else
		{
			return Fail("Texture view dimension is unsupported.");
		}

		const ETextureCreateFlags Flags = Texture->GetFlags();
		switch (Desc.Usage)
		{
		case ERHITextureViewUsage::Sampled:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::ShaderResource))
				return Fail("Sampled texture view requires ShaderResource usage.");
			break;
		case ERHITextureViewUsage::Storage:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::Storage))
				return Fail("Storage texture view requires Storage usage.");
			if (Texture->GetNumSamples() != 1 || Desc.Dimension != ERHITextureViewDimension::Texture2D
				|| Desc.Range.Aspects != ERHITextureAspect::Color)
				return Fail("Storage texture views require a single-sampled 2D color range.");
			break;
		case ERHITextureViewUsage::ColorAttachment:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable))
				return Fail("Color attachment view requires render-target or resolve usage.");
			if (Desc.Dimension != ERHITextureViewDimension::Texture2D
				|| Desc.Range.Aspects != ERHITextureAspect::Color
				|| Desc.Range.NumMips != 1 || Desc.Range.NumArrayLayers != 1)
				return Fail("Color attachment views require one 2D color mip and layer.");
			break;
		case ERHITextureViewUsage::DepthStencilAttachment:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::DepthStencilTargetable))
				return Fail("Depth/stencil attachment view requires DepthStencilTargetable usage.");
			if (Desc.Dimension != ERHITextureViewDimension::Texture2D
				|| EnumHasAnyFlags(Desc.Range.Aspects, ERHITextureAspect::Color)
				|| Desc.Range.NumMips != 1 || Desc.Range.NumArrayLayers != 1)
				return Fail("Depth/stencil attachment views require one 2D depth/stencil mip and layer.");
			break;
		case ERHITextureViewUsage::TransferSource:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
				return Fail("Transfer-source view requires SourceCopy usage.");
			break;
		case ERHITextureViewUsage::TransferDestination:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::DestinationCopy))
				return Fail("Transfer-destination view requires DestinationCopy usage.");
			break;
		default:
			return Fail("Texture view usage is invalid.");
		}
		OutError.clear();
		return true;
	}

	namespace
	{
		auto IsSingleCopyAspect(ERHITextureAspect Aspect) -> bool
		{
			return Aspect == ERHITextureAspect::Color
				|| Aspect == ERHITextureAspect::Depth
				|| Aspect == ERHITextureAspect::Stencil;
		}

		auto ValidateCopyTextureRegion(
			const FRHITexture& Texture,
			ERHITextureAspect Aspect,
			uint32 Mip,
			uint32 FirstLayer,
			uint32 NumLayers,
			const FRHITextureOffset3D& Offset,
			const FRHITextureExtent3D& Extent,
			std::string& OutError) -> bool
		{
			auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
			if (!IsSingleCopyAspect(Aspect) || !EnumHasAllFlags(GetTextureAspects(Texture.GetFormat()), Aspect))
				return Fail("Texture copy aspect is unsupported by the texture format.");
			if (Aspect != ERHITextureAspect::Color)
				return Fail("Depth and stencil copies are deferred by the current transfer contract.");
			if (Mip >= Texture.GetNumMips()) return Fail("Texture copy mip exceeds the texture.");
			if (NumLayers == 0 || FirstLayer > Texture.GetArraySize()
				|| NumLayers > Texture.GetArraySize() - FirstLayer)
				return Fail("Texture copy layer range exceeds the texture.");
			if (Offset.X < 0 || Offset.Y < 0 || Offset.Z != 0)
				return Fail("Texture copy offset is invalid for a 2D or cube texture.");
			if (Extent.Width == 0 || Extent.Height == 0 || Extent.Depth != 1)
				return Fail("Texture copy extent must be nonempty and two-dimensional.");
			if (Texture.GetDimension() != ETextureDimension::Texture2D
				&& Texture.GetDimension() != ETextureDimension::TextureCube)
				return Fail("Texture copy dimension is unsupported.");
			const uint32 MipWidth = std::max(1u, Texture.GetSizeX() >> Mip);
			const uint32 MipHeight = std::max(1u, Texture.GetSizeY() >> Mip);
			const uint32 X = static_cast<uint32>(Offset.X);
			const uint32 Y = static_cast<uint32>(Offset.Y);
			if (X > MipWidth || Extent.Width > MipWidth - X
				|| Y > MipHeight || Extent.Height > MipHeight - Y)
				return Fail("Texture copy box exceeds the selected mip.");
			const uint32 BlockSize = GetPixelFormatInfo(Texture.GetFormat()).BlockSize;
			if (BlockSize == 0) return Fail("Texture copy format has no block layout.");
			if ((X % BlockSize) != 0 || (Y % BlockSize) != 0
				|| ((Extent.Width % BlockSize) != 0 && X + Extent.Width != MipWidth)
				|| ((Extent.Height % BlockSize) != 0 && Y + Extent.Height != MipHeight))
				return Fail("Texture copy box is not aligned to compressed blocks or a mip edge.");
			return true;
		}

		auto GetBufferTextureFootprint(
			const FRHITexture& Texture,
			const FRHIBufferTextureCopyRegion& Region,
			uint64& OutSize,
			std::string& OutError) -> bool
		{
			auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
			const FPixelFormatInfo& Format = GetPixelFormatInfo(Texture.GetFormat());
			const uint32 RowLength = Region.BufferRowLength != 0
				? Region.BufferRowLength : Region.TextureExtent.Width;
			const uint32 ImageHeight = Region.BufferImageHeight != 0
				? Region.BufferImageHeight : Region.TextureExtent.Height;
			if (RowLength < Region.TextureExtent.Width || ImageHeight < Region.TextureExtent.Height)
				return Fail("Buffer-texture copy layout is smaller than the texture extent.");
			if ((Region.BufferRowLength != 0 && (RowLength % Format.BlockSize) != 0)
				|| (Region.BufferImageHeight != 0 && (ImageHeight % Format.BlockSize) != 0))
				return Fail("Buffer-texture row length and image height must align to format blocks.");
			if ((Region.BufferOffset % Format.BytesPerBlock) != 0)
				return Fail("Buffer-texture offset must align to the texel block size.");
			const uint64 BlocksPerRow = (static_cast<uint64>(RowLength) + Format.BlockSize - 1) / Format.BlockSize;
			const uint64 BlockRows = (static_cast<uint64>(ImageHeight) + Format.BlockSize - 1) / Format.BlockSize;
			if (BlocksPerRow > std::numeric_limits<uint64>::max() / Format.BytesPerBlock)
				return Fail("Buffer-texture row pitch overflows.");
			const uint64 RowPitch = BlocksPerRow * Format.BytesPerBlock;
			if (BlockRows > std::numeric_limits<uint64>::max() / RowPitch)
				return Fail("Buffer-texture image pitch overflows.");
			const uint64 ImagePitch = BlockRows * RowPitch;
			if (Region.TextureNumArrayLayers > std::numeric_limits<uint64>::max() / ImagePitch)
				return Fail("Buffer-texture layer footprint overflows.");
			OutSize = Region.TextureNumArrayLayers * ImagePitch;
			return OutSize != 0 || Fail("Buffer-texture footprint must be nonzero.");
		}

		auto TextureBoxesOverlap(
			uint32 FirstLayerA, uint32 NumLayersA, const FRHITextureOffset3D& OffsetA,
			const FRHITextureExtent3D& ExtentA, uint32 FirstLayerB, uint32 NumLayersB,
			const FRHITextureOffset3D& OffsetB, const FRHITextureExtent3D& ExtentB) -> bool
		{
			return RangesOverlap(FirstLayerA, NumLayersA, FirstLayerB, NumLayersB)
				&& OffsetA.X < OffsetB.X + static_cast<int64>(ExtentB.Width)
				&& OffsetB.X < OffsetA.X + static_cast<int64>(ExtentA.Width)
				&& OffsetA.Y < OffsetB.Y + static_cast<int64>(ExtentB.Height)
				&& OffsetB.Y < OffsetA.Y + static_cast<int64>(ExtentA.Height);
		}
	}

	auto GetBufferTextureCopyFootprint(const FRHITexture& Texture,
		const FRHIBufferTextureCopyRegion& Region, uint64& OutSize,
		std::string& OutError) -> bool
	{
		return GetBufferTextureFootprint(Texture, Region, OutSize, OutError);
	}

	auto ValidateBufferCopies(FRHIBuffer* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferCopyRegion> Regions, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (!Source || !Destination) return Fail("Buffer copy resources must be nonnull.");
		if (!EnumHasAnyFlags(Source->GetUsage(), EBufferUsageFlags::SourceCopy))
			return Fail("Buffer copy source requires SourceCopy usage.");
		if (!EnumHasAnyFlags(Destination->GetUsage(), EBufferUsageFlags::DestinationCopy))
			return Fail("Buffer copy destination requires DestinationCopy usage.");
		for (size_t Index = 0; Index < Regions.size(); ++Index)
		{
			const auto& Region = Regions[Index];
			if (Region.Size == 0) return Fail("Buffer copy size must be nonzero.");
			if (Region.SourceOffset > Source->GetSize() || Region.Size > Source->GetSize() - Region.SourceOffset)
				return Fail("Buffer copy source range exceeds the resource.");
			if (Region.DestinationOffset > Destination->GetSize()
				|| Region.Size > Destination->GetSize() - Region.DestinationOffset)
				return Fail("Buffer copy destination range exceeds the resource.");
			for (size_t Other = 0; Other < Index; ++Other)
			{
				if (RangesOverlap(Region.DestinationOffset, Region.Size,
					Regions[Other].DestinationOffset, Regions[Other].Size))
					return Fail("Buffer copy batch contains overlapping destinations.");
			}
		}
		if (Source == Destination)
		{
			for (const auto& A : Regions)
				for (const auto& B : Regions)
					if (RangesOverlap(A.SourceOffset, A.Size, B.DestinationOffset, B.Size))
						return Fail("Same-buffer copy source and destination ranges overlap.");
		}
		OutError.clear();
		return true;
	}

	static auto ValidateBufferTextureCopies(
		FRHIBuffer* Buffer, FRHITexture* Texture,
		std::span<const FRHIBufferTextureCopyRegion> Regions,
		bool bBufferIsSource, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (!Buffer || !Texture) return Fail("Buffer-texture copy resources must be nonnull.");
		if (Texture->GetNumSamples() != 1) return Fail("Buffer-texture copies require single-sampled textures.");
		if (bBufferIsSource)
		{
			if (!EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::SourceCopy))
				return Fail("Buffer-to-texture source requires SourceCopy usage.");
			if (!EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::DestinationCopy))
				return Fail("Buffer-to-texture destination requires DestinationCopy usage.");
		}
		else
		{
			if (!EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
				return Fail("Texture-to-buffer source requires SourceCopy usage.");
			if (!EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::DestinationCopy))
				return Fail("Texture-to-buffer destination requires DestinationCopy usage.");
		}
		std::vector<std::pair<uint64, uint64>> BufferRanges;
		BufferRanges.reserve(Regions.size());
		for (size_t Index = 0; Index < Regions.size(); ++Index)
		{
			const auto& Region = Regions[Index];
			if (!ValidateCopyTextureRegion(*Texture, Region.TextureAspect, Region.TextureMip,
				Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers,
				Region.TextureOffset, Region.TextureExtent, OutError)) return false;
			uint64 Footprint = 0;
			if (!GetBufferTextureFootprint(*Texture, Region, Footprint, OutError)) return false;
			if (Region.BufferOffset > Buffer->GetSize() || Footprint > Buffer->GetSize() - Region.BufferOffset)
				return Fail("Buffer-texture copy footprint exceeds the buffer.");
			BufferRanges.emplace_back(Region.BufferOffset, Footprint);
			for (size_t Other = 0; Other < Index; ++Other)
			{
				if (!bBufferIsSource && RangesOverlap(Region.BufferOffset, Footprint,
					BufferRanges[Other].first, BufferRanges[Other].second))
					return Fail("Texture-to-buffer copy batch contains overlapping destinations.");
				const auto& Previous = Regions[Other];
				if (bBufferIsSource && Region.TextureAspect == Previous.TextureAspect
					&& Region.TextureMip == Previous.TextureMip
					&& TextureBoxesOverlap(Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers,
						Region.TextureOffset, Region.TextureExtent,
						Previous.TextureFirstArrayLayer, Previous.TextureNumArrayLayers,
						Previous.TextureOffset, Previous.TextureExtent))
					return Fail("Buffer-to-texture copy batch contains overlapping destinations.");
			}
		}
		OutError.clear();
		return true;
	}

	auto ValidateBufferToTextureCopies(FRHIBuffer* Source, FRHITexture* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions, std::string& OutError) -> bool
	{
		return ValidateBufferTextureCopies(Source, Destination, Regions, true, OutError);
	}

	auto ValidateTextureToBufferCopies(FRHITexture* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions, std::string& OutError) -> bool
	{
		return ValidateBufferTextureCopies(Destination, Source, Regions, false, OutError);
	}

	auto ValidateTextureCopies(FRHITexture* Source, FRHITexture* Destination,
		std::span<const FRHITextureCopyRegion> Regions, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](const char* Message) { OutError = Message; return false; };
		if (!Source || !Destination) return Fail("Texture copy resources must be nonnull.");
		if (!EnumHasAnyFlags(Source->GetFlags(), ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
			return Fail("Texture copy source requires SourceCopy usage.");
		if (!EnumHasAnyFlags(Destination->GetFlags(), ETextureCreateFlags::DestinationCopy))
			return Fail("Texture copy destination requires DestinationCopy usage.");
		if (Source->GetFormat() != Destination->GetFormat())
			return Fail("Texture copies require identical formats.");
		if (Source->GetNumSamples() != 1 || Destination->GetNumSamples() != 1)
			return Fail("Texture copies require sample count one.");
		for (size_t Index = 0; Index < Regions.size(); ++Index)
		{
			const auto& Region = Regions[Index];
			if (Region.SourceAspect != Region.DestinationAspect)
				return Fail("Texture copy source and destination aspects must match.");
			if (!ValidateCopyTextureRegion(*Source, Region.SourceAspect, Region.SourceMip,
				Region.SourceFirstArrayLayer, Region.NumArrayLayers,
				Region.SourceOffset, Region.Extent, OutError)
				|| !ValidateCopyTextureRegion(*Destination, Region.DestinationAspect,
					Region.DestinationMip, Region.DestinationFirstArrayLayer,
					Region.NumArrayLayers, Region.DestinationOffset, Region.Extent, OutError)) return false;
			for (size_t Other = 0; Other < Index; ++Other)
			{
				const auto& Previous = Regions[Other];
				if (Region.DestinationAspect == Previous.DestinationAspect
					&& Region.DestinationMip == Previous.DestinationMip
					&& TextureBoxesOverlap(Region.DestinationFirstArrayLayer, Region.NumArrayLayers,
						Region.DestinationOffset, Region.Extent,
						Previous.DestinationFirstArrayLayer, Previous.NumArrayLayers,
						Previous.DestinationOffset, Previous.Extent))
					return Fail("Texture copy batch contains overlapping destinations.");
			}
		}
		if (Source == Destination)
		{
			for (const auto& A : Regions)
				for (const auto& B : Regions)
					if (A.SourceAspect == B.DestinationAspect && A.SourceMip == B.DestinationMip
						&& TextureBoxesOverlap(A.SourceFirstArrayLayer, A.NumArrayLayers,
							A.SourceOffset, A.Extent, B.DestinationFirstArrayLayer,
							B.NumArrayLayers, B.DestinationOffset, B.Extent))
						return Fail("Same-texture copy source and destination regions overlap.");
		}
		OutError.clear();
		return true;
	}

	auto ValidateTextureCreateDesc(const FRHITextureCreateDesc& CreateDesc, std::string& OutError) -> bool
	{
		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};

		if (CreateDesc.Extent.x <= 0 || CreateDesc.Extent.y <= 0) return Fail("Texture extent must be nonzero.");
		if (CreateDesc.Depth == 0) return Fail("Texture depth must be nonzero.");
		if (CreateDesc.ArraySize == 0) return Fail("Texture array size must be nonzero.");
		if (CreateDesc.NumMips == 0) return Fail("Texture mip count must be nonzero.");
		if (CreateDesc.NumSamples == 0) return Fail("Texture sample count must be nonzero.");
		if (CreateDesc.Format == EPixelFormat::Unknown) return Fail("Texture pixel format must be specified.");
		if (CreateDesc.NumSamples != 1 && CreateDesc.NumSamples != 2
			&& CreateDesc.NumSamples != 4 && CreateDesc.NumSamples != 8
			&& CreateDesc.NumSamples != 16)
		{
			return Fail("Texture sample count must be one of 1, 2, 4, 8, or 16.");
		}

		switch (CreateDesc.Dimension)
		{
		case ETextureDimension::Texture2D:
			if (CreateDesc.Depth != 1) return Fail("Texture2D depth must be one.");
			if (CreateDesc.ArraySize != 1) return Fail("Texture2D array size must be one.");
			break;
		case ETextureDimension::Texture2DArray:
			if (CreateDesc.Depth != 1) return Fail("Texture2DArray depth must be one.");
			break;
		case ETextureDimension::Texture3D:
			if (CreateDesc.ArraySize != 1) return Fail("Texture3D array size must be one.");
			break;
		case ETextureDimension::TextureCube:
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return Fail("TextureCube width and height must be equal.");
			if (CreateDesc.ArraySize != TextureCubeFaceCount) return Fail("TextureCube must contain exactly six array layers.");
			if (CreateDesc.Depth != 1) return Fail("TextureCube depth must be one.");
			break;
		case ETextureDimension::TextureCubeArray:
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return Fail("TextureCubeArray width and height must be equal.");
			if (CreateDesc.Depth != 1) return Fail("TextureCubeArray depth must be one.");
			if (CreateDesc.ArraySize % TextureCubeFaceCount != 0) return Fail("TextureCubeArray layer count must be divisible by six.");
			break;
		default:
			return Fail("Texture dimension is invalid.");
		}

		const uint32 MaxDimension = CreateDesc.Dimension == ETextureDimension::Texture3D
			? std::max({static_cast<uint32>(CreateDesc.Extent.x), static_cast<uint32>(CreateDesc.Extent.y), static_cast<uint32>(CreateDesc.Depth)})
			: std::max(static_cast<uint32>(CreateDesc.Extent.x), static_cast<uint32>(CreateDesc.Extent.y));
		uint32 MaximumMipCount = 1;
		for (uint32 Remaining = MaxDimension; Remaining > 1; Remaining >>= 1) ++MaximumMipCount;
		if (CreateDesc.NumMips > MaximumMipCount) return Fail("Texture mip count exceeds the complete mip chain for its extent.");
		if (CreateDesc.NumSamples != 1 && CreateDesc.NumMips != 1) return Fail("Multisampled textures must have exactly one mip.");
		if ((CreateDesc.Dimension == ETextureDimension::Texture3D
			|| CreateDesc.Dimension == ETextureDimension::TextureCube
			|| CreateDesc.Dimension == ETextureDimension::TextureCubeArray)
			&& CreateDesc.NumSamples != 1)
		{
			return Fail("Texture3D, TextureCube, and TextureCubeArray must be single-sampled.");
		}

		const bool bDepthStencil = EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bColorOrResolve = EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable);
		if (bDepthStencil && (bColorOrResolve || EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage)))
			return Fail("Depth-stencil texture usage is mutually exclusive with color, resolve, and storage usage.");
		if (CreateDesc.NumSamples != 1 && EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::Storage | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ResolveTargetable))
			return Fail("Storage, CPU-readback, and resolve textures must be single-sampled.");

		const uint64 SubresourceCount = static_cast<uint64>(CreateDesc.NumMips) * CreateDesc.ArraySize;
		if (SubresourceCount > std::numeric_limits<uint32>::max()) return Fail("Texture subresource count exceeds the portable range.");

		OutError.clear();
		return true;
	}

	auto ValidateTexture2DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch,
		std::string& OutError
	) -> bool
	{
		auto Fail = [&OutError](std::string Message) {
			OutError = std::move(Message);
			return false;
		};

		if (MipIndex >= TextureDesc.NumMips) return Fail("Texture upload mip index is outside the texture mip range.");
		if (ArraySlice >= TextureDesc.ArraySize) return Fail("Texture upload array slice is outside the texture layer range.");
		if (UpdateRegion.SrcX < 0 || UpdateRegion.SrcY < 0) return Fail("Texture upload source offsets must be nonnegative.");
		if (UpdateRegion.Width == 0 || UpdateRegion.Height == 0) return Fail("Texture upload region must be nonzero.");

		const uint32 MipWidth = std::max(1u, static_cast<uint32>(TextureDesc.Extent.x) >> MipIndex);
		const uint32 MipHeight = std::max(1u, static_cast<uint32>(TextureDesc.Extent.y) >> MipIndex);
		if (static_cast<uint64>(UpdateRegion.DestX) + UpdateRegion.Width > MipWidth
			|| static_cast<uint64>(UpdateRegion.DestY) + UpdateRegion.Height > MipHeight)
		{
			return Fail("Texture upload destination region exceeds the selected mip.");
		}

		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(TextureDesc.Format);
		if (FormatInfo.BytesPerBlock == 0 || FormatInfo.BlockSize == 0)
		{
			return Fail("Texture upload requires a valid pixel format layout.");
		}

		const uint32 BlockSize = FormatInfo.BlockSize;
		if (UpdateRegion.DestX % BlockSize != 0 || UpdateRegion.DestY % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcX) % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcY) % BlockSize != 0)
		{
			return Fail("Texture upload source and destination offsets must be block-aligned.");
		}
		if ((UpdateRegion.Width % BlockSize != 0 && UpdateRegion.DestX + UpdateRegion.Width != MipWidth)
			|| (UpdateRegion.Height % BlockSize != 0 && UpdateRegion.DestY + UpdateRegion.Height != MipHeight))
		{
			return Fail("Texture upload dimensions must be block-aligned unless the region reaches the mip edge.");
		}

		const uint64 SourceBlockX = static_cast<uint32>(UpdateRegion.SrcX) / BlockSize;
		const uint64 RegionBlocksWide = (static_cast<uint64>(UpdateRegion.Width) + BlockSize - 1) / BlockSize;
		const uint64 RequiredPitch = (SourceBlockX + RegionBlocksWide) * FormatInfo.BytesPerBlock;
		if (RequiredPitch > SourcePitch) return Fail("Texture upload source pitch is too small for the requested source region.");

		OutError.clear();
		return true;
	}

	auto ResolveTextureCubeFaceUv(const FVector3& Direction, ETextureCubeFace& OutFace, FVector2f& OutUv) -> bool
	{
		const double AbsX = std::abs(Direction.x);
		const double AbsY = std::abs(Direction.y);
		const double AbsZ = std::abs(Direction.z);
		const double MajorAxis = std::max({AbsX, AbsY, AbsZ});
		if (!std::isfinite(MajorAxis) || MajorAxis <= 0.0) return false;

		double Sc = 0.0;
		double Tc = 0.0;
		if (AbsX >= AbsY && AbsX >= AbsZ)
		{
			if (Direction.x >= 0.0)
			{
				OutFace = ETextureCubeFace::PositiveX;
				Sc = -Direction.z;
				Tc = -Direction.y;
			}
			else
			{
				OutFace = ETextureCubeFace::NegativeX;
				Sc = Direction.z;
				Tc = -Direction.y;
			}
		}
		else if (AbsY >= AbsZ)
		{
			if (Direction.y >= 0.0)
			{
				OutFace = ETextureCubeFace::PositiveY;
				Sc = Direction.x;
				Tc = Direction.z;
			}
			else
			{
				OutFace = ETextureCubeFace::NegativeY;
				Sc = Direction.x;
				Tc = -Direction.z;
			}
		}
		else if (Direction.z >= 0.0)
		{
			OutFace = ETextureCubeFace::PositiveZ;
			Sc = Direction.x;
			Tc = -Direction.y;
		}
		else
		{
			OutFace = ETextureCubeFace::NegativeZ;
			Sc = -Direction.x;
			Tc = -Direction.y;
		}

		OutUv = FVector2f(
			static_cast<float>((Sc / MajorAxis + 1.0) * 0.5),
			static_cast<float>((Tc / MajorAxis + 1.0) * 0.5)
		);
		return true;
	}

	auto ResolveTextureCubeFacePixelDirection(ETextureCubeFace Face, uint32 PixelX, uint32 PixelY,
		uint32 FaceDimension, FVector3& OutDirection) -> bool
	{
		if (FaceDimension == 0 || PixelX >= FaceDimension || PixelY >= FaceDimension) return false;
		const double A = 2.0 * (static_cast<double>(PixelX) + 0.5) / FaceDimension - 1.0;
		const double B = 2.0 * (static_cast<double>(PixelY) + 0.5) / FaceDimension - 1.0;
		switch (Face)
		{
		case ETextureCubeFace::PositiveX: OutDirection = FVector3(1.0, -B, -A); break;
		case ETextureCubeFace::NegativeX: OutDirection = FVector3(-1.0, -B, A); break;
		case ETextureCubeFace::PositiveY: OutDirection = FVector3(A, 1.0, B); break;
		case ETextureCubeFace::NegativeY: OutDirection = FVector3(A, -1.0, -B); break;
		case ETextureCubeFace::PositiveZ: OutDirection = FVector3(A, -B, 1.0); break;
		case ETextureCubeFace::NegativeZ: OutDirection = FVector3(-A, -B, -1.0); break;
		default: return false;
		}
		const double Length = Math::Length(OutDirection);
		if (!std::isfinite(Length) || Length <= 0.0) return false;
		OutDirection /= Length;
		return true;
	}

	// May use a multiple producer single consumer queue here if the contention is high, but currently we don't have that many threads creating resources, so a simple vector with mutex should be fine.
	std::vector<FRHIResource*> PendingDeletes;
	std::mutex PendingDeletesMutex;

#if DO_CHECK
	// This pointer will be set before any FRHIResource being deleted, then it will be checked and reset in the destructor of FRHIResource.
	// This is to catch any unexpected deletion, such as deleting a resource manually without calling DeleteResources.
	thread_local const FRHIResource* CurrentDeleting = nullptr;
#endif

	FRHIResource::FRHIResource(ERHIResourceType InResourceType)
		: ResourceType(InResourceType)
	{
	}

	FRHIResource::~FRHIResource()
	{
#if DO_CHECK
		// A derived constructor may fail before the resource is ever referenced.
		// Published resources must still arrive through DeleteResources.
		check(IsEngineExitRequested() || CurrentDeleting == this
			|| AtomicFlags.IsUnpublished(std::memory_order_relaxed));
		if (CurrentDeleting == this)
		{
			CurrentDeleting = nullptr;
		}
#endif
	}

	auto FRHIResource::EnqueueForDelete() const -> void
	{
		std::lock_guard<std::mutex> lock(PendingDeletesMutex);
		PendingDeletes.push_back(const_cast<FRHIResource*>(this));
	}

	auto FRHIResource::DeleteResources(const std::vector<FRHIResource*>& ResourcesToDelete) -> void
	{
		for (FRHIResource* Resource : ResourcesToDelete)
		{
			const bool bBeganDeleting = Resource->AtomicFlags.BeginDelete();
			if (!bBeganDeleting)
			{
				checkf(false,
					"Deferred RHI resource was not in the pending-delete state.");
				std::terminate();
			}
#if DO_CHECK
			CurrentDeleting = Resource;
#endif
			delete Resource;
#if DO_CHECK
			check(CurrentDeleting == nullptr);
#endif
		}
	}

	auto FRHIResource::GatherResourcesToDelete(std::vector<FRHIResource*>& OutResourcesToDelete) -> void
	{
		std::vector<FRHIResource*> LocalTemp;
		{
			std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
			LocalTemp.swap(PendingDeletes);
		}
		OutResourcesToDelete.insert(
			OutResourcesToDelete.end(),
			std::make_move_iterator(LocalTemp.begin()),
			std::make_move_iterator(LocalTemp.end())
		);
	}

	auto FRHIResource::GetNumPendingDeletes() -> size_t
	{
		std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
		return PendingDeletes.size();
	}

} // namespace Durin
