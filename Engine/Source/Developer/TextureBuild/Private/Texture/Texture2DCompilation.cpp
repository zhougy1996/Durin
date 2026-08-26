#include "Texture/Texture2DCompilation.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/Texture2DCompilationDomain.h"
#include "Texture/TextureBuilder.h"
#include "Texture/Texture2DPostLoad.h"

namespace Durin::Asset
{
	namespace
	{
		struct FTexture2DCompilationState
		{
			TWeakObjectPtr<DTexture2D> Texture;
			uint64 Generation = 0;
			uint64 ActiveRequestId = 0;
			uint64 LastRequestId = 0;
			bool bLastRequestFailed = false;
			FTexture2DPublicationContext PublicationContext;
			FTexture2DCompilationCompletion Completion;
		};

		std::mutex GTexture2DCompilationMutex;
		std::unordered_map<std::string, FTexture2DCompilationState> GTexture2DCompilationStates;
		uint64 GNextTexture2DGeneration = 1;
		std::vector<FWeakObjectPtr> GSuccessfullyPublishedTextures;
		std::mutex GTextureCompilationDomainMutex;
		std::shared_ptr<FTexture2DCompilationScheduler> GTextureCompilationScheduler;
		FAssetCompilingManagerRegistration GTextureCompilingRegistration;
		auto CancelTexture2DCompilation(DTexture2D& Texture) -> bool;

		auto GetTexture2DCompilationScheduler() -> std::shared_ptr<FTexture2DCompilationScheduler>
		{
			std::lock_guard Lock(GTextureCompilationDomainMutex);
			return GTextureCompilationScheduler;
		}

		auto FindStateLocked(std::string_view Identity) -> FTexture2DCompilationState*
		{
			const auto It = GTexture2DCompilationStates.find(std::string(Identity));
			return It == GTexture2DCompilationStates.end() ? nullptr : &It->second;
		}

		auto ApplyCompletion(FTexture2DCompilationJobResult&& Result) -> void
		{
			CheckGameThread();
			TWeakObjectPtr<DTexture2D> WeakTexture;
			FTexture2DPublicationContext PublicationContext;
			FTexture2DCompilationCompletion Completion;
			{
				std::lock_guard Lock(GTexture2DCompilationMutex);
				FTexture2DCompilationState* State = FindStateLocked(Result.AssetIdentity);
				if (!State || State->Generation != Result.Generation
					|| State->ActiveRequestId != Result.RequestId) return;
				State->ActiveRequestId = 0;
				State->LastRequestId = Result.RequestId;
				State->bLastRequestFailed = Result.Phase == ETexture2DCompilationPhase::Failed;
				WeakTexture = State->Texture;
				PublicationContext = State->PublicationContext;
				Completion = std::move(State->Completion);
			}
			DTexture2D* Texture = WeakTexture.Get();
			if (!Texture || Texture->GetObjectPath() != Result.AssetIdentity)
			{
				if (Completion) Completion({
					.Status = ETexture2DCompilationStatus::Failed,
					.Diagnostic = "The Texture2D compilation target is unavailable."});
				return;
			}
			if (Result.Phase != ETexture2DCompilationPhase::UploadPending
				|| !Result.SourceData || !Result.PlatformData)
			{
				if (Completion) Completion({
					.Status = Result.Phase == ETexture2DCompilationPhase::Cancelled
						? ETexture2DCompilationStatus::Canceled : ETexture2DCompilationStatus::Failed,
					.Diagnostic = Result.Error.empty()
						? "The Texture2D compilation build did not produce a publishable product."
						: std::move(Result.Error)});
				return;
			}

			FTexture2DBuildProduct Product{
				.SourceData = std::move(*Result.SourceData),
				.PlatformData = std::move(*Result.PlatformData),
				.DerivedDataKey = std::move(Result.DerivedDataKey),
				.SourceContentHashLow = Result.SourceHash.HashLow,
				.SourceContentHashHigh = Result.SourceHash.HashHigh,
				.Settings = {
					.Usage = Result.Settings.Usage,
					.CompressionQuality = Result.Settings.CompressionQuality,
					.AlphaMipMode = Result.Settings.AlphaMipMode,
					.AlphaCoverageThreshold = Result.Settings.AlphaCoverageThreshold,
					.MaxResolution = Result.Settings.MaxResolution,
					.bSRGB = Result.Settings.bSRGB},
				.bSRGB = Result.Settings.bSRGB};
			std::string Error;
			if (!PublishTexture2DProduct(
				*Texture, std::move(Product), PublicationContext, Error))
			{
				{
					std::lock_guard Lock(GTexture2DCompilationMutex);
					if (FTexture2DCompilationState* State = FindStateLocked(Result.AssetIdentity);
						State && State->Generation == Result.Generation)
						State->bLastRequestFailed = true;
				}
				DURIN_ERROR("Texture2D compilation publication failed for {}: {}",
					Result.AssetIdentity, Error);
				if (Completion) Completion({
					.Status = ETexture2DCompilationStatus::Failed,
					.Diagnostic = std::move(Error)});
				return;
			}
			{
				std::lock_guard Lock(GTexture2DCompilationMutex);
				GSuccessfullyPublishedTextures.emplace_back(Texture);
			}
			if (Completion) Completion({.Status = ETexture2DCompilationStatus::Succeeded});
		}

