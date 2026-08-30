#include "Shader/Shader.h"

#include "DynamicRHI.h"
#include "ShaderCompileService.h"
#include "ShaderCompileUtilities.h"

namespace Durin
{
	namespace
	{
		struct FShaderBindingKey
		{
			uint32 SetIndex = 0;
			uint32 BindingIndex = 0;

			auto operator==(const FShaderBindingKey& Other) const -> bool
			{
				return SetIndex == Other.SetIndex && BindingIndex == Other.BindingIndex;
			}
		};

		struct FShaderBindingKeyHasher
		{
			auto operator()(const FShaderBindingKey& Key) const noexcept -> size_t
			{
				return std::hash<uint64>{}((static_cast<uint64>(Key.SetIndex) << 32) | Key.BindingIndex);
			}
		};

		class FShaderTypeRegistry
		{
		public:
			auto Register(const FShaderType* ShaderType) -> void
			{
				checkf(ShaderType, "Shader type must be valid");
				const FName TypeName = ShaderType->GetFName();
				const auto FoundIt = std::ranges::find_if(ShaderTypes, [TypeName](const FShaderType* ExistingType) {
					return ExistingType && ExistingType->GetFName() == TypeName;
				});
				checkf(FoundIt == ShaderTypes.end(), "Duplicate shader type registration: {}", TypeName.ToString());
				ShaderTypes.push_back(ShaderType);
			}

			auto Unregister(const FShaderType* ShaderType) -> void
			{
				const auto FoundIt = std::ranges::find(ShaderTypes, ShaderType);
				if (FoundIt != ShaderTypes.end())
				{
					ShaderTypes.erase(FoundIt);
				}
			}

			auto GetTypeList() const -> const std::vector<const FShaderType*>&
			{
				return ShaderTypes;
			}

		private:
			std::vector<const FShaderType*> ShaderTypes;
		};

		auto GetShaderTypeRegistry() -> FShaderTypeRegistry&
		{
			static FShaderTypeRegistry Registry;
			return Registry;
		}

		auto MakeDefaultShaderInstance(
			const FShaderType* ShaderType,
			FShaderMapBase* ShaderMap,
			const FShaderReflectionData& Reflection
		) -> std::unique_ptr<FShader>
		{
			return std::make_unique<FShader>(ShaderType, ShaderMap, Reflection);
		}

		struct FShaderMapCacheEntry
		{
			std::shared_ptr<FShaderMapResourceCode> Code;
			std::shared_ptr<FShaderMapResource> Resource;
		};

		struct FWeakShaderMapCacheEntry
		{
			std::weak_ptr<FShaderMapResourceCode> Code;
			std::weak_ptr<FShaderMapResource> Resource;
		};

		class FShaderMapResourceCache
		{
		public:
			auto FindOrAdd(const FXxHash128& Key, const FShaderCompilerOutput& Output) -> FShaderMapCacheEntry
			{
				std::lock_guard Lock(Mutex);
				if (const auto FoundIt = Entries.find(Key); FoundIt != Entries.end())
				{
					FShaderMapCacheEntry Existing{FoundIt->second.Code.lock(), FoundIt->second.Resource.lock()};
					if (Existing.Code && Existing.Resource)
					{
						return Existing;
					}
					Entries.erase(FoundIt);
				}

				FShaderMapCacheEntry Entry;
				Entry.Code = std::make_shared<FShaderMapResourceCode>();
				Entry.Resource = std::make_shared<FShaderMapResource>(Entry.Code);
				Entry.Resource->AddShaderCompilerOutput(Output);
				Entries.emplace(Key, FWeakShaderMapCacheEntry{Entry.Code, Entry.Resource});
				return Entry;
			}

			auto GetStats() -> FShaderMapResourceCacheStats
			{
				std::lock_guard Lock(Mutex);
				for (auto It = Entries.begin(); It != Entries.end();)
				{
					if (It->second.Code.expired() || It->second.Resource.expired()) It = Entries.erase(It);
					else ++It;
				}
				return {.EntryCount = Entries.size(), .LiveEntryCount = Entries.size()};
			}

			auto Clear() -> void
			{
				std::lock_guard Lock(Mutex);
				Entries.clear();
			}

		private:
			std::mutex Mutex;
			std::unordered_map<FXxHash128, FWeakShaderMapCacheEntry> Entries;
		};

		auto GetShaderMapResourceCache() -> FShaderMapResourceCache&
		{
			static FShaderMapResourceCache Cache;
			return Cache;
		}

