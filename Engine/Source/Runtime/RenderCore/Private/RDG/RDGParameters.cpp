#include "RDGBuilderInternal.h"

namespace Durin::RDGPrivate
{
	namespace
	{
		auto MetadataContext(const FRDGParametersMetadata& Metadata,
			const FRDGParameterMemberMetadata& Member, uint64 ExpectedSize = 0) -> FRDGMetadataErrorContext;
		auto ValidateParameterMetadata(
			const FRDGParametersMetadata* Metadata,
			uint32 ExpectedSize, uint32 ExpectedAlignment,
			uint32 Depth = 0) -> FRDGMetadataResult;
		auto ValidateShaderCompositionMetadata(
			const FRDGParametersMetadata* Metadata)
			-> FRDGMetadataResult;

		auto MetadataContext(const FRDGParametersMetadata& Metadata,
			const FRDGParameterMemberMetadata& Member, uint64 ExpectedSize) -> FRDGMetadataErrorContext
		{
			return {.StructName = Metadata.StructName, .MemberName = Member.Name ? Member.Name : "",
				.BindingName = Member.ShaderBindingName ? Member.ShaderBindingName : "",
				.MemberIndex = static_cast<uint64>(&Member - Metadata.Members.data()),
				.ExpectedSize = ExpectedSize ? ExpectedSize : Metadata.StructSize, .ActualSize = Member.ElementSize,
				.Offset = Member.Offset, .ArraySize = Member.ArraySize};
		}

