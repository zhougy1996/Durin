#include "Shader/ShaderParameterSchema.h"

namespace Durin
{
	namespace
	{
		auto Canonicalize(FShaderParameterSchema Schema)
			-> std::expected<FShaderParameterSchema, std::string>
		{
			if (Schema.Version != 1 || Schema.Fields.size() > 4096)
				return std::unexpected("Unsupported or oversized shader parameter schema");
			std::ranges::sort(Schema.Fields, {}, &FShaderParameterField::QualifiedName);
			std::set<std::string> Names;
			std::string Previous;
			FXxHash128Builder Hash;
			Hash.UpdateValue(Schema.Version);
			Hash.UpdateValue(static_cast<uint32>(Schema.Fields.size()));
			for (const auto& Field : Schema.Fields)
			{
				if (Field.QualifiedName.empty() || Field.QualifiedName == Previous
					|| Field.QualifiedName.size() > 1024 || Field.QualifiedName.front() == '.'
					|| Field.QualifiedName.back() == '.' || Field.QualifiedName.find("..") != std::string::npos
					|| Field.ShaderName.empty() || !Names.insert(Field.ShaderName).second || !Field.ArraySize
					|| static_cast<uint8>(Field.Type) > static_cast<uint8>(ERHIBindingType::StorageImage)
					|| (Field.Kind != EShaderParameterMemberKind::Resource && Field.Kind != EShaderParameterMemberKind::Value))
					return std::unexpected("Invalid or ambiguous shader parameter field");
				Previous = Field.QualifiedName;
				for (const auto* Name : {&Field.QualifiedName, &Field.ShaderName})
				{
					Hash.UpdateValue(static_cast<uint32>(Name->size()));
					Hash.Update(*Name);
				}
				Hash.UpdateValue(static_cast<uint8>(Field.Kind));
				Hash.UpdateValue(static_cast<uint8>(Field.Type));
				Hash.UpdateValue(Field.ArraySize);
				Hash.UpdateValue(Field.ValueSize);
				Hash.UpdateValue(static_cast<uint8>(Field.bOptional));
			}
			Schema.Identity = Hash.Finalize();
			return Schema;
		}

		auto Collect(const FShaderParametersMetadata& Metadata, std::string_view Prefix,
			std::vector<const FShaderParametersMetadata*>& Stack, FShaderParameterSchema& Schema)
			-> std::expected<void, std::string>
		{
			if (std::ranges::contains(Stack, &Metadata) || Stack.size() >= 64)
				return std::unexpected("Shader parameter metadata cycle or excessive nesting");
			Stack.push_back(&Metadata);
			if (Metadata.IncludedParameters)
			{
				if (Metadata.IncludedParameters->StructSize > Metadata.StructSize)
					return std::unexpected("Included shader parameters exceed structure storage");
				auto Result = Collect(*Metadata.IncludedParameters, Prefix, Stack, Schema);
				if (!Result) return Result;
			}
			for (const auto& Member : Metadata.Members)
			{
				if (!Member.Name || !*Member.Name || std::string_view(Member.Name).find('.') != std::string_view::npos
					|| !Member.ArraySize || Member.Offset > Metadata.StructSize
					|| Member.Size > Metadata.StructSize - Member.Offset)
					return std::unexpected("Invalid shader parameter member layout or name");
				const std::string Path = Prefix.empty() ? Member.Name : std::format("{}.{}", Prefix, Member.Name);
				if (Member.Kind == EShaderParameterMemberKind::Struct)
				{
					if (!Member.StructMetadata || Member.ArraySize != 1 || Member.StructMetadata->StructSize > Member.Size)
						return std::unexpected("Invalid nested shader parameter metadata");
					auto Result = Collect(*Member.StructMetadata, Path, Stack, Schema);
					if (!Result) return Result;
				}
				else
				{
					if (Member.StructMetadata || (Member.Kind != EShaderParameterMemberKind::Resource
						&& Member.Kind != EShaderParameterMemberKind::Value))
						return std::unexpected("Invalid shader parameter member kind");
					std::string ShaderName = Path;
					std::ranges::replace(ShaderName, '.', '_');
					Schema.Fields.push_back({Path, std::move(ShaderName), Member.Kind, Member.Type,
						Member.ArraySize, Member.Kind == EShaderParameterMemberKind::Value ? Member.Size : 0, Member.bOptional});
				}
			}
			Stack.pop_back();
			return {};
		}
	}

	auto BuildShaderParameterSchema(const FShaderParametersMetadata& Metadata)
		-> std::expected<FShaderParameterSchema, std::string>
	{
		FShaderParameterSchema Schema;
		std::vector<const FShaderParametersMetadata*> Stack;
		auto Result = Collect(Metadata, {}, Stack, Schema);
		if (!Result) return std::unexpected(Result.error());
		return Canonicalize(std::move(Schema));
	}

	auto ComposeShaderParameterSchemas(std::span<const FShaderParameterSchema> Schemas)
		-> std::expected<FShaderParameterSchema, std::string>
	{
		FShaderParameterSchema Combined;
		for (const auto& Schema : Schemas)
		{
			if (Schema.Version != Combined.Version) return std::unexpected("Unsupported shader parameter schema version");
			auto Validated = Canonicalize(Schema);
			if (!Validated || Validated->Identity != Schema.Identity)
				return std::unexpected("Invalid shader parameter schema identity");
			Combined.Fields.insert(Combined.Fields.end(), Schema.Fields.begin(), Schema.Fields.end());
		}
		return Canonicalize(std::move(Combined));
	}

