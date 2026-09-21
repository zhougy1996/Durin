#include "RHIResources.h"
#include "RHIShaderParameterValidationInternal.h"

#include "RHI.h"
#include "RHICapabilities.h"

#include "Math/Operations.h"

#include "Math/Vector.h"

namespace Durin
{
	auto FRHICreationError::GetSemanticFingerprint() const -> size_t
	{
		size_t Fingerprint = 0;
		auto Add = [&]<typename T>(const T& Value) {
			Fingerprint ^= std::hash<T>{}(Value) + 0x9e3779b9 + (Fingerprint << 6) + (Fingerprint >> 2);
		};
		Add(Failure); Add(Source); Add(NativeCode);
		return Fingerprint;
	}

	std::atomic<uint64> FRHIGPUTimingQuery::InvalidRecordingCount = 0;

	FRHIGPUTimingQuery::FRHIGPUTimingQuery()
		: FRHIResource(ERHIResourceType::GPUTimingQuery)
	{
	}

	auto FRHIGPUTimingQuery::GetResult() const -> FRHIGPUTimingResult
	{
		const ERHIGPUTimingResultState State =
			ResultState.load(std::memory_order_acquire);
		return {State, State == ERHIGPUTimingResultState::Ready
			? DurationNanoseconds.load(std::memory_order_relaxed) : 0};
	}

	auto FRHIGPUTimingQuery::TryReserveRecording() -> bool
	{
		ERecordingState Expected = ERecordingState::Idle;
		if (!RecordingState.compare_exchange_strong(Expected,
			ERecordingState::Recorded, std::memory_order_acq_rel))
		{
			RecordInvalidRecording();
			return false;
		}
		DurationNanoseconds.store(0, std::memory_order_relaxed);
		ResultState.store(ERHIGPUTimingResultState::Pending,
			std::memory_order_release);
		return true;
	}

	auto FRHIGPUTimingQuery::RecordInvalidRecording() -> void
	{
		uint64 Current = InvalidRecordingCount.load(std::memory_order_relaxed);
		while (Current != std::numeric_limits<uint64>::max()
			&& !InvalidRecordingCount.compare_exchange_weak(Current, Current + 1,
				std::memory_order_relaxed, std::memory_order_relaxed)) {}
	}

	auto FRHIGPUTimingQuery::GetInvalidRecordingCount() -> uint64
	{
		return InvalidRecordingCount.load(std::memory_order_relaxed);
	}

	auto FRHIGPUTimingQuery::ResetInvalidRecordingCount() -> void
	{
		InvalidRecordingCount.store(0, std::memory_order_relaxed);
	}

	auto FRHIGPUTimingQuery::CommitRecording() -> bool
	{
		ERecordingState Expected = ERecordingState::Recorded;
		return RecordingState.compare_exchange_strong(Expected,
			ERecordingState::Committed, std::memory_order_acq_rel);
	}

	auto FRHIGPUTimingQuery::CancelRecording() -> void
	{
		ERecordingState Expected = ERecordingState::Recorded;
		if (RecordingState.compare_exchange_strong(Expected,
			ERecordingState::Idle, std::memory_order_acq_rel))
			ResultState.store(ERHIGPUTimingResultState::Invalid,
				std::memory_order_release);
	}

	auto FRHIGPUTimingQuery::PublishReady(uint64 InDurationNanoseconds) -> void
	{
		DurationNanoseconds.store(InDurationNanoseconds,
			std::memory_order_relaxed);
		RecordingState.store(ERecordingState::Idle, std::memory_order_release);
		ResultState.store(ERHIGPUTimingResultState::Ready,
			std::memory_order_release);
	}

	auto FRHIGPUTimingQuery::PublishInvalid() -> void
	{
		DurationNanoseconds.store(0, std::memory_order_relaxed);
		RecordingState.store(ERecordingState::Idle, std::memory_order_release);
		ResultState.store(ERHIGPUTimingResultState::Invalid,
			std::memory_order_release);
	}

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

		auto IsValidAccessShape(ERHIAccess Access, bool bExpected) -> bool
		{
			if (Access == ERHIAccess::Discard || Access == ERHIAccess::None) return bExpected;
			if (EnumHasAnyFlags(Access, ERHIAccess::Discard)) return false;
			const ERHIAccess KnownMask = ReadAccessMask | ExclusiveAccessMask;
			if ((static_cast<uint32>(Access) & ~static_cast<uint32>(KnownMask)) != 0) return false;
			return !EnumHasAnyFlags(Access, ExclusiveAccessMask) || IsSingleBit(Access);
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

	auto ValidateShaderParameterUpdate(const FPipelineLayoutDesc& Layout,
		EShaderStageFlags ShaderStage,
		std::span<const FRHIShaderParameterResource> Resources) -> std::expected<void, FRHIShaderBindingError>
	{
		if (ShaderStage != EShaderStageFlags::Vertex
			&& ShaderStage != EShaderStageFlags::Fragment
			&& ShaderStage != EShaderStageFlags::Compute)
			return std::unexpected(FRHIShaderBindingError{ERHIShaderBindingError::InvalidStage});
		for (uint32 Index = 0; Index < Resources.size(); ++Index)
		{
			const auto& Resource = Resources[Index];
			auto Fail = [&](ERHIShaderBindingError Code) {
				FRHIShaderBindingError Error{Code, Index};
				Error.SetIndex = Resource.SetIndex;
				Error.BindingIndex = Resource.BindingIndex;
				Error.ArrayElement = Resource.ArrayElement;
				return std::unexpected(std::move(Error));
			};
			if (Resource.SetIndex >= Layout.BindingLayouts.size())
				return Fail(ERHIShaderBindingError::SetOutOfRange);
			const FBindingLayout& Set = Layout.BindingLayouts[Resource.SetIndex];
			const auto BindingIt = std::ranges::find(Set.BindingLayouts,
				Resource.BindingIndex, &FBindingLayoutItem::Slot);
			if (BindingIt == Set.BindingLayouts.end()) return Fail(ERHIShaderBindingError::UnexpectedBinding);
			if (BindingIt->Type != Resource.Type)
			{
				auto Result = Fail(ERHIShaderBindingError::TypeMismatch);
				Result.error().ExpectedBindingType = BindingIt->Type;
				Result.error().ActualBindingType = Resource.Type;
				return Result;
			}
			if (Resource.ArrayElement >= BindingIt->ArraySize) return Fail(ERHIShaderBindingError::ArrayElementOutOfRange);
			if (!EnumHasAnyFlags(BindingIt->StageFlags, ShaderStage)) return Fail(ERHIShaderBindingError::StageMismatch);
		}
		return {};
	}

	auto ValidateShaderBindingCompleteness(const FPipelineLayoutDesc& Layout,
		std::span<const FRHIShaderParameterResource> Resources) -> std::expected<void, FRHIShaderBindingError>
	{
		return RHIShaderParameterValidationInternal::VisitOrderedBindings(
			Layout, Resources, [](const auto&, const auto&) {});
	}

	static auto ValidateGraphicsPipelineInitializer(
		const FGraphicsPipelineStateInitializer& Initializer,
		const FRHICapabilities* Capabilities) -> std::expected<void, ERHIGraphicsPipelineError>
	{
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
			return std::unexpected(ERHIGraphicsPipelineError::InvalidFixedState);
		for (const FRHIColorBlendState& Blend : Initializer.ColorBlendStates)
			if (!IsBlendValid(Blend))
				return std::unexpected(ERHIGraphicsPipelineError::InvalidBlendState);

		if (!Initializer.BoundShaders.VertexShader
			|| !Initializer.BoundShaders.FragmentShader)
			return std::unexpected(ERHIGraphicsPipelineError::MissingShaders);
		if (Initializer.BoundShaders.VertexShader->GetFrequency() != EShaderFrequency::Vertex
			|| Initializer.BoundShaders.FragmentShader->GetFrequency() != EShaderFrequency::Fragment)
			return std::unexpected(ERHIGraphicsPipelineError::ShaderStageMismatch);
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
					return std::unexpected(ERHIGraphicsPipelineError::InvalidReflectedLayout);
			}
		}
		for (const FPushConstantRange& Range : Initializer.PipelineLayout.PushConstantRanges)
		{
			const auto KnownStages = EShaderStageFlags::Vertex | EShaderStageFlags::Fragment;
			if (Range.StageFlags == EShaderStageFlags::None || Range.Size == 0
				|| (Range.Offset % 4) != 0 || (Range.Size % 4) != 0
				|| (static_cast<uint32>(Range.StageFlags)
					& ~static_cast<uint32>(KnownStages)) != 0)
				return std::unexpected(ERHIGraphicsPipelineError::InvalidPushConstants);
		}