		auto ValidateParameterMetadata(
			const FRDGParametersMetadata* Metadata,
			uint32 ExpectedSize, uint32 ExpectedAlignment,
			uint32 Depth) -> FRDGMetadataResult
		{
			if (Metadata == nullptr)
			{
				return std::unexpected(FRDGMetadataError{ERDGMetadataError::MetadataNull});
			}
			if (Metadata->StructName == nullptr || Metadata->StructName[0] == '\0')
			{
				return std::unexpected(FRDGMetadataError{ERDGMetadataError::MetadataNameEmpty});
			}
			if (Metadata->StructSize != ExpectedSize
				|| Metadata->StructAlignment != ExpectedAlignment)
			{
				return std::unexpected(FRDGMetadataError{ERDGMetadataError::MetadataLayoutMismatch,
					FRDGMetadataErrorContext{.StructName = Metadata->StructName,
						.ExpectedSize = ExpectedSize,
						.ActualSize = Metadata->StructSize,
						.ExpectedAlignment = ExpectedAlignment,
						.ActualAlignment = Metadata->StructAlignment}});
			}
			if (Depth >= 32)
			{
				return std::unexpected(FRDGMetadataError{ERDGMetadataError::MetadataNestingLimit,
					FRDGMetadataErrorContext{.StructName = Metadata->StructName,
						.ExpectedSize = 32,
						.Depth = Depth}});
			}

			uint64 PreviousEnd = 0;
			std::vector<std::string_view> MemberNames;
			std::vector<std::string_view> ShaderBindingNames;
			MemberNames.reserve(Metadata->Members.size());
			ShaderBindingNames.reserve(Metadata->Members.size());
			for (const auto& Member : Metadata->Members)
			{
				if (Member.Name == nullptr || Member.Name[0] == '\0')
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::MemberNameEmpty, MetadataContext(*Metadata, Member)});
				}
				if (std::ranges::find(MemberNames, Member.Name)
					!= MemberNames.end())
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::MemberNameDuplicate, MetadataContext(*Metadata, Member)});
				}
				MemberNames.emplace_back(Member.Name);
				if (Member.ElementSize == 0 || Member.ArraySize == 0)
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::MemberLayoutEmpty, MetadataContext(*Metadata, Member)});
				}
				const uint64 End = static_cast<uint64>(Member.Offset)
					+ static_cast<uint64>(Member.ElementSize) * Member.ArraySize;
				if (Member.Offset < PreviousEnd || End > Metadata->StructSize)
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::MemberOffsetInvalid, MetadataContext(*Metadata, Member)});
				}
				PreviousEnd = End;

				if (Member.Kind == ERDGParameterMemberKind::Nested)
				{
					if (Member.bOptional || Member.NestedParameters == nullptr
						|| Member.ElementSize != Member.NestedParameters->StructSize)
					{
						return std::unexpected(FRDGMetadataError{ERDGMetadataError::NestedMetadataInvalid, MetadataContext(*Metadata, Member)});
					}
					if (auto Error = ValidateParameterMetadata(Member.NestedParameters,
						Member.NestedParameters->StructSize,
						Member.NestedParameters->StructAlignment, Depth + 1); !Error.has_value())
						return Error;
					continue;
				}
				if (Member.NestedParameters != nullptr)
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::UnexpectedNestedMetadata, MetadataContext(*Metadata, Member)});
				}
				uint32 ExpectedElementSize = 0;
				switch (Member.Kind)
				{
				case ERDGParameterMemberKind::Texture:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGTextureParameter>)
						: sizeof(FRDGTextureParameter);
					break;
				case ERDGParameterMemberKind::Buffer:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGBufferParameter>)
						: sizeof(FRDGBufferParameter);
					break;
				case ERDGParameterMemberKind::Token:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGTokenParameter>)
						: sizeof(FRDGTokenParameter);
					break;
				case ERDGParameterMemberKind::ColorAttachment:
				case ERDGParameterMemberKind::ManagedColorAttachment:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGColorAttachmentParameter>)
						: sizeof(FRDGColorAttachmentParameter);
					break;
				case ERDGParameterMemberKind::DepthStencilAttachment:
				case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<
							FRDGDepthStencilAttachmentParameter>)
						: sizeof(FRDGDepthStencilAttachmentParameter);
					break;
				case ERDGParameterMemberKind::ManagedTexture:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<FRDGManagedTextureParameter>)
						: sizeof(FRDGManagedTextureParameter);
					break;
				case ERDGParameterMemberKind::ValueRead:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<TRDGValueRead<std::byte>>)
						: sizeof(TRDGValueRead<std::byte>);
					break;
				case ERDGParameterMemberKind::ValueWrite:
					ExpectedElementSize = Member.bOptional
						? sizeof(std::optional<TRDGValueWrite<std::byte>>)
						: sizeof(TRDGValueWrite<std::byte>);
					break;
				case ERDGParameterMemberKind::Nested: break;
				}
				if (Member.ElementSize != ExpectedElementSize)
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::WrapperLayoutMismatch, MetadataContext(*Metadata, Member, ExpectedElementSize)});
				}
				if (Member.bOptional
					!= (Member.ReadOptionalValueAddress != nullptr))
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::OptionalLayoutMismatch, MetadataContext(*Metadata, Member)});
				}

				const bool bTextureKind = Member.ResourceKind
					== ERDGResourceKind::Texture;
				const bool bBufferKind = Member.ResourceKind
					== ERDGResourceKind::Buffer;
				const bool bTokenKind = Member.ResourceKind
					== ERDGResourceKind::Token;
				bool bShapeValid = false;
				switch (Member.Kind)
				{
				case ERDGParameterMemberKind::Texture:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::Buffer:
					bShapeValid = bBufferKind
						&& Member.RangeKind == ERDGParameterRangeKind::BufferBytes
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::Token:
					bShapeValid = bTokenKind
						&& Member.RangeKind == ERDGParameterRangeKind::None
						&& Member.Access == ERHIAccess::None
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ColorAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::ColorAttachmentReadWrite
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::DepthStencilAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::DepthStencilReadWrite
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ManagedColorAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::ColorAttachmentReadWrite
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.Use == ERDGUse::ReadWrite
						&& Member.Access == ERHIAccess::DepthStencilReadWrite
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ManagedTexture:
					bShapeValid = bTextureKind
						&& Member.RangeKind == ERDGParameterRangeKind::TextureSubresource
						&& Member.bPassManagedTransition
						&& Member.ResultAccess != ERHIAccess::None;
					break;
				case ERDGParameterMemberKind::ValueRead:
				case ERDGParameterMemberKind::ValueWrite:
					bShapeValid = bTokenKind
						&& Member.RangeKind == ERDGParameterRangeKind::None
						&& Member.Access == ERHIAccess::None
						&& !Member.bPassManagedTransition
						&& Member.ResultAccess == ERHIAccess::None
						&& Member.ValueTypeIdentity != nullptr
						&& Member.ReadValueHandle != nullptr
						&& Member.Use == (Member.Kind
							== ERDGParameterMemberKind::ValueRead
							? ERDGUse::Read : ERDGUse::Write);
					break;
				case ERDGParameterMemberKind::Nested: break;
				}
				if (!bShapeValid)
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::DeclarationSemanticsInvalid, MetadataContext(*Metadata, Member)});
				}

				if (Member.bShaderBinding)
				{
					if (Member.ShaderBindingName == nullptr
						|| Member.ShaderBindingName[0] == '\0')
					{
						return std::unexpected(FRDGMetadataError{ERDGMetadataError::ShaderBindingNameEmpty, MetadataContext(*Metadata, Member)});
					}
					if (std::ranges::find(ShaderBindingNames,
						Member.ShaderBindingName) != ShaderBindingNames.end())
					{
						return std::unexpected(FRDGMetadataError{ERDGMetadataError::ShaderBindingDuplicate, MetadataContext(*Metadata, Member)});
					}
					ShaderBindingNames.emplace_back(Member.ShaderBindingName);

					const bool bTextureBinding = Member.Kind
						== ERDGParameterMemberKind::Texture
						&& (Member.ShaderBindingType == ERHIBindingType::Texture
							|| Member.ShaderBindingType
								== ERHIBindingType::StorageImage);
					const bool bBufferBinding = Member.Kind
						== ERDGParameterMemberKind::Buffer
						&& Member.ShaderBindingType
							== ERHIBindingType::StorageBuffer;
					const bool bUav = Member.ShaderBindingType
						== ERHIBindingType::StorageImage
						|| (Member.ShaderBindingType == ERHIBindingType::StorageBuffer
							&& Member.Use != ERDGUse::Read);
					const ERHIAccess ShaderRead = ERHIAccess::GraphicsShaderRead
						| ERHIAccess::ComputeShaderRead;
					const ERHIAccess ShaderReadWrite =
						ERHIAccess::GraphicsShaderReadWrite
						| ERHIAccess::ComputeShaderReadWrite;
					const bool bAccessCompatible = bUav
						? Member.Use != ERDGUse::Read
							&& EnumHasAnyFlags(Member.Access, ShaderReadWrite)
						: Member.Use != ERDGUse::Write
							&& EnumHasAnyFlags(Member.Access,
								ShaderRead | ShaderReadWrite);
					if ((!bTextureBinding && !bBufferBinding)
						|| !bAccessCompatible)
					{
						return std::unexpected(FRDGMetadataError{ERDGMetadataError::ShaderDeclarationIncompatible, MetadataContext(*Metadata, Member)});
					}
				}
				else if (Member.ShaderBindingName != nullptr)
				{
					return std::unexpected(FRDGMetadataError{ERDGMetadataError::ShaderBindingAuthorityMissing, MetadataContext(*Metadata, Member)});
				}
			}
			return {};
		}

		auto ValidateShaderCompositionMetadata(
			const FRDGParametersMetadata* Metadata)
			-> FRDGMetadataResult
		{
			std::vector<std::pair<std::string_view, std::string>> Bindings;
			std::function<FRDGMetadataResult(const FRDGParametersMetadata*,
				const std::string&)> Traverse;
			Traverse = [&](const FRDGParametersMetadata* StructMetadata,
				const std::string& ParentPath) -> FRDGMetadataResult {
				for (const auto& Member : StructMetadata->Members)
				{
					const std::string Path = ParentPath.empty()
						? std::string(StructMetadata->StructName) + "." + Member.Name
						: ParentPath + "." + Member.Name;
					if (Member.Kind == ERDGParameterMemberKind::Nested)
					{
						if (auto Error = Traverse(Member.NestedParameters, Path);
							!Error.has_value()) return Error;
						continue;
					}
					if (!Member.bShaderBinding) continue;
					const auto Existing = std::ranges::find_if(Bindings,
						[&](const auto& Binding) {
							return Binding.first == Member.ShaderBindingName;
						});
					if (Existing != Bindings.end())
					{
						return std::unexpected(FRDGMetadataError{ERDGMetadataError::NestedShaderBindingDuplicate,
							FRDGMetadataErrorContext{.StructName = Metadata->StructName,
								.MemberName = Path,
								.OtherMemberName = Existing->second,
								.BindingName = Member.ShaderBindingName}});
					}
					Bindings.emplace_back(Member.ShaderBindingName, Path);
				}
				return {};
			};
			return Traverse(Metadata, {});
		}

		}

	auto MakeParameterUse(const FRDGParameterMemberMetadata& Member,
		std::string_view FieldPath) -> FGraphUse
	{
		FGraphUse Use;
		Use.Kind = Member.ResourceKind;
		Use.Use = Member.Use;
		Use.Access = Member.Access;
		Use.bDiscard = Member.bDiscard;
		Use.bPassManagedTransition = Member.bPassManagedTransition;
		Use.ResultAccess = Member.ResultAccess;
		Use.ParameterPath = FieldPath;
		if (Member.bShaderBinding)
		{
			Use.ShaderBindingName = Member.ShaderBindingName;
			Use.ShaderBindingType = Member.ShaderBindingType;
		}
		if (Member.Kind == ERDGParameterMemberKind::ColorAttachment
			|| Member.Kind == ERDGParameterMemberKind::ManagedColorAttachment
			|| Member.Kind == ERDGParameterMemberKind::DepthStencilAttachment
			|| Member.Kind == ERDGParameterMemberKind::ManagedDepthStencilAttachment)
		{
			Use.bDiscard = Member.LoadAction != ERHIRenderTargetLoadAction::Load;
			Use.bStore = Member.StoreAction == ERHIRenderTargetStoreAction::Store;
		}
		if (Member.Kind == ERDGParameterMemberKind::ValueRead
			|| Member.Kind == ERDGParameterMemberKind::ValueWrite)
			Use.bDiscard = Member.Use == ERDGUse::Write;
		return Use;
	}

}