		auto AreShaderBindingTypesCompatible(ERHIBindingType ReflectedType, ERHIBindingType ParameterType) -> bool
		{
			if (ReflectedType == ParameterType)
			{
				return true;
			}
			return ReflectedType == ERHIBindingType::UniformBuffer && ParameterType == ERHIBindingType::UniformBufferDynamic;
		}

		auto ApplyShaderParameterBindingOverrides(
			const FShaderType& ShaderType,
			const FShaderReflectionData& InReflection,
			FShaderReflectionData& OutReflection
		) -> void
		{
			OutReflection = InReflection;
			for (FShaderResourceBinding& ResourceBinding : OutReflection.ResourceBindings)
			{
				const std::span<const FShaderParameterMemberMetadata> ParameterMetadata = ShaderType.GetParameterMetadata();
				const auto FoundIt = std::ranges::find_if(ParameterMetadata, [&ResourceBinding](const FShaderParameterMemberMetadata& Parameter) {
					return Parameter.Kind == EShaderParameterMemberKind::Resource
						&& Parameter.Name != nullptr
						&& ResourceBinding.Name == Parameter.Name;
				});
				if (FoundIt != ParameterMetadata.end() && AreShaderBindingTypesCompatible(ResourceBinding.Type, FoundIt->Type))
				{
					ResourceBinding.Type = FoundIt->Type;
				}
			}
		}

		template <typename TBuilder>
		auto UpdateHashStringField(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
		}

		auto BuildShaderMapCompileOptions(
			std::span<const FShaderType* const> ShaderTypes,
			const FShaderCompileOptions* InCompileOptions,
			FShaderCompileOptions& OutCompileOptions,
			std::string& OutErrorMessage
		) -> bool
		{
			OutCompileOptions = {};
			OutErrorMessage.clear();

			if (InCompileOptions)
			{
				OutCompileOptions.Macros = InCompileOptions->Macros;
				OutCompileOptions.bForceRecompile = InCompileOptions->bForceRecompile;
			}

			if (ShaderTypes.empty())
			{
				if (InCompileOptions && !InCompileOptions->VirtualShaderPath.empty())
				{
					OutCompileOptions.VirtualShaderPath = InCompileOptions->VirtualShaderPath;
				}
				return true;
			}

			const FShaderType* FirstShaderType = ShaderTypes.front();
			checkf(FirstShaderType, "Shader type must not be null");

			const std::string_view ShaderPath = FirstShaderType->GetVirtualShaderPath();
			if (ShaderPath.empty())
			{
				OutErrorMessage = "Shader type virtual shader path must not be empty";
				return false;
			}

			if (InCompileOptions && !InCompileOptions->VirtualShaderPath.empty() && InCompileOptions->VirtualShaderPath != ShaderPath)
			{
				OutErrorMessage = std::format(
					"Shader compile options virtual path '{}' does not match shader type virtual path '{}'",
					InCompileOptions->VirtualShaderPath,
					ShaderPath
				);
				return false;
			}

			OutCompileOptions.VirtualShaderPath = std::string(ShaderPath);
			OutCompileOptions.EntryPoints.reserve(ShaderTypes.size());
			OutCompileOptions.Frequencies.reserve(ShaderTypes.size());

			for (size_t ShaderTypeIndex = 0; ShaderTypeIndex < ShaderTypes.size(); ++ShaderTypeIndex)
			{
				const FShaderType* ShaderType = ShaderTypes[ShaderTypeIndex];
				checkf(ShaderType, "Shader type must not be null");

				if (ShaderType->GetVirtualShaderPath() != ShaderPath)
				{
					OutErrorMessage = std::format(
						"Shader type '{}' uses path '{}' but shader map expects '{}'",
						ShaderType->GetName(),
						ShaderType->GetVirtualShaderPath(),
						ShaderPath
					);
					return false;
				}

				OutCompileOptions.EntryPoints.push_back(ShaderType->GetEntryPoint().data());
				OutCompileOptions.Frequencies.push_back(ShaderType->GetFrequency());

				if (InCompileOptions && !InCompileOptions->EntryPoints.empty())
				{
					if (ShaderTypeIndex >= InCompileOptions->EntryPoints.size()
						|| std::string_view(InCompileOptions->EntryPoints[ShaderTypeIndex]) != ShaderType->GetEntryPoint())
					{
						OutErrorMessage = std::format(
							"Shader compile options entry point at index {} does not match shader type '{}'",
							ShaderTypeIndex,
							ShaderType->GetName()
						);
						return false;
					}
				}

				if (InCompileOptions && !InCompileOptions->Frequencies.empty())
				{
					if (ShaderTypeIndex >= InCompileOptions->Frequencies.size()
						|| InCompileOptions->Frequencies[ShaderTypeIndex] != ShaderType->GetFrequency())
					{
						OutErrorMessage = std::format(
							"Shader compile options frequency at index {} does not match shader type '{}'",
							ShaderTypeIndex,
							ShaderType->GetName()
						);
						return false;
					}
				}
			}

			if (InCompileOptions && !InCompileOptions->EntryPoints.empty() && InCompileOptions->EntryPoints.size() != ShaderTypes.size())
			{
				OutErrorMessage = std::format(
					"Shader compile options entry point count ({}) does not match shader type count ({})",
					InCompileOptions->EntryPoints.size(),
					ShaderTypes.size()
				);
				return false;
			}

			if (InCompileOptions && !InCompileOptions->Frequencies.empty() && InCompileOptions->Frequencies.size() != ShaderTypes.size())
			{
				OutErrorMessage = std::format(
					"Shader compile options frequency count ({}) does not match shader type count ({})",
					InCompileOptions->Frequencies.size(),
					ShaderTypes.size()
				);
				return false;
			}

			return true;
		}

