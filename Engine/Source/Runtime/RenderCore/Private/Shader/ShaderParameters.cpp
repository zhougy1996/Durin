#include "Shader/Shader.h"
#include "ShaderBindingInternal.h"

namespace Durin
{
	namespace
	{
		using ShaderPrivate::AreShaderBindingTypesCompatible;

		auto FlattenShaderParameterMembers(
			const FShaderParametersMetadata* ParametersMetadata,
			std::vector<FShaderParameterMemberMetadata>& OutMembers,
			std::vector<const FShaderParametersMetadata*>& TraversalStack
		) -> void
		{
			if (ParametersMetadata == nullptr)
			{
				return;
			}

			const auto ExistingIt = std::ranges::find(TraversalStack, ParametersMetadata);
			checkf(ExistingIt == TraversalStack.end(), "Shader parameter metadata include cycle detected");
			TraversalStack.push_back(ParametersMetadata);

			FlattenShaderParameterMembers(ParametersMetadata->IncludedParameters, OutMembers, TraversalStack);
			for (const FShaderParameterMemberMetadata& Member : ParametersMetadata->Members)
			{
				if (Member.Name != nullptr && Member.Name[0] != '\0')
				{
					const auto DuplicateIt = std::ranges::find_if(OutMembers, [&Member](const FShaderParameterMemberMetadata& ExistingMember) {
						return ExistingMember.Name != nullptr && std::string_view(ExistingMember.Name) == Member.Name;
					});
					checkf(DuplicateIt == OutMembers.end(), "Duplicate shader parameter member '{}'", Member.Name);
				}

				OutMembers.push_back(Member);
			}

			TraversalStack.pop_back();
		}
	} // namespace

	auto BuildShaderParameterBindings(
		const FShaderParametersMetadata* ParametersMetadata,
		const FShaderReflectionData& Reflection,
		std::vector<FShaderParameterBinding>& OutBindings) -> FShaderOperationResult
	{
		OutBindings.clear();

		const std::span<const FShaderParameterMemberMetadata> ParameterMetadata = ParametersMetadata ? ParametersMetadata->Members : std::span<const FShaderParameterMemberMetadata>{};
		for (const FShaderParameterMemberMetadata& Parameter : ParameterMetadata)
		{
			if (Parameter.Kind != EShaderParameterMemberKind::Resource)
			{
				continue;
			}

			if (Parameter.Name == nullptr || Parameter.Name[0] == '\0')
			{
				return std::unexpected(FShaderError{.Code = EShaderError::EmptyParameterName});
			}

			const auto FoundIt = std::ranges::find_if(Reflection.ResourceBindings, [&Parameter](const FShaderResourceBinding& Binding) {
				return Binding.Name == Parameter.Name;
			});

			if (FoundIt == Reflection.ResourceBindings.end())
			{
				if (Parameter.bOptional)
				{
					continue;
				}
				return std::unexpected(FShaderError{.Code = EShaderError::MissingParameter, .Parameter = Parameter.Name});
			}

			if (!AreShaderBindingTypesCompatible(FoundIt->Type, Parameter.Type))
			{
				return std::unexpected(FShaderError{.Code = EShaderError::ParameterTypeMismatch,
					.Parameter = Parameter.Name,
					.Expected = static_cast<uint64>(Parameter.Type),
					.Actual = static_cast<uint64>(FoundIt->Type)});
			}

			if (FoundIt->ArraySize != Parameter.ArraySize)
			{
				return std::unexpected(FShaderError{.Code = EShaderError::ParameterArraySizeMismatch,
					.Parameter = Parameter.Name,
					.Expected = Parameter.ArraySize,
					.Actual = FoundIt->ArraySize});
			}

			FShaderParameterBinding Binding;
			Binding.Name = Parameter.Name;
			Binding.Offset = Parameter.Offset;
			Binding.SetIndex = FoundIt->SetIndex;
			Binding.BindingIndex = FoundIt->BindingIndex;
			Binding.Type = Parameter.Type;
			Binding.ArraySize = FoundIt->ArraySize;
			Binding.bGraphResource = Parameter.bGraphResource;
			OutBindings.push_back(Binding);
		}

		return {};
	}

