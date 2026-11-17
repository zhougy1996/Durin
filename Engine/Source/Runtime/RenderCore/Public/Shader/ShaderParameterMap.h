#pragma once

#include "CoreMinimal.h"
#include "RenderCoreAPI.h"
#include "RHIResources.h"
#include "RHIShaderParameters.h"
#include "Shader/ShaderDiagnostics.h"

namespace Durin
{
	class FShaderType;
	struct FRDGParameterLayout;
	// One compiler-assigned resource location. No C++ member offsets belong here.
	struct FParameterAllocation
	{
		uint32 SetIndex = 0;
		uint32 BaseIndex = 0;
		uint32 Size = 1;
		ERHIBindingType Type = ERHIBindingType::UniformBuffer;
        EShaderResourceShape ResourceShape = EShaderResourceShape::Unknown;
        uint32 ConstantBufferSize = 0;
        std::vector<FShaderConstantMember> ConstantMembers;
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
		RENDERCORE_API FShaderParameterBindings();
		RENDERCORE_API auto GetRDGLayoutBindings(const FRDGParameterLayout& Layout) const
			-> std::expected<std::shared_ptr<const std::vector<uint32>>, std::string>;
		// Registered shader types own their immutable semantic schema. Admission and
		// host-storage binding share this entry point for authored and cooked products.
		RENDERCORE_API auto Bind(const FShaderType& Type, const FShaderReflectionData& Reflection)
			-> FShaderOperationResult;
		// Failure clears prior bindings; successful records own their emitted names.
		RENDERCORE_API auto Bind(const FShaderParametersMetadata* Metadata, const FShaderParameterMap& ParameterMap)
			-> FShaderOperationResult;
		auto GetResources() const -> std::span<const FShaderParameterBinding> { return Resources; }
	private:
		auto Reset() -> void;
		struct FRDGLayoutCache;
		std::shared_ptr<FRDGLayoutCache> RDGLayoutCache;
		std::vector<FShaderParameterBinding> Resources;
	};
}
