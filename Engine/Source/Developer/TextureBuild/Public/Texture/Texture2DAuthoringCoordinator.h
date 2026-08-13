#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Asset/SourcePath.h"
#include "Texture/Texture2D.h"

namespace Durin::Asset::Build
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
	struct FTexture2DQueuedBuildRequest
	{
		std::string AssetIdentity;
		FSourcePath SourcePath;
		FTextureSourceData SourceData;
		FXxHash128 SourceHash;
		FTexture2DBuildSettingsSnapshot Settings;
		uint64 Generation = 0;
		uint32 EstimatedWidth = 0;
		uint32 EstimatedHeight = 0;
		ETexture2DBuildPriority Priority = ETexture2DBuildPriority::Background;
		bool bPersistDerivedData = true;
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

	// Owns detached build output until a main-thread consumer accepts or discards it.
	struct FTexture2DQueuedBuildResult
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

	using FTexture2DBuildCompletion = std::function<void(FTexture2DQueuedBuildResult&&)>;

	// Owns bounded Texture2D worker admission and a main-thread completion mailbox.
	class FTexture2DBuildCoordinator
	{
	public:
		TEXTUREBUILD_API explicit FTexture2DBuildCoordinator(
			const FTexture2DBuildCoordinatorConfig& Config = {});
		TEXTUREBUILD_API ~FTexture2DBuildCoordinator();
		FTexture2DBuildCoordinator(const FTexture2DBuildCoordinator&) = delete;
		auto operator=(const FTexture2DBuildCoordinator&) -> FTexture2DBuildCoordinator& = delete;

		// Returns zero when admission has stopped or the process task system rejects work.
		TEXTUREBUILD_API auto Submit(
			FTexture2DQueuedBuildRequest Request,
			FTexture2DBuildCompletion Completion) -> uint64;
		TEXTUREBUILD_API auto Cancel(uint64 RequestId) -> bool;
		TEXTUREBUILD_API auto CancelAsset(std::string_view AssetIdentity) -> uint32;
		TEXTUREBUILD_API auto GetDiagnostic(uint64 RequestId) const -> FTexture2DBuildDiagnostic;
		TEXTUREBUILD_API auto GetQueuedCount() const -> uint32;
		TEXTUREBUILD_API auto GetRunningCount() const -> uint32;
		TEXTUREBUILD_API auto GetInFlightEstimatedBytes() const -> uint64;
		// Installs a controlled-barrier seam used only by deterministic native tests.
		TEXTUREBUILD_API auto SetPhaseHookForTests(
			std::function<void(uint64, ETexture2DBuildPhase)> Hook) -> void;

		// Reopens admission after a completed Shutdown so the owning build host
		// can honor its restartable process-lifecycle contract.
		TEXTUREBUILD_API auto Start() -> bool;
		// Applies completed callbacks on the GameThread and returns the number pumped.
		TEXTUREBUILD_API auto PumpCompletions(uint32 MaximumCount = 64) -> uint32;
		// Explicit blocking boundary for save, cook, tools, and deterministic tests.
		TEXTUREBUILD_API auto WaitForRequest(uint64 RequestId, double TimeoutSeconds) -> bool;
		// Stops admission, cooperatively cancels all jobs, waits for workers, and drains callbacks.
		TEXTUREBUILD_API auto Shutdown() -> void;

	private:
		struct FState;
		std::shared_ptr<FState> State;
	};

}
