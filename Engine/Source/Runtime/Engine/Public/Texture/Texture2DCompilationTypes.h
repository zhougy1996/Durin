#pragma once

#include "EngineAPI.h"

namespace Durin
{
	// Identifies the externally visible phase of one object-level Texture2D compilation.
	enum class ETexture2DCompilationPhase : uint8
	{
		None,
		Queued,
		Preparing,
		Building,
		Persisting,
		UploadPending,
		Ready,
		Failed,
		Cancelled,
	};

	enum class ETexture2DCompilationPriority : uint8
	{
		Background,
		Interactive,
	};

	enum class ETexture2DCompilationOrigin : uint8
	{
		Unobserved,
		CacheHit,
		Rebuilt,
	};

	// Records worker timing and conservative versus observed retained bytes.
	struct FTexture2DCompilationMetrics
	{
		uint64 PreparationNanoseconds = 0;
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PersistenceNanoseconds = 0;
		uint64 WorkerNanoseconds = 0;
		uint64 CompletionNanoseconds = 0;
		uint64 EstimatedBytes = 0;
		uint64 DecodedBytes = 0;
		uint64 PeakIntermediateBytes = 0;
		uint64 ResultBytes = 0;
	};

	// Provides a thread-safe snapshot suitable for editor diagnostics.
	struct FTexture2DCompilationDiagnostic
	{
		uint64 RequestId = 0;
		// Latest-wins serial owned by the compiling manager; unrelated to DDC identity.
		uint64 RequestSerial = 0;
		std::string AssetIdentity;
		std::string DerivedDataKey;
		std::string Message;
		FTexture2DCompilationMetrics Metrics;
		uint64 QueuedNanoseconds = 0;
		uint64 WorkerNanoseconds = 0;
		ETexture2DCompilationPhase FailurePhase = ETexture2DCompilationPhase::None;
		ETexture2DCompilationPhase Phase = ETexture2DCompilationPhase::None;
		ETexture2DCompilationOrigin Origin = ETexture2DCompilationOrigin::Unobserved;
		bool bSourceDecoderInvoked = false;
	};

	struct FTexture2DCompilationManagerDiagnostics
	{
		uint64 ActiveRecordCount = 0;
		uint64 RetainedWorkCount = 0;
		uint64 InFlightEstimatedBytes = 0;
		uint32 QueuedWorkCount = 0;
		uint32 RunningWorkCount = 0;
		uint32 PendingCompletionCount = 0;
	};
}
