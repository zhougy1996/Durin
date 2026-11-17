#include "Shader/ShaderParameterMetadataBuilder.h"
#include "Shader/ShaderParameterSchema.h"

namespace Durin
{
	auto FShaderParametersMetadataBuilder::AddMember(std::string_view Name,
		FShaderParameterMemberMetadata Member, uint32 Alignment) -> FShaderOperationResult
	{
		if (Name.empty() || Names.size() >= 4096 || !Alignment || (Alignment & (Alignment - 1))
			|| std::ranges::find(Names, Name) != Names.end())
			return std::unexpected("Invalid or duplicate data-driven parameter member");
		const uint64 Offset = (uint64(NextOffset) + Alignment - 1) & ~(uint64(Alignment) - 1);
		if (Offset + Member.Size > std::numeric_limits<uint32>::max())
			return std::unexpected("Data-driven parameter layout exceeds supported size");
		Member.Offset = static_cast<uint32>(Offset);
		Names.emplace_back(Name);
		ShaderTypes.emplace_back(); Owners.emplace_back();
		Members.push_back(Member);
		NextOffset = static_cast<uint32>(Offset + Member.Size);
		StructAlignment = std::max(StructAlignment, Alignment);
		return {};
	}

	auto FShaderParametersMetadataBuilder::AddResourceParameter(std::string_view Name,
		ERHIBindingType Type, uint32 ArraySize, bool bOptional, std::string_view ShaderType,
		std::string_view OwnerName, EShaderParameterBindingScope Scope) -> FShaderOperationResult
	{
		if (!ArraySize || static_cast<uint8>(Type) > static_cast<uint8>(ERHIBindingType::StorageImage))
			return std::unexpected("Invalid data-driven resource shape");
		const bool Uniform = Type == ERHIBindingType::UniformBuffer || Type == ERHIBindingType::UniformBufferDynamic;
		const uint32 ElementSize = Uniform ? sizeof(FRHIUniformBufferRange)
			: Type == ERHIBindingType::StorageBuffer ? sizeof(FRHIStorageBufferRange) : sizeof(FRHIResource*);
		const uint32 Alignment = Uniform ? alignof(FRHIUniformBufferRange)
			: Type == ERHIBindingType::StorageBuffer ? alignof(FRHIStorageBufferRange) : alignof(FRHIResource*);
		if (ArraySize > std::numeric_limits<uint32>::max() / ElementSize)
			return std::unexpected("Data-driven resource array exceeds supported size");
		auto Added = AddMember(Name, {.Size = ElementSize * ArraySize, .ArraySize = ArraySize,
			.Type = Type, .bOptional = bOptional, .Scope = Scope}, Alignment);
		if (Added) { ShaderTypes.back() = ShaderType; Owners.back() = OwnerName; }
		return Added;
	}

	auto FShaderParametersMetadataBuilder::AddNestedStruct(std::string_view Name,
		const FShaderParametersMetadata& Metadata) -> FShaderOperationResult
	{
		return AddMember(Name, {.Size = Metadata.StructSize, .Kind = EShaderParameterMemberKind::Struct,
			.StructMetadata = &Metadata}, Metadata.StructAlignment);
	}

	auto FShaderParametersMetadataBuilder::Build(std::string_view StructName) const
		-> std::expected<std::shared_ptr<const FShaderParametersMetadataStorage>, std::string>
	{
		if (StructName.empty()) return std::unexpected("Data-driven parameter structure requires a name");
		auto Storage = std::make_shared<FShaderParametersMetadataStorage>();
		Storage->OwnedStructName = StructName;
		Storage->OwnedNames = Names;
		Storage->OwnedNames.insert(Storage->OwnedNames.end(), ShaderTypes.begin(), ShaderTypes.end());
		Storage->OwnedNames.insert(Storage->OwnedNames.end(), Owners.begin(), Owners.end());
		Storage->OwnedMembers = Members;
		for (size_t Index = 0; Index < Members.size(); ++Index) {
			Storage->OwnedMembers[Index].Name = Storage->OwnedNames[Index].c_str();
			Storage->OwnedMembers[Index].ShaderType = ShaderTypes[Index].empty() ? nullptr : Storage->OwnedNames[Index + Members.size()].c_str();
			Storage->OwnedMembers[Index].OwnerName = Owners[Index].empty() ? nullptr : Storage->OwnedNames[Index + Members.size() * 2].c_str();
		}
		const uint64 Size = (uint64(NextOffset) + StructAlignment - 1) & ~(uint64(StructAlignment) - 1);
		if (Size > std::numeric_limits<uint32>::max()) return std::unexpected("Data-driven parameter structure is too large");
		Storage->Metadata = {.StructName = Storage->OwnedStructName.c_str(), .StructSize = static_cast<uint32>(Size),
			.StructAlignment = StructAlignment, .Members = Storage->OwnedMembers};
		auto Schema = BuildShaderParameterSchema(Storage->Metadata);
		if (!Schema) return std::unexpected(Schema.error());
		return std::shared_ptr<const FShaderParametersMetadataStorage>(std::move(Storage));
	}
}
