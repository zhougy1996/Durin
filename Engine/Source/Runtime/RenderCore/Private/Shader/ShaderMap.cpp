#include "Shader/Shader.h"
#include "Hash/CanonicalHash.h"
#include "DynamicRHI.h"
#include "RHIGlobals.h"
#include "Shader/IShaderBuildModule.h"
#include "Shader/ShaderMapPreparation.h"
#include "Shader/ShaderCookedLibrary.h"
#include "Shader/ShaderData.h"
#include "ShaderBindingInternal.h"

namespace Durin
{
	struct FPreparedShaderMapState
	{
		uint64 Generation = 0;
		uint64 Epoch = 0;
		std::vector<const FShaderType*> Types;
		FShaderCompileOptions Options;
		std::mutex Mutex;
		std::optional<FShaderBuildResult> Output;
		FShaderBuildHandle Handle;
		EShaderBuildPriority Priority = EShaderBuildPriority::Low;
	};

	namespace
	{
		std::mutex PreparationMutex;
		std::vector<std::shared_ptr<FPreparedShaderMapState>> Preparations;
		thread_local uint32 PreparedScopeDepth = 0;
		thread_local uint64 PendingPreparedRequests = 0;
		std::atomic<uint64> PreparationEpoch = 1;
		std::atomic_bool ForcePreparation = false;

		auto MatchesPreparation(const FPreparedShaderMapState& State,
			const std::vector<const FShaderType*>& Types, const FShaderCompileOptions& Options) -> bool
		{
			if (State.Generation != GetShaderReloadGeneration() || State.Epoch != PreparationEpoch.load() || State.Types != Types
				|| State.Options.Target != Options.Target
				|| (Options.bForceRecompile && !State.Options.bForceRecompile)
				|| State.Options.Macros.size() != Options.Macros.size()) return false;
			for (size_t Index = 0; Index < Options.Macros.size(); ++Index)
				if (State.Options.Macros[Index].Name != Options.Macros[Index].Name
					|| State.Options.Macros[Index].Value != Options.Macros[Index].Value) return false;
			return true;
		}
	}
	FShaderCompileOptions::FShaderCompileOptions()
		: Target(GetActiveRHIBackend().value_or(
			ResolveRHIBackend(std::getenv("DURIN_RHI_BACKEND")).value_or(ERHIBackend::Vulkan))
			== ERHIBackend::Metal ? MetalShaderTarget : VulkanShaderTarget)
	{
	}

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

