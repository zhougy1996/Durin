#pragma once

#include "CoreMinimal.h"
#include "MetalShaderFormat.h"

namespace Durin
{
	// Test authoring state is finalized into owned generic bytes at each admission call.
	// Keeping it separate permits malformed-map tests without adding native fields to RHI.
	struct FMetalShaderFixture : FRHIShaderCreateDesc
	{
		explicit FMetalShaderFixture(FRHIShaderCreateDesc Desc) : FRHIShaderCreateDesc(std::move(Desc)) {}
		static auto Create(const char* Name, EShaderFrequency Stage, FByteView Code, FXxHash128 Hash)
			-> FMetalShaderFixture { return FMetalShaderFixture(FRHIShaderCreateDesc::Create(Name, Stage, Code, Hash)); }
		std::vector<FMetalShaderBinding> Bindings;
		uint32 PushSlot = UINT32_MAX;
		FXxHash128 RemapIdentity{};
	};

	inline auto FinalizeMetalFixture(const FRHIShaderCreateDesc& Desc) -> FRHIShaderCreateDesc { return Desc; }
	inline auto FinalizeMetalFixture(const FMetalShaderFixture& Fixture) -> FRHIShaderCreateDesc
	{
		FRHIShaderCreateDesc Desc = Fixture;
		FShaderCode Value;
		Value.Target = Desc.Target; Value.CodeFormat = Desc.CodeFormat;
		Value.Frequency = Desc.Frequency; Value.ComputeThreadGroupSize = Desc.ComputeThreadGroupSize;
		Value.BinaryEntryPoint = Desc.EntryPoint ? Desc.EntryPoint : "";
		Value.DebugName = Desc.DebugName ? Desc.DebugName : "";
		Value.Code = FSharedByteBuffer::Copy(Desc.Code);
		Value.CodeHash = Desc.Hash;
		const auto Bytes = ShaderFormat::EncodeMetalMetadata(Desc.Frequency,
			{Fixture.Bindings, Fixture.PushSlot, Fixture.RemapIdentity});
		if (Bytes) Value.PlatformData = *Bytes;
		const auto Stage = Desc.Frequency == EShaderFrequency::Vertex ? EShaderStageFlags::Vertex
			: Desc.Frequency == EShaderFrequency::Fragment ? EShaderStageFlags::Fragment : EShaderStageFlags::Compute;
		for (const auto& Binding : Fixture.Bindings)
		{
			if (Binding.SetIndex > 65535) continue;
			Value.Reflection.ResourceBindings.push_back({"", Stage, Binding.SetIndex, Binding.BindingIndex, Binding.Type, Binding.Count});
		}
		if (Fixture.PushSlot != UINT32_MAX) Value.Reflection.PushConstantRanges.push_back({Stage, 0, 4});
		Value.ShaderIdentity = ShaderFormat::ComputeShaderIdentity(Value);
		Desc.CodeOwner = std::make_shared<const FShaderCode>(std::move(Value));
		Desc.ShaderIdentity = Desc.CodeOwner->ShaderIdentity;
		Desc.Code = Desc.CodeOwner->Code.GetBytes();
		Desc.EntryPoint = Desc.CodeOwner->BinaryEntryPoint.c_str();
		Desc.DebugName = Desc.CodeOwner->DebugName.c_str();
		return Desc;
	}
}