namespace Durin
{
	using namespace RDGPrivate;

	auto BuildRDGParameterLayout(const FRDGParametersMetadata* Metadata,
		uint32 ExpectedSize, uint32 ExpectedAlignment)
		-> FRDGParameterLayoutBuildResult
	{
		if (auto Result = ValidateParameterMetadata(Metadata, ExpectedSize, ExpectedAlignment); !Result)
			return std::unexpected(std::move(Result.error()));
		if (auto Result = ValidateShaderCompositionMetadata(Metadata); !Result)
			return std::unexpected(std::move(Result.error()));

		auto Layout = std::make_unique<FRDGParameterLayout>();
		Layout->Metadata = Metadata;
		std::function<void(const FRDGParametersMetadata*, uint32,
			const std::string&)> Flatten;
		Flatten = [&](const FRDGParametersMetadata* StructMetadata,
			uint32 BaseOffset, const std::string& ParentPath) {
			for (const FRDGParameterMemberMetadata& Member : StructMetadata->Members)
			{
				const uint32 MemberOffset = BaseOffset + Member.Offset;
				const std::string MemberPath = ParentPath.empty()
					? std::string(Member.Name) : ParentPath + "." + Member.Name;
				if (Member.Kind == ERDGParameterMemberKind::Nested)
				{
					for (uint32 ElementIndex = 0;
						ElementIndex < Member.ArraySize; ++ElementIndex)
					{
						std::string ElementPath = MemberPath;
						if (Member.ArraySize > 1)
							ElementPath += "[" + std::to_string(ElementIndex) + "]";
						Flatten(Member.NestedParameters,
							MemberOffset + ElementIndex * Member.ElementSize,
							ElementPath);
					}
					continue;
				}

				const uint32 LeafIndex = static_cast<uint32>(Layout->Leaves.size());
				const uint32 FirstElementIndex =
					static_cast<uint32>(Layout->Elements.size());
				Layout->Leaves.push_back({&Member, MemberOffset,
					FirstElementIndex, MemberPath});
				for (uint32 ElementIndex = 0;
					ElementIndex < Member.ArraySize; ++ElementIndex)
				{
					const uint32 FlatElementIndex =
						static_cast<uint32>(Layout->Elements.size());
					std::string FieldPath = std::string(Metadata->StructName)
						+ "." + MemberPath;
					if (Member.ArraySize > 1)
						FieldPath += "[" + std::to_string(ElementIndex) + "]";
					Layout->Elements.push_back({
						.Offset = MemberOffset + ElementIndex * Member.ElementSize,
						.LeafIndex = LeafIndex,
						.ArrayElementIndex = ElementIndex,
						.FieldPath = std::move(FieldPath),
					});
					auto AddCategory = [&](std::vector<uint32>& Category) {
						Category.push_back(FlatElementIndex);
					};
					switch (Member.Kind)
					{
					case ERDGParameterMemberKind::Texture:
					case ERDGParameterMemberKind::ManagedTexture:
						AddCategory(Layout->TextureElements); break;
					case ERDGParameterMemberKind::Buffer:
						AddCategory(Layout->BufferElements); break;
					case ERDGParameterMemberKind::ValueRead:
					case ERDGParameterMemberKind::ValueWrite:
						AddCategory(Layout->ValueElements); break;
					case ERDGParameterMemberKind::Token:
						AddCategory(Layout->TokenElements); break;
					case ERDGParameterMemberKind::ColorAttachment:
					case ERDGParameterMemberKind::DepthStencilAttachment:
					case ERDGParameterMemberKind::ManagedColorAttachment:
					case ERDGParameterMemberKind::ManagedDepthStencilAttachment:
						AddCategory(Layout->AttachmentElements); break;
					case ERDGParameterMemberKind::Nested: break;
					}
				}
				if (Member.bShaderBinding)
					Layout->ShaderBindings.push_back({Member.ShaderBindingName,
						LeafIndex});
			}
		};
		Flatten(Metadata, 0, {});
		Layout->OffsetIndex.resize(Layout->Elements.size());
		std::iota(Layout->OffsetIndex.begin(), Layout->OffsetIndex.end(), 0u);
		std::ranges::sort(Layout->OffsetIndex, {},
			[&](uint32 Index) { return Layout->Elements[Index].Offset; });
		std::ranges::sort(Layout->ShaderBindings, {},
			&FRDGParameterShaderBinding::Name);
		return Layout;
	}

