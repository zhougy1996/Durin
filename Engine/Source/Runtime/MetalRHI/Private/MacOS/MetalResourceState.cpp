#include "MetalResourceState.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"

namespace Durin
{
	auto ApplyMetalBufferTransitions(std::span<const FRHIBufferTransition> Transitions)
		-> std::optional<std::string>
	{
		if (!ValidateBufferTransitions(Transitions)) return "Invalid Metal buffer transition batch.";
		for (const auto& Transition : Transitions)
		{
			auto* Buffer = dynamic_cast<FMetalBuffer*>(Transition.Buffer);
			if (!Buffer) return "Metal buffer transition requires a native Metal buffer.";
			ERHIAccess Tracked = ERHIAccess::None;
			if (!Buffer->GetStateTracker().Validate(Transition.Offset, Transition.Size, Transition.ExpectedBefore, Tracked))
				return std::format("Metal buffer transition state mismatch: resource={}, offset={}, size={}, expected={}, tracked={}, requested={}.",
					static_cast<const void*>(Buffer), Transition.Offset, Transition.Size,
					static_cast<uint32>(Transition.ExpectedBefore), static_cast<uint32>(Tracked), static_cast<uint32>(Transition.RequiredAfter));
		}
		for (const auto& Transition : Transitions)
			static_cast<FMetalBuffer*>(Transition.Buffer)->GetStateTracker().Apply(Transition.Offset, Transition.Size, Transition.RequiredAfter);
		return {};
	}

	auto ApplyMetalTextureTransitions(std::span<const FRHITextureTransition> Transitions)
		-> std::optional<std::string>
	{
		if (!ValidateTextureTransitions(Transitions)) return "Invalid Metal texture transition batch.";
		for (const auto& Transition : Transitions)
		{
			auto* Texture = dynamic_cast<FMetalTexture*>(Transition.Texture);
			if (!Texture) return "Metal texture transition requires a native Metal texture.";
			ERHIAccess Tracked = ERHIAccess::None;
			if (!Texture->ValidateAccess(Transition.Range, Transition.ExpectedBefore, Tracked))
				return std::format("Metal texture transition state mismatch: resource={}, aspects={}, mip={}+{}, layer={}+{}, expected={}, tracked={}, requested={}.",
					static_cast<const void*>(Texture), static_cast<uint32>(Transition.Range.Aspects),
					Transition.Range.FirstMip, Transition.Range.NumMips, Transition.Range.FirstArrayLayer, Transition.Range.NumArrayLayers,
					static_cast<uint32>(Transition.ExpectedBefore), static_cast<uint32>(Tracked), static_cast<uint32>(Transition.RequiredAfter));
		}
		for (const auto& Transition : Transitions)
			static_cast<FMetalTexture*>(Transition.Texture)->ApplyAccess(Transition.Range, Transition.RequiredAfter);
		return {};
	}

	auto GetMetalCanonicalBufferAccess(EBufferUsageFlags Usage) -> ERHIAccess
	{
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::UnorderedAccess)) return ERHIAccess::GraphicsShaderReadWrite;
		ERHIAccess Access = ERHIAccess::None;
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::VertexBuffer)) Access |= ERHIAccess::VertexBufferRead;
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::IndexBuffer)) Access |= ERHIAccess::IndexBufferRead;
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::UniformBuffer)) Access |= ERHIAccess::GraphicsUniformRead;
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::ShaderResource | EBufferUsageFlags::StructuredBuffer | EBufferUsageFlags::ByteAddressBuffer)) Access |= ERHIAccess::GraphicsShaderRead;
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::SourceCopy)) Access |= ERHIAccess::TransferRead;
		if (EnumHasAnyFlags(Usage, EBufferUsageFlags::DrawIndirect)) Access |= ERHIAccess::IndirectArgumentRead;
		return Access;
	}
}
