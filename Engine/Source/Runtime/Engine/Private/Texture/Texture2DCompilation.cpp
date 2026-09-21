#include "Texture/Texture2DCompilation.h"

#include "Texture/Texture2DBuild.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/AssetImportData.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/TextureCompilingManager.h"

namespace Durin
{
	auto FormatTexture2DCompilationError(const FTexture2DCompilationError& Error) -> std::string
	{
		if (Error.InputCause) return FormatTexture2DInputError(*Error.InputCause);
		if (Error.BuildCause) return FormatTexture2DBuildError(*Error.BuildCause);
		if (Error.ImportCause) return FormatAssetImportDataError(*Error.ImportCause);
		if (Error.SaveCause) return Error.SaveCause->Message;
		switch (Error.Code)
		{
		case ETexture2DCompilationError::InvalidSource: return "Texture2D compilation submission requires valid normalized source pixels.";
		case ETexture2DCompilationError::MissingSourceIdentity: return "Texture2D compilation submission requires valid normalized source pixels.";
		case ETexture2DCompilationError::ManagerUnavailable: return "The Texture compiling manager is unavailable.";
		case ETexture2DCompilationError::ManagerNotStarted: return "The Texture compiling manager has not started.";
		case ETexture2DCompilationError::InvalidOwner: return "Texture2D compilation submission requires a live object handle.";
		case ETexture2DCompilationError::AdmissionRejected: return "The Texture compiling manager rejected the request.";
		case ETexture2DCompilationError::MissingPackage: return "Texture2D result application requires a package.";
		case ETexture2DCompilationError::InvalidProduct: return "Texture2D result application requires a complete detached product.";
		case ETexture2DCompilationError::SourceMismatch: return "Texture2D build result does not match the source selected for commit.";
		case ETexture2DCompilationError::InvalidSettings: return "Texture2D build settings are invalid.";
		case ETexture2DCompilationError::InputMismatch: return "Texture2D build input identity changed before application.";
		case ETexture2DCompilationError::Superseded: return "Texture2D compilation was superseded by a newer request.";
		case ETexture2DCompilationError::ImportValidation: return "Texture2D import metadata is invalid.";
		case ETexture2DCompilationError::ImportAllocation: return "Could not allocate Texture2D import metadata.";
		case ETexture2DCompilationError::Save: return "Texture2D save failed.";
		case ETexture2DCompilationError::None: return {};
		case ETexture2DCompilationError::BuildFailed: return "Texture build failed.";
		case ETexture2DCompilationError::WorkerFailed: return "Texture build task failed.";
		case ETexture2DCompilationError::Cancelled: return "Texture build was cancelled.";
		case ETexture2DCompilationError::CancelledBeforeAdmission: return "Texture build was cancelled before admission.";
		case ETexture2DCompilationError::CancelledDuringShutdown: return "Texture build was cancelled during shutdown.";
		}
		return {};
	}

	struct FTextureCompilingManager::FCompilationState
	{
struct FAssetState
		{
			TWeakObjectPtr<DTexture> Texture;
			uint64 RequestSerial = 0;
			uint64 ActiveRequestId = 0;
			uint64 LastRequestId = 0;
			bool bLastRequestFailed = false;
			FTexture2DResultApplicationContext ResultApplicationContext;
			FTexture2DBuildInputIdentity InputIdentity;
			FTexture2DCompilationCompletion Completion;
		};

		auto FindLocked(FObjectKey Owner) -> FAssetState*
		{
			const auto It = Assets.find(Owner);
			return It == Assets.end() ? nullptr : &It->second;
		}

		auto FindLocked(FObjectKey Owner) const -> const FAssetState*
		{
			const auto It = Assets.find(Owner);
			return It == Assets.end() ? nullptr : &It->second;
		}

		mutable std::mutex Mutex;
		std::unordered_map<FObjectKey, FAssetState, FObjectKeyHash> Assets;
		std::deque<FObjectKey> CompletedOrder;
		static constexpr size_t MaximumRetainedAssetDiagnostics = 256;
		std::vector<FWeakObjectPtr> SuccessfullyAppliedTextures;

