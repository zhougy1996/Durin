#include "Texture/Texture2DCompilation.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/Texture2DCompilationDomain.h"
#include "Texture/TextureBuilder.h"
#include "Texture/Texture2DPostLoad.h"

namespace Durin::Asset
{
	struct FTexture2DCompilationDomain::FCompilationState
	{
		struct FAssetState
		{
			TWeakObjectPtr<DTexture2D> Texture;
			uint64 Generation = 0;
			uint64 ActiveRequestId = 0;
			uint64 LastRequestId = 0;
			bool bLastRequestFailed = false;
			FTexture2DPublicationContext PublicationContext;
			FTexture2DCompilationCompletion Completion;
		};

		auto FindLocked(std::string_view Identity) -> FAssetState*
		{
			const auto It = Assets.find(std::string(Identity));
			return It == Assets.end() ? nullptr : &It->second;
		}

		auto FindLocked(std::string_view Identity) const -> const FAssetState*
		{
			const auto It = Assets.find(std::string(Identity));
			return It == Assets.end() ? nullptr : &It->second;
		}

		mutable std::mutex Mutex;
		std::unordered_map<std::string, FAssetState> Assets;
		uint64 NextGeneration = 1;
		std::vector<FWeakObjectPtr> SuccessfullyPublishedTextures;
	};

	namespace
	{
		std::mutex GTexture2DCompilationDomainMutex;
		std::shared_ptr<FTexture2DCompilationDomain> GTexture2DCompilationDomain;
		FAssetCompilationDomainRegistration GTexture2DCompilationRegistration;

		auto GetTexture2DCompilationDomain() -> std::shared_ptr<FTexture2DCompilationDomain>
		{
			std::lock_guard Lock(GTexture2DCompilationDomainMutex);
			return GTexture2DCompilationDomain;
		}

		auto AppendProcessResult(
			FAssetCompileProcessResult& Aggregate,
			FAssetCompileProcessResult Item) -> void
		{
			Aggregate.ProcessedCompletionCount += Item.ProcessedCompletionCount;
			Aggregate.SuccessfullyCompiledAssets.insert(
				Aggregate.SuccessfullyCompiledAssets.end(),
				Item.SuccessfullyCompiledAssets.begin(),
				Item.SuccessfullyCompiledAssets.end());
		}
	}

	auto FTexture2DCompilationDomain::GetDomainName() const -> FName
	{
		return FName("Durin.TextureCompilation");
	}

	auto FTexture2DCompilationDomain::Start(std::string* OutError) -> bool
	{
		if (!CompilationState) CompilationState = std::make_shared<FCompilationState>();
		if (StartWorkAdmission())
		{
			if (OutError) OutError->clear();
			return true;
		}
		if (OutError) *OutError = "Texture2D compilation work admission could not start.";
		return false;
	}

	auto FTexture2DCompilationDomain::StopAdmission() -> void
	{
		StopWorkAdmission();
	}

	auto FTexture2DCompilationDomain::GetNumRemainingAssets() const -> uint64
	{
		if (!CompilationState) return 0;
		std::lock_guard Lock(CompilationState->Mutex);
		return static_cast<uint64>(std::ranges::count_if(
			CompilationState->Assets, [](const auto& Item) {
				return Item.second.ActiveRequestId != 0 && Item.second.Texture.IsValid();
			}));
	}