	auto BuildCombinedShaderParametersMetadataStorage(
		std::string_view StructName,
		uint32 StructSize,
		uint32 StructAlignment,
		std::span<const FShaderParameterMemberMetadata> OwnMembers,
		const FShaderParametersMetadata* IncludedParameters
	) -> FShaderParametersMetadataStorage
	{
		FShaderParametersMetadataStorage Storage;
		Storage.OwnedMembers.reserve(OwnMembers.size() + (IncludedParameters ? IncludedParameters->Members.size() : 0));

		std::vector<const FShaderParametersMetadata*> TraversalStack;
		FlattenShaderParameterMembers(IncludedParameters, Storage.OwnedMembers, TraversalStack);
		for (const FShaderParameterMemberMetadata& Member : OwnMembers)
		{
			if (Member.Name != nullptr && Member.Name[0] != '\0')
			{
				const auto DuplicateIt = std::ranges::find_if(Storage.OwnedMembers, [&Member](const FShaderParameterMemberMetadata& ExistingMember) {
					return ExistingMember.Name != nullptr && std::string_view(ExistingMember.Name) == Member.Name;
				});
				checkf(DuplicateIt == Storage.OwnedMembers.end(), "Duplicate shader parameter member '{}'", Member.Name);
			}

			Storage.OwnedMembers.push_back(Member);
		}

		Storage.Metadata.StructName = StructName.data();
		Storage.Metadata.StructSize = StructSize;
		Storage.Metadata.StructAlignment = StructAlignment;
		Storage.Metadata.IncludedParameters = IncludedParameters;
		Storage.Metadata.Members = Storage.OwnedMembers;
		return Storage;
	}

	static auto ResolveShaderParameterResources(
		const FShaderParametersMetadata& ParametersMetadata,
		std::span<const FShaderParameterBinding> ParameterBindings,
		const void* ParameterData
	) -> std::vector<FRHIShaderParameterResource>
	{
		checkf(ParameterData != nullptr, "Shader parameter data must not be null");
		checkf(
			ParametersMetadata.IncludedParameters == nullptr,
			"SetShaderParameters does not support included shader parameter metadata until parameter layout composition is implemented"
		);
		for (const FShaderParameterMemberMetadata& Member : ParametersMetadata.Members)
		{
			checkf(
				Member.Kind == EShaderParameterMemberKind::Resource,
				"Unsupported shader parameter kind {} in '{}'",
				static_cast<uint32>(Member.Kind),
				Member.Name ? Member.Name : "<unnamed>"
			);
		}

		size_t ResolvedCount = 0;
		for (const FShaderParameterBinding& Binding : ParameterBindings)
			ResolvedCount += Binding.ArraySize;
		std::vector<FRHIShaderParameterResource> ResourceParameters;
		ResourceParameters.reserve(ResolvedCount);

		const auto* ParameterBytes = reinterpret_cast<const std::byte*>(ParameterData);
		for (size_t BindingIndex = 0; BindingIndex < ParameterBindings.size(); ++BindingIndex)
		{
			const FShaderParameterBinding& Binding = ParameterBindings[BindingIndex];
			const size_t ElementSize = Binding.Type == ERHIBindingType::UniformBuffer
				|| Binding.Type == ERHIBindingType::UniformBufferDynamic
					? sizeof(FRHIUniformBufferRange)
					: Binding.Type == ERHIBindingType::StorageBuffer
						? sizeof(FRHIStorageBufferRange) : sizeof(FRHIResource*);
			checkf(Binding.ArraySize > 0 && Binding.Offset <= ParametersMetadata.StructSize
				&& static_cast<size_t>(Binding.ArraySize) * ElementSize
					<= ParametersMetadata.StructSize - Binding.Offset,
				"Shader parameter binding array is out of bounds");
			for (uint32 ArrayElement = 0; ArrayElement < Binding.ArraySize;
				++ArrayElement)
			{
				FRHIShaderParameterResource& ResourceParameter =
					ResourceParameters.emplace_back();
				ResourceParameter.SetIndex = Binding.SetIndex;
				ResourceParameter.BindingIndex = Binding.BindingIndex;
				ResourceParameter.ArrayElement = ArrayElement;
				ResourceParameter.Type = Binding.Type;
				const std::byte* ElementBytes = ParameterBytes + Binding.Offset
					+ static_cast<size_t>(ArrayElement) * ElementSize;
				if (Binding.Type == ERHIBindingType::UniformBuffer
					|| Binding.Type == ERHIBindingType::UniformBufferDynamic)
				{
					const auto* Range = reinterpret_cast<const FRHIUniformBufferRange*>(ElementBytes);
					ResourceParameter.Resource = Range->Buffer;
					ResourceParameter.Offset = Range->Offset;
					ResourceParameter.Size = Range->Size;
				}
				else if (Binding.Type == ERHIBindingType::StorageBuffer)
				{
					const auto* Range = reinterpret_cast<const FRHIStorageBufferRange*>(ElementBytes);
					ResourceParameter.Resource = Range->Buffer;
					ResourceParameter.Offset = Range->Offset;
					ResourceParameter.Size = Range->Size;
				}
				else
				{
					ResourceParameter.Resource =
						*reinterpret_cast<FRHIResource* const*>(ElementBytes);
				}
			}
		}

		return ResourceParameters;
	}