		auto PumpTextureCompletions(uint32 MaximumCount) -> FAssetCompileProcessResult
		{
			FAssetCompileProcessResult Result;
			if (const auto Scheduler = GetTexture2DCompilationScheduler())
				Result.ProcessedCompletionCount = Scheduler->PumpCompletions(MaximumCount);
			std::lock_guard Lock(GTexture2DCompilationMutex);
			Result.SuccessfullyCompiledAssets = std::move(GSuccessfullyPublishedTextures);
			GSuccessfullyPublishedTextures.clear();
			return Result;
		}

		class FTextureCompilingManager final : public IAssetCompilingManager
		{
		public:
			explicit FTextureCompilingManager(
				std::shared_ptr<FTexture2DCompilationScheduler> InScheduler)
				: Scheduler(std::move(InScheduler))
			{
			}

			auto GetDomainName() const -> FName override
			{
				return FName("Durin.TextureCompilation");
			}
			auto Start(std::string* OutError) -> bool override
			{
				if (Scheduler && Scheduler->Start())
				{
					if (OutError) OutError->clear();
					return true;
				}
				if (OutError) *OutError = "Texture2D build scheduler could not start.";
				return false;
			}
			auto StopAdmission() -> void override
			{
				if (Scheduler) Scheduler->StopAdmission();
			}
			auto GetNumRemainingAssets() const -> uint64 override
			{
				std::lock_guard Lock(GTexture2DCompilationMutex);
				return static_cast<uint64>(std::ranges::count_if(
					GTexture2DCompilationStates, [](const auto& Item) {
						return Item.second.ActiveRequestId != 0
							&& Item.second.Texture.IsValid();
					}));
			}
			auto ProcessAsyncTasks(const FAssetCompileProcessParams& Params)
				-> FAssetCompileProcessResult override
			{
				return PumpTextureCompletions(Params.MaximumCompletions);
			}
			auto FinishCompilationForObjects(std::span<DObject* const> Objects)
				-> FAssetCompileProcessResult override
			{
				FAssetCompileProcessResult Aggregate;
				for (DObject* Object : Objects)
				{
					auto* Texture = Cast<DTexture2D>(Object);
					if (!IsValid(Texture)) continue;
					uint64 RequestId = 0;
					{
						std::lock_guard Lock(GTexture2DCompilationMutex);
						if (FTexture2DCompilationState* State = FindStateLocked(
							Texture->GetObjectPath()); State && State->Texture.Get() == Texture)
							RequestId = State->ActiveRequestId;
					}
					if (RequestId == 0) continue;
					if (Scheduler) Scheduler->WaitForRequest(RequestId, 300.0);
					auto Item = PumpTextureCompletions(std::numeric_limits<uint32>::max());
					Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(
						Aggregate.SuccessfullyCompiledAssets.end(),
						Item.SuccessfullyCompiledAssets.begin(),
						Item.SuccessfullyCompiledAssets.end());
				}
				return Aggregate;
			}
			auto MarkCompilationAsCanceled(std::span<DObject* const> Objects)
				-> void override
			{
				for (DObject* Object : Objects)
					if (auto* Texture = Cast<DTexture2D>(Object); IsValid(Texture))
						CancelTexture2DCompilation(*Texture);
			}
			auto FinishAllCompilation() -> FAssetCompileProcessResult override
			{
				FAssetCompileProcessResult Aggregate;
				while (Scheduler && (Scheduler->GetQueuedCount() != 0
					|| Scheduler->GetRunningCount() != 0))
				{
					auto Item = PumpTextureCompletions(std::numeric_limits<uint32>::max());
					Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
					Aggregate.SuccessfullyCompiledAssets.insert(
						Aggregate.SuccessfullyCompiledAssets.end(),
						Item.SuccessfullyCompiledAssets.begin(),
						Item.SuccessfullyCompiledAssets.end());
					if (Item.ProcessedCompletionCount == 0) std::this_thread::yield();
				}
				auto Item = PumpTextureCompletions(std::numeric_limits<uint32>::max());
				Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
				Aggregate.SuccessfullyCompiledAssets.insert(
					Aggregate.SuccessfullyCompiledAssets.end(),
					Item.SuccessfullyCompiledAssets.begin(),
					Item.SuccessfullyCompiledAssets.end());
				return Aggregate;
			}
			auto Shutdown() -> void override
			{
				if (Scheduler) Scheduler->Shutdown();
			}

