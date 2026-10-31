#pragma once
#include "CoreMinimal.h"
#include "EngineAPI.h"
#include "Asset/AssetCompilingManager.h"
#include "Materials/MaterialCompileTypes.h"
#include "Shader/ShaderBuildRequest.h"

namespace Durin
{
	// Owns shader-map compilation, shared Shader jobs and asset-framework integration.
	class ENGINE_API FShaderCompilingManager final : public IAssetCompilingManager
	{
	public:
		static auto Get() -> FShaderCompilingManager&;
		~FShaderCompilingManager();
		auto PrepareJobs(std::vector<FGeneratedShaderCompileRequest> Jobs, uint64 MaximumOutputBytes,
			FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion)
			-> FShaderBuildHandle;
		auto PrepareJobs(std::string_view Path, const FShaderCompileOptions& CompileOptions,
			FShaderBuildRequestOptions Options, FShaderBuildCompletion Completion)
			-> FShaderBuildHandle;
		// Accepts a job group prepared by this manager; repeated submission is inert.
		auto SubmitJobs(const FShaderBuildHandle& Jobs) -> void;
		// Explicit blocking startup/tool operations; rendering-thread calls fail.
		auto CompileGeneratedSync(const FGeneratedShaderCompileRequest& Job) -> FShaderCompilerOutput;
		auto CompileMountedSync(std::string_view Path, const FShaderCompileOptions& Options) -> FShaderCompilerOutput;
		auto FinishCompilation(const FShaderBuildHandle& Handle) -> EShaderBuildWaitResult;
		auto CancelAllCompilation() -> void;
		auto GetPeakJobStats() const -> std::pair<uint64, uint64>;

		auto FinishCompilation(const MaterialBuildPrivate::FMaterialProgramCompileHandle& Handle) -> void;
		auto CancelCompilation(const MaterialBuildPrivate::FMaterialProgramCompileHandle& Handle) -> void;
		auto PrioritizeCompilation(const MaterialBuildPrivate::FMaterialProgramCompileHandle& Handle) -> void;
		auto IsCompiling(const MaterialBuildPrivate::FMaterialProgramCompileHandle& Handle) const -> bool;
		auto ProcessAsyncResults(const FAssetCompileProcessParams& Params = {}) -> FAssetCompileProcessResult;

		auto Start() -> FAssetCompilerStartResult override;
		auto StopAdmission() -> void override;
		auto GetNumRemainingAssets() const -> uint64 override;
		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult override;
		auto FinishCompilationForObjects(std::span<DObject* const> Objects) -> FAssetCompileProcessResult override;
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override;
		auto FinishAllCompilation() -> FAssetCompileProcessResult override;
		auto Shutdown() -> void override;
		auto CancelCompilation(FWeakObjectPtr Owner) -> bool;
	private:
		auto SubmitJobs(FMaterialCompileRequest Request) -> EMaterialCompileState;
		friend class MaterialBuildPrivate::FMaterialCompilation;
		friend auto MaterialBuildPrivate::GetMaterialProgramCompileStatsForTests() -> MaterialBuildPrivate::FMaterialProgramCompileStats;
		friend struct Private::FMaterialCompilationLifecycle;
		friend auto IsMaterialCompilationAcceptingRequests() -> bool;
		friend auto HasPendingMaterialCompilation(const DMaterialInterface&) -> bool;
		friend auto GetMaterialCompilationDiagnostics() -> FMaterialCompilationDiagnostics;
		FShaderCompilingManager();
		struct FJobGroup;
		auto FinishShaderJobs() -> void;
		auto GetNumRemainingShaderJobs() const -> uint64;
		mutable std::mutex JobMutex;
		std::vector<std::weak_ptr<FJobGroup>> Groups;
		std::atomic_uint64_t PeakActiveJobs = 0, PeakOutputBytes = 0;
		std::atomic_bool Accepting = true;
		using FShaderMapPtr = MaterialBuildPrivate::FMaterialProgramCompileHandle;
		std::atomic_uint64_t ProgramJoins = 0;
		mutable std::mutex MaterialMutex;
		std::vector<FShaderMapPtr> CompilingShaderMaps;
#if DURIN_WITH_EDITOR
		std::unordered_map<FMaterialProgramIdentity, std::weak_ptr<MaterialBuildPrivate::FMaterialCompilation>> CompilingPrograms[2];
#endif
		struct FMaterialCompileKey
		{
			FMaterialProgramIdentity Identity;
			bool bForceRecompile = false;

