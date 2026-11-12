#pragma once

#include "CoreMinimal.h"

#include "DObject/WeakObjectPtr.h"
#include "EngineAPI.h"
#include "Materials/MaterialProgramCompiler.h"

namespace Durin
{
	class FObjectCacheContext;
	class DMaterialInterface;
	struct FMaterialCompilationResource;

	enum class EMaterialShaderPrecompileMode : uint8
	{
		None,
		Background,
		Synchronous,
		Default = Background,
	};

	// Editor policy for authored edits; explicit requests and loading bypass it.
	enum class EMaterialEditCompileMode : uint8
	{
		Immediate,
		Automatic,
		Manual,
	};

	// Query projection: Running comes from the live resource association, other values are outcomes.
	enum class EMaterialCompileState : uint8
	{
		NeverRequested,
		Running,
		Ready,
		Failed,
		Canceled,
	};

	enum class EMaterialCompileCacheOutcome : uint8
	{
		None,
		RetainedHit,
		SingleFlight,
		Compiled,
		Forced,
	};

	enum class EMaterialCompileOutcome : uint8 { Succeeded, Failed, Canceled };

	// Detached input plus resource association captured on GameThread before dispatch.
	struct FMaterialCompileRequest
	{
		std::shared_ptr<FMaterialCompilationResource> Resource;
		uint64 RequestId = 0;
		uint64 ShaderRevision = 0;
		std::shared_ptr<const MIR::FCompilerInput> CompilerInput;
		bool bForceRecompile = false;
		bool bSingleFlightConsumer = false;
	};

	// Manager completion handoff; only the still-associated resource may install it.
	struct FMaterialCompileResult
	{
		std::shared_ptr<FMaterialCompilationResource> Resource;
		uint64 RequestId = 0;
		uint64 ShaderRevision = 0;
		FMaterialStaticProperties StaticProperties;
		EMaterialCompileOutcome Outcome = EMaterialCompileOutcome::Failed;
		EMaterialCompileCacheOutcome CacheOutcome = EMaterialCompileCacheOutcome::None;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
		std::vector<MIR::FSource> ExpressionSources;
		std::vector<FMaterialProgramDiagnostic> Diagnostics;
	};

	// Asset-visible value snapshot. Scheduling and installed freshness are independent of request outcome.
	struct FMaterialCompileStatus
	{
		uint64 AuthoredRevision = 1;
		uint64 SubmittedRevision = 1;
		uint64 CompiledRevision = 0;
		uint64 CompiledDependencyRevision = 0;
		bool bDependenciesCurrent = true;
		bool bHasScheduledEdits = false;
		uint64 RequestId = 0;
		EMaterialCompileState State = EMaterialCompileState::NeverRequested;
		EMaterialCompileCacheOutcome CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		FMaterialProgramIdentity CompiledIdentity;
		std::string Target;

		auto IsCurrent() const -> bool
		{
			return CompiledIdentity.IsValid() && CompiledRevision == AuthoredRevision
				&& bDependenciesCurrent;
		}
		auto HasUnsubmittedEdits() const -> bool
		{
			return SubmittedRevision != AuthoredRevision;
		}
		auto HasScheduledEdits() const -> bool { return bHasScheduledEdits; }
	};

	// Fixed aggregate counters retain no per-request history.
	struct FMaterialCompilationDiagnostics
	{
		bool bAcceptingRequests = false;
		uint64 AcceptedRequests = 0;
		uint64 CompletedRequests = 0;
		uint64 FailedRequests = 0;
		uint64 CanceledRequests = 0;
		uint64 RetainedHits = 0;
		uint64 SingleFlightConsumers = 0;
		uint32 InFlightCount = 0;
		uint32 OutstandingConsumerCount = 0;
		uint32 PendingPublicationCount = 0;
		uint32 RetainedProgramCount = 0;
		uint64 RetainedProgramBytes = 0;
	};

	// Detached facts derived from one owner's currently accepted program. This is
	// a value snapshot: it never retains the material or compiler result.
	enum class EMaterialProgramStatisticsState : uint8
	{
		Absent,
		Current,
		LastKnownGood,
	};

	struct FMaterialProgramStatistics
	{
		EMaterialProgramStatisticsState State = EMaterialProgramStatisticsState::Absent;
		FMaterialProgramIdentity ProgramIdentity;
		EMaterialCompileCacheOutcome CacheOutcome = EMaterialCompileCacheOutcome::None;
		uint64 NormalizedIRNodeCount = 0;
		uint64 ActiveParameterCount = 0;
		uint64 UniformPayloadBytes = 0;
		uint64 TextureCount = 0;
		uint64 SamplerCount = 0;
		uint64 GeneratedSourceBytes = 0;
		uint64 CompiledShaderCount = 0;
		uint64 CompiledCodeBytes = 0;
		uint64 DependencyCount = 0;
		float CompileTime = 0;
		bool bSizeOverflow = false;
	};

	ENGINE_API auto IsMaterialCompilationAcceptingRequests() -> bool;
	// GameThread-only observation including canceled work awaiting owner-side reap.
	ENGINE_API auto HasPendingMaterialCompilation(const DMaterialInterface& Material) -> bool;
	ENGINE_API auto GetMaterialCompilationDiagnostics()
		-> FMaterialCompilationDiagnostics;

	// Thread-safe reload notification; object discovery and requests remain GameThread-only.
	ENGINE_API auto NotifyMaterialShaderReload(bool bForceRecompile) -> void;
	ENGINE_API auto RequestMaterialRecompile(
		DMaterialInterface& Material, bool bForceRecompile = false) -> bool;

	namespace Private
	{
		struct FMaterialCompilationLifecycle
		{
			ENGINE_API static auto Invalidate(DMaterialInterface& Material) -> void;
			// Coalesces edits before compiler-input construction, using the root's policy.
			ENGINE_API static auto ScheduleEdit(DMaterialInterface& Material, FObjectCacheContext* Context = nullptr) -> void;
			ENGINE_API static auto Submit(
				DMaterialInterface& Material,
				MIR::FCompilerInput Input,
				bool bForceRecompile, FObjectCacheContext* Context = nullptr,
				std::shared_ptr<FMaterialCompilationResource> Resource = {}) -> bool;
			ENGINE_API static auto Admit(
				DMaterialInterface& Material,
				FMaterialCompileResult Result, FObjectCacheContext* Context = nullptr) -> bool;
			ENGINE_API static auto MarkCanceled(DMaterialInterface& Material) -> void;
			ENGINE_API static auto SubmitScheduledEdit(DMaterialInterface& Material) -> void;
			ENGINE_API static auto RequestCurrent(
				DMaterialInterface& Material, bool bForceRecompile, FObjectCacheContext* Context = nullptr) -> bool;
		private:
			friend class Durin::DMaterialInterface;
			static auto ResetRequest(DMaterialInterface& Material, const std::shared_ptr<FMaterialCompilationResource>& Resource) -> void;
		};
	}
}