		auto BuildShaderMapCacheKey(
			const FShaderCompileOptions& CompileOptions,
			const FShaderCompilerOutput& Output,
			FXxHash128& OutCacheKey,
			std::string& OutErrorMessage
		) -> bool
		{
			OutCacheKey = {};
			OutErrorMessage.clear();

			if (CompileOptions.VirtualShaderPath.empty())
			{
				return true;
			}

			std::vector<FShaderMacroDefinition> NormalizedMacros;
			if (!ShaderCompileUtilities::NormalizeMacros(CompileOptions, NormalizedMacros, OutErrorMessage))
			{
				return false;
			}

			FXxHash128Builder Builder;
			UpdateHashStringField(Builder, "DurinShaderMapCacheKey_v1");
			UpdateHashStringField(Builder, CompileOptions.VirtualShaderPath);

			const uint64 EntryPointCount = static_cast<uint64>(CompileOptions.EntryPoints.size());
			Builder.UpdateValue(EntryPointCount);
			for (const char8* EntryPoint : CompileOptions.EntryPoints)
			{
				UpdateHashStringField(Builder, EntryPoint ? std::string_view(EntryPoint) : std::string_view{});
			}

			const uint64 FrequencyCount = static_cast<uint64>(CompileOptions.Frequencies.size());
			Builder.UpdateValue(FrequencyCount);
			for (EShaderFrequency Frequency : CompileOptions.Frequencies)
			{
				Builder.UpdateValue(Frequency);
			}

			const uint64 MacroCount = static_cast<uint64>(NormalizedMacros.size());
			Builder.UpdateValue(MacroCount);
			for (const FShaderMacroDefinition& Macro : NormalizedMacros)
			{
				UpdateHashStringField(Builder, Macro.Name);
				Builder.UpdateValue(Macro.HasValue());
				if (Macro.Value)
				{
					UpdateHashStringField(Builder, *Macro.Value);
				}
			}

			const uint64 ShaderCount = static_cast<uint64>(Output.CompiledShaders.size());
			Builder.UpdateValue(ShaderCount);
			for (const FCompiledShader& CompiledShader : Output.CompiledShaders)
			{
				Builder.UpdateValue(CompiledShader.Hash);
			}

			OutCacheKey = Builder.Finalize();
			return true;
		}

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

	FShaderType::FShaderType(
		std::string_view InName,
		std::string_view InVirtualShaderPath,
		EShaderFrequency InFrequency,
		std::string_view InEntryPoint,
		std::string_view InDebugName,
		FShaderFactoryFunction InFactory,
		FShouldCompilePermutationFunction InShouldCompilePermutation,
		FModifyCompilationEnvironmentFunction InModifyCompilationEnvironment,
		const FShaderParametersMetadata* InParametersMetadata
	)
		: Name(InName)
		, TypeName(Name)
		, VirtualShaderPath(InVirtualShaderPath)
		, Frequency(InFrequency)
		, EntryPoint(InEntryPoint)
		, DebugName(InDebugName.empty() ? InName : InDebugName)
		, ParametersMetadata(InParametersMetadata)
		, Factory(InFactory ? InFactory : &MakeDefaultShaderInstance)
		, ShouldCompilePermutationFn(InShouldCompilePermutation)
		, ModifyCompilationEnvironmentFn(InModifyCompilationEnvironment)
	{
		GetShaderTypeRegistry().Register(this);
	}

