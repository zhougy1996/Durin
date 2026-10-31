#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialShaderMap.h"
#include "Threading/CancellationToken.h"
#include "Shader/ShaderCompilerCore.h"
#include "Materials/MaterialCompileLifecycle.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataRequestOwner.h"
#include "Threading/TaskRuntime.h"
#endif

namespace Durin { struct FMaterialTranslationInput; struct FMaterialTranslatedProgram; class FShaderBuildHandle; }

namespace Durin::MaterialBuildPrivate
{

	class FMaterialCompilation;
	using FMaterialProgramCompileHandle = std::shared_ptr<FMaterialCompilation>;

	struct FMaterialBuildResult
	{
		std::shared_ptr<const FMaterialCompilerResult> Program;
		std::vector<FMaterialTranslationSource> Sources;
		bool RetainedHit = false;
		bool SingleFlight = false;
		explicit operator bool() const { return Program && bool(*Program); }
	};

	enum class EMaterialCompileAdmissionError { Capacity, InputTooLarge, Closed, Unavailable };
	struct FMaterialProgramCompileOptions
	{
		bool bForceRecompile = false;
		FCancellationToken Cancellation;
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput;
		FMaterialProgramCompileHandle Request;
	};
	ENGINE_API auto SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void;
	struct FMaterialProgramCompileStats { uint64 Joins = 0; };
	ENGINE_API auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats;
	ENGINE_API auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult;

	// Request lifetime and scheduling state never become part of the published map.
	class FMaterialCompilation final
	{
	public:
		ENGINE_API FMaterialCompilation();
		ENGINE_API ~FMaterialCompilation();
		ENGINE_API static auto Compile(std::shared_ptr<const MIR::FCompilerInput> Input,
			FMaterialProgramCompileOptions Options,
			std::function<void(FMaterialBuildResult)> Completion)
			-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>;
		ENGINE_API static auto Compile(const MIR::FCompilerInput& Input,
			FMaterialProgramCompileOptions Options,
			std::function<void(FMaterialBuildResult)> Completion)
			-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>;
		uint64 CompilingId = 0;
		FMaterialProgramIdentity ShaderMapId;
		auto GetShaderMapId() const -> FMaterialProgramIdentity
		{
#if DURIN_WITH_EDITOR
			std::lock_guard Lock(Mutex);
#endif
			return ShaderMapId;
		}
		FCancellationToken Cancellation;
		bool Force = false;
		FMaterialProgramIdentity CanonicalIdentity;
		std::vector<FMaterialCompileRequest> CompilingMaterialDependencies;
		std::vector<FWeakObjectPtr> CompletingOwners;
		std::shared_ptr<const MIR::FCompilerInput> DiagnosticInput;
		std::vector<FWeakObjectPtr> Origins;
		FWeakObjectPtr InitialOwner;
		uint64 CapturedBytes = 0;
#if DURIN_WITH_EDITOR
		static auto Start(std::shared_ptr<FMaterialCompilation> ShaderMap) -> void;
		static auto ProcessTranslation(std::shared_ptr<FMaterialCompilation> ShaderMap, FMaterialTranslatedProgram Translation) -> void;
		static auto CompleteCompilation(std::shared_ptr<FMaterialCompilation> ShaderMap, FMaterialCompilerResult Result,
			std::shared_ptr<const FMaterialCompilerResult> Retained = {}) -> void;
		mutable std::mutex Mutex;
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		FCancellationToken BuildCancellation;
		std::vector<std::shared_ptr<FMaterialCompilation>> CompileDependencies;
		FTaskCancellationSource CpuCancellation;
		std::shared_ptr<FShaderBuildHandle> ShaderJobs;
		std::shared_ptr<const MIR::FCompilerInput> Input;
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput;
		std::vector<FMaterialTranslationSource> Sources;
		std::function<void(FMaterialBuildResult)> Completion;
		std::unique_ptr<FMaterialTranslatedProgram> Translation;
		bool SingleFlight = false;
		std::chrono::steady_clock::time_point CompilationBegin;
		bool ResultReady = false, HighPriority = false, Inline = false;
#endif
	};

	// Request configuration/default rebinding and result budgets belong to the
	// compilation lifecycle, rather than the immutable ShaderMap content API.
	auto BindMaterialProgramCollections(std::shared_ptr<const FMaterialCompilerResult> Program,
		std::span<const FMaterialParameterCollectionLayout> Collections,
		const FMaterialCompilerEnvironment* Environment = nullptr) -> std::shared_ptr<const FMaterialCompilerResult>;
	auto GetMaterialProgramRetainedBytes(const FMaterialCompilerResult& Result) -> uint64;

	ENGINE_API auto FinalizeMaterialShaderMap(FMaterialTranslatedProgram Translation,
		FShaderCompilerOutput Output) -> FMaterialCompilerResult;
}
