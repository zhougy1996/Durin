#include "Shader/Shader.h"
#include "DynamicRHI.h"
#include "ShaderDataInternal.h"
#include "ShaderBindingInternal.h"

namespace Durin
{
	namespace
	{
		using ShaderPrivate::AreShaderBindingTypesCompatible;

		auto NormalizeShaderMacros(
			const FShaderCompileOptions& Options,
			std::vector<FShaderMacroDefinition>& OutMacros) -> FShaderOperationResult
		{
			OutMacros = Options.Macros;
			std::ranges::sort(OutMacros,
				[](const FShaderMacroDefinition& Left,
					const FShaderMacroDefinition& Right) {
					return std::tie(Left.Name, Left.Value)
						< std::tie(Right.Name, Right.Value);
				});
			for (size_t Index = 1; Index < OutMacros.size(); ++Index)
			{
				if (OutMacros[Index - 1].Name != OutMacros[Index].Name) continue;
				return std::unexpected(FShaderError{.Code = EShaderError::DuplicateMacro, .Parameter = OutMacros[Index].Name});
			}
			return {};
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
			bool bAllowMixedSources = false) -> FShaderOperationResult
		{
			OutCompileOptions = {};

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
				return {};
			}

			const FShaderType* FirstShaderType = ShaderTypes.front();
			checkf(FirstShaderType, "Shader type must not be null");

			const std::string_view ShaderPath = FirstShaderType->GetVirtualShaderPath();
			if (ShaderPath.empty())
			{
				return std::unexpected(FShaderError{.Code = EShaderError::MissingVirtualPath});
			}

			if (InCompileOptions && !InCompileOptions->VirtualShaderPath.empty() && InCompileOptions->VirtualShaderPath != ShaderPath)
			{
				return std::unexpected(FShaderError{.Code = EShaderError::VirtualPathMismatch,
					.ExpectedIdentity = std::string(ShaderPath),
					.ActualIdentity = InCompileOptions->VirtualShaderPath});
			}

			OutCompileOptions.VirtualShaderPath = std::string(ShaderPath);
			OutCompileOptions.EntryPoints.reserve(ShaderTypes.size());
			OutCompileOptions.Frequencies.reserve(ShaderTypes.size());

			for (size_t ShaderTypeIndex = 0; ShaderTypeIndex < ShaderTypes.size(); ++ShaderTypeIndex)
			{
				const FShaderType* ShaderType = ShaderTypes[ShaderTypeIndex];
				checkf(ShaderType, "Shader type must not be null");

				if (!bAllowMixedSources && ShaderType->GetVirtualShaderPath() != ShaderPath)
				{
					return std::unexpected(FShaderError{.Code = EShaderError::ShaderTypePathMismatch,
						.ShaderType = std::string(ShaderType->GetName()),
						.ExpectedIdentity = std::string(ShaderPath),
						.ActualIdentity = std::string(ShaderType->GetVirtualShaderPath())});
				}

				OutCompileOptions.EntryPoints.push_back(ShaderType->GetEntryPoint().data());
				OutCompileOptions.Frequencies.push_back(ShaderType->GetFrequency());

				if (InCompileOptions && !InCompileOptions->EntryPoints.empty())
				{
					if (ShaderTypeIndex >= InCompileOptions->EntryPoints.size()
						|| std::string_view(InCompileOptions->EntryPoints[ShaderTypeIndex]) != ShaderType->GetEntryPoint())
					{
						return std::unexpected(FShaderError{.Code = EShaderError::EntryPointMismatch,
							.ShaderType = std::string(ShaderType->GetName()),
							.Index = ShaderTypeIndex});
					}
				}

				if (InCompileOptions && !InCompileOptions->Frequencies.empty())
				{
					if (ShaderTypeIndex >= InCompileOptions->Frequencies.size()
						|| InCompileOptions->Frequencies[ShaderTypeIndex] != ShaderType->GetFrequency())
					{
						return std::unexpected(FShaderError{.Code = EShaderError::FrequencyMismatch,
							.ShaderType = std::string(ShaderType->GetName()),
							.Index = ShaderTypeIndex});
					}
				}
			}

			if (InCompileOptions && !InCompileOptions->EntryPoints.empty() && InCompileOptions->EntryPoints.size() != ShaderTypes.size())
			{
				return std::unexpected(FShaderError{.Code = EShaderError::EntryPointCountMismatch,
					.Expected = ShaderTypes.size(),
					.Actual = InCompileOptions->EntryPoints.size()});
			}