			auto operator==(const FMaterialCompileKey&) const -> bool = default;
		};

		struct FMaterialCompileKeyHash
		{
			auto operator()(const FMaterialCompileKey& Key) const
				noexcept -> size_t
			{
				return std::hash<FMaterialProgramIdentity>{}(Key.Identity)
					^ (Key.bForceRecompile ? size_t{0x9e3779b9} : size_t{});
			}
		};

		std::unordered_map<FMaterialCompileKey, FShaderMapPtr, FMaterialCompileKeyHash> RequestedShaderMaps;
		std::deque<FMaterialCompileResult> MaterialResults;
		uint32 OutstandingMaterialConsumers = 0;
		uint64 CapturedMaterialBytes = 0;
		FMaterialCompilationDiagnostics MaterialDiagnostics;
		auto IsAcceptingMaterialRequests() const -> bool;
		auto IsMaterialCompiling(FWeakObjectPtr Owner) const -> bool;
		auto GetMaterialDiagnostics() const -> FMaterialCompilationDiagnostics;
		auto RaiseMaterialPriority(std::span<const FWeakObjectPtr> Owners = {}) -> void;
		auto CancelLatchedMaterialCompiles() -> void;
		auto ProcessMaterialResults(const FAssetCompileProcessParams& Params, const std::function<void(FMaterialCompileResult)>& Process) -> uint32;
		auto CompleteMaterialRequests(const FShaderMapPtr& ShaderMap, MaterialBuildPrivate::FMaterialBuildResult Build) -> void;
		auto CancelMaterialOwnerLocked(FWeakObjectPtr Owner) -> bool;
		static auto MakeMaterialResult(const FMaterialCompileRequest& Request,
			const MaterialBuildPrivate::FMaterialBuildResult& Build,
			const MIR::FCompilerInput* DiagnosticInput = nullptr, bool Canceled = false) -> FMaterialCompileResult;
		auto FindCompilingShaderMap(FShaderMapPtr Request) const -> FShaderMapPtr;
		auto FindOrAddCompilingShaderMap(FShaderMapPtr ShaderMap) -> std::shared_ptr<const FMaterialCompilerResult>;
		auto FinalizeCompilation(FShaderMapPtr ShaderMap, std::shared_ptr<const FMaterialCompilerResult> Program,
			bool RetainedHit, std::vector<FShaderMapPtr>& Subscribers,
			std::function<void(MaterialBuildPrivate::FMaterialBuildResult)>& Completion) -> MaterialBuildPrivate::FMaterialBuildResult;
		auto Submit(std::shared_ptr<const MIR::FCompilerInput> Input, MaterialBuildPrivate::FMaterialProgramCompileOptions Options,
			std::function<void(MaterialBuildPrivate::FMaterialBuildResult)> Completion, bool CopyInput)
			-> std::expected<MaterialBuildPrivate::FMaterialProgramCompileHandle, MaterialBuildPrivate::EMaterialCompileAdmissionError>;
		auto CleanupFinishedShaderMaps() -> void;
		auto FinishMaterialCompilation(std::span<const FWeakObjectPtr> Owners = {}) -> void;
		auto CancelMaterialCompilation() -> void;
		auto GetNumRemainingMaterialShaderMaps() const -> uint64;
		static auto LaunchTask(FShaderMapPtr ShaderMap, std::function<void()> Work) -> void;
		static auto SubmitCompileJobs(FShaderMapPtr ShaderMap) -> void;
		static auto IsBuildCanceled(FShaderMapPtr ShaderMap) -> bool;
	};
}
