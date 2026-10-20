#pragma once
#include "CoreMinimal.h"
#include "Materials/MaterialCompileLifecycle.h"
#include "MaterialProgramCompilingManager.h"

namespace Durin
{
	// Admits asset dependencies and delivers their results; never resolves an owner.
	class FMaterialCompilationService final
	{
	public:
		auto Initialize() -> bool;
		auto Shutdown() -> void;
		auto StopAdmission() -> void;
		auto RaisePendingPriority(std::span<const FWeakObjectPtr> Owners = {}) -> void;
		auto FinishAllCompilation() -> void;
		auto FinishCompilation(std::span<const FWeakObjectPtr> Owners) -> void;
		auto SubmitJobs(FMaterialCompileRequest Request) -> EMaterialCompileState;
		auto CancelCompilation(FWeakObjectPtr Owner) -> bool;
		auto ProcessAsyncResults(uint32 MaximumCount,
			const std::function<void(FMaterialCompileResult)>& Process) -> uint32;
		auto IsCompiling(FWeakObjectPtr Owner) const -> bool;
		auto GetDiagnostics() const -> FMaterialCompilationDiagnostics;
		auto IsAccepting() const -> bool;
	private:
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

		auto RetireCompiles() -> void;
		auto CancelLatchedCompiles() -> void;
		auto MakeReadyResult(
			const FMaterialCompileRequest& Request,
			std::shared_ptr<const FMaterialCompilerResult> Program,
			EMaterialCompileCacheOutcome CacheOutcome,
			uint64 TaskId) -> FMaterialCompileResult;
		static auto MakeTerminalResult(const FMaterialCompileRequest& Consumer, EMaterialCompileState State,
			EMaterialCompileResultCategory Category, uint64 TaskId) -> FMaterialCompileResult;
		auto CompleteFlight(
			const std::shared_ptr<FMaterialShaderMap>& Flight,
			MaterialBuildPrivate::FMaterialBuildResult Build,
			EMaterialCompileState CompletionState) -> void;
		auto CancelOwnerLocked(FWeakObjectPtr Owner, bool bSuperseded) -> bool;

		mutable std::mutex Mutex;
		bool bAcceptingRequests = false;
		uint32 OutstandingConsumers = 0;
		std::unordered_map<FMaterialCompileKey,
			std::shared_ptr<FMaterialShaderMap>,
			FMaterialCompileKeyHash> Flights;
		std::deque<FMaterialCompileResult> Mailbox;
		std::vector<std::shared_ptr<FMaterialShaderMap>> Compiles;
		std::vector<std::shared_ptr<FMaterialShaderMap>> CompletingFlights;
		uint64 CapturedBytes = 0;
		FMaterialCompilationDiagnostics Diagnostics;
	};

	auto MapMaterialProgramCategory(EMaterialProgramDiagnosticCategory Category) -> EMaterialCompileResultCategory;
}
