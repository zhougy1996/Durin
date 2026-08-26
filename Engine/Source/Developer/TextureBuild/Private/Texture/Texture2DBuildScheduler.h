#pragma once

#include "TextureBuildAPI.h"
#include "Hash/XxHash.h"
#include "Asset/SourcePath.h"
#include "Threading/Task.h"
#include "Texture/Texture2D.h"
#include "Texture/Texture2DAuthoringTypes.h"

namespace Durin::Asset
{
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

	struct FTexture2DBuildSchedulerConfig
	{
		uint32 MaxWorkers = 2;
		uint32 InteractiveBurstLimit = 4;
		uint64 InFlightByteBudget = 1024ull * 1024ull * 1024ull;
		FTaskCancellationToken OwnerCancellationToken;
		FTaskScopeToken OwnerTaskScope;
	};

	using FTexture2DBuildCompletion = std::function<void(FTexture2DQueuedBuildResult&&)>;

	// Owns bounded Texture2D worker admission and a main-thread completion mailbox.
	class FTexture2DBuildScheduler
	{
	public:
		explicit FTexture2DBuildScheduler(
			const FTexture2DBuildSchedulerConfig& Config = {});
		~FTexture2DBuildScheduler();
		FTexture2DBuildScheduler(const FTexture2DBuildScheduler&) = delete;
		auto operator=(const FTexture2DBuildScheduler&)
			-> FTexture2DBuildScheduler& = delete;

		auto Submit(
			FTexture2DQueuedBuildRequest Request,
			FTexture2DBuildCompletion Completion) -> uint64;
		auto Cancel(uint64 RequestId) -> bool;
		auto GetDiagnostic(uint64 RequestId) const
			-> FTexture2DBuildDiagnostic;
		auto GetQueuedCount() const -> uint32;
		auto GetRunningCount() const -> uint32;
		auto SetPhaseHookForTests(
			std::function<void(uint64, ETexture2DBuildPhase)> Hook) -> void;
		auto Start() -> bool;
		auto StopAdmission() -> void;
		auto PumpCompletions(uint32 MaximumCount = 64) -> uint32;
		auto WaitForRequest(
			uint64 RequestId, double TimeoutSeconds) -> bool;
		auto Shutdown() -> void;

	private:
		struct FState;
		std::shared_ptr<FState> State;
	};
}
