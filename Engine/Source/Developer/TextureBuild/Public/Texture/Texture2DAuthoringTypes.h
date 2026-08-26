#pragma once

#include "TextureBuildAPI.h"

namespace Durin::Asset
{
	// Identifies the externally visible phase of one editor Texture2D CPU build.
	enum class ETexture2DBuildPhase : uint8
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

	enum class ETexture2DBuildPriority : uint8
	{
		Background,
		Interactive,
	};

	// Records worker timing and conservative versus observed retained bytes.
	struct FTexture2DBuildMetrics
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
	struct FTexture2DBuildDiagnostic
	{
		uint64 RequestId = 0;
		uint64 Generation = 0;
		std::string AssetIdentity;
		std::string DerivedDataKey;
		std::string Message;
		FTexture2DBuildMetrics Metrics;
		uint64 QueuedNanoseconds = 0;
		uint64 WorkerNanoseconds = 0;
		ETexture2DBuildPhase FailurePhase = ETexture2DBuildPhase::None;
		ETexture2DBuildPhase Phase = ETexture2DBuildPhase::None;
	};
}