		auto BuildShaderMapCompileOptions(
			std::span<const FShaderType* const> ShaderTypes,
			const FShaderCompileOptions* InCompileOptions,
			FShaderCompileOptions& OutCompileOptions,
			bool bAllowMixedSources = false) -> FShaderOperationResult
		{
			OutCompileOptions = {};

			if (InCompileOptions)
			{
				OutCompileOptions.Target = InCompileOptions->Target;
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
			UpdateCanonicalHashString(Builder, "DurinShaderMapCacheKey_v3");
			UpdateCanonicalHashString(Builder, CompileOptions.VirtualShaderPath);
			UpdateCanonicalHash(Builder, CompileOptions.Target.Platform);
			UpdateCanonicalHash(Builder, CompileOptions.Target.Backend);
			UpdateCanonicalHash(Builder, CompileOptions.Target.IntermediateFormat);
			UpdateCanonicalHash(Builder, CompileOptions.Target.OutputFormat);
			UpdateCanonicalHash(Builder, CompileOptions.Target.SettingsDigest.HashLow);
			UpdateCanonicalHash(Builder, CompileOptions.Target.SettingsDigest.HashHigh);

			const uint64 EntryPointCount = static_cast<uint64>(CompileOptions.EntryPoints.size());
			UpdateCanonicalHash(Builder, EntryPointCount);
			for (const char8* EntryPoint : CompileOptions.EntryPoints)
			{
				UpdateCanonicalHashString(Builder, EntryPoint ? std::string_view(EntryPoint) : std::string_view{});
			}

			const uint64 FrequencyCount = static_cast<uint64>(CompileOptions.Frequencies.size());
			UpdateCanonicalHash(Builder, FrequencyCount);
			for (EShaderFrequency Frequency : CompileOptions.Frequencies)
			{
				UpdateCanonicalHash(Builder, Frequency);
			}

			const uint64 MacroCount = static_cast<uint64>(NormalizedMacros.size());
			UpdateCanonicalHash(Builder, MacroCount);
			for (const FShaderMacroDefinition& Macro : NormalizedMacros)
			{
				UpdateCanonicalHashString(Builder, Macro.Name);
				UpdateCanonicalHash(Builder, Macro.HasValue());
				if (Macro.Value)
				{
					UpdateCanonicalHashString(Builder, *Macro.Value);
				}
			}

			const uint64 ShaderCount = static_cast<uint64>(Output.CompiledShaders.size());
			UpdateCanonicalHash(Builder, ShaderCount);
			for (size_t ShaderIndex = 0; ShaderIndex < Output.CompiledShaders.size(); ++ShaderIndex)
			{
				const FShaderCode& CompiledShader = Output.CompiledShaders[ShaderIndex];
				if (CompiledShader.Target != CompileOptions.Target
					|| CompiledShader.CodeFormat != CompileOptions.Target.OutputFormat)
					return std::unexpected(FShaderError{
						.Code = EShaderError::PayloadOutputInvalid, .Index = ShaderIndex});
				UpdateCanonicalHash(Builder, CompiledShader.Target.Platform);
				UpdateCanonicalHash(Builder, CompiledShader.Target.Backend);
				UpdateCanonicalHash(Builder, CompiledShader.Target.IntermediateFormat);
				UpdateCanonicalHash(Builder, CompiledShader.Target.OutputFormat);
				UpdateCanonicalHash(Builder, CompiledShader.Target.SettingsDigest.HashLow);
				UpdateCanonicalHash(Builder, CompiledShader.Target.SettingsDigest.HashHigh);
				UpdateCanonicalHash(Builder, CompiledShader.CodeFormat);
				UpdateCanonicalHashString(Builder, CompiledShader.BinaryEntryPoint);
				UpdateCanonicalHash(Builder, CompiledShader.CodeHash);
				if (!ShaderFormat::ValidateShaderCode(CompiledShader)) return std::unexpected(FShaderError{.Code = EShaderError::PayloadOutputInvalid, .Index = ShaderIndex});
				UpdateCanonicalHash(Builder, ShaderFormat::ComputeShaderIdentity(CompiledShader));
			}

			OutCacheKey = Builder.Finalize();
			return {};
		}

	} // namespace

	auto FShaderMapResourceCode::AddCompiledShader(const FShaderCode& CompiledShader) -> uint32
	{
		auto Published = ShareShaderCode(CompiledShader);
		requiref(Published, "Cannot publish invalid shader code");
		CompiledShaders.push_back(std::move(Published));
		return static_cast<uint32>(CompiledShaders.size() - 1);
	}

	FShaderMapResource::FShaderMapResource(std::shared_ptr<FShaderMapResourceCode> InCode)
		: Code(std::move(InCode))
	{
		checkf(Code, "Shader map resource code must not be null");
	}

	auto FShaderMapResource::AddShaderCompilerOutput(const FShaderCompilerOutput& Output) -> void
	{
		for (const FShaderCode& CompiledShader : Output.CompiledShaders)
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

			const FShaderCode& CompiledShader = Code->GetCompiledShader(ShaderIndex);
			const FRHIShaderCreateDesc ShaderCreateDesc = MakeShaderCreateDesc(Code->GetSharedShaderCode(ShaderIndex));
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

		for (uint32 ShaderIndex = 0; ShaderIndex < ShaderTypes.size(); ++ShaderIndex)
		{
			const FShaderType* ShaderType = ShaderTypes[ShaderIndex];
			checkf(ShaderType, "Shader type must not be null");

			const FShaderCode& CompiledShader = Code->GetCompiledShader(ShaderIndex);
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
		}

		std::vector<FShaderReflectionData> PipelineReflectionData;
		PipelineReflectionData.reserve(ShaderTypes.size());
		for (uint32 ShaderIndex = 0; ShaderIndex < ShaderTypes.size(); ++ShaderIndex)
		{
			FShaderReflectionData ReflectionWithOverrides;
			ApplyShaderParameterBindingOverrides(*ShaderTypes[ShaderIndex], Code->GetCompiledShader(ShaderIndex).Reflection, ReflectionWithOverrides);
			PipelineReflectionData.push_back(std::move(ReflectionWithOverrides));
		}

		if (!(Result = BuildPipelineLayoutFromReflection(PipelineReflectionData, MergedPipelineLayout)))
		{
			Reset();
			return Result;
		}

		return {};
	}

