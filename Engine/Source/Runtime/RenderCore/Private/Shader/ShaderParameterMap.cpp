#include "Shader/ShaderParameterMap.h"
#include "Shader/Shader.h"
#include "ShaderParameterSchemaInternal.h"

namespace Durin
{
	struct FShaderParameterBindings::FRDGLayoutCache
	{
		std::mutex Mutex;
		std::map<std::shared_ptr<const uint8>,
			std::expected<std::shared_ptr<const std::vector<uint32>>, std::string>> Layouts;
	};

	FShaderParameterBindings::FShaderParameterBindings() : RDGLayoutCache(std::make_shared<FRDGLayoutCache>()) {}

	auto FShaderParameterBindings::GetRDGLayoutBindings(const FRDGParameterLayout& Layout) const
		-> std::expected<std::shared_ptr<const std::vector<uint32>>, std::string>
	{
		std::lock_guard Lock(RDGLayoutCache->Mutex);
		if (const auto Found = RDGLayoutCache->Layouts.find(Layout.Identity); Found != RDGLayoutCache->Layouts.end())
			return Found->second;
		auto Join = std::make_shared<std::vector<uint32>>();
		Join->reserve(Resources.size());
		std::expected<std::shared_ptr<const std::vector<uint32>>, std::string> Result = Join;
		for (const auto& Binding : Resources)
		{
			const auto Found = std::lower_bound(Layout.ShaderBindings.begin(), Layout.ShaderBindings.end(), Binding.Name,
				[](const FRDGParameterShaderBinding& Candidate, std::string_view Name) { return Candidate.Name < Name; });
			if (Found == Layout.ShaderBindings.end() || Found->Name != Binding.Name)
			{
				if (Binding.bGraphResource)
				{
					Result = std::unexpected(std::format("binding '{}' is declared as a graph resource but has no composed graph member", Binding.Name));
					break;
				}
				Join->push_back(UINT32_MAX);
				continue;
			}
			const auto& Leaf = Layout.Leaves[Found->LeafIndex];
			if (Leaf.Metadata->ShaderBindingType != Binding.Type || Leaf.Metadata->ArraySize != Binding.ArraySize)
			{
				Result = std::unexpected(std::format("parameter '{}' shader binding '{}' {} does not match shader layout",
					Leaf.Path, Binding.Name, Leaf.Metadata->ShaderBindingType != Binding.Type ? "type" : "array extent"));
				break;
			}
			Join->push_back(Found->LeafIndex);
		}
		RDGLayoutCache->Layouts.emplace(Layout.Identity, Result);
		return Result;
	}

	auto FShaderParameterMap::AddParameterAllocation(std::string_view Name, const FParameterAllocation& Allocation)
		-> FShaderOperationResult
	{
		if (Name.empty() || !Allocation.Size || ParameterMap.contains(Name)
			|| static_cast<uint8>(Allocation.Type) > static_cast<uint8>(ERHIBindingType::StorageImage)
			|| Allocation.ResourceShape > EShaderResourceShape::Texture1DArray || Allocation.ConstantBufferSize > 16 * 1024 * 1024)
			return std::unexpected("Invalid or duplicate shader parameter allocation");
		if (std::ranges::any_of(ParameterMap, [&](const auto& Pair) {
			return Pair.second.SetIndex == Allocation.SetIndex && Pair.second.BaseIndex == Allocation.BaseIndex;
		})) return std::unexpected("Overlapping shader parameter resource allocations");
		if (Allocation.ConstantMembers.size() > 2048) return std::unexpected("Oversized shader constant layout");
		std::set<std::string_view> Names;
		uint32 End = 0;
		for (const auto& M : Allocation.ConstantMembers) {
			if (M.Name.empty() || M.Name.size() > 256 || !Names.insert(M.Name).second || !M.Size || M.Offset < End
				|| M.Offset > Allocation.ConstantBufferSize || M.Size > Allocation.ConstantBufferSize - M.Offset)
				return std::unexpected("Invalid shader constant allocation");
			End = M.Offset + M.Size;
		}
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
				{Resource.SetIndex, Resource.BindingIndex, Resource.ArraySize, Resource.Type, Resource.ResourceShape, Resource.ConstantBufferSize, Resource.ConstantMembers});
			if (!Result) return std::unexpected(Result.error());
		}
		return Map;
	}

	auto FShaderParameterBindings::Bind(const FShaderType& Type, const FShaderReflectionData& Reflection)
		-> FShaderOperationResult
	{
		Resources.clear();
		RDGLayoutCache = std::make_shared<FRDGLayoutCache>();
		if (!Type.ParameterSchemaError.empty()) return std::unexpected(Type.ParameterSchemaError);
		auto Map = Type.ParameterSchema
			? Private::BuildShaderParameterMapFromCanonicalSchema(*Type.ParameterSchema, Type.GetFrequency(), Reflection)
			: FShaderParameterMap::FromReflection(Reflection);
		if (!Map) return std::unexpected(Map.error());
		// Schema admission already checked names, types and extents. Only compiler
		// locations vary between instances; host offsets belong to registration.
		for (auto Binding : Type.ParameterHostBindings)
			if (const auto* Allocation = Map->FindParameterAllocation(Binding.Name))
			{
				Binding.SetIndex = Allocation->SetIndex;
				Binding.BindingIndex = Allocation->BaseIndex;
				Resources.push_back(std::move(Binding));
			}
		return {};
	}

	auto FShaderParameterBindings::Bind(const FShaderParametersMetadata* Metadata, const FShaderParameterMap& ParameterMap)
		-> FShaderOperationResult
	{
		Resources.clear();
		RDGLayoutCache = std::make_shared<FRDGLayoutCache>();
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
					Member.Type, Allocation->Size, Member.bGraphResource, Member.OwnerName ? Member.OwnerName : "", Member.Scope});
			}
			Stack.pop_back();
			return {};
		};
		auto Result = Visit(Visit, *Metadata, 0, {});
		if (Result) Resources = std::move(Candidate);
		return Result;
	}
}
