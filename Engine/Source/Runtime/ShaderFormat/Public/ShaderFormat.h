#pragma once

#include "CoreMinimal.h"
#include "RHIResources.h"
#include "ShaderFormatAPI.h"

namespace Durin::ShaderFormat
{
	inline constexpr uint32 ShaderMetadataVersion = 1;
	inline constexpr uint64 MaximumMetadataBytes = 4096;
	SHADERFORMAT_API auto ValidateExecutionMetadata(const FShaderTargetIdentity& Target,
		EShaderFrequency Frequency, const std::array<uint32, 3>& GroupSize) -> bool;
	SHADERFORMAT_API auto ComputeShaderIdentity(const FShaderCode& Shader) -> FXxHash128;
	SHADERFORMAT_API auto ValidateShaderCode(const FShaderCode& Shader) -> bool;
	SHADERFORMAT_API auto ValidateCode(const FShaderTargetIdentity& Target,
		EShaderCodeFormat Format, FByteView Code) -> bool;
	// Validates raw code inputs too. An attached code owner is authoritative and must
	// agree with the generic creation arguments and any explicitly supplied identity.
	SHADERFORMAT_API auto ValidateShaderCreateDesc(const FRHIShaderCreateDesc& Desc) -> bool;

	// Flattened existing logical layout values; no compiler reflection or native SDK types.
	struct FLogicalBinding
	{
		uint32 SetIndex = 0;
		FBindingLayoutItem Declaration;
	};

	SHADERFORMAT_API auto BuildLogicalBindings(const FShaderReflectionData& Reflection) -> std::vector<FLogicalBinding>;
	// Shared bytecode digest check; callers retain their own error classification.
	SHADERFORMAT_API auto ValidateCodeHash(FByteView Code, FXxHash128 Hash) -> bool;

	// Checks code shape and target metadata against the stage's logical declarations.
	// Hash checks at the enclosing container remain independent of semantic admission.
	SHADERFORMAT_API auto ValidateShaderCodeParts(const FShaderTargetIdentity& Target,
		EShaderCodeFormat Format, uint32 Version, EShaderFrequency Frequency,
		FByteView Code, std::string_view EntryPoint, FByteView Metadata,
		std::span<const FLogicalBinding> Bindings, std::span<const FPushConstantRange> PushRanges) -> bool;
}