		if (!Initializer.RenderTargetLayout.IsValid())
			return std::unexpected(ERHIGraphicsPipelineError::InvalidRenderTargets);
		const uint8 RasterSamples = Initializer.RenderTargetLayout.NumColorRenderTargets > 0
			? Initializer.RenderTargetLayout.ColorAttachments[0].RenderTarget.NumSamples
			: Initializer.RenderTargetLayout.DepthStencilAttachment.NumSamples;
		if (Initializer.MultisampleState.RasterSamples != RasterSamples)
			return std::unexpected(ERHIGraphicsPipelineError::SampleCountMismatch);
		if ((Initializer.DepthStencilState.bEnableTest
			|| Initializer.DepthStencilState.bEnableWrite
			|| Initializer.DepthStencilState.bEnableStencil)
			&& !Initializer.RenderTargetLayout.bHasDepthStencil)
			return std::unexpected(ERHIGraphicsPipelineError::MissingDepthAttachment);
		if (Initializer.DepthStencilState.bEnableStencil
			&& !GetPixelFormatInfo(Initializer.RenderTargetLayout.DepthStencilAttachment.Format).bHasStencil)
			return std::unexpected(ERHIGraphicsPipelineError::MissingStencilAttachment);

		if (!Initializer.VertexDeclaration)
			return std::unexpected(ERHIGraphicsPipelineError::MissingVertexDeclaration);
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
				return std::unexpected(ERHIGraphicsPipelineError::InvalidVertexDeclaration);
			const auto [StreamIt, bInserted] = Streams.emplace(Element.StreamIndex,
				std::pair{Element.Stride, Element.InputRate});
			if (!bInserted && StreamIt->second != std::pair{Element.Stride, Element.InputRate})
				return std::unexpected(ERHIGraphicsPipelineError::InconsistentVertexStream);
			for (const FVertexElement& Existing : Initializer.VertexDeclaration->GetElements())
			{
				if (&Existing == &Element) break;
				const uint32 ExistingSize = GetVertexElementSize(Existing.Type);
				if (Existing.StreamIndex == Element.StreamIndex
					&& Element.Offset < Existing.Offset + ExistingSize
					&& Existing.Offset < Element.Offset + ElementSize)
					return std::unexpected(ERHIGraphicsPipelineError::OverlappingVertexElements);
			}
		}

		if (Capabilities)
		{
			if (Initializer.RenderTargetLayout.NumColorRenderTargets > Capabilities->MaxColorAttachments)
				return std::unexpected(ERHIGraphicsPipelineError::TooManyColorAttachments);
			const ERHISampleCountFlags SampleFlag = SampleCountFlag(RasterSamples);
			if (!EnumHasAllFlags(Capabilities->ColorSampleCounts, SampleFlag)
				|| (Initializer.RenderTargetLayout.bHasDepthStencil
					&& !EnumHasAllFlags(Capabilities->DepthSampleCounts, SampleFlag)))
				return std::unexpected(ERHIGraphicsPipelineError::UnsupportedSampleCount);
			if (Initializer.RasterizerState.PolygonMode != ERHIPolygonMode::Fill
				&& !Capabilities->bSupportsNonSolidFill)
				return std::unexpected(ERHIGraphicsPipelineError::UnsupportedFillMode);
			if (Initializer.RasterizerState.bEnableDepthClamp
				&& !Capabilities->bSupportsDepthClamp)
				return std::unexpected(ERHIGraphicsPipelineError::UnsupportedDepthClamp);
			if (Initializer.RasterizerState.LineWidth != 1.0f
				&& !Capabilities->bSupportsWideLines)
				return std::unexpected(ERHIGraphicsPipelineError::UnsupportedWideLines);
		}

		return {};
	}

	auto FGraphicsPipelineStateInitializer::IsValid() const -> bool
	{
		return ValidateGraphicsPipelineInitializer(*this, nullptr).has_value();
	}

	auto BuildGraphicsPipelineStateKey(
		const FGraphicsPipelineStateInitializer& Initializer,
		const FRHICapabilities* Capabilities) -> std::expected<FGraphicsPipelineStateKey, ERHIGraphicsPipelineError>
	{
		if (auto Validation = ValidateGraphicsPipelineInitializer(Initializer, Capabilities); !Validation)
			return std::unexpected(Validation.error());
		const auto CanonicalizeNumericFloat = [](float Value) {
			// Accepted key floats use numeric equality. Collapse both signed-zero
			// representations so their byte-wise hash has the same identity.
			return Value == 0.0f ? 0.0f : Value;
		};

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
		for (const FVertexElement& Element : Initializer.VertexDeclaration->GetElements())
		{
			if (Element.Type == EVertexElementType::None) break;
			Key.VertexElements.push_back({Element.StreamIndex, Element.Offset,
				Element.Type, Element.AttributeIndex, Element.Stride, Element.InputRate});
		}
		Key.PipelineLayout = Initializer.PipelineLayout;
		for (FBindingLayout& Set : Key.PipelineLayout.BindingLayouts)
			std::ranges::sort(Set.BindingLayouts, {}, &FBindingLayoutItem::Slot);
		std::ranges::sort(Key.PipelineLayout.PushConstantRanges,
			[](const FPushConstantRange& A, const FPushConstantRange& B) {
				return std::tie(A.Offset, A.Size, A.StageFlags)
					< std::tie(B.Offset, B.Size, B.StageFlags);
			});
		Key.RasterizerState = Initializer.RasterizerState;
		// Depth-bias values are dynamic draw state. Only whether the pipeline
		// enables depth bias remains structural pipeline identity.
		Key.RasterizerState.DepthBiasConstantFactor = 0.0f;
		Key.RasterizerState.DepthBiasClamp = 0.0f;
		Key.RasterizerState.DepthBiasSlopeFactor = 0.0f;
		if (Key.RasterizerState.PolygonMode != ERHIPolygonMode::Line)
			Key.RasterizerState.LineWidth = 1.0f;
		Key.RasterizerState.LineWidth = CanonicalizeNumericFloat(
			Key.RasterizerState.LineWidth);
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
		return Key;
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

	static auto ValidateComputePipelineInitializer(
		const FComputePipelineStateInitializer& Initializer,
		const FRHICapabilities* Capabilities) -> std::expected<void, ERHIComputePipelineError>
	{
		if (!Initializer.ComputeShader)
			return std::unexpected(ERHIComputePipelineError::MissingShader);
		if (Initializer.ComputeShader->GetFrequency() != EShaderFrequency::Compute)
			return std::unexpected(ERHIComputePipelineError::ShaderStageMismatch);
		for (const FBindingLayout& Set : Initializer.PipelineLayout.BindingLayouts)
		{
			std::unordered_set<uint32> Slots;
			for (const FBindingLayoutItem& Binding : Set.BindingLayouts)
			{
				if (Binding.StageFlags != EShaderStageFlags::Compute
					|| Binding.Type > ERHIBindingType::StorageImage
					|| Binding.ArraySize == 0
					|| !Slots.insert(Binding.Slot).second)
					return std::unexpected(ERHIComputePipelineError::InvalidReflectedLayout);
			}
		}
		for (size_t Index = 0;
			Index < Initializer.PipelineLayout.PushConstantRanges.size(); ++Index)
		{
			const FPushConstantRange& Range =
				Initializer.PipelineLayout.PushConstantRanges[Index];
			if (Range.StageFlags != EShaderStageFlags::Compute || Range.Size == 0
				|| (Range.Offset % 4) != 0 || (Range.Size % 4) != 0)
				return std::unexpected(ERHIComputePipelineError::InvalidPushConstants);
			for (size_t OtherIndex = Index + 1;
				OtherIndex < Initializer.PipelineLayout.PushConstantRanges.size();
				++OtherIndex)
			{
				const FPushConstantRange& Other =
					Initializer.PipelineLayout.PushConstantRanges[OtherIndex];
				if (RangesOverlap(Range.Offset, Range.Size, Other.Offset, Other.Size))
					return std::unexpected(ERHIComputePipelineError::OverlappingPushConstants);
			}
		}
		if (Capabilities && std::ranges::any_of(
			Capabilities->MaxComputeWorkGroupCount,
			[](uint32 Limit) { return Limit == 0; }))
			return std::unexpected(ERHIComputePipelineError::MissingDispatchLimits);

		return {};
	}

	auto FComputePipelineStateInitializer::IsValid() const -> bool
	{
		return ValidateComputePipelineInitializer(*this, nullptr).has_value();
	}

	auto BuildComputePipelineStateKey(
		const FComputePipelineStateInitializer& Initializer,
		const FRHICapabilities* Capabilities) -> std::expected<FComputePipelineStateKey, ERHIComputePipelineError>
	{
		if (auto Validation = ValidateComputePipelineInitializer(Initializer, Capabilities); !Validation)
			return std::unexpected(Validation.error());

		FComputePipelineStateKey Key;
		Key.ComputeShaderHash = Initializer.ComputeShader->GetHash();
		Key.PipelineLayout = Initializer.PipelineLayout;
		for (FBindingLayout& Set : Key.PipelineLayout.BindingLayouts)
			std::ranges::sort(Set.BindingLayouts, {}, &FBindingLayoutItem::Slot);
		std::ranges::sort(Key.PipelineLayout.PushConstantRanges,
			[](const FPushConstantRange& A, const FPushConstantRange& B) {
				return std::tie(A.Offset, A.Size, A.StageFlags)
					< std::tie(B.Offset, B.Size, B.StageFlags);
			});
		return Key;
	}

	auto FComputePipelineStateKeyHasher::operator()(
		const FComputePipelineStateKey& Key) const -> size_t
	{
		FXxHash64Builder Builder;
		HashValue(Builder, Key.ComputeShaderHash);
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

	auto ValidateBufferTransition(const FRHIBufferTransition& Transition) -> std::expected<void, ERHIBufferTransitionError>
	{
		if (Transition.Buffer == nullptr) return std::unexpected(ERHIBufferTransitionError::NullResource);
		if (Transition.Buffer->GetResourceType() != ERHIResourceType::Buffer) return std::unexpected(ERHIBufferTransitionError::InvalidResourceType);
		if (Transition.Size == 0) return std::unexpected(ERHIBufferTransitionError::EmptyRange);
		const uint64 ResourceSize = Transition.Buffer->GetSize();
		if (Transition.Offset > ResourceSize || Transition.Size > ResourceSize - Transition.Offset)
			return std::unexpected(ERHIBufferTransitionError::RangeOutOfBounds);
		if (!IsValidAccessShape(Transition.ExpectedBefore, true)
			|| !IsValidAccessShape(Transition.RequiredAfter, false))
			return std::unexpected(ERHIBufferTransitionError::InvalidAccess);
		if (!BufferUsageAdmits(Transition.Buffer->GetUsage(), Transition.ExpectedBefore)
			|| !BufferUsageAdmits(Transition.Buffer->GetUsage(), Transition.RequiredAfter))
			return std::unexpected(ERHIBufferTransitionError::IncompatibleUsage);
		return {};
	}

	auto ValidateTextureTransition(const FRHITextureTransition& Transition) -> std::expected<void, ERHITextureTransitionError>
	{
		if (Transition.Texture == nullptr) return std::unexpected(ERHITextureTransitionError::NullResource);
		if (Transition.Texture->GetResourceType() != ERHIResourceType::Texture) return std::unexpected(ERHITextureTransitionError::InvalidResourceType);
		if (Transition.Range.Aspects == ERHITextureAspect::None) return std::unexpected(ERHITextureTransitionError::EmptyAspects);
		constexpr ERHITextureAspect KnownAspects = ERHITextureAspect::Color | ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		if ((static_cast<uint8>(Transition.Range.Aspects) & ~static_cast<uint8>(KnownAspects)) != 0
			|| !EnumHasAllFlags(GetTextureAspects(Transition.Texture->GetFormat()), Transition.Range.Aspects))
			return std::unexpected(ERHITextureTransitionError::UnsupportedAspects);
		if (Transition.Range.NumMips == 0 || Transition.Range.NumArrayLayers == 0)
			return std::unexpected(ERHITextureTransitionError::EmptyRange);
		if (Transition.Range.FirstMip > Transition.Texture->GetNumMips()
			|| Transition.Range.NumMips > Transition.Texture->GetNumMips() - Transition.Range.FirstMip)
			return std::unexpected(ERHITextureTransitionError::MipOutOfBounds);
		if (Transition.Range.FirstArrayLayer > Transition.Texture->GetArraySize()
			|| Transition.Range.NumArrayLayers > Transition.Texture->GetArraySize() - Transition.Range.FirstArrayLayer)
			return std::unexpected(ERHITextureTransitionError::LayerOutOfBounds);
		if (!IsValidAccessShape(Transition.ExpectedBefore, true)
			|| !IsValidAccessShape(Transition.RequiredAfter, false))
			return std::unexpected(ERHITextureTransitionError::InvalidAccess);
		ERHITextureLayout IgnoredLayout;
		if (!GetTextureLayoutForAccess(Transition.ExpectedBefore, IgnoredLayout)
			|| !GetTextureLayoutForAccess(Transition.RequiredAfter, IgnoredLayout))
			return std::unexpected(ERHITextureTransitionError::IndeterminateLayout);
		if (!TextureUsageAdmits(*Transition.Texture, Transition.ExpectedBefore)
			|| !TextureUsageAdmits(*Transition.Texture, Transition.RequiredAfter))
			return std::unexpected(ERHITextureTransitionError::IncompatibleUsage);
		if (!TextureAspectsAdmit(Transition.Range.Aspects, Transition.ExpectedBefore)
			|| !TextureAspectsAdmit(Transition.Range.Aspects, Transition.RequiredAfter))
			return std::unexpected(ERHITextureTransitionError::IncompatibleAspects);
		return {};
	}

	auto ValidateBufferTransitions(std::span<const FRHIBufferTransition> Transitions) -> std::expected<void, FRHIBufferTransitionError>
	{
		for (size_t Index = 0; Index < Transitions.size(); ++Index)
		{
			if (auto Validation = ValidateBufferTransition(Transitions[Index]); !Validation)
			{
				return std::unexpected(FRHIBufferTransitionError{Validation.error(), static_cast<uint32>(Index)});
			}
			for (size_t OtherIndex = 0; OtherIndex < Index; ++OtherIndex)
			{
				if (Transitions[Index].Buffer == Transitions[OtherIndex].Buffer
					&& RangesOverlap(Transitions[Index].Offset, Transitions[Index].Size,
						Transitions[OtherIndex].Offset, Transitions[OtherIndex].Size))
				{
					return std::unexpected(FRHIBufferTransitionError{ERHIBufferTransitionError::OverlappingRanges, static_cast<uint32>(Index), static_cast<uint32>(OtherIndex)});
				}
			}
		}
		return {};
	}

	auto ValidateTextureTransitions(std::span<const FRHITextureTransition> Transitions) -> std::expected<void, FRHITextureTransitionError>
	{
		for (size_t Index = 0; Index < Transitions.size(); ++Index)
		{
			if (auto Validation = ValidateTextureTransition(Transitions[Index]); !Validation)
			{
				return std::unexpected(FRHITextureTransitionError{Validation.error(), static_cast<uint32>(Index)});
			}
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
					return std::unexpected(FRHITextureTransitionError{ERHITextureTransitionError::OverlappingRanges, static_cast<uint32>(Index), static_cast<uint32>(OtherIndex)});
				}
			}
		}
		return {};
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
			.Dimension = !bAttachment && Texture.GetDimension() == ETextureDimension::Texture3D
				? ERHITextureViewDimension::Texture3D
				: (!bAttachment && Texture.GetDimension() == ETextureDimension::TextureCube
				? ERHITextureViewDimension::TextureCube
				: (!bAttachment
						&& Texture.GetDimension() == ETextureDimension::Texture2DArray
					? ERHITextureViewDimension::Texture2DArray
					: ERHITextureViewDimension::Texture2D)),
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
		const FRHIBufferViewDesc& Desc) -> std::expected<void, ERHIBufferViewError>
	{
		if (Buffer == nullptr) return std::unexpected(ERHIBufferViewError::NullParent);
		if (Buffer->GetResourceType() != ERHIResourceType::Buffer) return std::unexpected(ERHIBufferViewError::InvalidParentType);
		if (Desc.Size == 0) return std::unexpected(ERHIBufferViewError::EmptyRange);
		if (Desc.Offset > Buffer->GetSize() || Desc.Size > Buffer->GetSize() - Desc.Offset)
			return std::unexpected(ERHIBufferViewError::RangeOutOfBounds);

		const EBufferUsageFlags Usage = Buffer->GetUsage();
		switch (Desc.Type)
		{
		case ERHIBufferViewType::Uniform:
			if (Desc.Format != EPixelFormat::Unknown) return std::unexpected(ERHIBufferViewError::UniformFormat);
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer))
				return std::unexpected(ERHIBufferViewError::UniformUsage);
			if ((Desc.Offset % 16) != 0 || (Desc.Size % 16) != 0)
				return std::unexpected(ERHIBufferViewError::UniformAlignment);
			break;
		case ERHIBufferViewType::StructuredStorage:
			if (Desc.Format != EPixelFormat::Unknown) return std::unexpected(ERHIBufferViewError::StructuredFormat);
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::UnorderedAccess))
				return std::unexpected(ERHIBufferViewError::StructuredUsage);
			if (Buffer->GetStride() == 0 || (Desc.Offset % Buffer->GetStride()) != 0
				|| (Desc.Size % Buffer->GetStride()) != 0)
				return std::unexpected(ERHIBufferViewError::StructuredAlignment);
			break;
		case ERHIBufferViewType::ByteAddressStorage:
			if (Desc.Format != EPixelFormat::Unknown) return std::unexpected(ERHIBufferViewError::ByteAddressFormat);
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::ByteAddressBuffer))
				return std::unexpected(ERHIBufferViewError::ByteAddressUsage);
			if ((Desc.Offset % 4) != 0 || (Desc.Size % 4) != 0)
				return std::unexpected(ERHIBufferViewError::ByteAddressAlignment);
			break;
		case ERHIBufferViewType::Formatted:
		{
			if (!EnumHasAnyFlags(Usage, EBufferUsageFlags::FormattedBuffer))
				return std::unexpected(ERHIBufferViewError::FormattedUsage);
			const FPixelFormatInfo& Format = GetPixelFormatInfo(Desc.Format);
			if (Desc.Format == EPixelFormat::Unknown || Format.BlockSize != 1
				|| Format.BytesPerBlock == 0 || Format.Kind == EPixelFormatKind::DepthStencil)
				return std::unexpected(ERHIBufferViewError::InvalidFormattedFormat);
			if ((Desc.Offset % Format.BytesPerBlock) != 0 || (Desc.Size % Format.BytesPerBlock) != 0)
				return std::unexpected(ERHIBufferViewError::FormattedAlignment);
			break;
		}
		default:
			return std::unexpected(ERHIBufferViewError::InvalidType);
		}
		return {};
	}

	auto ValidateTextureViewDesc(
		const FRHITexture* Texture,
		const FRHITextureViewDesc& Desc) -> std::expected<void, ERHITextureViewError>
	{
		if (Texture == nullptr) return std::unexpected(ERHITextureViewError::NullParent);
		if (Texture->GetResourceType() != ERHIResourceType::Texture) return std::unexpected(ERHITextureViewError::InvalidParentType);
		if (Desc.Format != Texture->GetFormat()) return std::unexpected(ERHITextureViewError::FormatMismatch);
		if (Desc.Range.Aspects == ERHITextureAspect::None)
			return std::unexpected(ERHITextureViewError::EmptyAspects);
		constexpr ERHITextureAspect KnownAspects = ERHITextureAspect::Color
			| ERHITextureAspect::Depth | ERHITextureAspect::Stencil;
		if ((static_cast<uint8>(Desc.Range.Aspects) & ~static_cast<uint8>(KnownAspects)) != 0
			|| !EnumHasAllFlags(GetTextureAspects(Texture->GetFormat()), Desc.Range.Aspects))
			return std::unexpected(ERHITextureViewError::UnsupportedAspects);
		if (Desc.Range.NumMips == 0 || Desc.Range.NumArrayLayers == 0)
			return std::unexpected(ERHITextureViewError::EmptyRange);
		if (Desc.Range.FirstMip > Texture->GetNumMips()
			|| Desc.Range.NumMips > Texture->GetNumMips() - Desc.Range.FirstMip)
			return std::unexpected(ERHITextureViewError::MipOutOfBounds);
		if (Desc.Range.FirstArrayLayer > Texture->GetArraySize()
			|| Desc.Range.NumArrayLayers > Texture->GetArraySize() - Desc.Range.FirstArrayLayer)
			return std::unexpected(ERHITextureViewError::LayerOutOfBounds);

		if (Desc.Dimension == ERHITextureViewDimension::TextureCube)
		{
			if (Texture->GetDimension() != ETextureDimension::TextureCube
				|| Desc.Range.FirstArrayLayer != 0
				|| Desc.Range.NumArrayLayers != TextureCubeFaceCount)
				return std::unexpected(ERHITextureViewError::InvalidCubeRange);
		}
		else if (Desc.Dimension == ERHITextureViewDimension::Texture3D)
		{
			if (Texture->GetDimension() != ETextureDimension::Texture3D
				|| Desc.Range.FirstArrayLayer != 0
				|| Desc.Range.NumArrayLayers != 1)
				return std::unexpected(ERHITextureViewError::InvalidVolumeRange);
		}
		else if (Desc.Dimension == ERHITextureViewDimension::Texture2D)
		{
			if ((Texture->GetDimension() != ETextureDimension::Texture2D
				&& Texture->GetDimension() != ETextureDimension::Texture2DArray
				&& Texture->GetDimension() != ETextureDimension::TextureCube)
				|| Desc.Range.NumArrayLayers != 1)
				return std::unexpected(ERHITextureViewError::Invalid2DRange);
		}
		else if (Desc.Dimension == ERHITextureViewDimension::Texture2DArray)
		{
			if (Texture->GetDimension() != ETextureDimension::Texture2DArray)
				return std::unexpected(ERHITextureViewError::InvalidArrayParent);
		}
		else
		{
			return std::unexpected(ERHITextureViewError::UnsupportedDimension);
		}

		const ETextureCreateFlags Flags = Texture->GetFlags();
		switch (Desc.Usage)
		{
		case ERHITextureViewUsage::Sampled:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::ShaderResource))
				return std::unexpected(ERHITextureViewError::SampledUsage);
			break;
		case ERHITextureViewUsage::Storage:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::Storage))
				return std::unexpected(ERHITextureViewError::StorageUsage);
			if (Texture->GetNumSamples() != 1
				|| (Desc.Dimension != ERHITextureViewDimension::Texture2D
					&& Desc.Dimension != ERHITextureViewDimension::Texture3D)
				|| Desc.Range.Aspects != ERHITextureAspect::Color)
				return std::unexpected(ERHITextureViewError::InvalidStorageRange);
			break;
		case ERHITextureViewUsage::ColorAttachment:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable))
				return std::unexpected(ERHITextureViewError::ColorAttachmentUsage);
			if (Desc.Dimension != ERHITextureViewDimension::Texture2D
				|| Desc.Range.Aspects != ERHITextureAspect::Color
				|| Desc.Range.NumMips != 1 || Desc.Range.NumArrayLayers != 1)
				return std::unexpected(ERHITextureViewError::InvalidColorAttachmentRange);
			break;
		case ERHITextureViewUsage::DepthStencilAttachment:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::DepthStencilTargetable))
				return std::unexpected(ERHITextureViewError::DepthAttachmentUsage);
			if (Desc.Dimension != ERHITextureViewDimension::Texture2D
				|| EnumHasAnyFlags(Desc.Range.Aspects, ERHITextureAspect::Color)
				|| Desc.Range.NumMips != 1 || Desc.Range.NumArrayLayers != 1)
				return std::unexpected(ERHITextureViewError::InvalidDepthAttachmentRange);
			break;
		case ERHITextureViewUsage::TransferSource:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
				return std::unexpected(ERHITextureViewError::TransferSourceUsage);
			break;
		case ERHITextureViewUsage::TransferDestination:
			if (!EnumHasAnyFlags(Flags, ETextureCreateFlags::DestinationCopy))
				return std::unexpected(ERHITextureViewError::TransferDestinationUsage);
			break;
		default:
			return std::unexpected(ERHITextureViewError::InvalidUsage);
		}
		return {};
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
			const FRHITextureExtent3D& Extent) -> std::expected<void, ERHITextureCopyRegionError>
		{
			if (!IsSingleCopyAspect(Aspect) || !EnumHasAllFlags(GetTextureAspects(Texture.GetFormat()), Aspect))
				return std::unexpected(ERHITextureCopyRegionError::UnsupportedAspect);
			if (Aspect != ERHITextureAspect::Color)
				return std::unexpected(ERHITextureCopyRegionError::UnsupportedDepthStencil);
			if (Mip >= Texture.GetNumMips()) return std::unexpected(ERHITextureCopyRegionError::MipOutOfBounds);
			if (NumLayers == 0 || FirstLayer > Texture.GetArraySize()
				|| NumLayers > Texture.GetArraySize() - FirstLayer)
				return std::unexpected(ERHITextureCopyRegionError::LayerOutOfBounds);
			const bool bTexture3D = Texture.GetDimension() == ETextureDimension::Texture3D;
			if (Offset.X < 0 || Offset.Y < 0 || Offset.Z < 0)
				return std::unexpected(ERHITextureCopyRegionError::NegativeOffset);
			if (Extent.Width == 0 || Extent.Height == 0 || Extent.Depth == 0)
				return std::unexpected(ERHITextureCopyRegionError::EmptyExtent);
			if (Texture.GetDimension() != ETextureDimension::Texture2D
				&& Texture.GetDimension() != ETextureDimension::TextureCube
				&& !bTexture3D)
				return std::unexpected(ERHITextureCopyRegionError::UnsupportedDimension);
			if (bTexture3D)
			{
				if (FirstLayer != 0 || NumLayers != 1)
					return std::unexpected(ERHITextureCopyRegionError::InvalidVolumeLayer);
			}
			else if (Offset.Z != 0 || Extent.Depth != 1)
				return std::unexpected(ERHITextureCopyRegionError::Invalid2DDepth);
			const uint32 MipWidth = std::max(1u, Texture.GetSizeX() >> Mip);
			const uint32 MipHeight = std::max(1u, Texture.GetSizeY() >> Mip);
			const uint32 MipDepth = std::max(1u, Texture.GetSizeZ() >> Mip);
			const uint32 X = static_cast<uint32>(Offset.X);
			const uint32 Y = static_cast<uint32>(Offset.Y);
			const uint32 Z = static_cast<uint32>(Offset.Z);
			if (X > MipWidth || Extent.Width > MipWidth - X
				|| Y > MipHeight || Extent.Height > MipHeight - Y
				|| Z > MipDepth || Extent.Depth > MipDepth - Z)
				return std::unexpected(ERHITextureCopyRegionError::BoxOutOfBounds);
			const uint32 BlockSize = GetPixelFormatInfo(Texture.GetFormat()).BlockSize;
			if (BlockSize == 0) return std::unexpected(ERHITextureCopyRegionError::InvalidBlockLayout);
			if ((X % BlockSize) != 0 || (Y % BlockSize) != 0
				|| ((Extent.Width % BlockSize) != 0 && X + Extent.Width != MipWidth)
				|| ((Extent.Height % BlockSize) != 0 && Y + Extent.Height != MipHeight))
				return std::unexpected(ERHITextureCopyRegionError::BlockAlignment);
			return {};
		}

		auto GetBufferTextureFootprint(
			const FRHITexture& Texture,
			const FRHIBufferTextureCopyRegion& Region) -> std::expected<uint64, ERHICopyFootprintError>
		{
			const FPixelFormatInfo& Format = GetPixelFormatInfo(Texture.GetFormat());
			if (Format.BlockSize == 0 || Format.BytesPerBlock == 0)
				return std::unexpected(ERHICopyFootprintError::InvalidBlockLayout);
			if (Region.TextureExtent.Width == 0 || Region.TextureExtent.Height == 0
				|| (Texture.GetDimension() == ETextureDimension::Texture3D
					? Region.TextureExtent.Depth == 0 : Region.TextureNumArrayLayers == 0))
				return std::unexpected(ERHICopyFootprintError::EmptyFootprint);
			const uint32 RowLength = Region.BufferRowLength != 0
				? Region.BufferRowLength : Region.TextureExtent.Width;
			const uint32 ImageHeight = Region.BufferImageHeight != 0
				? Region.BufferImageHeight : Region.TextureExtent.Height;
			if (RowLength < Region.TextureExtent.Width || ImageHeight < Region.TextureExtent.Height)
				return std::unexpected(ERHICopyFootprintError::LayoutTooSmall);
			if ((Region.BufferRowLength != 0 && (RowLength % Format.BlockSize) != 0)
				|| (Region.BufferImageHeight != 0 && (ImageHeight % Format.BlockSize) != 0))
				return std::unexpected(ERHICopyFootprintError::BlockAlignment);
			if ((Region.BufferOffset % Format.BytesPerBlock) != 0)
				return std::unexpected(ERHICopyFootprintError::OffsetAlignment);
			const uint64 BlocksPerRow = (static_cast<uint64>(RowLength) + Format.BlockSize - 1) / Format.BlockSize;
			const uint64 BlockRows = (static_cast<uint64>(ImageHeight) + Format.BlockSize - 1) / Format.BlockSize;
			if (BlocksPerRow > std::numeric_limits<uint64>::max() / Format.BytesPerBlock)
				return std::unexpected(ERHICopyFootprintError::RowPitchOverflow);
			const uint64 RowPitch = BlocksPerRow * Format.BytesPerBlock;
			if (BlockRows > std::numeric_limits<uint64>::max() / RowPitch)
				return std::unexpected(ERHICopyFootprintError::ImagePitchOverflow);
			const uint64 ImagePitch = BlockRows * RowPitch;
			const uint64 ImageCount = Texture.GetDimension() == ETextureDimension::Texture3D
				? Region.TextureExtent.Depth : Region.TextureNumArrayLayers;
			if (ImageCount > std::numeric_limits<uint64>::max() / ImagePitch)
				return std::unexpected(ERHICopyFootprintError::FootprintOverflow);
			return ImageCount * ImagePitch;
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
				&& OffsetB.Y < OffsetA.Y + static_cast<int64>(ExtentA.Height)
				&& OffsetA.Z < OffsetB.Z + static_cast<int64>(ExtentB.Depth)
				&& OffsetB.Z < OffsetA.Z + static_cast<int64>(ExtentA.Depth);
		}
	}

	auto GetBufferTextureCopyFootprint(const FRHITexture& Texture,
		const FRHIBufferTextureCopyRegion& Region) -> std::expected<uint64, ERHICopyFootprintError>
	{
		return GetBufferTextureFootprint(Texture, Region);
	}

	auto ValidateBufferCopies(FRHIBuffer* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferCopyRegion> Regions) -> std::expected<void, FRHIBufferCopyError>
	{
		if (!Source || !Destination) return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::NullResource});
		if (!EnumHasAnyFlags(Source->GetUsage(), EBufferUsageFlags::SourceCopy))
			return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::SourceUsage});
		if (!EnumHasAnyFlags(Destination->GetUsage(), EBufferUsageFlags::DestinationCopy))
			return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::DestinationUsage});
		for (size_t Index = 0; Index < Regions.size(); ++Index)
		{
			const auto& Region = Regions[Index];
			if (Region.Size == 0) return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::EmptyRange, static_cast<uint32>(Index)});
			if (Region.SourceOffset > Source->GetSize() || Region.Size > Source->GetSize() - Region.SourceOffset)
				return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::SourceOutOfBounds, static_cast<uint32>(Index)});
			if (Region.DestinationOffset > Destination->GetSize()
				|| Region.Size > Destination->GetSize() - Region.DestinationOffset)
				return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::DestinationOutOfBounds, static_cast<uint32>(Index)});
			for (size_t Other = 0; Other < Index; ++Other)
			{
				if (RangesOverlap(Region.DestinationOffset, Region.Size,
					Regions[Other].DestinationOffset, Regions[Other].Size))
					return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::OverlappingDestinations, static_cast<uint32>(Index), static_cast<uint32>(Other)});
			}
		}
		if (Source == Destination)
		{
			for (const auto& A : Regions)
				for (const auto& B : Regions)
					if (RangesOverlap(A.SourceOffset, A.Size, B.DestinationOffset, B.Size))
						return std::unexpected(FRHIBufferCopyError{ERHIBufferCopyError::AliasedRanges});
		}
		return {};
	}

	static auto ValidateBufferTextureCopies(
		FRHIBuffer* Buffer, FRHITexture* Texture,
		std::span<const FRHIBufferTextureCopyRegion> Regions,
		bool bBufferIsSource) -> std::expected<void, FRHIBufferTextureCopyError>
	{
		if (!Buffer || !Texture) return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::NullResource});
		if (Texture->GetNumSamples() != 1) return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::MultisampledTexture});
		if (bBufferIsSource)
		{
			if (!EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::SourceCopy))
				return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::BufferSourceUsage});
			if (!EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::DestinationCopy))
				return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::TextureDestinationUsage});
		}
		else
		{
			if (!EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
				return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::TextureSourceUsage});
			if (!EnumHasAnyFlags(Buffer->GetUsage(), EBufferUsageFlags::DestinationCopy))
				return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::BufferDestinationUsage});
		}
		std::vector<std::pair<uint64, uint64>> BufferRanges;
		BufferRanges.reserve(Regions.size());
		for (size_t Index = 0; Index < Regions.size(); ++Index)
		{
			const auto& Region = Regions[Index];
			if (auto Validation = ValidateCopyTextureRegion(*Texture, Region.TextureAspect, Region.TextureMip,
				Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers,
				Region.TextureOffset, Region.TextureExtent); !Validation)
			{
				return std::unexpected(FRHIBufferTextureCopyError{Validation.error(), static_cast<uint32>(Index)});
			}
			const auto Footprint = GetBufferTextureFootprint(*Texture, Region);
			if (!Footprint)
			{
				return std::unexpected(FRHIBufferTextureCopyError{Footprint.error(), static_cast<uint32>(Index)});
			}
			if (Region.BufferOffset > Buffer->GetSize() || *Footprint > Buffer->GetSize() - Region.BufferOffset)
				return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::BufferOutOfBounds, static_cast<uint32>(Index)});
			BufferRanges.emplace_back(Region.BufferOffset, *Footprint);
			for (size_t Other = 0; Other < Index; ++Other)
			{
				if (!bBufferIsSource && RangesOverlap(Region.BufferOffset, *Footprint,
					BufferRanges[Other].first, BufferRanges[Other].second))
					return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::OverlappingBufferDestinations, static_cast<uint32>(Index)});
				const auto& Previous = Regions[Other];
				if (bBufferIsSource && Region.TextureAspect == Previous.TextureAspect
					&& Region.TextureMip == Previous.TextureMip
					&& TextureBoxesOverlap(Region.TextureFirstArrayLayer, Region.TextureNumArrayLayers,
						Region.TextureOffset, Region.TextureExtent,
						Previous.TextureFirstArrayLayer, Previous.TextureNumArrayLayers,
						Previous.TextureOffset, Previous.TextureExtent))
					return std::unexpected(FRHIBufferTextureCopyError{ERHIBufferTextureCopyError::OverlappingTextureDestinations, static_cast<uint32>(Index)});
			}
		}
		return {};
	}

	auto ValidateBufferToTextureCopies(FRHIBuffer* Source, FRHITexture* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> std::expected<void, FRHIBufferTextureCopyError>
	{
		return ValidateBufferTextureCopies(Source, Destination, Regions, true);
	}

	auto ValidateTextureToBufferCopies(FRHITexture* Source, FRHIBuffer* Destination,
		std::span<const FRHIBufferTextureCopyRegion> Regions) -> std::expected<void, FRHIBufferTextureCopyError>
	{
		return ValidateBufferTextureCopies(Destination, Source, Regions, false);
	}

	auto ValidateTextureCopies(FRHITexture* Source, FRHITexture* Destination,
		std::span<const FRHITextureCopyRegion> Regions) -> std::expected<void, FRHITextureCopyError>
	{
		std::expected<void, ERHITextureCopyRegionError> Validation;
		if (!Source || !Destination) return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::NullResource});
		if (!EnumHasAnyFlags(Source->GetFlags(), ETextureCreateFlags::SourceCopy | ETextureCreateFlags::CPUReadback))
			return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::SourceUsage});
		if (!EnumHasAnyFlags(Destination->GetFlags(), ETextureCreateFlags::DestinationCopy))
			return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::DestinationUsage});
		if (Source->GetFormat() != Destination->GetFormat())
			return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::FormatMismatch});
		if (Source->GetNumSamples() != 1 || Destination->GetNumSamples() != 1)
			return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::MultisampledTexture});
		for (size_t Index = 0; Index < Regions.size(); ++Index)
		{
			const auto& Region = Regions[Index];
			if (Region.SourceAspect != Region.DestinationAspect)
				return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::AspectMismatch, static_cast<uint32>(Index)});
			if (!(Validation = ValidateCopyTextureRegion(*Source, Region.SourceAspect, Region.SourceMip,
				Region.SourceFirstArrayLayer, Region.NumArrayLayers,
				Region.SourceOffset, Region.Extent))
				|| !(Validation = ValidateCopyTextureRegion(*Destination, Region.DestinationAspect,
					Region.DestinationMip, Region.DestinationFirstArrayLayer,
					Region.NumArrayLayers, Region.DestinationOffset, Region.Extent)))
			{
				return std::unexpected(FRHITextureCopyError{Validation.error(), static_cast<uint32>(Index)});
			}
			for (size_t Other = 0; Other < Index; ++Other)
			{
				const auto& Previous = Regions[Other];
				if (Region.DestinationAspect == Previous.DestinationAspect
					&& Region.DestinationMip == Previous.DestinationMip
					&& TextureBoxesOverlap(Region.DestinationFirstArrayLayer, Region.NumArrayLayers,
						Region.DestinationOffset, Region.Extent,
						Previous.DestinationFirstArrayLayer, Previous.NumArrayLayers,
						Previous.DestinationOffset, Previous.Extent))
					return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::OverlappingDestinations, static_cast<uint32>(Index)});
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
						return std::unexpected(FRHITextureCopyError{ERHITextureCopyError::AliasedRegions});
		}
		return {};
	}

	auto ValidateTextureCreateDesc(const FRHITextureCreateDesc& CreateDesc) -> std::expected<void, ERHITextureCreateError>
	{

		if (CreateDesc.Extent.x <= 0 || CreateDesc.Extent.y <= 0) return std::unexpected(ERHITextureCreateError::EmptyExtent);
		if (CreateDesc.Depth == 0) return std::unexpected(ERHITextureCreateError::EmptyDepth);
		if (CreateDesc.ArraySize == 0) return std::unexpected(ERHITextureCreateError::EmptyArray);
		if (CreateDesc.NumMips == 0) return std::unexpected(ERHITextureCreateError::EmptyMips);
		if (CreateDesc.NumSamples == 0) return std::unexpected(ERHITextureCreateError::EmptySamples);
		if (CreateDesc.Format == EPixelFormat::Unknown) return std::unexpected(ERHITextureCreateError::UnknownFormat);
		if (CreateDesc.NumSamples != 1 && CreateDesc.NumSamples != 2
			&& CreateDesc.NumSamples != 4 && CreateDesc.NumSamples != 8
			&& CreateDesc.NumSamples != 16)
		{
			return std::unexpected(ERHITextureCreateError::InvalidSampleCount);
		}

		switch (CreateDesc.Dimension)
		{
		case ETextureDimension::Texture2D:
			if (CreateDesc.Depth != 1) return std::unexpected(ERHITextureCreateError::Invalid2DDepth);
			if (CreateDesc.ArraySize != 1) return std::unexpected(ERHITextureCreateError::Invalid2DArraySize);
			break;
		case ETextureDimension::Texture2DArray:
			if (CreateDesc.Depth != 1) return std::unexpected(ERHITextureCreateError::InvalidArrayDepth);
			break;
		case ETextureDimension::Texture3D:
			if (CreateDesc.ArraySize != 1) return std::unexpected(ERHITextureCreateError::InvalidVolumeArraySize);
			if (CreateDesc.NumSamples != 1) return std::unexpected(ERHITextureCreateError::MultisampledVolume);
			if (EnumHasAnyFlags(CreateDesc.Flags,
				ETextureCreateFlags::RenderTargetable
				| ETextureCreateFlags::ResolveTargetable
				| ETextureCreateFlags::DepthStencilTargetable
				| ETextureCreateFlags::CPUReadback))
				return std::unexpected(ERHITextureCreateError::UnsupportedVolumeUsage);
			if (GetPixelFormatInfo(CreateDesc.Format).Kind == EPixelFormatKind::DepthStencil)
				return std::unexpected(ERHITextureCreateError::InvalidVolumeFormat);
			break;
		case ETextureDimension::TextureCube:
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return std::unexpected(ERHITextureCreateError::NonSquareCube);
			if (CreateDesc.ArraySize != TextureCubeFaceCount) return std::unexpected(ERHITextureCreateError::InvalidCubeLayers);
			if (CreateDesc.Depth != 1) return std::unexpected(ERHITextureCreateError::InvalidCubeDepth);
			break;
		case ETextureDimension::TextureCubeArray:
			if (CreateDesc.Extent.x != CreateDesc.Extent.y) return std::unexpected(ERHITextureCreateError::NonSquareCubeArray);
			if (CreateDesc.Depth != 1) return std::unexpected(ERHITextureCreateError::InvalidCubeArrayDepth);
			if (CreateDesc.ArraySize % TextureCubeFaceCount != 0) return std::unexpected(ERHITextureCreateError::InvalidCubeArrayLayers);
			break;
		default:
			return std::unexpected(ERHITextureCreateError::InvalidDimension);
		}

		const uint32 MaxDimension = CreateDesc.Dimension == ETextureDimension::Texture3D
			? std::max({static_cast<uint32>(CreateDesc.Extent.x), static_cast<uint32>(CreateDesc.Extent.y), static_cast<uint32>(CreateDesc.Depth)})
			: std::max(static_cast<uint32>(CreateDesc.Extent.x), static_cast<uint32>(CreateDesc.Extent.y));
		uint32 MaximumMipCount = 1;
		for (uint32 Remaining = MaxDimension; Remaining > 1; Remaining >>= 1) ++MaximumMipCount;
		if (CreateDesc.NumMips > MaximumMipCount) return std::unexpected(ERHITextureCreateError::TooManyMips);
		if (CreateDesc.NumSamples != 1 && CreateDesc.NumMips != 1) return std::unexpected(ERHITextureCreateError::MultisampledMips);
		if ((CreateDesc.Dimension == ETextureDimension::Texture3D
			|| CreateDesc.Dimension == ETextureDimension::TextureCube
			|| CreateDesc.Dimension == ETextureDimension::TextureCubeArray)
			&& CreateDesc.NumSamples != 1)
		{
			return std::unexpected(ERHITextureCreateError::MultisampledDimension);
		}

		const bool bDepthStencil = EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::DepthStencilTargetable);
		const bool bColorOrResolve = EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::RenderTargetable | ETextureCreateFlags::ResolveTargetable);
		if (bDepthStencil && (bColorOrResolve || EnumHasAnyFlags(CreateDesc.Flags, ETextureCreateFlags::Storage)))
			return std::unexpected(ERHITextureCreateError::ConflictingDepthUsage);
		if (CreateDesc.NumSamples != 1 && EnumHasAnyFlags(CreateDesc.Flags,
			ETextureCreateFlags::Storage | ETextureCreateFlags::CPUReadback | ETextureCreateFlags::ResolveTargetable))
			return std::unexpected(ERHITextureCreateError::MultisampledUsage);

		const uint64 SubresourceCount = static_cast<uint64>(CreateDesc.NumMips) * CreateDesc.ArraySize;
		if (SubresourceCount > std::numeric_limits<uint32>::max()) return std::unexpected(ERHITextureCreateError::TooManySubresources);
		return {};
	}

	auto ValidateTexture2DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		uint32 ArraySlice,
		const FUpdateTextureRegion2D& UpdateRegion,
		uint32 SourcePitch
	) -> std::expected<void, ERHITextureUploadError>
	{

		if (MipIndex >= TextureDesc.NumMips) return std::unexpected(ERHITextureUploadError::MipOutOfBounds);
		if (ArraySlice >= TextureDesc.ArraySize) return std::unexpected(ERHITextureUploadError::LayerOutOfBounds);
		if (UpdateRegion.SrcX < 0 || UpdateRegion.SrcY < 0) return std::unexpected(ERHITextureUploadError::NegativeOffset);
		if (UpdateRegion.Width == 0 || UpdateRegion.Height == 0) return std::unexpected(ERHITextureUploadError::EmptyExtent);

		const uint32 MipWidth = std::max(1u, static_cast<uint32>(TextureDesc.Extent.x) >> MipIndex);
		const uint32 MipHeight = std::max(1u, static_cast<uint32>(TextureDesc.Extent.y) >> MipIndex);
		if (static_cast<uint64>(UpdateRegion.DestX) + UpdateRegion.Width > MipWidth
			|| static_cast<uint64>(UpdateRegion.DestY) + UpdateRegion.Height > MipHeight)
		{
			return std::unexpected(ERHITextureUploadError::BoxOutOfBounds);
		}

		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(TextureDesc.Format);
		if (FormatInfo.BytesPerBlock == 0 || FormatInfo.BlockSize == 0)
		{
			return std::unexpected(ERHITextureUploadError::InvalidBlockLayout);
		}

		const uint32 BlockSize = FormatInfo.BlockSize;
		if (UpdateRegion.DestX % BlockSize != 0 || UpdateRegion.DestY % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcX) % BlockSize != 0
			|| static_cast<uint32>(UpdateRegion.SrcY) % BlockSize != 0)
		{
			return std::unexpected(ERHITextureUploadError::OffsetAlignment);
		}
		if ((UpdateRegion.Width % BlockSize != 0 && UpdateRegion.DestX + UpdateRegion.Width != MipWidth)
			|| (UpdateRegion.Height % BlockSize != 0 && UpdateRegion.DestY + UpdateRegion.Height != MipHeight))
		{
			return std::unexpected(ERHITextureUploadError::ExtentAlignment);
		}

		const uint64 SourceBlockX = static_cast<uint32>(UpdateRegion.SrcX) / BlockSize;
		const uint64 RegionBlocksWide = (static_cast<uint64>(UpdateRegion.Width) + BlockSize - 1) / BlockSize;
		const uint64 RequiredPitch = (SourceBlockX + RegionBlocksWide) * FormatInfo.BytesPerBlock;
		if (RequiredPitch > SourcePitch) return std::unexpected(ERHITextureUploadError::InsufficientPitch);
		return {};
	}

	auto ValidateTexture3DUpdate(
		const FRHITextureDesc& TextureDesc,
		uint32 MipIndex,
		const FUpdateTextureRegion3D& UpdateRegion,
		uint32 SourceRowPitch,
		uint32 SourceDepthPitch) -> std::expected<void, ERHIVolumeUploadError>
	{
		if (TextureDesc.Dimension != ETextureDimension::Texture3D)
			return std::unexpected(ERHIVolumeUploadError::InvalidDimension);
		if (MipIndex >= TextureDesc.NumMips)
			return std::unexpected(ERHIVolumeUploadError::MipOutOfBounds);
		if (UpdateRegion.SrcX < 0 || UpdateRegion.SrcY < 0 || UpdateRegion.SrcZ < 0)
			return std::unexpected(ERHIVolumeUploadError::NegativeOffset);
		if (UpdateRegion.Width == 0 || UpdateRegion.Height == 0 || UpdateRegion.Depth == 0)
			return std::unexpected(ERHIVolumeUploadError::EmptyExtent);
		const uint32 MipWidth = std::max(1u, static_cast<uint32>(TextureDesc.Extent.x) >> MipIndex);
		const uint32 MipHeight = std::max(1u, static_cast<uint32>(TextureDesc.Extent.y) >> MipIndex);
		const uint32 MipDepth = std::max(1u, static_cast<uint32>(TextureDesc.Depth) >> MipIndex);
		if (static_cast<uint64>(UpdateRegion.DestX) + UpdateRegion.Width > MipWidth
			|| static_cast<uint64>(UpdateRegion.DestY) + UpdateRegion.Height > MipHeight
			|| static_cast<uint64>(UpdateRegion.DestZ) + UpdateRegion.Depth > MipDepth)
			return std::unexpected(ERHIVolumeUploadError::BoxOutOfBounds);

		const FPixelFormatInfo& FormatInfo = GetPixelFormatInfo(TextureDesc.Format);
		if (FormatInfo.BytesPerBlock == 0 || FormatInfo.BlockSize == 0)
			return std::unexpected(ERHIVolumeUploadError::InvalidBlockLayout);
		const uint32 BlockSize = FormatInfo.BlockSize;
		if ((UpdateRegion.DestX % BlockSize) != 0 || (UpdateRegion.DestY % BlockSize) != 0
			|| (static_cast<uint32>(UpdateRegion.SrcX) % BlockSize) != 0
			|| (static_cast<uint32>(UpdateRegion.SrcY) % BlockSize) != 0)
			return std::unexpected(ERHIVolumeUploadError::OffsetAlignment);
		if ((UpdateRegion.Width % BlockSize != 0 && UpdateRegion.DestX + UpdateRegion.Width != MipWidth)
			|| (UpdateRegion.Height % BlockSize != 0 && UpdateRegion.DestY + UpdateRegion.Height != MipHeight))
			return std::unexpected(ERHIVolumeUploadError::ExtentAlignment);
		const uint64 SourceBlockX = static_cast<uint32>(UpdateRegion.SrcX) / BlockSize;
		const uint64 SourceBlockY = static_cast<uint32>(UpdateRegion.SrcY) / BlockSize;
		const uint64 RegionBlocksWide = (static_cast<uint64>(UpdateRegion.Width) + BlockSize - 1) / BlockSize;
		const uint64 RegionBlockRows = (static_cast<uint64>(UpdateRegion.Height) + BlockSize - 1) / BlockSize;
		const uint64 RequiredRowPitch = (SourceBlockX + RegionBlocksWide) * FormatInfo.BytesPerBlock;
		if (RequiredRowPitch > SourceRowPitch)
			return std::unexpected(ERHIVolumeUploadError::InsufficientRowPitch);
		if (SourceBlockY > std::numeric_limits<uint64>::max() / SourceRowPitch)
			return std::unexpected(ERHIVolumeUploadError::RowOffsetOverflow);
		const uint64 RequiredDepthPitch = (SourceBlockY + RegionBlockRows) * SourceRowPitch;
		if (RequiredDepthPitch > SourceDepthPitch)
			return std::unexpected(ERHIVolumeUploadError::InsufficientDepthPitch);
		const uint64 SourceZ = static_cast<uint32>(UpdateRegion.SrcZ);
		if (SourceZ > std::numeric_limits<uint64>::max() / SourceDepthPitch
			|| UpdateRegion.Depth - 1 > (std::numeric_limits<uint64>::max()
				- SourceZ * SourceDepthPitch) / SourceDepthPitch)
			return std::unexpected(ERHIVolumeUploadError::FootprintOverflow);
		return {};
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
	FRHIResource* PendingDeleteHead = nullptr;
	FRHIResource* PendingDeleteTail = nullptr;
	size_t PendingDeleteCount = 0;
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
		auto* Resource = const_cast<FRHIResource*>(this);
		if (PendingDeleteTail) PendingDeleteTail->NextPendingDelete = Resource;
		else PendingDeleteHead = Resource;
		PendingDeleteTail = Resource;
		++PendingDeleteCount;
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
		std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
		// Allocate on the deletion owner before transferring any queue ownership.
		// If reserve fails, the intact intrusive queue remains available for retry.
		if (PendingDeleteCount > OutResourcesToDelete.max_size() - OutResourcesToDelete.size())
			throw std::length_error("RHI deferred deletion batch exceeds vector capacity.");
		OutResourcesToDelete.reserve(OutResourcesToDelete.size() + PendingDeleteCount);
		while (PendingDeleteHead)
		{
			auto* Resource = PendingDeleteHead;
			PendingDeleteHead = Resource->NextPendingDelete;
			Resource->NextPendingDelete = nullptr;
			OutResourcesToDelete.push_back(Resource);
		}
		PendingDeleteTail = nullptr;
		PendingDeleteCount = 0;
	}

	auto FRHIResource::GetNumPendingDeletes() -> size_t
	{
		std::lock_guard<std::mutex> Lock(PendingDeletesMutex);
		return PendingDeleteCount;
	}

} // namespace Durin
