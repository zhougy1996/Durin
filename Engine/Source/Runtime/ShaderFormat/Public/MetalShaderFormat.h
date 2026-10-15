#pragma once

#include "CoreMinimal.h"
#include "ShaderFormat.h"

namespace Durin
{
	struct FMetalShaderBinding
	{
		uint32 SetIndex = 0;
		uint32 BindingIndex = 0;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		uint32 Slot = 0;
		uint32 Count = 1;

		auto operator==(const FMetalShaderBinding&) const -> bool = default;
	};
	SHADERFORMAT_API auto ComputeMetalBindingRemapIdentity(EShaderFrequency Frequency,
		std::span<const FMetalShaderBinding> Bindings, uint32 PushConstantBufferSlot)
		-> FXxHash128;
	SHADERFORMAT_API auto ValidateMetalBindingRemap(EShaderFrequency Frequency,
		std::span<const FMetalShaderBinding> Bindings, uint32 PushConstantBufferSlot,
		FXxHash128 Identity) -> bool;

}

namespace Durin::ShaderFormat
{
	inline constexpr uint32 MaximumMetalBuffers = 31;
	inline constexpr uint32 MaximumMetalTextures = 128;
	inline constexpr uint32 MaximumMetalSamplers = 16;
	inline constexpr uint32 MaximumMetalBindings = MaximumMetalBuffers + MaximumMetalTextures + MaximumMetalSamplers;
	inline constexpr uint32 FirstMetalVertexResourceBuffer = 16;

	// Per-thread qualification counters; snapshot on the command replay thread.
	struct FMetalFormatWorkCounters
	{
		uint64 MapBuilds = 0;
		uint64 MetadataDecodes = 0;
	};
	SHADERFORMAT_API auto GetMetalFormatWorkCounters() -> FMetalFormatWorkCounters&;

	// Settings profile 2.1 is immutable; new output-affecting settings require a new digest.
	struct FMetalCompileSettings
	{
		uint32 MslLanguageVersion = 200;
		uint32 BindingRemapSchema = 1;
	};
	SHADERFORMAT_API auto ResolveMetalCompileSettings(const FShaderTargetIdentity& Target)
		-> std::optional<FMetalCompileSettings>;
	SHADERFORMAT_API auto ValidateMetalCode(FByteView Code) -> bool;

	// Decoded schema-1 compiler-agreed slots. Native function/device checks belong to MetalRHI.
	struct FMetalBindingMap
	{
		std::vector<FMetalShaderBinding> Bindings;
		uint32 PushConstantBufferSlot = UINT32_MAX;
		FXxHash128 Identity{};
	};

	SHADERFORMAT_API auto BuildMetalBindingMap(EShaderFrequency Frequency,
		std::span<const FLogicalBinding> Bindings, bool bHasPushConstants)
		-> std::optional<FMetalBindingMap>;
	SHADERFORMAT_API auto EncodeMetalMetadata(EShaderFrequency Frequency,
		const FMetalBindingMap& Map) -> std::optional<FSharedByteBuffer>;
	SHADERFORMAT_API auto DecodeMetalMetadata(EShaderFrequency Frequency,
		FByteView Bytes) -> std::optional<FMetalBindingMap>;
}