	FShaderType::~FShaderType()
	{
		GetShaderTypeRegistry().Unregister(this);
	}

	auto FShaderType::CreateShaderInstance(FShaderMapBase* ShaderMap, const FShaderReflectionData& Reflection) const -> std::unique_ptr<FShader>
	{
		return Factory(this, ShaderMap, Reflection);
	}

	auto FShaderType::ShouldCompilePermutation(const FShaderPermutationParameters& Parameters) const -> bool
	{
		return ShouldCompilePermutationFn ? ShouldCompilePermutationFn(Parameters) : true;
	}

	auto FShaderType::ModifyCompilationEnvironment(const FShaderPermutationParameters& Parameters, FShaderCompileOptions& CompileOptions) const -> void
	{
		if (ModifyCompilationEnvironmentFn)
		{
			ModifyCompilationEnvironmentFn(Parameters, CompileOptions);
		}
	}

	auto FShaderType::GetTypeList() -> const std::vector<const FShaderType*>&
	{
		return GetShaderTypeRegistry().GetTypeList();
	}

	FShader::FShader(const FShaderType* InType, FShaderMapBase* InShaderMap, const FShaderReflectionData& InReflection)
		: Type(InType)
		, ShaderMap(InShaderMap)
		, Reflection(InReflection)
	{
	}

	auto FShader::GetOrCreateRHIShader(bool bRequired) -> FRHIShader*
	{
		return (ShaderMap && Type) ? ShaderMap->GetOrCreateShaderRHI(Type, bRequired) : nullptr;
	}

	auto FShader::InitializeParameterBindings(std::string& OutErrorMessage) -> bool
	{
		ParameterBindings.clear();
		return BuildShaderParameterBindings(Type ? Type->GetParametersMetadata() : nullptr, Reflection, ParameterBindings, OutErrorMessage);
	}