	auto FRDGParameterResolver::FindMember(const void* Address,
		ERDGParameterMemberKind ExpectedKind,
		ERDGParameterMemberKind AlternateKind, bool bOptional) const
		-> const FRDGParameterMemberMetadata&
	{
		const FRDGParameterMemberMetadata* Found = nullptr;
		if (Layout != nullptr && Parameters != nullptr && Address != nullptr)
		{
			const uintptr_t RootAddress = reinterpret_cast<uintptr_t>(Parameters);
			const uintptr_t RequestedAddress = reinterpret_cast<uintptr_t>(Address);
			if (RequestedAddress >= RootAddress
				&& RequestedAddress - RootAddress < Layout->Metadata->StructSize)
			{
				const uint32 Offset = static_cast<uint32>(RequestedAddress - RootAddress);
				uint32 ElementIndex = std::numeric_limits<uint32>::max();
				if (bOptional)
				{
					const auto It = std::lower_bound(Layout->OffsetIndex.begin(),
						Layout->OffsetIndex.end(), Offset,
						[&](uint32 Index, uint32 Value) {
							return Layout->Elements[Index].Offset < Value;
						});
					if (It != Layout->OffsetIndex.end()
						&& Layout->Elements[*It].Offset == Offset)
						ElementIndex = *It;
				}
				else
				{
					const auto Alias = std::lower_bound(OptionalAliases.begin(),
						OptionalAliases.end(), Offset,
						[](const auto& Entry, uint32 Value) {
							return Entry.first < Value;
						});
					if (Alias != OptionalAliases.end() && Alias->first == Offset)
						ElementIndex = Alias->second;
					else
					{
						const auto It = std::lower_bound(Layout->OffsetIndex.begin(),
							Layout->OffsetIndex.end(), Offset,
							[&](uint32 Index, uint32 Value) {
								return Layout->Elements[Index].Offset < Value;
							});
						if (It != Layout->OffsetIndex.end()
							&& Layout->Elements[*It].Offset == Offset
							&& !Layout->Leaves[Layout->Elements[*It].LeafIndex]
								.Metadata->bOptional)
							ElementIndex = *It;
					}
				}
				if (ElementIndex < Layout->Elements.size())
				{
					const auto& Member = *Layout->Leaves[
						Layout->Elements[ElementIndex].LeafIndex].Metadata;
					if (Member.bOptional == bOptional
						&& (Member.Kind == ExpectedKind
							|| Member.Kind == AlternateKind))
						Found = &Member;
					else if (!bOptional && Member.bOptional
						&& (Member.Kind == ExpectedKind
							|| Member.Kind == AlternateKind))
						Found = &Member;
				}
			}
		}
		requiref(Found != nullptr,
			"Render graph pass '{}' parameter resolver accessed a member that is not "
			"declared by the executing pass parameters (requested capability '{}', "
			"optional={}).", PassName, ParameterMemberKindName(ExpectedKind), bOptional);
		return *Found;
	}

