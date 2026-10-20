#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "Threading/CancellationToken.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataRequestOwner.h"
#endif

namespace Durin
{
	class FShaderBuildHandle;
	class FMaterialCompilationService;
	struct FMaterialTranslatedProgram;
	struct FMaterialTranslationInput;
	namespace MaterialBuildPrivate
	{
		class FMaterialProgramCompilingManager;
		struct FMaterialBuildResult;
	}

	// Owns a complete material program. Published content is shared as const;
	// compile dependencies and pending outputs exist only while building it.
	class FMaterialShaderMap final
	{
	public:
		ENGINE_API FMaterialShaderMap();
		ENGINE_API ~FMaterialShaderMap();
		// Copies finalized content only; a copy never inherits in-flight work.
		ENGINE_API FMaterialShaderMap(const FMaterialShaderMap& Other);
		auto operator=(const FMaterialShaderMap&) -> FMaterialShaderMap& = delete;

		std::vector<FMaterialCompilerParameterDeclaration> ActiveParameters;
		FMaterialRenderLayout Layout;
		FMaterialProgramRequirements Requirements;
		std::vector<FMaterialCompilerDependency> Dependencies;
		std::vector<FShaderCode> CompiledShaders;
		FMaterialProgramIdentity ShaderMapId;

		auto GetShaderMapId() const -> FMaterialProgramIdentity { return ShaderMapId; }
		auto GetCompilingId() const -> uint64 { return CompilingId; }
		ENGINE_API static auto FindId(FMaterialProgramIdentity Identity) -> std::shared_ptr<const FMaterialCompilerResult>;
		ENGINE_API static auto Register(std::shared_ptr<const FMaterialCompilerResult> Program, bool Replace = false)
			-> std::shared_ptr<const FMaterialCompilerResult>;
		ENGINE_API static auto ClearCache() -> void;
		ENGINE_API static auto GetCacheStats() -> std::pair<uint32, uint64>;
		ENGINE_API static auto GetRetainedBytes(const FMaterialCompilerResult& Result) -> uint64;
		ENGINE_API static auto BindCollections(std::shared_ptr<const FMaterialCompilerResult> Program,
			std::span<const FMaterialParameterCollectionLayout> Collections,
			const FMaterialCompilerEnvironment* Environment = nullptr) -> std::shared_ptr<const FMaterialCompilerResult>;
		ENGINE_API static auto Configure(std::shared_ptr<const FMaterialCompilerResult> Program, const MIR::FCompilerInput& Input)
			-> std::shared_ptr<const FMaterialCompilerResult>;
		ENGINE_API static auto FinalizeContent(FMaterialTranslatedProgram Translation, FShaderBuildResult Output,
			std::shared_ptr<FMaterialShaderMap> ShaderMap = {}) -> FMaterialCompilerResult;

	private:
		friend class MaterialBuildPrivate::FMaterialProgramCompilingManager;
		friend class FMaterialCompilationService;
		uint64 CompilingId = 0;
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
		static auto Compile(std::shared_ptr<FMaterialShaderMap> ShaderMap) -> void;
		auto ProcessCompilationResults(size_t Index, FShaderBuildResult Output) -> void;
		auto FinalizeCompilation(std::shared_ptr<FMaterialShaderMap> Self) -> FMaterialCompilerResult;
		mutable std::mutex Mutex;
		std::shared_ptr<DerivedData::FRequestOwner> Owner;
		FCancellationToken BuildCancellation;
		std::weak_ptr<FMaterialShaderMap> SharedCompile;
		std::vector<std::shared_ptr<FMaterialShaderMap>> CompileDependencies;
		FTaskCancellationSource CpuCancellation;
		std::vector<std::shared_ptr<FShaderBuildHandle>> Shaders;
		std::shared_ptr<const MIR::FCompilerInput> Input;
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput;
		std::vector<MIR::FSource> Sources;
		std::vector<uint32> SourceOrdinals;
		std::function<void(MaterialBuildPrivate::FMaterialBuildResult)> Completion;
		std::unique_ptr<FMaterialTranslatedProgram> Translation;
		std::vector<FGeneratedShaderCompileRequest> Jobs;
		std::vector<std::optional<FShaderBuildResult>> Outputs;
		size_t NextJob = 0;
		size_t ActiveJobs = 0;
		bool Dispatching = false, AssemblyScheduled = false, SingleFlight = false, ResultTooLarge = false;
		uint64 ReservedOutputBytes = 0;
		uint64 RetainedStageBytes = 0;
		std::chrono::steady_clock::time_point CompilationBegin;
		bool ResultReady = false, HighPriority = false, Inline = false;
		uint64 TaskId = 0;
#endif
	};
}