	auto BuildShaderParameterBindings(
		const FShaderParametersMetadata* ParametersMetadata,
		const FShaderReflectionData& Reflection,
		std::vector<FShaderParameterBinding>& OutBindings,
		std::string& OutErrorMessage
	) -> bool
	{
		OutBindings.clear();
		OutErrorMessage.clear();

		const std::span<const FShaderParameterMemberMetadata> ParameterMetadata = ParametersMetadata ? ParametersMetadata->Members : std::span<const FShaderParameterMemberMetadata>{};
		for (const FShaderParameterMemberMetadata& Parameter : ParameterMetadata)
		{
			if (Parameter.Kind != EShaderParameterMemberKind::Resource)
			{
				continue;
			}

			if (Parameter.Name == nullptr || Parameter.Name[0] == '\0')
			{
				OutErrorMessage = "Shader parameter metadata contains an empty name";
				return false;
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
				OutErrorMessage = std::format("Shader parameter '{}' was not found in shader reflection", Parameter.Name);
				return false;
			}

			if (!AreShaderBindingTypesCompatible(FoundIt->Type, Parameter.Type))
			{
				OutErrorMessage = std::format("Shader parameter '{}' type does not match reflection", Parameter.Name);
				return false;
			}

			if (FoundIt->ArraySize != Parameter.ArraySize)
			{
				OutErrorMessage = std::format("Shader parameter '{}' array size does not match reflection", Parameter.Name);
				return false;
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

		return true;
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

	auto SetShaderParametersImpl(
		FRHICommandListBase& RHICmdList,
		FRHIShader* RHIShader,
		const FShaderParametersMetadata& ParametersMetadata,
		std::span<const FShaderParameterBinding> ParameterBindings,
		const void* ParameterData
	) -> void
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

		RHICmdList.SetShaderParameters(RHIShader, ResourceParameters);
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

		struct FComposedMember
		{
			const FRDGParameterMemberMetadata* Metadata = nullptr;
			const void* Data = nullptr;
			std::string Path;
		};
		std::vector<FComposedMember> ComposedMembers;
		std::function<void(const void*, const FRDGParametersMetadata*,
			const std::string&)> Traverse;
		Traverse = [&](const void* StructData,
			const FRDGParametersMetadata* Metadata,
			const std::string& ParentPath) {
			const auto* Bytes = static_cast<const std::byte*>(StructData);
			for (const FRDGParameterMemberMetadata& Member : Metadata->Members)
			{
				const std::string Path = ParentPath.empty()
					? std::string(Member.Name) : ParentPath + "." + Member.Name;
				if (Member.Kind == ERDGParameterMemberKind::Nested)
				{
					for (uint32 Index = 0; Index < Member.ArraySize; ++Index)
					{
						std::string ElementPath = Path;
						if (Member.ArraySize > 1)
							ElementPath += "[" + std::to_string(Index) + "]";
						Traverse(Bytes + Member.Offset
							+ static_cast<size_t>(Index) * Member.ElementSize,
							Member.NestedParameters, ElementPath);
					}
					continue;
				}
				if (Member.bShaderBinding)
					ComposedMembers.push_back({&Member, Bytes + Member.Offset, Path});
			}
		};
		Traverse(GraphParameters.GetData(), GraphParameters.GetMetadata(), {});

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
			const auto MatchesBinding = [&](const FComposedMember& Candidate) {
				return Candidate.Metadata->ShaderBindingName != nullptr
					&& std::string_view(Candidate.Metadata->ShaderBindingName)
						== Binding.Name;
			};
			const auto Found = std::ranges::find_if(ComposedMembers, MatchesBinding);
			if (Found != ComposedMembers.end())
			{
				checkf(std::ranges::count_if(ComposedMembers, MatchesBinding) == 1,
					"Render graph pass '{}' has duplicate composed shader binding '{}'",
					Resolver.GetPassName(), Binding.Name);
				const FRDGParameterMemberMetadata& Member = *Found->Metadata;
				checkf(Member.ShaderBindingType == Binding.Type,
					"Render graph pass '{}' parameter '{}' shader binding '{}' type "
					"does not match shader '{}'",
					Resolver.GetPassName(), Found->Path, Binding.Name, ShaderName);
				checkf(Member.ArraySize == Binding.ArraySize,
					"Render graph pass '{}' parameter '{}' shader binding '{}' array "
					"extent does not match shader '{}'",
					Resolver.GetPassName(), Found->Path, Binding.Name, ShaderName);

				for (uint32 ArrayElement = 0; ArrayElement < Binding.ArraySize;
					++ArrayElement)
				{
					const void* ElementData = static_cast<const std::byte*>(Found->Data)
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
								Resolver.GetPassName(), Found->Path, ArrayElement,
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
							"texture backing", Resolver.GetPassName(), Found->Path);
						FRHITextureViewDesc Desc = MakeDefaultTextureViewDesc(*Texture,
							Binding.Type == ERHIBindingType::StorageImage
								? ERHITextureViewUsage::Storage
								: ERHITextureViewUsage::Sampled);
						Desc.Range = GraphTexture->Range;
						FTextureViewRHIRef View;
						if (GDynamicRHI)
							View = GDynamicRHI->RHIGetOrCreateTextureView(Texture, Desc);
						else
						{
							std::string Error;
							if (ValidateTextureViewDesc(Texture, Desc, Error))
								View = new FRHITextureView(Texture, Desc);
						}
						checkf(View,
							"Render graph pass '{}' parameter '{}' could not create "
							"an exact view for shader '{}' binding '{}'",
							Resolver.GetPassName(), Found->Path, ShaderName, Binding.Name);
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
								Resolver.GetPassName(), Found->Path, ArrayElement,
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
							"shader submission limits", Resolver.GetPassName(), Found->Path);
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

	auto MakeShaderCreateDesc(const FCompiledShader& CompiledShader) -> FRHIShaderCreateDesc
	{
		checkf(CompiledShader.Code, "Compiled shader code must not be null");
		FRHIShaderCreateDesc CreateDesc = FRHIShaderCreateDesc::Create(
			CompiledShader.DebugName.empty() ? nullptr : CompiledShader.DebugName.c_str(),
			CompiledShader.Frequency,
			*CompiledShader.Code,
			CompiledShader.Hash
		);
		CreateDesc.SetEntryPoint(CompiledShader.BinaryEntryPoint.c_str());
		return CreateDesc;
	}

	auto BuildPipelineLayoutFromReflection(
		std::span<const FShaderReflectionData> ReflectionData,
		FPipelineLayoutDesc& OutPipelineLayout,
		std::string& OutErrorMessage
	) -> bool
	{
		OutPipelineLayout = {};
		OutErrorMessage.clear();

		std::unordered_map<FShaderBindingKey, FShaderResourceBinding, FShaderBindingKeyHasher> MergedBindings;
		std::vector<FPushConstantRange> MergedPushConstants;
		uint32 MaxSetIndex = 0;

		for (const FShaderReflectionData& StageReflection : ReflectionData)
		{
			for (const FShaderResourceBinding& Binding : StageReflection.ResourceBindings)
			{
				FShaderBindingKey BindingKey{Binding.SetIndex, Binding.BindingIndex};
				MaxSetIndex = std::max(MaxSetIndex, Binding.SetIndex);

				auto FoundIt = MergedBindings.find(BindingKey);
				if (FoundIt == MergedBindings.end())
				{
					MergedBindings.emplace(BindingKey, Binding);
					continue;
				}

				FShaderResourceBinding& ExistingBinding = FoundIt->second;
				if (ExistingBinding.Type != Binding.Type)
				{
					OutErrorMessage = std::format(
						"Conflicting shader binding types at set {}, binding {}",
						Binding.SetIndex,
						Binding.BindingIndex
					);
					return false;
				}
				if (ExistingBinding.ArraySize != Binding.ArraySize)
				{
					OutErrorMessage = std::format(
						"Conflicting shader binding array sizes at set {}, binding {}",
						Binding.SetIndex,
						Binding.BindingIndex
					);
					return false;
				}

				ExistingBinding.StageFlags |= Binding.StageFlags;
				if (ExistingBinding.Name.empty())
				{
					ExistingBinding.Name = Binding.Name;
				}
			}

			for (const FPushConstantRange& PushConstantRange : StageReflection.PushConstantRanges)
			{
				bool bMergedRange = false;
				for (FPushConstantRange& ExistingRange : MergedPushConstants)
				{
					const uint32 ExistingBegin = ExistingRange.Offset;
					const uint32 ExistingEnd = ExistingRange.Offset + ExistingRange.Size;
					const uint32 NewBegin = PushConstantRange.Offset;
					const uint32 NewEnd = PushConstantRange.Offset + PushConstantRange.Size;

					if (ExistingRange.Offset == PushConstantRange.Offset && ExistingRange.Size == PushConstantRange.Size)
					{
						ExistingRange.StageFlags |= PushConstantRange.StageFlags;
						bMergedRange = true;
						break;
					}

					if (NewBegin < ExistingEnd && ExistingBegin < NewEnd)
					{
						OutErrorMessage = std::format(
							"Conflicting push constant ranges: existing [{}..{}), new [{}..{})",
							ExistingBegin,
							ExistingEnd,
							NewBegin,
							NewEnd
						);
						return false;
					}
				}

				if (!bMergedRange)
				{
					MergedPushConstants.push_back(PushConstantRange);
				}
			}
		}

		OutPipelineLayout.BindingLayouts.clear();
		OutPipelineLayout.BindingLayouts.resize(MergedBindings.empty() ? 0 : MaxSetIndex + 1);
		for (const auto& [Key, Binding] : MergedBindings)
		{
			OutPipelineLayout.BindingLayouts[Key.SetIndex].BindingLayouts.emplace_back(
				Binding.StageFlags,
				Binding.BindingIndex,
				Binding.Type,
				Binding.ArraySize
			);
		}

		for (FBindingLayout& BindingLayout : OutPipelineLayout.BindingLayouts)
		{
			std::ranges::sort(BindingLayout.BindingLayouts, [](const FBindingLayoutItem& A, const FBindingLayoutItem& B) {
				return A.Slot < B.Slot;
			});
		}

		std::ranges::sort(MergedPushConstants, [](const FPushConstantRange& A, const FPushConstantRange& B) {
			if (A.Offset != B.Offset)
			{
				return A.Offset < B.Offset;
			}
			return A.Size < B.Size;
		});
		OutPipelineLayout.PushConstantRanges = std::move(MergedPushConstants);
		return true;
	}

	auto BuildPipelineLayoutFromShaders(
		std::span<const FCompiledShader> CompiledShaders,
		FPipelineLayoutDesc& OutPipelineLayout,
		std::string& OutErrorMessage
	) -> bool
	{
		std::vector<FShaderReflectionData> ReflectionData;
		ReflectionData.reserve(CompiledShaders.size());
		for (const FCompiledShader& CompiledShader : CompiledShaders)
		{
			ReflectionData.push_back(CompiledShader.Reflection);
		}

		return BuildPipelineLayoutFromReflection(ReflectionData, OutPipelineLayout, OutErrorMessage);
	}

	auto FShaderMapResourceCode::AddCompiledShader(const FCompiledShader& CompiledShader) -> uint32
	{
		CompiledShaders.push_back(CompiledShader);
		return static_cast<uint32>(CompiledShaders.size() - 1);
	}

	FShaderMapResource::FShaderMapResource(std::shared_ptr<FShaderMapResourceCode> InCode)
		: Code(std::move(InCode))
	{
		checkf(Code, "Shader map resource code must not be null");
	}

	auto FShaderMapResource::AddShaderCompilerOutput(const FShaderCompilerOutput& Output) -> void
	{
		for (const FCompiledShader& CompiledShader : Output.CompiledShaders)
		{
			Code->AddCompiledShader(CompiledShader);
		}

		Shaders.resize(Code->GetNumShaders());
	}

	auto FShaderMapResource::GetShader(uint32 ShaderIndex, bool bRequired) const -> FRHIShader*
	{
		check(ShaderIndex < Code->GetNumShaders());
		std::lock_guard Lock(Mutex);
		if (ShaderIndex >= Shaders.size())
		{
			Shaders.resize(Code->GetNumShaders());
		}

		if (!Shaders[ShaderIndex])
		{
			checkf(GDynamicRHI, "Cannot create RHI shader without an active DynamicRHI");

			const FCompiledShader& CompiledShader = Code->GetCompiledShader(ShaderIndex);
			const FRHIShaderCreateDesc ShaderCreateDesc = MakeShaderCreateDesc(CompiledShader);
			Shaders[ShaderIndex] = GDynamicRHI->RHICreateShader(ShaderCreateDesc);

			if (bRequired)
			{
				checkf(Shaders[ShaderIndex], "Failed to create required shader '{}'", CompiledShader.DebugName);
			}
		}

		return Shaders[ShaderIndex];
	}

	auto FShaderMapResource::CreateRHIShader(uint32 ShaderIndex, bool bRequired) const -> FRHIShader*
	{
		return GetShader(ShaderIndex, bRequired);
	}

	auto FShaderMapResource::ReleaseRHIShader(uint32 ShaderIndex) -> FRHIShader*
	{
		check(ShaderIndex < Shaders.size());
		FRHIShader* Shader = Shaders[ShaderIndex];
		Shaders[ShaderIndex] = nullptr;
		return Shader;
	}

	FShaderMapBase::FShaderMapBase()
	{
		Reset();
	}

	auto FShaderMapBase::Initialize(std::span<const FShaderType* const> ShaderTypes, const FShaderCompilerOutput& Output, std::string& OutErrorMessage) -> bool
	{
		FShaderCompileOptions CompileOptions;
		if (!BuildShaderMapCompileOptions(ShaderTypes, nullptr, CompileOptions, OutErrorMessage))
		{
			Reset();
			return false;
		}
		return Initialize(ShaderTypes, Output, CompileOptions, OutErrorMessage);
	}

	auto FShaderMapBase::Initialize(
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompilerOutput& Output,
		const FShaderCompileOptions& CompileOptions,
		std::string& OutErrorMessage
	) -> bool
	{
		Reset();
		OutErrorMessage.clear();

		FShaderCompileOptions EffectiveCompileOptions;
		if (!BuildShaderMapCompileOptions(ShaderTypes, &CompileOptions, EffectiveCompileOptions, OutErrorMessage))
		{
			Reset();
			return false;
		}

		if (ShaderTypes.size() != Output.CompiledShaders.size())
		{
			OutErrorMessage = std::format(
				"Shader type count ({}) does not match compiled shader count ({})",
				ShaderTypes.size(),
				Output.CompiledShaders.size()
			);
			return false;
		}

		if (!BuildShaderMapCacheKey(EffectiveCompileOptions, Output, CacheKey, OutErrorMessage))
		{
			Reset();
			return false;
		}

		if (CacheKey.IsZero())
		{
			Code = std::make_shared<FShaderMapResourceCode>();
			Resource = std::make_shared<FShaderMapResource>(Code);
			Resource->AddShaderCompilerOutput(Output);
		}
		else
		{
			FShaderMapCacheEntry CacheEntry = GetShaderMapResourceCache().FindOrAdd(CacheKey, Output);
			checkf(CacheEntry.Code && CacheEntry.Resource, "Shader map cache entry must contain valid code and resource");
			Code = std::move(CacheEntry.Code);
			Resource = std::move(CacheEntry.Resource);
		}

		std::vector<FCompiledShader> CompiledShaders;
		CompiledShaders.reserve(Output.CompiledShaders.size());

		for (uint32 ShaderIndex = 0; ShaderIndex < ShaderTypes.size(); ++ShaderIndex)
		{
			const FShaderType* ShaderType = ShaderTypes[ShaderIndex];
			checkf(ShaderType, "Shader type must not be null");

			const FCompiledShader& CompiledShader = Code->GetCompiledShader(ShaderIndex);
			if (CompiledShader.Frequency != ShaderType->GetFrequency())
			{
				OutErrorMessage = std::format(
					"Compiled shader frequency does not match shader type '{}' at index {}",
					ShaderType->GetName(),
					ShaderIndex
				);
				Reset();
				return false;
			}

			if (!ShaderType->GetEntryPoint().empty() && CompiledShader.SourceEntryPoint != ShaderType->GetEntryPoint())
			{
				OutErrorMessage = std::format(
					"Compiled shader entry point '{}' does not match shader type '{}' entry point '{}'",
					CompiledShader.SourceEntryPoint,
					ShaderType->GetName(),
					ShaderType->GetEntryPoint()
				);
				Reset();
				return false;
			}

			ShaderTypeToIndex.emplace(ShaderType, ShaderIndex);
			std::unique_ptr<FShader> ShaderInstance = ShaderType->CreateShaderInstance(this, CompiledShader.Reflection);
			if (!ShaderInstance)
			{
				OutErrorMessage = std::format("Shader type '{}' failed to create a shader instance", ShaderType->GetName());
				Reset();
				return false;
			}
			if (!ShaderInstance->InitializeParameterBindings(OutErrorMessage))
			{
				OutErrorMessage = std::format("Shader type '{}' parameter binding failed: {}", ShaderType->GetName(), OutErrorMessage);
				Reset();
				return false;
			}
			ShaderInstances.emplace(ShaderType, std::move(ShaderInstance));
			CompiledShaders.push_back(CompiledShader);
		}

		std::vector<FShaderReflectionData> PipelineReflectionData;
		PipelineReflectionData.reserve(CompiledShaders.size());
		for (uint32 ShaderIndex = 0; ShaderIndex < ShaderTypes.size(); ++ShaderIndex)
		{
			FShaderReflectionData ReflectionWithOverrides;
			ApplyShaderParameterBindingOverrides(*ShaderTypes[ShaderIndex], CompiledShaders[ShaderIndex].Reflection, ReflectionWithOverrides);
			PipelineReflectionData.push_back(std::move(ReflectionWithOverrides));
		}

		if (!BuildPipelineLayoutFromReflection(PipelineReflectionData, MergedPipelineLayout, OutErrorMessage))
		{
			Reset();
			return false;
		}

		return true;
	}

	auto FShaderMapBase::InitializeFromShaderTypes(
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompileOptions& CompileOptions,
		std::string& OutErrorMessage
	) -> bool
	{
		FShaderCompileOptions EffectiveCompileOptions;
		if (!BuildShaderMapCompileOptions(ShaderTypes, &CompileOptions, EffectiveCompileOptions, OutErrorMessage))
		{
			Reset();
			return false;
		}

		const FShaderCompilerOutput Output = GetOrCompileShader(EffectiveCompileOptions.VirtualShaderPath, EffectiveCompileOptions);
		if (!Output)
		{
			Reset();
			OutErrorMessage = Output.ErrorMessage;
			return false;
		}

		return Initialize(ShaderTypes, Output, EffectiveCompileOptions, OutErrorMessage);
	}

	auto FShaderMapBase::FindShaderIndex(const FShaderType* ShaderType) const -> const uint32*
	{
		const auto FoundIt = ShaderTypeToIndex.find(ShaderType);
		return FoundIt != ShaderTypeToIndex.end() ? &FoundIt->second : nullptr;
	}

	auto FShaderMapBase::GetShader(const FShaderType* ShaderType) const -> FShader*
	{
		const auto FoundIt = ShaderInstances.find(ShaderType);
		return FoundIt != ShaderInstances.end() ? FoundIt->second.get() : nullptr;
	}

	auto FShaderMapBase::GetOrCreateShaderRHI(const FShaderType* ShaderType, bool bRequired) -> FRHIShader*
	{
		const uint32* ShaderIndex = FindShaderIndex(ShaderType);
		checkf(ShaderIndex, "Shader type '{}' is not part of this shader map", ShaderType ? ShaderType->GetName() : std::string_view("<null>"));
		return Resource ? Resource->GetShader(*ShaderIndex, bRequired) : nullptr;
	}

	auto FShaderMapBase::Reset() -> void
	{
		ShaderTypeToIndex.clear();
		ShaderInstances.clear();
		MergedPipelineLayout = {};
		CacheKey = {};
		Code = std::make_shared<FShaderMapResourceCode>();
		Resource = std::make_shared<FShaderMapResource>(Code);
	}

	auto ClearShaderMapResourceCache() -> void
	{
		GetShaderMapResourceCache().Clear();
	}

	auto GetShaderMapResourceCacheStats() -> FShaderMapResourceCacheStats
	{
		return GetShaderMapResourceCache().GetStats();
	}
} // namespace Durin
