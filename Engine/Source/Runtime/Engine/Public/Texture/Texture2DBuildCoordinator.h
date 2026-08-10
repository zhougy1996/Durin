#pragma once

#include "EngineAPI.h"
#include "Hash/XxHash.h"
#include "Source/SourcePath.h"
#include "Texture/Texture2D.h"

namespace Durin
{
	// Identifies the externally visible phase of one editor Texture2D CPU build.
	enum class ETexture2DBuildPhase : uint8
	{
		None,
		Queued,
		Decoding,
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

	// Captures every setting that contributes to a Texture2D platform build and DDC key.
	struct FTexture2DBuildSettingsSnapshot
	{
		ETextureUsage Usage = ETextureUsage::Color;
		bool bSRGB = true;
		uint32 MaxResolution = 0;
		ETextureCompressionQuality CompressionQuality = ETextureCompressionQuality::Normal;
		ETextureAlphaMipMode AlphaMipMode = ETextureAlphaMipMode::Average;
		float AlphaCoverageThreshold = 0.5f;

		auto operator==(const FTexture2DBuildSettingsSnapshot&) const -> bool = default;
	};

	// Immutable value snapshot consumed without reflected-object access on a worker.
	struct FTexture2DBuildRequest
	{
		std::string AssetIdentity;
		FSourcePath SourcePath;
		std::vector<uint8> EncodedSource;
		FXxHash128 ExpectedSourceHash;
		FTexture2DBuildSettingsSnapshot Settings;
		uint64 Generation = 0;
		uint32 EstimatedWidth = 0;
		uint32 EstimatedHeight = 0;
		ETexture2DBuildPriority Priority = ETexture2DBuildPriority::Background;
		bool bHasExpectedSourceHash = false;
		bool bPersistDerivedData = true;
	};

	// Records worker timing and conservative versus observed retained bytes.
	struct FTexture2DBuildMetrics
	{
		uint64 DecodeNanoseconds = 0;
		uint64 MipGenerationNanoseconds = 0;
		uint64 CompressionNanoseconds = 0;
		uint64 PersistenceNanoseconds = 0;
		uint64 WorkerNanoseconds = 0;
		uint64 EstimatedBytes = 0;
		uint64 DecodedBytes = 0;
		uint64 PeakIntermediateBytes = 0;
		uint64 ResultBytes = 0;
	};

	// Owns detached build output until a main-thread consumer accepts or discards it.
	struct FTexture2DBuildResult
	{
		uint64 RequestId = 0;
		uint64 Generation = 0;
		std::string AssetIdentity;
		FSourcePath SourcePath;
		FXxHash128 SourceHash;
		FTexture2DBuildSettingsSnapshot Settings;
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		std::string Error;
		FTexture2DBuildMetrics Metrics;
		ETexture2DBuildPhase FailurePhase = ETexture2DBuildPhase::None;
		ETexture2DBuildPhase Phase = ETexture2DBuildPhase::Failed;
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

	struct FTexture2DBuildCoordinatorConfig
	{
		uint32 MaxWorkers = 2;
		uint32 InteractiveBurstLimit = 4;
		uint64 InFlightByteBudget = 1024ull * 1024ull * 1024ull;
	};

	using FTexture2DBuildCompletion = std::function<void(FTexture2DBuildResult&&)>;

	// Owns bounded Texture2D worker admission and a main-thread completion mailbox.
	class FTexture2DBuildCoordinator
	{
	public:
		ENGINE_API explicit FTexture2DBuildCoordinator(
			const FTexture2DBuildCoordinatorConfig& Config = {});
		ENGINE_API ~FTexture2DBuildCoordinator();
		FTexture2DBuildCoordinator(const FTexture2DBuildCoordinator&) = delete;
		auto operator=(const FTexture2DBuildCoordinator&) -> FTexture2DBuildCoordinator& = delete;

		// Returns zero when admission has stopped or the process task system rejects work.
		ENGINE_API auto Submit(
			FTexture2DBuildRequest Request,
			FTexture2DBuildCompletion Completion) -> uint64;
		ENGINE_API auto Cancel(uint64 RequestId) -> bool;
		ENGINE_API auto CancelAsset(std::string_view AssetIdentity) -> uint32;
		ENGINE_API auto GetDiagnostic(uint64 RequestId) const -> FTexture2DBuildDiagnostic;
		ENGINE_API auto GetQueuedCount() const -> uint32;
		ENGINE_API auto GetRunningCount() const -> uint32;
		ENGINE_API auto GetInFlightEstimatedBytes() const -> uint64;
		// Installs a controlled-barrier seam used only by deterministic native tests.
		ENGINE_API auto SetPhaseHookForTests(
			std::function<void(uint64, ETexture2DBuildPhase)> Hook) -> void;

		// Applies completed callbacks on the GameThread and returns the number pumped.
		ENGINE_API auto PumpCompletions(uint32 MaximumCount = 64) -> uint32;
		// Explicit blocking boundary for save, cook, tools, and deterministic tests.
		ENGINE_API auto WaitForRequest(uint64 RequestId, double TimeoutSeconds) -> bool;
		// Stops admission, cooperatively cancels all jobs, waits for workers, and drains callbacks.
		ENGINE_API auto Shutdown() -> void;

	private:
		struct FState;
		std::shared_ptr<FState> State;
	};

	ENGINE_API auto InitializeTexture2DBuildCoordinator(
		const FTexture2DBuildCoordinatorConfig& Config = {}) -> bool;
	ENGINE_API auto GetTexture2DBuildCoordinator() -> FTexture2DBuildCoordinator*;
	ENGINE_API auto PumpTexture2DBuildCompletions(uint32 MaximumCount = 64) -> uint32;
	ENGINE_API auto ShutdownTexture2DBuildCoordinator() -> void;
}