	auto FShaderMapPreparation::Request(std::string_view Owner,
		std::initializer_list<std::string_view> Programs, EShaderBuildPriority Priority) -> FShaderMapPreparation
	{
		FShaderMapPreparation Result;
		Result.Priority = Priority;
		Result.Add(Owner, Programs);
		return Result;
	}

	auto FShaderMapPreparation::Add(std::string_view Owner,
		std::initializer_list<std::string_view> Names) -> void
	{
		for (auto Name : Names) Programs.emplace_back(Owner, Name);
		Refresh();
	}

	auto FShaderMapPreparation::Refresh() -> void
	{
		States.clear();
		Error.clear();
		Generation = GetShaderReloadGeneration();
		Epoch = PreparationEpoch.load();
		if (Programs.empty()) return;
		if (GetShaderDataDomain() == EShaderDataDomain::Cooked) return;
		auto* Module = IShaderBuildModule::Get();
		if (!Module) { Error = "ShaderBuild is unavailable."; return; }
		std::vector<std::shared_ptr<FPreparedShaderMapState>> Retired;
		{
			std::lock_guard Lock(PreparationMutex);
			std::erase_if(Preparations, [&](const auto& State) {
				if (State->Generation == Generation && State->Epoch == Epoch) return false;
				Retired.push_back(State);
				return true;
			});
		}
		for (const auto& State : Retired) State->Handle.RequestCancel();
		for (const auto& [Owner, Program] : Programs)
		{
			std::vector<const FShaderType*> Types;
			if (auto Result = GetShaderRuntimeRequestBuildTypes({.Owner = Owner, .Name = Program}, Types); !Result)
			{ Error = FormatShaderError(Result.error()); return; }
			std::ranges::sort(Types, {}, &FShaderType::GetName);
			FShaderCompileOptions Options;
			if (auto Result = BuildShaderMapCompileOptions(Types, nullptr, Options); !Result)
			{ Error = FormatShaderError(Result.error()); return; }
			Options.bForceRecompile = ForcePreparation.load();
			std::shared_ptr<FPreparedShaderMapState> State;
			{
				std::lock_guard Lock(PreparationMutex);
				for (const auto& Existing : Preparations)
					if (MatchesPreparation(*Existing, Types, Options)) { State = Existing; break; }
				if (State && Priority < State->Priority)
				{
					State->Priority = Priority;
					State->Handle.SetPriority(Priority);
				}
				if (!State)
				{
					if (Preparations.size() >= 128) { Error = "Shader preparation capacity exceeded."; return; }
					State = std::make_shared<FPreparedShaderMapState>();
					State->Generation = Generation;
					State->Epoch = Epoch;
					State->Priority = Priority;
					State->Types = std::move(Types);
					State->Options = std::move(Options);
					Preparations.push_back(State);
					auto Submitted = Module->SubmitMounted(State->Options.VirtualShaderPath, State->Options, {.Priority = Priority},
						[State](FShaderBuildResult Output) {
							std::lock_guard ResultLock(State->Mutex);
							State->Output = std::move(Output);
						});
					if (Submitted) State->Handle = std::move(*Submitted);
					else State->Output = FShaderBuildResult::Failure("Shader preparation admission failed.");
				}
			}
			States.push_back(std::move(State));
		}
	}

	auto FShaderMapPreparation::Poll() -> std::expected<bool, std::string>
	{
		if (Generation != GetShaderReloadGeneration() || Epoch != PreparationEpoch.load()) Refresh();
		if (!Error.empty()) return std::unexpected(Error);
		bool Ready = true;
		for (const auto& State : States)
		{
			std::lock_guard Lock(State->Mutex);
			if (!State->Output) { Ready = false; continue; }
			if (!*State->Output) return std::unexpected(State->Output->Diagnostic.empty()
				? std::string("Shader preparation failed or was canceled.") : State->Output->Diagnostic);
		}
		return Ready;
	}

