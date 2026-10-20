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
		FMaterialProgramCompilingManager();
		~FMaterialProgramCompilingManager();
		struct FState;
		std::unique_ptr<FState> State;
		using FRecord = std::shared_ptr<FMaterialShaderMap>;
		auto Submit(std::shared_ptr<const MIR::FCompilerInput> Input, FMaterialProgramCompileOptions Options,
			std::function<void(FMaterialBuildResult)> Completion, bool CopyInput)
			-> std::expected<FMaterialProgramCompileHandle, EMaterialCompileAdmissionError>;
		static auto LaunchTask(FRecord Record, std::function<void()> Work) -> void;
		static auto ProcessTranslation(FRecord Record, FMaterialTranslatedProgram Translation) -> void;
		static auto SubmitCompileJobs(FRecord Record) -> void;
		static auto ProcessFinishedJob(FRecord Record, size_t Index, FShaderBuildResult Output) -> void;
		static auto IsBuildCanceled(FRecord Record) -> bool;
		static auto CompleteCompilation(FRecord Record, FMaterialCompilerResult Result,
			std::shared_ptr<const FMaterialCompilerResult> Retained = {}) -> void;
	};

	ENGINE_API auto SetMaterialBuildCompletionHookForTests(std::function<void(EMaterialQualityLevel, bool)> Hook) -> void;
	struct FMaterialProgramCompileStats { uint64 Joins = 0, PeakActiveShaders = 0, PeakOutputReservationBytes = 0; };
	ENGINE_API auto GetMaterialProgramCompileStatsForTests() -> FMaterialProgramCompileStats;
	ENGINE_API auto CompileMaterialInput(const MIR::FCompilerInput& Input, bool Force) -> FMaterialBuildResult;
}
