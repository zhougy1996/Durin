#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"
#include "RHIResources.h"
#include "RHIShaderParameters.h"
#include "Shader/ShaderDiagnostics.h"

namespace Durin
{
	// One compiler-assigned resource location. No C++ member offsets belong here.
	struct FParameterAllocation
	{
		uint32 SetIndex = 0;
		uint32 BaseIndex = 0;
		uint32 Size = 1;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
		auto operator==(const FParameterAllocation&) const -> bool = default;
	};

	// Name-to-allocation compiler output, independent of parameter structure storage.
	class FShaderParameterMap
	{
	public:
		using FAllocationTable = std::map<std::string, FParameterAllocation, std::less<>>;
		RENDERCORE_API auto AddParameterAllocation(std::string_view Name, const FParameterAllocation& Allocation)
			-> FShaderOperationResult;
		RENDERCORE_API auto FindParameterAllocation(std::string_view Name) const -> const FParameterAllocation*;
		auto ContainsParameterAllocation(std::string_view Name) const -> bool { return FindParameterAllocation(Name) != nullptr; }
		auto GetParameterAllocations() const -> const FAllocationTable& { return ParameterMap; }
		auto operator==(const FShaderParameterMap&) const -> bool = default;
		RENDERCORE_API static auto FromReflection(const FShaderReflectionData& Reflection)
			-> std::expected<FShaderParameterMap, std::string>;
	private:
		FAllocationTable ParameterMap;
	};

	// Compiled bridge between a C++ parameter structure and compiler allocations.
	class FShaderParameterBindings
	{
	public:
		// Failure clears prior bindings; successful records own their emitted names.
		RENDERCORE_API auto Bind(const FShaderParametersMetadata* Metadata, const FShaderParameterMap& ParameterMap)
			-> FShaderOperationResult;
		auto GetResources() const -> std::span<const FShaderParameterBinding> { return Resources; }
	private:
		std::vector<FShaderParameterBinding> Resources;
	};
}
