#pragma once

#include "Asset/AssetCompilingManager.h"
#include "Hash/XxHash.h"
#include "Modules/ModularFeature.h"
#include "Threading/Task.h"
#include "Texture/Texture2DCompilation.h"

namespace Durin
{
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

	struct FTexture2DCompilationWork
	{
		std::string AssetIdentity;
		FTextureSourceData SourceData;
		FXxHash128 SourceHash;
		FTexture2DBuildSettingsSnapshot Settings;
		FObjectHandle Owner;
		uint64 RequestSerial = 0;
		uint32 EstimatedWidth = 0;
		uint32 EstimatedHeight = 0;
		ETexture2DCompilationPriority Priority = ETexture2DCompilationPriority::Background;
		ECookTargetPlatform TargetPlatform = ECookTargetPlatform::Win64;
		ECookTargetProfile TargetProfile = ECookTargetProfile::Game;
		bool bPersistDerivedData = true;
	};

	struct FTexture2DCompilationWorkResult
	{
		uint64 RequestId = 0;
		FObjectHandle Owner;
		uint64 RequestSerial = 0;
		std::string AssetIdentity;
		FXxHash128 SourceHash;
		FTexture2DBuildSettingsSnapshot Settings;
		std::unique_ptr<FTextureSourceData> SourceData;
		std::unique_ptr<FTexturePlatformData> PlatformData;
		std::string DerivedDataKey;
		std::string PersistenceDiagnostic;
		std::string Error;
		FTexture2DCompilationMetrics Metrics;
		FTexture2DBuildInputIdentity InputIdentity;
		ETexture2DCompilationPhase FailurePhase = ETexture2DCompilationPhase::None;
		ETexture2DCompilationPhase Phase = ETexture2DCompilationPhase::Failed;
	};

	struct FTexture2DCompilationDomainConfig
	{
		uint32 MaxWorkers = 2;
		uint32 InteractiveBurstLimit = 4;
		uint64 InFlightByteBudget = 1024ull * 1024ull * 1024ull;
	};

	using FTexture2DCompilationWorkCompletion =
		std::function<void(FTexture2DCompilationWorkResult&&)>;

	// Owns the complete Texture2D compilation domain: typed asset state, bounded
	// worker admission, cancellation, completion pumping, and publication.
	class FTexture2DCompilationDomain final : public IAssetCompilationDomain
	{
	public:
		explicit FTexture2DCompilationDomain(
			const FTexture2DCompilationDomainConfig& Config = {});
		~FTexture2DCompilationDomain() override;
		FTexture2DCompilationDomain(const FTexture2DCompilationDomain&) = delete;
		auto operator=(const FTexture2DCompilationDomain&)
			-> FTexture2DCompilationDomain& = delete;

		auto GetDomainName() const -> FName override;
		auto Start(std::string* OutError) -> bool override;
		auto StopAdmission() -> void override;
		auto GetNumRemainingAssets() const -> uint64 override;
		auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
			-> FAssetCompileProcessResult override;
		auto FinishCompilationForObjects(std::span<DObject* const> Objects)
			-> FAssetCompileProcessResult override;
		auto MarkCompilationAsCanceled(std::span<DObject* const> Objects) -> void override;
		auto FinishAllCompilation() -> FAssetCompileProcessResult override;
		auto Shutdown() -> void override;

		auto Submit(
			DTexture2D& Texture,
			FTexture2DCompilationRequest Request,
			std::string& OutError,
			FTexture2DCompilationCompletion Completion) -> bool;
		auto GetDiagnostic(const DTexture2D& Texture) const
			-> FTexture2DCompilationDiagnostic;
		auto GetManagerDiagnostics() const -> FTexture2DCompilationManagerDiagnostics;
		auto HasPending(const DTexture2D& Texture) const -> bool;
		auto Wait(DTexture2D& Texture, double TimeoutSeconds) -> bool;
		auto SetPhaseHookForTests(
			std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void;

	private:
		struct FQueueState;
		struct FCompilationState;

		auto SubmitWork(
			FTexture2DCompilationWork Work,
			FTexture2DCompilationWorkCompletion Completion) -> uint64;
		auto CancelWork(uint64 RequestId) -> bool;
		auto GetWorkDiagnostic(uint64 RequestId) const -> FTexture2DCompilationDiagnostic;
		auto GetQueuedWorkCount() const -> uint32;
		auto GetRunningWorkCount() const -> uint32;
		auto GetWorkManagerDiagnostics() const -> FTexture2DCompilationManagerDiagnostics;
		auto PumpWorkCompletions(uint32 MaximumCount) -> uint32;
		auto WaitForWork(uint64 RequestId, double TimeoutSeconds) -> bool;
		auto StartWorkAdmission() -> bool;
		auto StopWorkAdmission() -> void;
		auto ShutdownWorkQueue() -> void;
		auto ApplyCompletion(FTexture2DCompilationWorkResult&& Result) -> void;
		auto PumpCompletions(uint32 MaximumCount) -> FAssetCompileProcessResult;
		auto Cancel(DTexture2D& Texture) -> bool;

		std::shared_ptr<FQueueState> QueueState;
		std::shared_ptr<FCompilationState> CompilationState;
	};
}

namespace Durin::AssetPrivate
{
	auto CreateTexture2DCompilationDomain() -> std::shared_ptr<IAssetCompilationDomain>;
	auto ReleaseTexture2DCompilationDomain() -> void;
}