			if (InCompileOptions && !InCompileOptions->Frequencies.empty() && InCompileOptions->Frequencies.size() != ShaderTypes.size())
			{
				return std::unexpected(FShaderError{.Code = EShaderError::FrequencyCountMismatch,
					.Expected = ShaderTypes.size(),
					.Actual = InCompileOptions->Frequencies.size()});
			}

			return {};
		}

		auto BuildShaderMapCacheKey(
			const FShaderCompileOptions& CompileOptions,
			const FShaderCompilerOutput& Output,
			FXxHash128& OutCacheKey) -> FShaderOperationResult
		{
			FShaderOperationResult Result;
			OutCacheKey = {};

			if (CompileOptions.VirtualShaderPath.empty())
			{
				return {};
			}

			std::vector<FShaderMacroDefinition> NormalizedMacros;
			if (!(Result = NormalizeShaderMacros(CompileOptions, NormalizedMacros)))
			{
				return Result;
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
			return {};
		}

	} // namespace

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

	auto FShaderMapBase::Initialize(
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompilerOutput& Output) -> FShaderOperationResult
	{
		FShaderOperationResult Result;
		FShaderCompileOptions CompileOptions;
		if (!(Result = BuildShaderMapCompileOptions(ShaderTypes, nullptr, CompileOptions)))
		{
			Reset();
			return Result;
		}
		return Initialize(ShaderTypes, Output, CompileOptions);
	}

	auto FShaderMapBase::Initialize(
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompilerOutput& Output,
		const FShaderCompileOptions& CompileOptions,
		bool bAllowMixedSources) -> FShaderOperationResult
	{
		FShaderOperationResult Result;
		Reset();

		FShaderCompileOptions EffectiveCompileOptions;
		if (!(Result = BuildShaderMapCompileOptions(ShaderTypes, &CompileOptions, EffectiveCompileOptions, bAllowMixedSources)))
		{
			Reset();
			return Result;
		}

		if (ShaderTypes.size() != Output.CompiledShaders.size())
		{
			return std::unexpected(FShaderError{.Code = EShaderError::CompiledShaderCountMismatch,
				.Expected = ShaderTypes.size(),
				.Actual = Output.CompiledShaders.size()});
		}

		if (!(Result = BuildShaderMapCacheKey(EffectiveCompileOptions, Output, CacheKey)))
		{
			Reset();
			return Result;
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
				Result = std::unexpected(FShaderError{.Code = EShaderError::CompiledFrequencyMismatch,
					.ShaderType = std::string(ShaderType->GetName()),
					.Index = ShaderIndex,
					.Expected = static_cast<uint64>(ShaderType->GetFrequency()),
					.Actual = static_cast<uint64>(CompiledShader.Frequency)});
				Reset();
				return Result;
			}

			if (!ShaderType->GetEntryPoint().empty() && CompiledShader.SourceEntryPoint != ShaderType->GetEntryPoint())
			{
				Result = std::unexpected(FShaderError{.Code = EShaderError::CompiledEntryPointMismatch,
					.ShaderType = std::string(ShaderType->GetName()),
					.ExpectedIdentity = std::string(ShaderType->GetEntryPoint()),
					.ActualIdentity = CompiledShader.SourceEntryPoint});
				Reset();
				return Result;
			}

			ShaderTypeToIndex.emplace(ShaderType, ShaderIndex);
			std::unique_ptr<FShader> ShaderInstance = ShaderType->CreateShaderInstance(this, CompiledShader.Reflection);
			if (!ShaderInstance)
			{
				Result = std::unexpected(FShaderError{.Code = EShaderError::ShaderInstanceCreationFailed,
					.ShaderType = std::string(ShaderType->GetName())});
				Reset();
				return Result;
			}
			if (!(Result = ShaderInstance->InitializeParameterBindings()))
			{
				Result.error().ShaderType = std::string(ShaderType->GetName());
				Reset();
				return Result;
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

		if (!(Result = BuildPipelineLayoutFromReflection(PipelineReflectionData, MergedPipelineLayout)))
		{
			Reset();
			return Result;
		}

		return {};
	}

	auto FShaderMapBase::InitializeFromShaderTypes(
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompileOptions& CompileOptions) -> FShaderOperationResult
	{
		FShaderOperationResult Result;
		FShaderCompileOptions EffectiveCompileOptions;
		if (!(Result = BuildShaderMapCompileOptions(ShaderTypes, &CompileOptions, EffectiveCompileOptions)))
		{
			Reset();
			return Result;
		}

		const FShaderCompilerOutput Output = GetOrCompileShader(EffectiveCompileOptions.VirtualShaderPath, EffectiveCompileOptions);
		if (!Output)
		{
			Reset();
			return std::unexpected(Output.Error);
		}

		return Initialize(ShaderTypes, Output, EffectiveCompileOptions);
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
}