	auto FTexture2DCompilationDomain::ApplyCompletion(
		FTexture2DCompilationWorkResult&& Result) -> void
	{
		CheckGameThread();
		TWeakObjectPtr<DTexture2D> WeakTexture;
		FTexture2DPublicationContext PublicationContext;
		FTexture2DCompilationCompletion Completion;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Result.AssetIdentity);
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
					? "The Texture2D compilation did not produce a publishable product."
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
		if (!PublishTexture2DProduct(*Texture, std::move(Product), PublicationContext, Error))
		{
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Result.AssetIdentity);
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
			std::lock_guard Lock(CompilationState->Mutex);
			CompilationState->SuccessfullyPublishedTextures.emplace_back(Texture);
		}
		if (Completion) Completion({.Status = ETexture2DCompilationStatus::Succeeded});
	}

	auto FTexture2DCompilationDomain::PumpCompletions(uint32 MaximumCount)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Result;
		Result.ProcessedCompletionCount = PumpWorkCompletions(MaximumCount);
		std::lock_guard Lock(CompilationState->Mutex);
		Result.SuccessfullyCompiledAssets =
			std::move(CompilationState->SuccessfullyPublishedTextures);
		CompilationState->SuccessfullyPublishedTextures.clear();
		return Result;
	}

	auto FTexture2DCompilationDomain::ProcessAsyncTasks(
		const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult
	{
		return PumpCompletions(Params.MaximumCompletions);
	}

	auto FTexture2DCompilationDomain::FinishCompilationForObjects(
		std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		for (DObject* Object : Objects)
		{
			auto* Texture = Cast<DTexture2D>(Object);
			if (!IsValid(Texture)) continue;
			uint64 RequestId = 0;
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Texture->GetObjectPath());
					State && State->Texture.Get() == Texture)
					RequestId = State->ActiveRequestId;
			}
			if (RequestId == 0) continue;
			WaitForWork(RequestId, 300.0);
			AppendProcessResult(
				Aggregate, PumpCompletions(std::numeric_limits<uint32>::max()));
		}
		return Aggregate;
	}

	auto FTexture2DCompilationDomain::MarkCompilationAsCanceled(
		std::span<DObject* const> Objects) -> void
	{
		for (DObject* Object : Objects)
			if (auto* Texture = Cast<DTexture2D>(Object); IsValid(Texture)) Cancel(*Texture);
	}

	auto FTexture2DCompilationDomain::FinishAllCompilation()
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		while (GetQueuedWorkCount() != 0 || GetRunningWorkCount() != 0)
		{
			FAssetCompileProcessResult Item =
				PumpCompletions(std::numeric_limits<uint32>::max());
			const uint32 ProcessedCount = Item.ProcessedCompletionCount;
			AppendProcessResult(Aggregate, std::move(Item));
			if (ProcessedCount == 0) std::this_thread::yield();
		}
		AppendProcessResult(
			Aggregate, PumpCompletions(std::numeric_limits<uint32>::max()));
		return Aggregate;
	}

	auto FTexture2DCompilationDomain::Shutdown() -> void
	{
		ShutdownWorkQueue();
	}

	auto FTexture2DCompilationDomain::Submit(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		CheckGameThread();
		if (!Request.Build.SourceData.IsValid()
			|| Request.Publication.SourceFilename.empty()
			|| Request.Publication.DecoderId.empty())
		{
			OutError = "Texture2D compilation submission requires normalized source and provenance.";
			return false;
		}
		if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		{
			OutError = "The Texture2D compilation domain is unavailable.";
			return false;
		}
		if (!CompilationState)
		{
			OutError = "The Texture2D compilation domain has not started.";
			return false;
		}

		const std::string Identity = Texture.GetObjectPath();
		uint64 Generation = 0;
		uint64 PreviousRequestId = 0;
		FTexture2DCompilationCompletion SupersededCompletion;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState& State = CompilationState->Assets[Identity];
			PreviousRequestId = State.ActiveRequestId;
			if (PreviousRequestId != 0) SupersededCompletion = std::move(State.Completion);
			Generation = CompilationState->NextGeneration++;
			State = {
				.Texture = TWeakObjectPtr<DTexture2D>(&Texture),
				.Generation = Generation,
				.PublicationContext = std::move(Request.Publication),
				.Completion = std::move(Completion)};
		}
		if (PreviousRequestId != 0) CancelWork(PreviousRequestId);

		const FTexture2DBuildSettings Settings = Request.Build.Settings;
		const bool bSRGB = Settings.bSRGB.value_or(
			TextureBuilder::GetDefaultSRGB(Settings.Usage));
		const uint32 Width = Request.Build.SourceData.Width;
		const uint32 Height = Request.Build.SourceData.Height;
		const uint64 RequestId = SubmitWork({
			.AssetIdentity = Identity,
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
			.bPersistDerivedData = Request.Build.bPersistDerivedData},
			[this](FTexture2DCompilationWorkResult&& Result) {
				ApplyCompletion(std::move(Result));
			});
		if (RequestId == 0)
		{
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Identity);
					State && State->Generation == Generation)
					CompilationState->Assets.erase(Identity);
			}
			if (SupersededCompletion) SupersededCompletion({
				.Status = ETexture2DCompilationStatus::Superseded,
				.Diagnostic = "The Texture2D compilation was superseded by a newer request."});
			OutError = "The Texture2D compilation domain rejected the request.";
			return false;
		}
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State = CompilationState->FindLocked(Identity);
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

	auto FTexture2DCompilationDomain::GetDiagnostic(const DTexture2D& Texture) const
		-> FTexture2DCompilationDiagnostic
	{
		if (!CompilationState) return {};
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Texture.GetObjectPath()))
				RequestId = State->ActiveRequestId != 0
					? State->ActiveRequestId : State->LastRequestId;
		}
		return RequestId != 0 ? GetWorkDiagnostic(RequestId) : FTexture2DCompilationDiagnostic{};
	}

	auto FTexture2DCompilationDomain::HasPending(const DTexture2D& Texture) const -> bool
	{
		if (!CompilationState) return false;
		std::lock_guard Lock(CompilationState->Mutex);
		const FCompilationState::FAssetState* State =
			CompilationState->FindLocked(Texture.GetObjectPath());
		return State && State->Texture.Get() == &Texture && State->ActiveRequestId != 0;
	}

	auto FTexture2DCompilationDomain::Cancel(DTexture2D& Texture) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Texture.GetObjectPath());
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		return RequestId != 0 && CancelWork(RequestId);
	}

	auto FTexture2DCompilationDomain::Wait(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Texture.GetObjectPath());
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		if (RequestId == 0)
			return Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
		if (!WaitForWork(RequestId, TimeoutSeconds)) return false;
		FAssetCompilingManager::Get().FinishCompilationForObject(Texture);
		bool bFailed = false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Texture.GetObjectPath()))
				bFailed = State->bLastRequestFailed;
		}
		return !HasPending(Texture) && !bFailed
			&& Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
	}

	namespace Private
	{
		auto InitializeTexture2DCompilationDomain(
			FModuleOwnedCallbackGate OwnerGate,
			const FTexture2DCompilationDomainConfig& Config) -> bool
		{
			CheckGameThread();
			if (GTexture2DCompilationRegistration.IsValid()) return true;
			auto Domain = std::make_shared<FTexture2DCompilationDomain>(Config);
			{
				std::lock_guard Lock(GTexture2DCompilationDomainMutex);
				GTexture2DCompilationDomain = Domain;
			}
			std::string Error;
			GTexture2DCompilationRegistration = FAssetCompilingManager::Get().RegisterDomain(
				Domain, std::move(OwnerGate), &Error);
			if (!GTexture2DCompilationRegistration.IsValid())
			{
				DURIN_ERROR("Texture compilation domain registration failed: {}", Error);
				Domain->Shutdown();
				std::lock_guard Lock(GTexture2DCompilationDomainMutex);
				GTexture2DCompilationDomain.reset();
			}
			return GTexture2DCompilationRegistration.IsValid();
		}

		auto ShutdownTexture2DCompilationDomain() -> void
		{
			CheckGameThread();
			GTexture2DCompilationRegistration.Reset();
			std::lock_guard Lock(GTexture2DCompilationDomainMutex);
			GTexture2DCompilationDomain.reset();
		}

		auto SetTexture2DCompilationPhaseHookForTests(
			std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void
		{
			if (const auto Domain = GetTexture2DCompilationDomain())
				Domain->SetPhaseHookForTests(std::move(Hook));
		}
	}

	auto SubmitTexture2DCompilation(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		const auto Domain = GetTexture2DCompilationDomain();
		if (!Domain)
		{
			OutError = "The Texture2D compilation domain is unavailable.";
			return false;
		}
		return Domain->Submit(
			Texture, std::move(Request), OutError, std::move(Completion));
	}

	auto GetTexture2DCompilationDiagnostic(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic
	{
		const auto Domain = GetTexture2DCompilationDomain();
		return Domain ? Domain->GetDiagnostic(Texture) : FTexture2DCompilationDiagnostic{};
	}

	auto HasPendingTexture2DCompilation(const DTexture2D& Texture) -> bool
	{
		const auto Domain = GetTexture2DCompilationDomain();
		return Domain && Domain->HasPending(Texture);
	}

	auto WaitForTexture2DCompilation(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		const auto Domain = GetTexture2DCompilationDomain();
		if (!Domain)
			return Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
		return Domain->Wait(Texture, TimeoutSeconds);
	}
}
