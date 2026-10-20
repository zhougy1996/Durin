#pragma once
#include "CoreMinimal.h"
#include "MaterialPreparedProgram.h"
#include "Materials/MaterialShaderMap.h"
#include "Threading/CancellationToken.h"
#include "Shader/ShaderCompilerCore.h"

namespace Durin::MaterialBuildPrivate
{

	using FMaterialProgramCompileHandle = std::shared_ptr<FMaterialShaderMap>;

	struct FMaterialBuildResult
	{
		std::shared_ptr<const FMaterialCompilerResult> Program;
		std::vector<MIR::FSource> Sources;
		std::vector<uint32> SourceOrdinals;
		bool RetainedHit = false;
		bool SingleFlight = false;
		uint64 TaskId = 0;
		explicit operator bool() const { return Program && bool(*Program); }
	};

	enum class EMaterialCompileAdmissionError { Capacity, InputTooLarge, Closed, Unavailable };
	struct FMaterialProgramCompileOptions
	{
		bool bForceRecompile = false;
		FCancellationToken Cancellation;
		std::shared_ptr<const FMaterialTranslationInput> EncodedInput;
		std::shared_ptr<FMaterialShaderMap> ShaderMap;
	};
	struct FMaterialProgramCompileStats;
	ENGINE_API auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats;

	// Schedules shader-map work for assets, tools and Cook.
	class ENGINE_API FMaterialProgramCompilingManager final
	{
	public:
		static auto Get() -> FMaterialProgramCompilingManager&;
		auto SubmitJobs(std::shared_ptr<const MIR::FCompilerInput> Input,
			FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
			-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>;
		auto SubmitJobs(const MIR::FCompilerInput& Input,
			FMaterialProgramCompileOptions Options, std::function<void(FMaterialBuildResult)> Completion)
			-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>;
		auto FinishCompilation(const FMaterialProgramCompileHandle& Handle) -> void;
		auto CancelCompilation(const FMaterialProgramCompileHandle& Handle) -> void;
		auto PrioritizeCompilation(const FMaterialProgramCompileHandle& Handle) -> void;
		auto IsCompiling(const FMaterialProgramCompileHandle& Handle) const -> bool;
		auto ProcessAsyncResults() -> void;
		auto FinishAllCompilation() -> void;
		auto Shutdown() -> void;
	private:
		friend class Durin::FMaterialShaderMap;
		friend ENGINE_API auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats;
		FMaterialProgramCompilingManager();
		~FMaterialProgramCompilingManager();
		using FShaderMapPtr = std::shared_ptr<FMaterialShaderMap>;
		std::atomic_uint64_t ProgramJoins = 0, PeakActiveShaders = 0, PeakOutputBytes = 0;
#if DURIN_WITH_EDITOR
		mutable std::mutex Mutex;
		std::vector<FShaderMapPtr> CompilingShaderMaps;
		std::unordered_map<FMaterialProgramIdentity, std::weak_ptr<FMaterialShaderMap>> CompilingPrograms[2];
		bool Accepting = true;
#endif
		auto FindCompilingShaderMap(FShaderMapPtr Request) const -> FShaderMapPtr;
		auto FindOrAddCompilingShaderMap(FShaderMapPtr ShaderMap) -> std::shared_ptr<const FMaterialCompilerResult>;
		auto FinalizeCompilation(FShaderMapPtr ShaderMap, std::shared_ptr<const FMaterialCompilerResult> Program,
			bool RetainedHit, std::vector<FShaderMapPtr>& Subscribers,
			std::function<void(FMaterialBuildResult)>& Completion) -> FMaterialBuildResult;
		auto Submit(std::shared_ptr<const MIR::FCompilerInput> Input, FMaterialProgramCompileOptions Options,
			std::function<void(FMaterialBuildResult)> Completion, bool CopyInput)
			-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>;
		static auto LaunchTask(FShaderMapPtr ShaderMap, std::function<void()> Work) -> void;
		static auto SubmitCompileJobs(FShaderMapPtr ShaderMap) -> void;
		static auto ProcessFinishedJob(FShaderMapPtr ShaderMap, size_t Index, FShaderBuildResult Output) -> void;
		static auto IsBuildCanceled(FShaderMapPtr ShaderMap) -> bool;
	};

	ENGINE_API auto SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void;
	struct FMaterialProgramCompileStats { uint64 Joins = 0, PeakActiveShaders = 0, PeakOutputReservationBytes = 0; };
	ENGINE_API auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats;
	ENGINE_API auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult;
}
