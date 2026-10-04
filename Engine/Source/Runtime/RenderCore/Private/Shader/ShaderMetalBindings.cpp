#include "Shader/ShaderCompilerCore.h"

#include "Hash/CanonicalHash.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 GMaximumBuffers = 31;
		constexpr uint32 GMaximumTextures = 128;
		constexpr uint32 GMaximumSamplers = 16;
		constexpr uint32 GFirstVertexResourceBuffer = 16;
	}

	auto BuildMetalShaderBindingMap(EShaderFrequency Frequency,
		const FShaderReflectionData& Reflection)
		-> std::expected<FMetalShaderBindingMap, FShaderError>
	{
		if (Frequency != EShaderFrequency::Vertex
			&& Frequency != EShaderFrequency::Fragment
			&& Frequency != EShaderFrequency::Compute)
			return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
		std::vector<FShaderResourceBinding> Reflected = Reflection.ResourceBindings;
		std::ranges::sort(Reflected, [](const auto& Left, const auto& Right)
		{
			return std::tie(Left.SetIndex, Left.BindingIndex, Left.Type)
				< std::tie(Right.SetIndex, Right.BindingIndex, Right.Type);
		});
		FMetalShaderBindingMap Result;
		Result.Bindings.reserve(Reflected.size());
		uint32 NextBuffer = Frequency == EShaderFrequency::Vertex
			? GFirstVertexResourceBuffer : 0;
		uint32 NextTexture = 0;
		uint32 NextSampler = 0;
		for (const auto& Binding : Reflected)
		{
			if (Binding.ArraySize == 0 || Binding.SetIndex > 65535
				|| Binding.BindingIndex > 65535)
				return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
			if (!Result.Bindings.empty())
			{
				const auto& Previous = Result.Bindings.back();
				if (Previous.SetIndex == Binding.SetIndex
					&& Previous.BindingIndex == Binding.BindingIndex)
					return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
			}
			uint32* Next = nullptr;
			uint32 Limit = 0;
			switch (Binding.Type)
			{
			case ERHIBindingType::UniformBuffer:
			case ERHIBindingType::UniformBufferDynamic:
			case ERHIBindingType::StorageBuffer:
				Next = &NextBuffer; Limit = GMaximumBuffers; break;
			case ERHIBindingType::Texture:
			case ERHIBindingType::StorageImage:
				Next = &NextTexture; Limit = GMaximumTextures; break;
			case ERHIBindingType::Sampler:
				Next = &NextSampler; Limit = GMaximumSamplers; break;
			default:
				return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
			}
			if (*Next > Limit || Binding.ArraySize > Limit - *Next)
				return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
			Result.Bindings.push_back({Binding.SetIndex, Binding.BindingIndex,
				Binding.Type, *Next, Binding.ArraySize});
			*Next += Binding.ArraySize;
		}
		if (!Reflection.PushConstantRanges.empty())
		{
			if (NextBuffer >= GMaximumBuffers)
				return std::unexpected(FShaderError{.Code = EShaderError::MetalBindingRemapInvalid});
			Result.PushConstantBufferSlot = NextBuffer;
		}
		FXxHash128Builder Identity;
		UpdateCanonicalHashString(Identity, "Durin.Metal.BindingRemap.v1");
		UpdateCanonicalHash(Identity, uint32(Frequency));
		UpdateCanonicalHash(Identity, uint64(Result.Bindings.size()));
		for (const auto& Binding : Result.Bindings)
		{
			UpdateCanonicalHash(Identity, Binding.SetIndex);
			UpdateCanonicalHash(Identity, Binding.BindingIndex);
			UpdateCanonicalHash(Identity, uint32(Binding.Type));
			UpdateCanonicalHash(Identity, Binding.Slot);
			UpdateCanonicalHash(Identity, Binding.Count);
		}
		UpdateCanonicalHash(Identity, Result.PushConstantBufferSlot);
		Result.Identity = Identity.Finalize();
		return Result;
	}
}