	auto PrepareShaderParametersImpl(FRHIShader* Shader,
		const FShaderParametersMetadata& Metadata,
		std::span<const FShaderParameterBinding> Bindings, const void* Data)
		-> std::shared_ptr<const FRHIShaderParameterBatch>
	{
		return FRHIShaderParameterBatch::Create(Shader, ResolveShaderParameterResources(Metadata, Bindings, Data));
	}

	auto SetShaderParametersImpl(FRHICommandListBase& Commands, FRHIShader* Shader,
		const FShaderParametersMetadata& Metadata,
		std::span<const FShaderParameterBinding> Bindings, const void* Data) -> void
	{
		Commands.SetShaderParameters(Shader, ResolveShaderParameterResources(Metadata, Bindings, Data));
	}

	auto SetRDGShaderParametersImpl(
		FRHICommandListBase& RHICmdList,
		FRHIShader* RHIShader,
		std::string_view ShaderName,
		EShaderFrequency ShaderFrequency,
		std::span<const FShaderParameterBinding> ParameterBindings,
		const FRDGShaderParameterScope& GraphParameters,
		const FShaderParametersMetadata* OrdinaryParametersMetadata,
		const void* OrdinaryParameterData) -> void
	{
		const FRDGParameterResolver& Resolver =
			GraphParameters.GetResolver();
		const bool bComputeShader = ShaderFrequency == EShaderFrequency::Compute;
		const bool bGraphicsShader = ShaderFrequency == EShaderFrequency::Vertex
			|| ShaderFrequency == EShaderFrequency::Fragment;
		checkf((bComputeShader
			&& Resolver.GetPassType() == ERDGPassType::Compute)
			|| (bGraphicsShader
				&& Resolver.GetPassType() == ERDGPassType::Graphics),
			"Render graph pass '{}' domain is incompatible with shader '{}' frequency",
			Resolver.GetPassName(), ShaderName);
		checkf(GraphParameters.GetData() != nullptr
			&& GraphParameters.GetMetadata() != nullptr,
			"Render graph pass '{}' has unavailable composed shader parameters",
			Resolver.GetPassName());

		const FRDGParameterLayout* Layout = GraphParameters.GetLayout();
		checkf(Layout != nullptr,
			"Render graph pass '{}' has unavailable composed parameter layout",
			Resolver.GetPassName());

		size_t ResolvedCount = 0;
		for (const FShaderParameterBinding& Binding : ParameterBindings)
			ResolvedCount += Binding.ArraySize;
		std::vector<FRHIShaderParameterResource> Resources;
		Resources.reserve(ResolvedCount);
		std::vector<TRefCountPtr<FRHIResource>> ExactViews;
		ExactViews.reserve(ResolvedCount);
		const auto* OrdinaryBytes = static_cast<const std::byte*>(
			OrdinaryParameterData);

		for (const FShaderParameterBinding& Binding : ParameterBindings)
		{
			const auto Found = std::lower_bound(Layout->ShaderBindings.begin(),
				Layout->ShaderBindings.end(), Binding.Name,
				[](const FRDGParameterShaderBinding& Candidate,
					std::string_view Name) { return Candidate.Name < Name; });
			if (Found != Layout->ShaderBindings.end() && Found->Name == Binding.Name)
			{
				const FRDGParameterLayoutLeaf& Leaf =
					Layout->Leaves[Found->LeafIndex];
				const FRDGParameterMemberMetadata& Member = *Leaf.Metadata;
				const void* LeafData = static_cast<const std::byte*>(
					GraphParameters.GetData()) + Leaf.Offset;
				checkf(Member.ShaderBindingType == Binding.Type,
					"Render graph pass '{}' parameter '{}' shader binding '{}' type "
					"does not match shader '{}'",
					Resolver.GetPassName(), Leaf.Path, Binding.Name, ShaderName);
				checkf(Member.ArraySize == Binding.ArraySize,
					"Render graph pass '{}' parameter '{}' shader binding '{}' array "
					"extent does not match shader '{}'",
					Resolver.GetPassName(), Leaf.Path, Binding.Name, ShaderName);

				for (uint32 ArrayElement = 0; ArrayElement < Binding.ArraySize;
					++ArrayElement)
				{
					const void* ElementData = static_cast<const std::byte*>(LeafData)
						+ static_cast<size_t>(ArrayElement) * Member.ElementSize;
					FRHIShaderParameterResource Parameter{
						.SetIndex = Binding.SetIndex,
						.BindingIndex = Binding.BindingIndex,
						.ArrayElement = ArrayElement,
						.Type = Binding.Type};
					if (Member.Kind == ERDGParameterMemberKind::Texture)
					{
						const FRDGTextureParameter* GraphTexture = nullptr;
						if (Member.bOptional)
						{
							const auto& Optional = *static_cast<const std::optional<
								FRDGTextureParameter>*>(ElementData);
							checkf(Optional.has_value(),
								"Render graph pass '{}' parameter '{}[{}]' is unavailable "
								"for required shader '{}' binding '{}'",
								Resolver.GetPassName(), Leaf.Path, ArrayElement,
								ShaderName, Binding.Name);
							GraphTexture = &*Optional;
						}
						else GraphTexture = static_cast<const
							FRDGTextureParameter*>(ElementData);
						FRHITexture* Texture = Resolver.GetTexture(*GraphTexture);
						if (Texture->GetResourceType()
							== ERHIResourceType::TextureReference)
							Texture = static_cast<FRHITextureReference*>(Texture)
								->GetReferencedTexture_RenderThread();
						checkf(Texture != nullptr && Texture->GetResourceType()
							== ERHIResourceType::Texture,
							"Render graph pass '{}' parameter '{}' resolved an unavailable "
							"texture backing", Resolver.GetPassName(), Leaf.Path);
						FRHITextureViewDesc Desc = MakeDefaultTextureViewDesc(*Texture,
							Binding.Type == ERHIBindingType::StorageImage
								? ERHITextureViewUsage::Storage
								: ERHITextureViewUsage::Sampled);
						Desc.Range = GraphTexture->Range;
                        // A cube storage write addresses a 2D face (or a face array),
                        // while sampling retains the cube interpretation.
                        if (Binding.Type == ERHIBindingType::StorageImage
                            && Texture->GetDimension() == ETextureDimension::TextureCube)
                            Desc.Dimension = Desc.Range.NumArrayLayers == 1
                                ? ERHITextureViewDimension::Texture2D : ERHITextureViewDimension::Texture2DArray;
						FTextureViewRHIRef View;
						if (GDynamicRHI)
							View = GDynamicRHI->RHIGetOrCreateTextureView(Texture, Desc);
						else
						{
							if (ValidateTextureViewDesc(Texture, Desc))
								View = new FRHITextureView(Texture, Desc);
						}
						checkf(View,
							"Render graph pass '{}' parameter '{}' could not create "
							"an exact view for shader '{}' binding '{}'",
							Resolver.GetPassName(), Leaf.Path, ShaderName, Binding.Name);
						Parameter.Resource = View.GetReference();
						ExactViews.emplace_back(View.GetReference());
					}
					else
					{
						const FRDGBufferParameter* GraphBuffer = nullptr;
						if (Member.bOptional)
						{
							const auto& Optional = *static_cast<const std::optional<
								FRDGBufferParameter>*>(ElementData);
							checkf(Optional.has_value(),
								"Render graph pass '{}' parameter '{}[{}]' is unavailable "
								"for required shader '{}' binding '{}'",
								Resolver.GetPassName(), Leaf.Path, ArrayElement,
								ShaderName, Binding.Name);
							GraphBuffer = &*Optional;
						}
						else GraphBuffer = static_cast<const
							FRDGBufferParameter*>(ElementData);
						Parameter.Resource = Resolver.GetBuffer(*GraphBuffer);
						Parameter.Offset = static_cast<uint32>(GraphBuffer->Offset);
						Parameter.Size = static_cast<uint32>(GraphBuffer->Size);
						checkf(Parameter.Offset == GraphBuffer->Offset
							&& Parameter.Size == GraphBuffer->Size,
							"Render graph pass '{}' parameter '{}' buffer range exceeds "
							"shader submission limits", Resolver.GetPassName(), Leaf.Path);
					}
					Resources.push_back(Parameter);
				}
				continue;
			}

			checkf(!Binding.bGraphResource,
				"Render graph pass '{}' shader '{}' binding '{}' is declared as a "
				"graph resource but has no composed graph member",
				Resolver.GetPassName(), ShaderName, Binding.Name);
			checkf(OrdinaryParametersMetadata != nullptr
				&& OrdinaryBytes != nullptr,
				"Render graph pass '{}' shader '{}' binding '{}' has no composed "
				"graph member or ordinary parameter source",
				Resolver.GetPassName(), ShaderName, Binding.Name);
			const size_t ElementSize = Binding.Type == ERHIBindingType::UniformBuffer
				|| Binding.Type == ERHIBindingType::UniformBufferDynamic
					? sizeof(FRHIUniformBufferRange)
					: Binding.Type == ERHIBindingType::StorageBuffer
						? sizeof(FRHIStorageBufferRange) : sizeof(FRHIResource*);
			checkf(Binding.Offset <= OrdinaryParametersMetadata->StructSize
				&& static_cast<size_t>(Binding.ArraySize) * ElementSize
					<= OrdinaryParametersMetadata->StructSize - Binding.Offset,
				"Shader '{}' ordinary parameter binding '{}' is out of bounds",
				ShaderName, Binding.Name);
			for (uint32 ArrayElement = 0; ArrayElement < Binding.ArraySize;
				++ArrayElement)
			{
				FRHIShaderParameterResource Parameter{
					.SetIndex = Binding.SetIndex,
					.BindingIndex = Binding.BindingIndex,
					.ArrayElement = ArrayElement,
					.Type = Binding.Type};
				const std::byte* ElementBytes = OrdinaryBytes + Binding.Offset
					+ static_cast<size_t>(ArrayElement) * ElementSize;
				if (Binding.Type == ERHIBindingType::UniformBuffer
					|| Binding.Type == ERHIBindingType::UniformBufferDynamic)
				{
					const auto& Range = *reinterpret_cast<const
						FRHIUniformBufferRange*>(ElementBytes);
					Parameter.Resource = Range.Buffer;
					Parameter.Offset = Range.Offset;
					Parameter.Size = Range.Size;
				}
				else if (Binding.Type == ERHIBindingType::StorageBuffer)
				{
					const auto& Range = *reinterpret_cast<const
						FRHIStorageBufferRange*>(ElementBytes);
					Parameter.Resource = Range.Buffer;
					Parameter.Offset = Range.Offset;
					Parameter.Size = Range.Size;
				}
				else Parameter.Resource =
					*reinterpret_cast<FRHIResource* const*>(ElementBytes);
				Resources.push_back(Parameter);
			}
		}

		RHICmdList.SetShaderParameters(RHIShader, Resources);
	}

}