	FPreparedShaderMapScope::FPreparedShaderMapScope()
	{
		if (PreparedScopeDepth++ == 0) PendingPreparedRequests = 0;
	}
	FPreparedShaderMapScope::~FPreparedShaderMapScope() { --PreparedScopeDepth; }
	auto FPreparedShaderMapScope::IsActive() -> bool { return PreparedScopeDepth != 0; }
	auto FPreparedShaderMapScope::GetPendingRequestCount() -> uint64
	{
		return IsActive() ? PendingPreparedRequests : 0;
	}

	auto ClearShaderMapPreparations() -> void
	{
		InvalidateShaderMapPreparations(false);
		std::vector<std::shared_ptr<FPreparedShaderMapState>> Retired;
		{ std::lock_guard Lock(PreparationMutex); Retired.swap(Preparations); }
		for (const auto& State : Retired) State->Handle.RequestCancel();
		for (const auto& State : Retired) (void)State->Handle.Wait();
	}

	auto InvalidateShaderMapPreparations(bool bForceRecompile) -> void
	{
		ForcePreparation = bForceRecompile;
		++PreparationEpoch;
	}

	auto FShaderMapBase::InitializeFromShaderTypes(
		std::span<const FShaderType* const> ShaderTypes,
		const FShaderCompileOptions& CompileOptions) -> FShaderCompletion
	{
		FShaderOperationResult Result;
		FShaderCompileOptions EffectiveCompileOptions;
		if (!(Result = BuildShaderMapCompileOptions(ShaderTypes, &CompileOptions, EffectiveCompileOptions)))
		{
			Reset();
			return Result;
		}

		std::vector<const FShaderType*> StableTypes(ShaderTypes.begin(), ShaderTypes.end());
		std::ranges::sort(StableTypes, {}, &FShaderType::GetName);
		std::vector<FShaderMacroDefinition> Macros;
		if (!(Result = NormalizeShaderMacros(EffectiveCompileOptions, Macros))) return Result;
		EffectiveCompileOptions.Macros = std::move(Macros);
		std::shared_ptr<FPreparedShaderMapState> Prepared;
		{
			std::lock_guard Lock(PreparationMutex);
			for (const auto& State : Preparations)
				if (MatchesPreparation(*State, StableTypes, EffectiveCompileOptions)) { Prepared = State; break; }
		}
		if (Prepared)
		{
			std::lock_guard Lock(Prepared->Mutex);
			if (Prepared->Output && *Prepared->Output)
			{
				FShaderCompilerOutput Output{.Error = {}};
				for (const auto* Type : ShaderTypes)
				{
					auto Found = std::ranges::find_if(Prepared->Output->CompiledShaders, [Type](const auto& Code) {
						return Code.SourceEntryPoint == Type->GetEntryPoint() && Code.Frequency == Type->GetFrequency();
					});
					if (Found == Prepared->Output->CompiledShaders.end())
						return std::unexpected(FShaderError{.Code = EShaderError::MissingShaderType});
					Output.CompiledShaders.push_back(*Found);
				}
				return Initialize(ShaderTypes, Output, EffectiveCompileOptions);
			}
			if (Prepared->Output) return {Prepared->Output->Status, Prepared->Output->Diagnostic};
		}
		if (PreparedScopeDepth != 0)
		{
			++PendingPreparedRequests;
			return std::unexpected(FShaderError{.Code = EShaderError::CompilationNotStarted});
		}
		auto* Module = IShaderBuildModule::Get();
		if (!Module)
		{
			Reset();
			return std::unexpected(FShaderError{.Code = EShaderError::BuildModuleUnavailable});
		}
		FShaderBuildResult Output = Module->CompileMounted(EffectiveCompileOptions.VirtualShaderPath, EffectiveCompileOptions);
		if (!Output)
		{
			Reset();
			return {Output.Status, Output.Diagnostic};
		}

		return Initialize(ShaderTypes, FShaderCompilerOutput{.CompiledShaders = std::move(Output.CompiledShaders), .Error = {}}, EffectiveCompileOptions);
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