		private:
			std::shared_ptr<FTexture2DCompilationScheduler> Scheduler;
		};
	}

	namespace Private
	{
		auto InitializeTexture2DCompilationDomain(
			FModuleOwnedCallbackGate OwnerGate,
			const FTexture2DCompilationSchedulerConfig& Config) -> bool
		{
			CheckGameThread();
			if (GTextureCompilingRegistration.IsValid()) return true;
			auto Scheduler = std::make_shared<FTexture2DCompilationScheduler>(Config);
			{
				std::lock_guard Lock(GTextureCompilationDomainMutex);
				GTextureCompilationScheduler = Scheduler;
			}
			std::string Error;
			GTextureCompilingRegistration = FAssetCompilingManager::Get().RegisterManager(
				std::make_shared<FTextureCompilingManager>(Scheduler),
				std::move(OwnerGate), &Error);
			if (!GTextureCompilingRegistration.IsValid())
			{
				DURIN_ERROR("Texture compilation manager registration failed: {}", Error);
				Scheduler->Shutdown();
				std::lock_guard Lock(GTextureCompilationDomainMutex);
				GTextureCompilationScheduler.reset();
			}
			return GTextureCompilingRegistration.IsValid();
		}

		auto ShutdownTexture2DCompilationDomain() -> void
		{
			CheckGameThread();
			GTextureCompilingRegistration.Reset();
			std::lock_guard Lock(GTextureCompilationDomainMutex);
			GTextureCompilationScheduler.reset();
		}

		auto SetTexture2DCompilationPhaseHookForTests(
			std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void
		{
			if (const auto Scheduler = GetTexture2DCompilationScheduler())
				Scheduler->SetPhaseHookForTests(std::move(Hook));
		}
	}