	auto FRDGParameterResolver::ValidateShaderParametersIdentity(
		const void* Data, const FRDGParametersMetadata* InMetadata) const
		-> void
	{
		requiref(Data == Parameters && Layout != nullptr
			&& InMetadata == Layout->Metadata,
			"Render graph pass '{}' attempted composed shader submission from a "
			"copied or foreign parameter object.", PassName);
	}

	auto FRDGParameterResolver::GetTexture(
		const FRDGTextureParameter& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Texture,
			ERDGParameterMemberKind::Texture, false);
		return Resources.GetTexture(Parameter.Texture);
	}

	auto FRDGParameterResolver::GetTexture(const std::optional<
		FRDGTextureParameter>& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Texture,
			ERDGParameterMemberKind::Texture, true);
		return Parameter ? Resources.GetTexture(Parameter->Texture) : nullptr;
	}

	auto FRDGParameterResolver::GetTexture(
		const FRDGManagedTextureParameter& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::ManagedTexture,
			ERDGParameterMemberKind::ManagedTexture, false);
		return Resources.GetTexture(Parameter.Texture);
	}

	auto FRDGParameterResolver::GetTexture(const std::optional<
		FRDGManagedTextureParameter>& Parameter) const -> FRHITexture*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::ManagedTexture,
			ERDGParameterMemberKind::ManagedTexture, true);
		return Parameter ? Resources.GetTexture(Parameter->Texture) : nullptr;
	}

	auto FRDGParameterResolver::GetBuffer(
		const FRDGBufferParameter& Parameter) const -> FRHIBuffer*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Buffer,
			ERDGParameterMemberKind::Buffer, false);
		return Resources.GetBuffer(Parameter.Buffer);
	}

	auto FRDGParameterResolver::GetBuffer(const std::optional<
		FRDGBufferParameter>& Parameter) const -> FRHIBuffer*
	{
		FindMember(&Parameter, ERDGParameterMemberKind::Buffer,
			ERDGParameterMemberKind::Buffer, true);
		return Parameter ? Resources.GetBuffer(Parameter->Buffer) : nullptr;
	}

	namespace
	{
		auto MakeAttachmentView(const FRDGPassResources& Resources,
			FRDGTextureHandle Texture,
			const FRHITextureSubresourceRange& Range,
			const FRDGParameterMemberMetadata& Member)
			-> FRDGAttachmentView
		{
			return {Resources.GetTexture(Texture), Range, Member.LoadAction,
				Member.StoreAction, Member.bPassManagedTransition,
				Member.ResultAccess};
		}
	}

	auto FRDGParameterResolver::GetColorAttachment(
		const FRDGColorAttachmentParameter& Parameter) const
		-> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::ColorAttachment,
			ERDGParameterMemberKind::ManagedColorAttachment, false);
		return MakeAttachmentView(Resources, Parameter.Texture, Parameter.Range,
			Member);
	}

	auto FRDGParameterResolver::GetColorAttachment(const std::optional<
		FRDGColorAttachmentParameter>& Parameter) const
		-> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::ColorAttachment,
			ERDGParameterMemberKind::ManagedColorAttachment, true);
		return Parameter ? MakeAttachmentView(Resources, Parameter->Texture,
			Parameter->Range, Member) : FRDGAttachmentView{};
	}

	auto FRDGParameterResolver::GetDepthStencilAttachment(
		const FRDGDepthStencilAttachmentParameter& Parameter) const
		-> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::DepthStencilAttachment,
			ERDGParameterMemberKind::ManagedDepthStencilAttachment, false);
		return MakeAttachmentView(Resources, Parameter.Texture, Parameter.Range,
			Member);
	}

	auto FRDGParameterResolver::GetDepthStencilAttachment(
		const std::optional<FRDGDepthStencilAttachmentParameter>& Parameter)
		const -> FRDGAttachmentView
	{
		const auto& Member = FindMember(&Parameter,
			ERDGParameterMemberKind::DepthStencilAttachment,
			ERDGParameterMemberKind::ManagedDepthStencilAttachment, true);
		return Parameter ? MakeAttachmentView(Resources, Parameter->Texture,
			Parameter->Range, Member) : FRDGAttachmentView{};
	}

	FRDGAllocatedResources::FRDGAllocatedResources(uint32 Count)
		: Textures(Count), Buffers(Count), AllocationIds(Count),
		  AllocationDispositions(Count)
	{
	}

}
