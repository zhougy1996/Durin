#include "Shader/ShaderParameterMap.h"

namespace Durin
{
	auto FShaderParameterMap::AddParameterAllocation(std::string_view Name, const FParameterAllocation& Allocation)
		-> FShaderOperationResult
	{
		if (Name.empty() || !Allocation.Size || ParameterMap.contains(Name)
			|| static_cast<uint8>(Allocation.Type) > static_cast<uint8>(ERHIBindingType::StorageImage))
			return std::unexpected("Invalid or duplicate shader parameter allocation");
		if (std::ranges::any_of(ParameterMap, [&](const auto& Pair) {
			return Pair.second.SetIndex == Allocation.SetIndex && Pair.second.BaseIndex == Allocation.BaseIndex;
		})) return std::unexpected("Overlapping shader parameter resource allocations");
		ParameterMap.emplace(std::string(Name), Allocation);
		return {};
	}

	auto FShaderParameterMap::FindParameterAllocation(std::string_view Name) const -> const FParameterAllocation*
	{
		const auto Found = ParameterMap.find(Name);
		return Found == ParameterMap.end() ? nullptr : &Found->second;
	}

	auto FShaderParameterMap::FromReflection(const FShaderReflectionData& Reflection)
		-> std::expected<FShaderParameterMap, std::string>
	{
		FShaderParameterMap Map;
		for (const auto& Resource : Reflection.ResourceBindings)
		{
			auto Result = Map.AddParameterAllocation(Resource.Name,
				{Resource.SetIndex, Resource.BindingIndex, Resource.ArraySize, Resource.Type});
			if (!Result) return std::unexpected(Result.error());
		}
		return Map;
	}

	auto FShaderParameterBindings::Bind(const FShaderParametersMetadata* Metadata, const FShaderParameterMap& ParameterMap)
		-> FShaderOperationResult
	{
		Resources.clear();
		if (!Metadata) return {};
		std::vector<FShaderParameterBinding> Candidate;
		std::vector<const FShaderParametersMetadata*> Stack;
		std::set<std::string> Names;
		auto Visit = [&](auto&& Self, const FShaderParametersMetadata& Struct, uint32 Offset,
			std::string_view Prefix) -> FShaderOperationResult {
			if (Stack.size() >= 64 || std::ranges::contains(Stack, &Struct))
				return std::unexpected("Cyclic or excessively nested shader parameter metadata");
			Stack.push_back(&Struct);
			if (Struct.IncludedParameters)
			{
				if (Struct.IncludedParameters->StructSize > Struct.StructSize)
					return std::unexpected("Included shader parameter layout exceeds parent storage");
				auto Result = Self(Self, *Struct.IncludedParameters, Offset, Prefix);
				if (!Result) return Result;
			}
			for (const auto& Member : Struct.Members)
			{
				if (!Member.Name || !*Member.Name || Member.Offset > Struct.StructSize
					|| Member.Size > Struct.StructSize - Member.Offset || !Member.ArraySize)
					return std::unexpected("Invalid shader parameter metadata member");
				const std::string Name = std::format("{}{}", Prefix, Member.Name);
				if (Member.Kind == EShaderParameterMemberKind::Struct)
				{
					if (!Member.StructMetadata || Member.ArraySize != 1 || Member.StructMetadata->StructSize > Member.Size)
						return std::unexpected("Invalid nested shader parameter structure");
					auto Result = Self(Self, *Member.StructMetadata, Offset + Member.Offset, Name + "_");
					if (!Result) return Result;
					continue;
				}
				if (Member.Kind != EShaderParameterMemberKind::Resource)
					return std::unexpected("Inline shader parameters require constant-layout binding support");
				if (!Names.insert(Name).second) return std::unexpected("Duplicate shader parameter name");
				const auto* Allocation = ParameterMap.FindParameterAllocation(Name);
				if (!Allocation)
				{
					if (Member.bOptional) continue;
					return std::unexpected(std::format("Shader parameter '{}' was not found in compiler allocations", Name));
				}
				if ((Allocation->Type != Member.Type && !(Allocation->Type == ERHIBindingType::UniformBuffer
					&& Member.Type == ERHIBindingType::UniformBufferDynamic)) || Allocation->Size != Member.ArraySize)
					return std::unexpected(std::format("Shader parameter '{}' kind or array does not match compiler allocation", Name));
				Candidate.push_back({Name, Offset + Member.Offset, Allocation->SetIndex, Allocation->BaseIndex,
					Member.Type, Allocation->Size, Member.bGraphResource});
			}
			Stack.pop_back();
			return {};
		};
		auto Result = Visit(Visit, *Metadata, 0, {});
		if (Result) Resources = std::move(Candidate);
		return Result;
	}
}