		auto RetainCompletedLocked(FObjectKey Owner) -> void
		{
			CompletedOrder.erase(std::remove(
				CompletedOrder.begin(), CompletedOrder.end(), Owner), CompletedOrder.end());
			CompletedOrder.push_back(Owner);
			while (CompletedOrder.size() > MaximumRetainedAssetDiagnostics)
			{
				const FObjectKey Oldest = CompletedOrder.front();
				CompletedOrder.pop_front();
				if (FAssetState* State = FindLocked(Oldest);
					State && State->ActiveRequestId == 0) Assets.erase(Oldest);
			}
		}
	};

	namespace
	{
		std::mutex GTextureCompilingManagerMutex;
		std::weak_ptr<FTextureCompilingManager> GTextureCompilingManager;
		auto ApplyTexture2DBuildResult(DTexture2D& Texture,
			FXxHash128 SourceIdentity,
			const FTexture2DBuildSettings& Settings,
			FTexture2DBuildProduct Product,
			const FTexture2DResultApplicationContext& Context) -> FTexture2DCompilationOperationResult;

		auto GetTextureCompilingManager() -> std::shared_ptr<FTextureCompilingManager>
		{
			std::lock_guard Lock(GTextureCompilingManagerMutex);
			return GTextureCompilingManager.lock();
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

		auto MatchesRequestedInput(
			const FTexture2DBuildInputIdentity& Expected,
			const FTexture2DBuildInputIdentity& Completed) -> bool
		{
			return Completed.Provider.IsValid()
				&& Expected.SourceIdentity == Completed.SourceIdentity
				&& Expected.Settings == Completed.Settings
				&& Expected.TargetPlatform == Completed.TargetPlatform
				&& Expected.TargetProfile == Completed.TargetProfile;
		}
	}

	auto FTextureCompilingManager::Start() -> FAssetCompilerStartResult
	{
		if (!CompilationState) CompilationState = std::make_shared<FCompilationState>();
		return StartWorkAdmission();
	}

	auto FTextureCompilingManager::StopAdmission() -> void
	{
		StopWorkAdmission();
	}

	auto FTextureCompilingManager::GetNumRemainingAssets() const -> uint64
	{
		if (!CompilationState) return 0;
		std::lock_guard Lock(CompilationState->Mutex);
		return static_cast<uint64>(std::ranges::count_if(
			CompilationState->Assets, [](const auto& Pair) {
				return Pair.second.ActiveRequestId != 0;
			}));
	}