	auto SubmitTexture2DCompilation(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		CheckGameThread();
		if (!Request.Build.SourceData.IsValid()
			|| Request.Publication.SourcePath.IsEmpty()
			|| Request.Publication.DecoderId.empty())
		{
			OutError = "Texture2D compilation submission requires normalized source and provenance.";
			return false;
		}
		const auto Scheduler = GetTexture2DCompilationScheduler();
		if (!Scheduler || !FAssetCompilingManager::Get().IsAcceptingRequests())
		{
			OutError = "The Texture2D compilation domain is unavailable.";
			return false;
		}

		const std::string Identity = Texture.GetObjectPath();
		const FSourcePath SourcePath = Request.Publication.SourcePath;
		uint64 Generation = 0;
		uint64 PreviousRequestId = 0;
		FTexture2DCompilationCompletion SupersededCompletion;
		{
			std::lock_guard Lock(GTexture2DCompilationMutex);
			FTexture2DCompilationState& State = GTexture2DCompilationStates[Identity];
			PreviousRequestId = State.ActiveRequestId;
			if (PreviousRequestId != 0)
				SupersededCompletion = std::move(State.Completion);
			Generation = GNextTexture2DGeneration++;
			State = {
				.Texture = TWeakObjectPtr<DTexture2D>(&Texture),
				.Generation = Generation,
				.PublicationContext = std::move(Request.Publication),
				.Completion = std::move(Completion)};
		}
		if (PreviousRequestId != 0) Scheduler->Cancel(PreviousRequestId);

		const FTexture2DBuildSettings Settings = Request.Build.Settings;
		const bool bSRGB = Settings.bSRGB.value_or(
			TextureBuilder::GetDefaultSRGB(Settings.Usage));
		const uint32 Width = Request.Build.SourceData.Width;
		const uint32 Height = Request.Build.SourceData.Height;
		const uint64 RequestId = Scheduler->Submit({
			.AssetIdentity = Identity,
			.SourcePath = SourcePath,
			.SourceData = std::move(Request.Build.SourceData),
			.SourceHash = {
				.HashLow = Request.Build.SourceContentHashLow,
				.HashHigh = Request.Build.SourceContentHashHigh},
			.Settings = {
				.Usage = Settings.Usage,
				.bSRGB = bSRGB,
				.MaxResolution = Settings.MaxResolution,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold},
			.Generation = Generation,
			.EstimatedWidth = Width,
			.EstimatedHeight = Height,
			.Priority = Request.Priority,
			.bPersistDerivedData = Request.Build.bPersistDerivedData}, ApplyCompletion);
		if (RequestId == 0)
		{
			{
				std::lock_guard Lock(GTexture2DCompilationMutex);
				if (FTexture2DCompilationState* State = FindStateLocked(Identity);
					State && State->Generation == Generation)
					GTexture2DCompilationStates.erase(Identity);
			}
			if (SupersededCompletion) SupersededCompletion({
				.Status = ETexture2DCompilationStatus::Superseded,
				.Diagnostic = "The Texture2D compilation was superseded by a newer request."});
			OutError = "The Texture2D compilation domain rejected the request.";
			return false;
		}
		{
			std::lock_guard Lock(GTexture2DCompilationMutex);
			FTexture2DCompilationState* State = FindStateLocked(Identity);
			if (State && State->Generation == Generation)
			{
				State->ActiveRequestId = RequestId;
				State->LastRequestId = RequestId;
			}
		}
		if (SupersededCompletion) SupersededCompletion({
			.Status = ETexture2DCompilationStatus::Superseded,
			.Diagnostic = "The Texture2D compilation was superseded by a newer request."});
		OutError.clear();
		return true;
	}

	auto GetTexture2DCompilationDiagnostic(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic
	{
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(GTexture2DCompilationMutex);
			if (FTexture2DCompilationState* State = FindStateLocked(Texture.GetObjectPath()))
				RequestId = State->ActiveRequestId != 0
					? State->ActiveRequestId : State->LastRequestId;
		}
		const auto Scheduler = GetTexture2DCompilationScheduler();
		return Scheduler && RequestId != 0
			? Scheduler->GetDiagnostic(RequestId) : FTexture2DCompilationDiagnostic{};
	}

	auto HasPendingTexture2DCompilation(const DTexture2D& Texture) -> bool
	{
		std::lock_guard Lock(GTexture2DCompilationMutex);
		const FTexture2DCompilationState* State = FindStateLocked(Texture.GetObjectPath());
		return State && State->Texture.Get() == &Texture && State->ActiveRequestId != 0;
	}

	namespace
	{
		auto CancelTexture2DCompilation(DTexture2D& Texture) -> bool
		{
			uint64 RequestId = 0;
			{
				std::lock_guard Lock(GTexture2DCompilationMutex);
				FTexture2DCompilationState* State = FindStateLocked(Texture.GetObjectPath());
				if (State && State->Texture.Get() == &Texture)
					RequestId = State->ActiveRequestId;
			}
			const auto Scheduler = GetTexture2DCompilationScheduler();
			return Scheduler && RequestId != 0 && Scheduler->Cancel(RequestId);
		}
	}

	auto WaitForTexture2DCompilation(DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(GTexture2DCompilationMutex);
			FTexture2DCompilationState* State = FindStateLocked(Texture.GetObjectPath());
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		if (RequestId == 0)
		{
			if (const auto AssetForge = TryWaitForTexture2DImportRecovery(
				Texture, TimeoutSeconds)) return *AssetForge;
			return true;
		}
		const auto Scheduler = GetTexture2DCompilationScheduler();
		if (!Scheduler || !Scheduler->WaitForRequest(RequestId, TimeoutSeconds)) return false;
		FAssetCompilingManager::Get().FinishCompilationForObject(Texture);
		bool bFailed = false;
		{
			std::lock_guard Lock(GTexture2DCompilationMutex);
			if (FTexture2DCompilationState* State = FindStateLocked(Texture.GetObjectPath()))
				bFailed = State->bLastRequestFailed;
		}
		return !HasPendingTexture2DCompilation(Texture) && !bFailed
			&& Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
	}
}
