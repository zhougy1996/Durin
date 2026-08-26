#pragma once

#include "DObject/ObjectHandle.h"
#include "EngineAPI.h"
#include "Materials/MaterialProgramCompiler.h"
#include "Threading/Task.h"

#include <span>
#include <string>
#include <vector>

namespace Durin
{
	class DMaterial;

	inline constexpr uint32 MaterialCompileMaxConcurrentRequests = 64;
	inline constexpr uint32 MaterialCompileMaxConsumers = 256;
	inline constexpr uint32 MaterialCompileMaxResidentPrograms = 128;
	inline constexpr uint64 MaterialCompileMaxRequestBytes = 2ull * 1024ull * 1024ull;
	inline constexpr uint64 MaterialCompileMaxResultBytes = 8ull * 1024ull * 1024ull;

	// Separates authored freshness from the terminal state of the latest request.
	enum class EMaterialCompileState : uint8
	{
		NeverRequested,
		Pending,
		Running,
		Ready,
		Failed,
		Canceled,
		Superseded,
		Rejected,
		Shutdown,
	};

	// Adds lifecycle failures that are intentionally outside the synchronous compiler taxonomy.
	enum class EMaterialCompileResultCategory : uint8
	{
		None,
		Validation,
		Normalization,
		Generation,
		Dependency,
		Compile,
		Reflection,
		Binding,
		Cache,
		Cook,
		RendererResource,
		Cancellation,
		Supersession,
		Admission,
		Shutdown,
	};

	enum class EMaterialCompileCacheOutcome : uint8
	{
		None,
		RetainedHit,
		SingleFlight,
		Compiled,
		Forced,
	};

	// Bounded asset-qualified diagnostic retained only for the latest request.
	struct FMaterialCompileDiagnostic
	{
		EMaterialCompileResultCategory Category =
			EMaterialCompileResultCategory::None;
		FMaterialProgramDiagnostic Source;
		std::string AssetPath;
		FMaterialProgramIdentity ProgramIdentity;
		uint64 Generation = 0;
		bool bLastKnownGoodDisplayed = false;
	};

	// Immutable value-owned request captured on GameThread before Worker admission.
	struct FMaterialCompileRequest
	{
		FObjectHandle Owner;
		uint64 AuthoredRevision = 0;
		uint64 Generation = 0;
		uint64 DependencyRevision = 0;
		FMaterialProgramIdentity ProgramIdentity;
		FMaterialCompilerInput CompilerInput;
		std::string AssetPath;
		std::string Target;
		bool bForceRecompile = false;
		bool bSingleFlightConsumer = false;
	};

	// Value-owned Worker result admitted only after all owner-generation qualifiers match.
	struct FMaterialCompileResult
	{
		FObjectHandle Owner;
		uint64 AuthoredRevision = 0;
		uint64 Generation = 0;
		uint64 DependencyRevision = 0;
		FMaterialProgramIdentity ProgramIdentity;
		FMaterialStaticProperties StaticProperties;
		std::string Target;
		EMaterialCompileState State = EMaterialCompileState::Failed;
		EMaterialCompileResultCategory Category =
			EMaterialCompileResultCategory::None;
		EMaterialCompileCacheOutcome CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		uint64 TaskId = 0;
		std::shared_ptr<const FMaterialCompilerResult> CompiledProgram;
		std::vector<FMaterialCompileDiagnostic> Diagnostics;
	};

	// Asset-visible snapshot; none of its process-local ordering state is serialized.
	struct FMaterialCompileStatus
	{
		uint64 AuthoredRevision = 1;
		uint64 RequestGeneration = 0;
		uint64 CompiledAuthoredRevision = 0;
		uint64 DependencyRevision = 0;
		uint64 TaskId = 0;
		EMaterialCompileState State = EMaterialCompileState::NeverRequested;
		EMaterialCompileResultCategory ResultCategory =
			EMaterialCompileResultCategory::None;
		EMaterialCompileCacheOutcome CacheOutcome =
			EMaterialCompileCacheOutcome::None;
		FMaterialProgramIdentity RequestedIdentity;
		FMaterialProgramIdentity CompiledIdentity;
		std::string Target;
		uint64 DurationMicroseconds = 0;
		bool bHasLastKnownGood = false;
		bool bLastKnownGoodDisplayed = false;

		auto IsCurrent() const -> bool
		{
			return State == EMaterialCompileState::Ready
				&& AuthoredRevision == CompiledAuthoredRevision;
		}
	};

	// Fixed aggregate counters retain no per-request history.
	struct FMaterialCompilationDiagnostics
	{
		bool bAcceptingRequests = false;
		uint64 AcceptedRequests = 0;
		uint64 RejectedRequests = 0;
		uint64 CompletedRequests = 0;
		uint64 FailedRequests = 0;
		uint64 CanceledRequests = 0;
		uint64 SupersededRequests = 0;
		uint64 RetainedHits = 0;
		uint64 SingleFlightConsumers = 0;
		uint32 InFlightCount = 0;
		uint32 OutstandingConsumerCount = 0;
		uint32 PendingPublicationCount = 0;
		uint32 RetainedProgramCount = 0;
		uint64 RetainedProgramBytes = 0;
	};

	ENGINE_API auto IsMaterialCompilationAcceptingRequests() -> bool;
	ENGINE_API auto GetMaterialCompilationDiagnostics()
		-> FMaterialCompilationDiagnostics;

	// Thread-safe reload notification; object discovery and requests remain GameThread-only.
	ENGINE_API auto NotifyMaterialShaderReload(bool bForceRecompile) -> void;
	ENGINE_API auto RequestMaterialRecompile(
		DMaterial& Material, bool bForceRecompile = false) -> bool;

	namespace Private
	{
		struct FMaterialCompilationLifecycle
		{
			ENGINE_API static auto Submit(
				DMaterial& Material,
				FMaterialCompilerInput Input,
				bool bForceRecompile) -> bool;
			ENGINE_API static auto Admit(
				DMaterial& Material,
				FMaterialCompileResult Result) -> bool;
			ENGINE_API static auto MarkCanceled(DMaterial& Material) -> void;
			ENGINE_API static auto RequestCurrent(
				DMaterial& Material, bool bForceRecompile) -> bool;
		};
	}
}