	auto FTextureCompilingManager::ApplyCompletion(
		FTexture2DCompilationWorkResult&& Result) -> void
	{
		CheckGameThread();
		TWeakObjectPtr<DTexture> WeakTexture;
		FTexture2DResultApplicationContext ResultApplicationContext;
		FTexture2DCompilationCompletion Completion;
		bool bInputMismatch = false;
		FTexture2DBuildInputIdentity ExpectedInput;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Result.Owner);
			if (!State || State->RequestSerial != Result.RequestSerial
				|| State->ActiveRequestId != Result.RequestId) return;
			bInputMismatch = !Result.PlatformCache && Result.Phase == ETexture2DCompilationPhase::UploadPending
				&& !MatchesRequestedInput(State->InputIdentity, Result.InputIdentity);
			ExpectedInput = State->InputIdentity;
			WeakTexture = State->Texture;
			ResultApplicationContext = std::move(State->ResultApplicationContext);
			State->ResultApplicationContext = {};
			Completion = std::move(State->Completion);
			State->ActiveRequestId = 0;
			State->LastRequestId = Result.RequestId;
			State->bLastRequestFailed =
				Result.Phase == ETexture2DCompilationPhase::Failed || bInputMismatch;
			CompilationState->RetainCompletedLocked(Result.Owner);
		}
		DTexture* Texture = WeakTexture.Get();
		if (!Texture || FObjectKey(Texture) != Result.Owner)
		{
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Error = {.Code = ETexture2DCompilationError::InvalidOwner, .ObjectPath = Result.AssetIdentity}, .PersistenceDiagnostic = Result.PersistenceDiagnostic});
			return;
		}
		if (bInputMismatch)
		{
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Error = {.Code = ETexture2DCompilationError::InputMismatch, .ObjectPath = Result.AssetIdentity,
					.ExpectedInput = std::make_shared<FTexture2DBuildInputIdentity>(ExpectedInput),
					.ActualInput = std::make_shared<FTexture2DBuildInputIdentity>(Result.InputIdentity)},
				.PersistenceDiagnostic = Result.PersistenceDiagnostic});
			return;
		}
		if (Result.PlatformCache)
		{
			const bool bSucceeded = Result.Phase == ETexture2DCompilationPhase::UploadPending
				&& Texture->GetSource().GetIdentity() == ExpectedInput.SourceIdentity;
			if (bSucceeded) Result.PlatformCache->Apply(*Texture);
			else if (!Result.PlatformCache->Error.empty())
				DURIN_ERROR("Texture cache failed for {}: {}", Result.AssetIdentity, Result.PlatformCache->Error);
			std::lock_guard Lock(CompilationState->Mutex);
			if (auto* State = CompilationState->FindLocked(Result.Owner))
				State->bLastRequestFailed = !bSucceeded;
			if (bSucceeded) CompilationState->SuccessfullyAppliedTextures.emplace_back(Texture);
			return;
		}
		if (Result.Phase != ETexture2DCompilationPhase::UploadPending
			|| !Result.PlatformData)
		{
			if (Completion) Completion({
				.Status = Result.Phase == ETexture2DCompilationPhase::Cancelled
					? ETexture2DCompilationStatus::Canceled : ETexture2DCompilationStatus::Failed,
				.Error = Result.Error.HasError() ? Result.Error
					: FTexture2DCompilationError{.Code = Result.Phase == ETexture2DCompilationPhase::Cancelled
						? ETexture2DCompilationError::Cancelled : ETexture2DCompilationError::InvalidProduct,
						.ObjectPath = Result.AssetIdentity}, .PersistenceDiagnostic = Result.PersistenceDiagnostic});
			return;
		}

		const auto PersistenceDiagnostic = Result.PersistenceDiagnostic;
		const FTexture2DBuildSettings& Settings = Result.InputIdentity.Settings;
		FTexture2DBuildProduct Product{
			.PlatformData = std::move(*Result.PlatformData),
			.DerivedDataKey = std::move(Result.DerivedDataKey),
			.PersistenceDiagnostic = std::move(Result.PersistenceDiagnostic),
			.Origin = Result.Origin};
		if (const auto Applied = ApplyTexture2DBuildResult(*Cast<DTexture2D>(Texture), Result.InputIdentity.SourceIdentity, Settings,
			std::move(Product), ResultApplicationContext); !Applied)
		{
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Result.Owner))
					State->bLastRequestFailed = true;
			}
			DURIN_ERROR("Texture2D compilation result application failed for {}: {}",
				Result.AssetIdentity, FormatTexture2DCompilationError(Applied.Error));
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Error = Applied.Error, .PersistenceDiagnostic = PersistenceDiagnostic});
			return;
		}
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Result.Owner))
				State->bLastRequestFailed = false;
		}
		{
			std::lock_guard Lock(CompilationState->Mutex);
			CompilationState->SuccessfullyAppliedTextures.emplace_back(Texture);
		}
		if (Completion) Completion({.Status = ETexture2DCompilationStatus::Succeeded,
			.PersistenceDiagnostic = PersistenceDiagnostic});
	}

	auto FTextureCompilingManager::PumpCompletions(uint32 MaximumCount,
		std::optional<std::chrono::steady_clock::time_point> Deadline, uint64 OnlyRequest)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Result;
		Result.ProcessedCompletionCount = PumpWorkCompletions(MaximumCount, Deadline, OnlyRequest);
		std::lock_guard Lock(CompilationState->Mutex);
		Result.SuccessfullyCompiledAssets =
			std::move(CompilationState->SuccessfullyAppliedTextures);
		CompilationState->SuccessfullyAppliedTextures.clear();
		return Result;
	}

	auto FTextureCompilingManager::ProcessAsyncTasks(
		const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult
	{
		return PumpCompletions(Params.MaximumCompletions, Params.Deadline);
	}

	auto FTextureCompilingManager::FinishCompilationForObjects(
		std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		for (DObject* Object : Objects)
		{
			auto* Texture = Cast<DTexture>(Object);
			if (!IsValid(Texture)) continue;
			uint64 RequestId = 0;
			const FObjectKey Owner = FObjectKey(Texture);
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Owner);
					State && State->Texture.Get() == Texture)
					RequestId = State->ActiveRequestId;
			}
			if (RequestId == 0) continue;
			WaitForWork(RequestId, 300.0);
			AppendProcessResult(
				Aggregate, PumpCompletions(1, {}, RequestId));
		}
		return Aggregate;
	}

	auto FTextureCompilingManager::MarkCompilationAsCanceled(
		std::span<DObject* const> Objects) -> void
	{
		for (DObject* Object : Objects)
			if (auto* Texture = Cast<DTexture>(Object); IsValid(Texture)) Cancel(*Texture);
	}

	auto FTextureCompilingManager::FinishAllCompilation()
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		while (GetNumRemainingAssets() != 0
			|| GetQueuedWorkCount() != 0 || GetRunningWorkCount() != 0)
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

	auto FTextureCompilingManager::Shutdown() -> void
	{
		ShutdownWorkQueue();
	}

	auto FTextureCompilingManager::Submit(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		FTexture2DCompilationCompletion Completion) -> FTexture2DCompilationOperationResult
	{
		CheckGameThread();
		if (Request.Build.DeferredSource && (!Request.Build.SourceMips.empty()
			|| !Request.Build.DeferredSource->IsValid() || Request.Build.DeferredSource->GetOwner()
			|| Request.Build.DeferredSource->GetKind() != ETextureSourceKind::Texture2D
			|| Request.Build.DeferredSource->GetIdentity() != Request.Build.SourceIdentity))
			return {.Error = {.Code = ETexture2DCompilationError::InvalidSource}};
		if (const auto Validation = ValidateTexture2DSourceMips(Request.Build.SourceMips); !Request.Build.DeferredSource && !Validation)
			return {.Error = {.Code = ETexture2DCompilationError::InvalidSource,
				.InputCause = Validation.Error, .ObjectPath = Texture.GetObjectPath()}};
		if (Request.Build.SourceIdentity.IsZero())
			return {.Error = {.Code = ETexture2DCompilationError::MissingSourceIdentity,
				.ObjectPath = Texture.GetObjectPath()}};
		if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		{
			return {.Error = {.Code = ETexture2DCompilationError::ManagerUnavailable,
				.ObjectPath = Texture.GetObjectPath()}};
		}
		if (!CompilationState)
		{
			return {.Error = {.Code = ETexture2DCompilationError::ManagerNotStarted,
				.ObjectPath = Texture.GetObjectPath()}};
		}

		const std::string Identity = Texture.GetObjectPath();
		const FObjectKey Owner = FObjectKey(&Texture);
		if (IsObjectKeyNull(Owner))
		{
			return {.Error = {.Code = ETexture2DCompilationError::InvalidOwner,
				.ObjectPath = Texture.GetObjectPath()}};
		}
		const FTexture2DBuildSettings Settings = Request.Build.Settings;
		const bool bSourceDecoderInvoked =
			Request.ResultApplication.bSourceDecoderInvoked;
		const bool bSRGB = ResolveTexture2DSRGB(Settings);
		const FXxHash128 SourceIdentity =
			Request.Build.SourceIdentity;
		uint64 RequestSerial = 0;
		uint64 PreviousRequestId = 0;
		FTexture2DCompilationCompletion SupersededCompletion;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState& State = CompilationState->Assets[Owner];
			RequestSerial = ++State.RequestSerial;
			if (RequestSerial == 0) RequestSerial = ++State.RequestSerial;
			PreviousRequestId = State.ActiveRequestId;
			if (PreviousRequestId != 0) SupersededCompletion = std::move(State.Completion);
			State.Texture = TWeakObjectPtr<DTexture>(&Texture);
			State.ActiveRequestId = 0;
			State.bLastRequestFailed = false;
			State.ResultApplicationContext = std::move(Request.ResultApplication);
			State.InputIdentity = {
					.SourceIdentity = SourceIdentity,
					.Settings = {
						.Usage = Settings.Usage,
						.CompressionQuality = Settings.CompressionQuality,
						.AlphaMipMode = Settings.AlphaMipMode,
						.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
						.MaxResolution = Settings.MaxResolution,
						.bSRGB = bSRGB},
					.TargetPlatform = Request.Build.TargetPlatform,
					.TargetProfile = Request.Build.TargetProfile};
			State.Completion = std::move(Completion);
		}
		if (PreviousRequestId != 0) CancelWork(PreviousRequestId);

		const uint32 Width = Request.Build.DeferredSource ? Request.Build.DeferredSource->GetWidth() : Request.Build.SourceMips.front().GetInfo().Width;
		const uint32 Height = Request.Build.DeferredSource ? Request.Build.DeferredSource->GetHeight() : Request.Build.SourceMips.front().GetInfo().Height;
		const uint64 RequestId = SubmitWork({
			.AssetIdentity = Identity,
			.Build = std::move(Request.Build),
			.Owner = Owner,
			.RequestSerial = RequestSerial,
			.EstimatedWidth = Width,
			.EstimatedHeight = Height,
			.Priority = Request.Priority,
			.bSourceDecoderInvoked = bSourceDecoderInvoked},
			[this](FTexture2DCompilationWorkResult&& Result) {
				ApplyCompletion(std::move(Result));
			});
		if (RequestId == 0)
		{
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Owner);
					State && State->RequestSerial == RequestSerial)
				{
					State->ActiveRequestId = 0;
					State->ResultApplicationContext = {};
					State->Completion = {};
					State->bLastRequestFailed = true;
					CompilationState->RetainCompletedLocked(Owner);
				}
			}
			if (SupersededCompletion) SupersededCompletion({
				.Status = ETexture2DCompilationStatus::Superseded,
				.Error = {.Code = ETexture2DCompilationError::Superseded, .ObjectPath = Texture.GetObjectPath()}});
			return {.Error = {.Code = ETexture2DCompilationError::AdmissionRejected,
				.ObjectPath = Texture.GetObjectPath()}};
		}
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State = CompilationState->FindLocked(Owner);
			if (State && State->RequestSerial == RequestSerial)
			{
				State->ActiveRequestId = RequestId;
				State->LastRequestId = RequestId;
				State->bLastRequestFailed = false;
			}
		}
		if (SupersededCompletion) SupersededCompletion({
			.Status = ETexture2DCompilationStatus::Superseded,
			.Error = {.Code = ETexture2DCompilationError::Superseded, .ObjectPath = Texture.GetObjectPath()}});
		return {};
	}

	auto FTextureCompilingManager::GetDiagnostic(const DTexture2D& Texture) const
		-> FTexture2DCompilationDiagnostic
	{
		if (!CompilationState) return {};
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(
					const_cast<DTexture2D*>(&Texture))))
				RequestId = State->ActiveRequestId != 0
					? State->ActiveRequestId : State->LastRequestId;
		}
		return RequestId != 0 ? GetWorkDiagnostic(RequestId) : FTexture2DCompilationDiagnostic{};
	}

	auto FTextureCompilingManager::GetManagerDiagnostics() const
		-> FTexture2DCompilationManagerDiagnostics
	{
		FTexture2DCompilationManagerDiagnostics Result = GetWorkManagerDiagnostics();
		if (!CompilationState) return Result;
		std::lock_guard Lock(CompilationState->Mutex);
		Result.ActiveRecordCount = std::ranges::count_if(
			CompilationState->Assets, [](const auto& Pair) {
				return Pair.second.ActiveRequestId != 0;
			});
		return Result;
	}

	auto FTextureCompilingManager::SubmitPlatformCache(DTexture& Texture,
		std::shared_ptr<const FTexturePlatformCacheInput> Input) -> bool
	{
		CheckGameThread();
		if (!CompilationState || !FAssetCompilingManager::Get().IsAcceptingRequests()
			|| !Input || !Input->Source.IsValid() || Input->Source.GetOwner()) return false;
		const FObjectKey Owner(&Texture);
		if (IsObjectKeyNull(Owner)) return false;
		uint64 Serial;
		uint64 PreviousId;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets[Owner];
			PreviousId = State.ActiveRequestId;
			State.Texture = TWeakObjectPtr<DTexture>(&Texture);
			Serial = ++State.RequestSerial;
			State.InputIdentity = {.SourceIdentity = Input->Source.GetIdentity()};
			State.bLastRequestFailed = false;
		}
		if (PreviousId) CancelWork(PreviousId);
		const uint64 Id = SubmitWork({.AssetIdentity = Texture.GetObjectPath(),
			.Build = {.SourceIdentity = Input->Source.GetIdentity()}, .Owner = Owner,
			.RequestSerial = Serial, .PlatformCache = std::move(Input)},
			[this](FTexture2DCompilationWorkResult&& Result) { ApplyCompletion(std::move(Result)); });
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets[Owner];
			State.ActiveRequestId = Id;
			State.bLastRequestFailed = Id == 0;
			if (!Id) CompilationState->RetainCompletedLocked(Owner);
		}
		return Id != 0;
	}

	auto SubmitTexturePlatformCache(DTexture& Texture,
		std::shared_ptr<const FTexturePlatformCacheInput> Input) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager && Manager->SubmitPlatformCache(Texture, std::move(Input));
	}

	auto HasPendingTextureCompilation(const DTexture& Texture) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager && Manager->HasPending(Texture);
	}

	auto FinishTextureCompilation(DTexture& Texture) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->Wait(Texture, 300.0) : Texture.HasPlatformData();
	}

	auto FTextureCompilingManager::HasPending(const DTexture& Texture) const -> bool
	{
		if (!CompilationState) return false;
		std::lock_guard Lock(CompilationState->Mutex);
		const FCompilationState::FAssetState* State =
			CompilationState->FindLocked(FObjectKey(
				const_cast<DTexture*>(&Texture)));
		return State && State->Texture.Get() == &Texture && State->ActiveRequestId != 0;
	}

	auto FTextureCompilingManager::Cancel(DTexture& Texture) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(&Texture));
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		return RequestId != 0 && CancelWork(RequestId);
	}

	auto FTextureCompilingManager::Wait(
		DTexture& Texture, double TimeoutSeconds) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		bool bLastRequestFailed = false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(&Texture));
			if (State && State->Texture.Get() == &Texture)
			{
				RequestId = State->ActiveRequestId;
				bLastRequestFailed = State->bLastRequestFailed;
			}
		}
		if (RequestId == 0)
			return !bLastRequestFailed && Texture.HasPlatformData();
		if (!WaitForWork(RequestId, TimeoutSeconds)) return false;
		FAssetCompilingManager::Get().FinishCompilationForObject(Texture);
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(&Texture)))
				bLastRequestFailed = State->bLastRequestFailed;
		}
		return !HasPending(Texture) && !bLastRequestFailed
			&& Texture.HasPlatformData();
	}

	namespace AssetPrivate
	{
		auto CreateTextureCompilingManager() -> std::shared_ptr<IAssetCompilingManager>
		{
			CheckGameThread();
			std::lock_guard Lock(GTextureCompilingManagerMutex);
			if (const auto Existing = GTextureCompilingManager.lock()) return Existing;
			auto Manager = std::make_shared<FTextureCompilingManager>();
			GTextureCompilingManager = Manager;
			return Manager;
		}

		auto SetTexture2DCompilationPhaseHookForTests(
			std::function<void(uint64, ETexture2DCompilationPhase)> Hook) -> void
		{
			if (const auto Manager = GetTextureCompilingManager())
				Manager->SetPhaseHookForTests(std::move(Hook));
		}
	}

	auto BuildTexture2DSynchronously(
		DTexture2D& Texture,
		FTexture2DBuildRequest Request,
		const FTexture2DResultApplicationContext& Context) -> FTexture2DCompilationOperationResult
	{
		CheckGameThread();
		FTexture2DBuildProduct Product;
		FTexture2DBuildInputIdentity Identity;
		const FTexture2DBuildResult BuildResult = InvokeTexture2DBuildProvider(
			Request, Product, Identity);
		if (!BuildResult)
		{
			return {.Error = {.Code = ETexture2DCompilationError::BuildFailed,
				.BuildCause = BuildResult.Error, .ObjectPath = Texture.GetObjectPath()}};
		}
		return ApplyTexture2DBuildResult(Texture, Request.SourceIdentity, Request.Settings,
			std::move(Product), Context);
	}

	namespace
	{
	auto ApplyTexture2DBuildResult(
		DTexture2D& Texture,
		FXxHash128 SourceIdentity,
		const FTexture2DBuildSettings& Settings,
		FTexture2DBuildProduct Product,
		const FTexture2DResultApplicationContext& Context) -> FTexture2DCompilationOperationResult
	{
		CheckGameThread();
		if (!Texture.GetPackage())
		{
			return {.Error = {.Code = ETexture2DCompilationError::MissingPackage,
				.ObjectPath = Texture.GetObjectPath()}};
		}
		if (!Product.PlatformData.IsValid()
			|| !Product.DerivedDataKey.IsValid())
		{
			return {.Error = {.Code = ETexture2DCompilationError::InvalidProduct,
				.ObjectPath = Texture.GetObjectPath()}};
		}
		const FTextureSource& Source = Context.SourceReplacement
			? *Context.SourceReplacement : Texture.GetSource();
		if (!Source.IsValid() || Source.GetIdentity() != SourceIdentity)
		{
			return {.Error = {.Code = ETexture2DCompilationError::SourceMismatch,
				.ObjectPath = Texture.GetObjectPath(), .ExpectedSourceIdentity = SourceIdentity,
				.ActualSourceIdentity = Source.GetIdentity()}};
		}
		if (const auto Validation = ValidateTexture2DBuildSettings(Settings); !Validation)
			return {.Error = {.Code = ETexture2DCompilationError::InvalidSettings,
				.InputCause = Validation.Error, .ObjectPath = Texture.GetObjectPath()}};
		auto PlatformData = std::make_unique<FTexturePlatformData>(
			std::move(Product.PlatformData));
		if (Context.SourceReplacement) Texture.SetSource(*Context.SourceReplacement);
		Texture.SetBuildSettings(Settings.Usage, ResolveTexture2DSRGB(Settings),
				Settings.MaxResolution, Settings.CompressionQuality,
				Settings.AlphaMipMode, Settings.AlphaCoverageThreshold);
		Texture.SetPlatformData(std::move(PlatformData));
		Texture.UpdateResource();
		if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
		if (Context.bReportLoadMutation && Context.SourceReplacement)
		{
			ReportAssetLoadMutation(&Texture,
				"Engine.Texture2D.SourceIdentity",
				"Texture source identity metadata was reconciled by an uncooked post-load build.");
		}
		return {};
	}
	}

	auto SubmitTexture2DCompilation(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		FTexture2DCompilationCompletion Completion) -> FTexture2DCompilationOperationResult
	{
		const auto Manager = GetTextureCompilingManager();
		if (!Manager)
		{
			return {.Error = {.Code = ETexture2DCompilationError::ManagerUnavailable,
				.ObjectPath = Texture.GetObjectPath()}};
		}
		return Manager->Submit(
			Texture, std::move(Request), std::move(Completion));
	}

	auto GetTexture2DCompilationDiagnostic(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->GetDiagnostic(Texture) : FTexture2DCompilationDiagnostic{};
	}

	auto GetTexture2DCompilationManagerDiagnostics()
		-> FTexture2DCompilationManagerDiagnostics
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->GetManagerDiagnostics()
			: FTexture2DCompilationManagerDiagnostics{};
	}

	auto HasPendingTexture2DCompilation(const DTexture2D& Texture) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager && Manager->HasPending(Texture);
	}

	auto WaitForTexture2DCompilation(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		if (!Manager) return Texture.HasPlatformData();
		return Manager->Wait(Texture, TimeoutSeconds);
	}
}
