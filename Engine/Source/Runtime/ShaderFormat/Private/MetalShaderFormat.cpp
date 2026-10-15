#include "MetalShaderFormat.h"

#include "Hash/CanonicalHash.h"

namespace Durin
{
	auto ComputeMetalBindingRemapIdentity(EShaderFrequency Frequency,
		std::span<const FMetalShaderBinding> Bindings, uint32 PushConstantBufferSlot)
		-> FXxHash128
	{
		FXxHash128Builder Identity;
		UpdateCanonicalHashString(Identity, "Durin.Metal.BindingRemap.v1");
		UpdateCanonicalHash(Identity, uint32(Frequency));
		UpdateCanonicalHash(Identity, uint64(Bindings.size()));
		for (const auto& Binding : Bindings)
		{
			UpdateCanonicalHash(Identity, Binding.SetIndex);
			UpdateCanonicalHash(Identity, Binding.BindingIndex);
			UpdateCanonicalHash(Identity, uint32(Binding.Type));
			UpdateCanonicalHash(Identity, Binding.Slot);
			UpdateCanonicalHash(Identity, Binding.Count);
		}
		UpdateCanonicalHash(Identity, PushConstantBufferSlot);
		return Identity.Finalize();
	}

	auto ValidateMetalBindingRemap(EShaderFrequency Frequency,
		std::span<const FMetalShaderBinding> Bindings, uint32 PushConstantBufferSlot,
		FXxHash128 Identity) -> bool
	{
		if (Frequency != EShaderFrequency::Vertex
			&& Frequency != EShaderFrequency::Fragment
			&& Frequency != EShaderFrequency::Compute) return false;
		uint32 NextBuffer = Frequency == EShaderFrequency::Vertex ? ShaderFormat::FirstMetalVertexResourceBuffer : 0;
		uint32 NextTexture = 0;
		uint32 NextSampler = 0;
		uint32 PreviousSet = 0;
		uint32 PreviousBinding = 0;
		bool bFirst = true;
		for (const auto& Binding : Bindings)
		{
			if (!bFirst && std::tie(Binding.SetIndex, Binding.BindingIndex)
					<= std::tie(PreviousSet, PreviousBinding)) return false;
			bFirst = false;
			PreviousSet = Binding.SetIndex;
			PreviousBinding = Binding.BindingIndex;
			if (Binding.SetIndex > 65535 || Binding.BindingIndex > 65535
				|| Binding.Count == 0) return false;
			uint32* Next = nullptr;
			uint32 Limit = 0;
			switch (Binding.Type)
			{
			case ERHIBindingType::UniformBuffer:
			case ERHIBindingType::UniformBufferDynamic:
			case ERHIBindingType::StorageBuffer:
				Next = &NextBuffer; Limit = ShaderFormat::MaximumMetalBuffers; break;
			case ERHIBindingType::Texture:
			case ERHIBindingType::StorageImage:
				Next = &NextTexture; Limit = ShaderFormat::MaximumMetalTextures; break;
			case ERHIBindingType::Sampler:
				Next = &NextSampler; Limit = ShaderFormat::MaximumMetalSamplers; break;
			default: return false;
			}
			if (*Next > Limit || Binding.Slot != *Next
				|| Binding.Count > Limit - *Next)
				return false;
			*Next += Binding.Count;
		}
		if (PushConstantBufferSlot != UINT32_MAX
			&& (PushConstantBufferSlot != NextBuffer || NextBuffer >= ShaderFormat::MaximumMetalBuffers))
			return false;
		return Identity == ComputeMetalBindingRemapIdentity(
			Frequency, Bindings, PushConstantBufferSlot);
	}

}

namespace Durin::ShaderFormat
{
	auto GetMetalFormatWorkCounters() -> FMetalFormatWorkCounters&
	{
		thread_local FMetalFormatWorkCounters Counters;
		return Counters;
	}
	auto ResolveMetalCompileSettings(const FShaderTargetIdentity& Target) -> std::optional<FMetalCompileSettings>
	{
		if (Target != MetalShaderTarget) return {};
		return FMetalCompileSettings{};
	}

	auto ValidateMetalCode(FByteView Code) -> bool
	{
		// Source formatting and language validity belong to the native compiler.
		// Headerless kernels and comments preceding includes are valid MSL inputs.
		return !Code.empty() && Code.size() <= 64ull * 1024 * 1024
			&& std::ranges::find(Code, std::byte{0}) == Code.end();
	}


	auto BuildMetalBindingMap(EShaderFrequency Frequency,
		std::span<const FLogicalBinding> Bindings, bool bHasPushConstants)
		-> std::optional<FMetalBindingMap>
	{
		++GetMetalFormatWorkCounters().MapBuilds;
		if (Frequency != EShaderFrequency::Vertex
			&& Frequency != EShaderFrequency::Fragment
			&& Frequency != EShaderFrequency::Compute)
			return std::nullopt;
		std::vector<FLogicalBinding> Reflected(Bindings.begin(), Bindings.end());
		std::ranges::sort(Reflected, [](const auto& Left, const auto& Right)
		{
			return std::tie(Left.SetIndex, Left.Declaration.Slot, Left.Declaration.Type)
				< std::tie(Right.SetIndex, Right.Declaration.Slot, Right.Declaration.Type);
		});
		FMetalBindingMap Result;
		Result.Bindings.reserve(Reflected.size());
		uint32 NextBuffer = Frequency == EShaderFrequency::Vertex
			? FirstMetalVertexResourceBuffer : 0;
		uint32 NextTexture = 0;
		uint32 NextSampler = 0;
		for (const auto& Binding : Reflected)
		{
			if (Binding.Declaration.ArraySize == 0 || Binding.SetIndex > 65535
				|| Binding.Declaration.Slot > 65535)
				return std::nullopt;
			if (!Result.Bindings.empty())
			{
				const auto& Previous = Result.Bindings.back();
				if (Previous.SetIndex == Binding.SetIndex
					&& Previous.BindingIndex == Binding.Declaration.Slot)
					return std::nullopt;
			}
			uint32* Next = nullptr;
			uint32 Limit = 0;
			switch (Binding.Declaration.Type)
			{
			case ERHIBindingType::UniformBuffer:
			case ERHIBindingType::UniformBufferDynamic:
			case ERHIBindingType::StorageBuffer:
				Next = &NextBuffer; Limit = MaximumMetalBuffers; break;
			case ERHIBindingType::Texture:
			case ERHIBindingType::StorageImage:
				Next = &NextTexture; Limit = MaximumMetalTextures; break;
			case ERHIBindingType::Sampler:
				Next = &NextSampler; Limit = MaximumMetalSamplers; break;
			default:
				return std::nullopt;
			}
			if (*Next > Limit || Binding.Declaration.ArraySize > Limit - *Next)
				return std::nullopt;
			Result.Bindings.push_back({Binding.SetIndex, Binding.Declaration.Slot,
				Binding.Declaration.Type == ERHIBindingType::UniformBufferDynamic
					? ERHIBindingType::UniformBuffer : Binding.Declaration.Type,
				*Next, Binding.Declaration.ArraySize});
			*Next += Binding.Declaration.ArraySize;
		}
		if (bHasPushConstants)
		{
			if (NextBuffer >= MaximumMetalBuffers)
				return std::nullopt;
			Result.PushConstantBufferSlot = NextBuffer;
		}
		Result.Identity = ComputeMetalBindingRemapIdentity(Frequency,
			Result.Bindings, Result.PushConstantBufferSlot);
		return Result;
	}
}