	auto BuildShaderParameterAllocationMap(const FShaderParameterSchema& Schema,
		const FShaderParameterProduct& Product, const FShaderReflectionData& Reflection)
		-> std::expected<FShaderParameterAllocationMap, std::string>
	{
		auto Canonical = Canonicalize(Schema);
		if (Schema.Version != 1 || !Canonical || Canonical->Identity != Schema.Identity
			|| Canonical->Fields != Schema.Fields || Product.ShaderType.empty()
			|| (Product.Frequency != EShaderFrequency::Vertex && Product.Frequency != EShaderFrequency::Fragment
				&& Product.Frequency != EShaderFrequency::Compute)
			|| !Reflection.PushConstantRanges.empty())
			return std::unexpected("Invalid parameter schema, product or unsupported push constants");
		FShaderParameterAllocationMap Map;
		Map.SchemaIdentity = Schema.Identity;
		Map.Product = Product;
		std::set<std::pair<uint32, uint32>> Locations;
		std::set<std::string> Names;
		for (const auto& Binding : Reflection.ResourceBindings)
		{
			const auto Field = std::ranges::find(Schema.Fields, Binding.Name, &FShaderParameterField::ShaderName);
			const auto Stage = Product.Frequency == EShaderFrequency::Vertex ? EShaderStageFlags::Vertex
				: Product.Frequency == EShaderFrequency::Fragment ? EShaderStageFlags::Fragment
				: EShaderStageFlags::Compute;
			if (Field == Schema.Fields.end() || Field->Kind != EShaderParameterMemberKind::Resource
				|| (Binding.Type != Field->Type && !(Binding.Type == ERHIBindingType::UniformBuffer
					&& Field->Type == ERHIBindingType::UniformBufferDynamic)) || Binding.ArraySize != Field->ArraySize
				|| Binding.StageFlags != Stage || !Names.insert(Binding.Name).second
				|| !Locations.emplace(Binding.SetIndex, Binding.BindingIndex).second)
				return std::unexpected("Reflected parameter is extra, duplicated or incompatible with its schema");
			auto Added = Map.Parameters.AddParameterAllocation(Field->ShaderName,
				{Binding.SetIndex, Binding.BindingIndex, Binding.ArraySize, Binding.Type});
			if (!Added) return std::unexpected(Added.error());
		}
		for (const auto& Field : Schema.Fields)
		{
			// Inline values require compiler constant-layout reflection before admission.
			if (Field.Kind != EShaderParameterMemberKind::Resource)
				return std::unexpected("Inline parameter allocation requires constant-layout reflection");
			if (!Field.bOptional && !Names.contains(Field.ShaderName))
				return std::unexpected("Required shader parameter is absent from reflection");
		}
		FXxHash128Builder Hash;
		const auto String = [&](std::string_view Value) {
			Hash.UpdateValue(static_cast<uint32>(Value.size())); Hash.Update(Value);
		};
		Hash.UpdateValue(Map.Version);
		Hash.UpdateValue(Map.SchemaIdentity.HashLow); Hash.UpdateValue(Map.SchemaIdentity.HashHigh);
		Hash.UpdateValue(static_cast<uint32>(Product.Target.Platform));
		Hash.UpdateValue(static_cast<uint32>(Product.Target.Backend));
		Hash.UpdateValue(static_cast<uint32>(Product.Target.IntermediateFormat));
		Hash.UpdateValue(static_cast<uint32>(Product.Target.OutputFormat));
		Hash.UpdateValue(Product.Target.SettingsDigest.HashLow); Hash.UpdateValue(Product.Target.SettingsDigest.HashHigh);
		String(Product.ShaderType); String(Product.VertexFactoryType);
		Hash.UpdateValue(Product.PermutationId); Hash.UpdateValue(static_cast<uint8>(Product.Frequency));
		Hash.UpdateValue(Product.CompileKey.HashLow); Hash.UpdateValue(Product.CompileKey.HashHigh);
		Hash.UpdateValue(static_cast<uint32>(Map.Parameters.GetParameterAllocations().size()));
		for (const auto& [Name, Allocation] : Map.Parameters.GetParameterAllocations())
		{
			String(Name);
			Hash.UpdateValue(Allocation.SetIndex); Hash.UpdateValue(Allocation.BaseIndex);
			Hash.UpdateValue(static_cast<uint8>(Allocation.Type)); Hash.UpdateValue(Allocation.Size);
		}
		Map.Identity = Hash.Finalize();
		return Map;
	}

	auto ValidateShaderParameterAllocationMap(const FShaderParameterSchema& Schema,
		const FShaderParameterProduct& Product, const FShaderParameterAllocationMap& Map,
		const FShaderReflectionData& Reflection) -> std::expected<void, std::string>
	{
		auto Expected = BuildShaderParameterAllocationMap(Schema, Product, Reflection);
		if (!Expected) return std::unexpected(Expected.error());
		if (Map != *Expected) return std::unexpected("Shader parameter allocation map does not match the accepted product");
		return {};
	}
}
