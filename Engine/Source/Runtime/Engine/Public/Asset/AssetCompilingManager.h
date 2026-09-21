#pragma once

#include "Delegates/Delegate.h"
#include "DObject/WeakObjectPtr.h"
#include "EngineAPI.h"
#include "Misc/Name.h"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Durin
{
	class DClass;
	class DObject;
	struct FAssetCompilingManagerInitializationResult;

	struct FAssetCompileProcessParams
	{
		uint32 MaximumCompletions = 64;
		std::optional<std::chrono::steady_clock::time_point> Deadline;
		// Shared by all passes of one aggregate pump; zero denotes a direct invocation.
		uint64 PumpIdentity = 0;
	};

	struct FAssetCompileProcessResult
	{
		uint32 ProcessedCompletionCount = 0;
		std::vector<FWeakObjectPtr> SuccessfullyCompiledAssets;
	};

	struct FAssetCompilerDiagnostics
	{
		FName CompilerName;
		uint64 RemainingAssetCount = 0;
	};

	struct FAssetCompilingManagerDiagnostics
	{
		uint32 CompilerCount = 0;
		uint64 RemainingAssetCount = 0;
		uint64 ProcessedCompletionCount = 0;
		bool bAcceptingRequests = false;
		bool bShutdown = false;
		std::vector<FAssetCompilerDiagnostics> Compilers;
	};

	struct FAssetPostCompileData
	{
		FName CompilerName;
		std::vector<FWeakObjectPtr> Assets;
	};

	DECLARE_MULTICAST_DELEGATE_OneParam(FAssetPostCompileEvent, const FAssetPostCompileData&)

	enum class EAssetCompilerStartError : uint8 { None, SchedulerUnavailable, Undrained, TaskScopeUnavailable, StateUnavailable };
	struct FAssetCompilerStartResult
	{
		EAssetCompilerStartError Error = EAssetCompilerStartError::None;
		uint64 PendingRequests = 0;
		explicit operator bool() const { return Error == EAssetCompilerStartError::None; }
	};
	ENGINE_API auto FormatAssetCompilerStartError(const FAssetCompilerStartResult& Result) -> std::string;

	// Defines the lifecycle and dispatch contract for one typed asset compiler.
	class IAssetCompilingManager
	{
	public:
		virtual ~IAssetCompilingManager() = default;
		virtual auto Start() -> FAssetCompilerStartResult = 0;
		virtual auto StopAdmission() -> void = 0;
		virtual auto GetNumRemainingAssets() const -> uint64 = 0;
		virtual auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
			-> FAssetCompileProcessResult = 0;
		virtual auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult = 0;
		virtual auto MarkCompilationAsCanceled(std::span<DObject* const> Objects)
			-> void = 0;
		virtual auto FinishAllCompilation() -> FAssetCompileProcessResult = 0;
		virtual auto Shutdown() -> void = 0;
	};

	// Describes one compiler lifecycle and every exact reflected class routed to it.
	struct FAssetCompilingManagerRegistration
	{
		FName Name;
		std::vector<DClass*> AssetClasses;
		std::shared_ptr<IAssetCompilingManager> Manager;
	};

	// Retires one external compiler registration when its module-owned scope ends.
	class FAssetCompilerRegistrationHandle
	{
	public:
		FAssetCompilerRegistrationHandle() = default;
		ENGINE_API ~FAssetCompilerRegistrationHandle();
		FAssetCompilerRegistrationHandle(const FAssetCompilerRegistrationHandle&) = delete;
		auto operator=(const FAssetCompilerRegistrationHandle&)
			-> FAssetCompilerRegistrationHandle& = delete;
		ENGINE_API FAssetCompilerRegistrationHandle(
			FAssetCompilerRegistrationHandle&& Other) noexcept;
		ENGINE_API auto operator=(FAssetCompilerRegistrationHandle&& Other) noexcept
			-> FAssetCompilerRegistrationHandle&;
		ENGINE_API auto Reset() -> void;
		[[nodiscard]] auto IsValid() const -> bool { return Generation != 0; }

	private:
		FName CompilerName;
		uint64 Generation = 0;

		friend class FAssetCompilingManager;
		friend ENGINE_API auto InitializeAssetCompilingManager() -> FAssetCompilingManagerInitializationResult;
	};

	// Aggregates typed asset compilers and routes object operations by reflected class.
	enum class EAssetCompilerRegistrationError : uint8 { None, InvalidRegistration, WrongThread, NotAccepting, DuplicateName, DuplicateClass, Start };
	struct FAssetCompilerRegistrationError
	{
		EAssetCompilerRegistrationError Code = EAssetCompilerRegistrationError::None;
		std::string CompilerName;
		std::string AssetClass;
		std::string Message;
	};
	struct FAssetCompilerRegistrationResult
	{
		FAssetCompilerRegistrationHandle Handle;
		FAssetCompilerRegistrationError Error;
		explicit operator bool() const { return Error.Code == EAssetCompilerRegistrationError::None; }
	};
	ENGINE_API auto FormatAssetCompilerRegistrationError(const FAssetCompilerRegistrationError& Error) -> std::string;

	struct FAssetCompilingManagerInitializationResult
	{
		FAssetCompilerRegistrationError Error;
		explicit operator bool() const { return Error.Code == EAssetCompilerRegistrationError::None; }
	};

	class FAssetCompilingManager final
	{
	public:
		ENGINE_API static auto Get() -> FAssetCompilingManager&;
		// GameThread only once its identity is established. Idempotent while running;
		// restarting after terminal Shutdown violates the process lifecycle contract.
		ENGINE_API auto Start() -> void;
		// GameThread only. Reset the handle outside compiler callbacks before module
		// unload; reset stops admission, finishes compilation, and shuts down the provider.
		ENGINE_API auto RegisterCompiler(
			FAssetCompilingManagerRegistration Registration) -> FAssetCompilerRegistrationResult;
		ENGINE_API auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params = {})
			-> FAssetCompileProcessResult;
		// Editor tick convenience: a shared 2ms soft deadline, checked between completions.
		ENGINE_API auto ProcessAsyncTasks(bool bLimitExecutionTime) -> FAssetCompileProcessResult;
		ENGINE_API auto GetNumRemainingAssets() const -> uint64;
		ENGINE_API auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult;
		ENGINE_API auto FinishCompilationForObject(DObject& Object)
			-> FAssetCompileProcessResult;
		ENGINE_API auto MarkCompilationAsCanceled(std::span<DObject* const> Objects)
			-> void;
		ENGINE_API auto MarkCompilationAsCanceled(DObject& Object) -> void;
		ENGINE_API auto FinishAllCompilation() -> FAssetCompileProcessResult;
		ENGINE_API auto GetDiagnostics() const -> FAssetCompilingManagerDiagnostics;
		ENGINE_API auto OnAssetPostCompile() -> FAssetPostCompileEvent&;
		ENGINE_API auto Shutdown() -> void;
		ENGINE_API auto IsAcceptingRequests() const -> bool;

	private:
		FAssetCompilingManager() = default;
		auto Unregister(FName CompilerName, uint64 Generation) -> void;

		friend class FAssetCompilerRegistrationHandle;
		friend ENGINE_API auto InitializeAssetCompilingManager() -> FAssetCompilingManagerInitializationResult;
	};

	ENGINE_API auto InitializeAssetCompilingManager() -> FAssetCompilingManagerInitializationResult;
	ENGINE_API auto ShutdownAssetCompilingManager() -> void;
}
