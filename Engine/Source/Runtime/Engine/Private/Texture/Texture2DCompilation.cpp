#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/Texture2DCompilation.h"

#include "Texture/Texture2DBuild.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "Asset/AssetImportData.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/TextureCompilingManager.h"
#include "Texture2DBuildDiagnostics.h"

namespace Durin
{
	auto FormatTexture2DCompilationError(const FTexture2DCompilationError& Error) -> std::string
	{
		if (Error.InputCause) return FormatTexture2DInputError(*Error.InputCause);
		if (!Error.InputReason.empty()) return Error.InputReason;
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
		case ETexture2DCompilationError::ImportValidation: return "Texture2D import metadata is invalid.";
		case ETexture2DCompilationError::ImportAllocation: return "Could not allocate Texture2D import metadata.";
		case ETexture2DCompilationError::Save: return "Texture2D save failed.";
		case ETexture2DCompilationError::None: return {};
		case ETexture2DCompilationError::BuildFailed: return "Texture build failed.";
		case ETexture2DCompilationError::WorkerFailed: return "Texture build task failed.";
		case ETexture2DCompilationError::Cancelled: return "Texture build was cancelled.";
		case ETexture2DCompilationError::CancelledBeforeAdmission: return "Texture build was cancelled before admission.";
		case ETexture2DCompilationError::CancelledDuringShutdown: return "Texture build was cancelled during shutdown.";
		case ETexture2DCompilationError::CancelledForMutation: return "Texture build was cancelled before modifying the texture.";
		}
		return {};
	}

	struct FTextureCompilingManager::FCompilationState
	{
		struct FTerminalResult
		{
			ETexture2DCompilationPhase Phase = ETexture2DCompilationPhase::None;
			FTexture2DCompilationError Error;
			std::string FailureMessage;

			auto AllowsPlatformData() const -> bool
			{
				return Phase == ETexture2DCompilationPhase::None || Phase == ETexture2DCompilationPhase::Ready;
			}
		};

		struct FAssetState
		{
			TWeakObjectPtr<DTexture> Texture;
			uint64 ActiveRequestId = 0;
			uint64 LastRequestId = 0;
			FTerminalResult Terminal;
			FTexture2DResultApplicationContext ResultApplicationContext;
			FXxHash128 SourceIdentity;
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
		std::unordered_set<FObjectKey, FObjectKeyHash> MutatingOwners;
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
			FTexturePlatformData Product,
			const FTexture2DResultApplicationContext& Context) -> std::expected<void, FTexture2DCompilationError>;

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
		FTexture2DResultApplicationContext ApplicationContext;
		FTexture2DCompilationCompletion Completion;
		FXxHash128 ExpectedSource;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto* State = CompilationState->FindLocked(Result.Owner);
			if (!State || State->ActiveRequestId != Result.RequestId) return;
			ExpectedSource = State->SourceIdentity;
			WeakTexture = State->Texture;
			ApplicationContext = std::move(State->ResultApplicationContext);
			State->ResultApplicationContext = {};
			Completion = std::move(State->Completion);
			State->ActiveRequestId = 0;
			State->LastRequestId = Result.RequestId;
			CompilationState->RetainCompletedLocked(Result.Owner);
		}

		DTexture* Texture = WeakTexture.Get();
		std::string FailureMessage;
		// Work completion supplies a CPU product. Object completion includes validation and Apply.
		const auto Finalize = [&]() -> FTexture2DCompilationResult {
			if (!Texture || FObjectKey(Texture) != Result.Owner)
				return {.Error = {.Code = ETexture2DCompilationError::InvalidOwner,
					.ObjectPath = Result.AssetIdentity}};
			if (Result.Status != DerivedData::EStatus::Ok)
			{
				const bool bCanceled = Result.Status == DerivedData::EStatus::Canceled;
				if (!bCanceled && !Result.Diagnostic.empty())
				{
					FailureMessage = Result.Diagnostic;
					DURIN_ERROR("Texture cache failed for {}: {}", Result.AssetIdentity, FailureMessage);
				}
				return {.Status = bCanceled ? ETexture2DCompilationStatus::Canceled : ETexture2DCompilationStatus::Failed,
					.Error = Result.Error};
			}
			require(static_cast<bool>(Result.PlatformData) != static_cast<bool>(Result.PlatformCache));
			if (Result.PlatformCache)
			{
				if (Texture->GetSource().GetIdentity() != ExpectedSource)
					return {.Error = {.Code = ETexture2DCompilationError::SourceMismatch,
						.ObjectPath = Result.AssetIdentity}};
				Result.PlatformCache->Apply(*Texture);
			}
			else
			{
				auto Applied = ApplyTexture2DBuildResult(*Cast<DTexture2D>(Texture),
					Result.InputIdentity.SourceIdentity, Result.InputIdentity.Settings,
					std::move(*Result.PlatformData), ApplicationContext);
				if (!Applied)
				{
					DURIN_ERROR("Texture2D compilation result application failed for {}: {}",
						Result.AssetIdentity, FormatTexture2DCompilationError(Applied.error()));
					return {.Error = std::move(Applied.error())};
				}
			}
			return {.Status = ETexture2DCompilationStatus::Succeeded};
		};
		auto Terminal = Finalize();
		const auto Phase = Terminal.Succeeded() ? ETexture2DCompilationPhase::Ready
			: Terminal.Status == ETexture2DCompilationStatus::Canceled
				? ETexture2DCompilationPhase::Cancelled : ETexture2DCompilationPhase::Failed;
		if (Phase == ETexture2DCompilationPhase::Failed && FailureMessage.empty())
			FailureMessage = FormatTexture2DCompilationError(Terminal.Error);
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (auto* State = CompilationState->FindLocked(Result.Owner);
				State && State->LastRequestId == Result.RequestId && State->ActiveRequestId == 0)
				State->Terminal = {Phase, Terminal.Error, std::move(FailureMessage)};
			if (Terminal.Succeeded()) CompilationState->SuccessfullyAppliedTextures.emplace_back(Texture);
		}
		// Publish terminal state before user code can submit another request or shut down.
		if (Completion) Completion(std::move(Terminal));
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
		FTexture2DCompilationCompletion Completion) -> std::expected<void, FTexture2DCompilationError>
	{
		CheckGameThread();
		if (const auto Validation = ValidateTexture2DBuildSource(Request.Build.Source); !Validation)
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidSource,
				.InputCause = Validation.error(), .ObjectPath = Texture.GetObjectPath()});
		if (Request.Build.Source.GetOwner())
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidSource,
				.ObjectPath = Texture.GetObjectPath()});
		if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::ManagerUnavailable,
				.ObjectPath = Texture.GetObjectPath()});
		}
		if (!CompilationState)
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::ManagerNotStarted,
				.ObjectPath = Texture.GetObjectPath()});
		}

		const std::string Identity = Texture.GetObjectPath();
		const FObjectKey Owner = FObjectKey(&Texture);
		if (IsObjectKeyNull(Owner))
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidOwner,
				.ObjectPath = Texture.GetObjectPath()});
		}
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (CompilationState->MutatingOwners.contains(Owner))
				return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::AdmissionRejected,
					.ObjectPath = Identity});
		}
		PrepareMutation(Texture);
		if (!IsValid(&Texture))
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidOwner,
				.ObjectPath = Identity});
		const auto SourceIdentity = Request.Build.Source.GetIdentity();
		const auto Width = Request.Build.Source.GetWidth();
		const auto Height = Request.Build.Source.GetHeight();
		const auto bSourceDecoderInvoked = Request.ResultApplication.bSourceDecoderInvoked;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets[Owner];
			require(State.ActiveRequestId == 0);
			State.Texture = TWeakObjectPtr<DTexture>(&Texture);
			State.Terminal = {};
			State.SourceIdentity = SourceIdentity;
			State.ResultApplicationContext = std::move(Request.ResultApplication);
			State.Completion = std::move(Completion);
		}
		const uint64 RequestId = SubmitWork({.AssetIdentity = Identity,
			.Build = std::move(Request.Build), .Owner = Owner,
			.EstimatedWidth = Width, .EstimatedHeight = Height,
			.Priority = Request.Priority, .bSourceDecoderInvoked = bSourceDecoderInvoked},
			[this](FTexture2DCompilationWorkResult&& Result) { ApplyCompletion(std::move(Result)); });
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets.at(Owner);
			State.ActiveRequestId = State.LastRequestId = RequestId;
			if (!RequestId)
			{
				State.ResultApplicationContext = {};
				State.Completion = {};
				State.Terminal.Phase = ETexture2DCompilationPhase::Failed;
				State.Terminal.Error = {.Code = ETexture2DCompilationError::AdmissionRejected, .ObjectPath = Identity};
				State.Terminal.FailureMessage = FormatTexture2DCompilationError(State.Terminal.Error);
				CompilationState->RetainCompletedLocked(Owner);
				return std::unexpected(State.Terminal.Error);
			}
		}
		return {};
	}

	auto FTextureCompilingManager::GetDiagnostic(const DTexture2D& Texture) const
		-> FTexture2DCompilationDiagnostic
	{
		if (!CompilationState) return {};
		uint64 RequestId = 0;
		bool bPending = false;
		ETexture2DCompilationPhase LastPhase = ETexture2DCompilationPhase::None;
		FTexture2DCompilationError LastError;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(
					const_cast<DTexture2D*>(&Texture)));
				State && State->Texture.Get() == &Texture)
			{
				RequestId = State->ActiveRequestId != 0
					? State->ActiveRequestId : State->LastRequestId;
				bPending = State->ActiveRequestId != 0;
				LastPhase = State->Terminal.Phase;
				LastError = State->Terminal.Error;
			}
		}
		FTexture2DCompilationDiagnostic Result = RequestId != 0
			? GetWorkDiagnostic(RequestId) : FTexture2DCompilationDiagnostic{};
		if (!bPending)
		{
			Result.Phase = LastPhase;
			Result.Error = std::move(LastError);
		}
		return Result;
	}

	auto FTextureCompilingManager::GetBuildStatus(const DTexture2D& Texture) const
		-> FTexture2DBuildStatus
	{
		if (!CompilationState) return {};
		FTexture2DBuildStatus Status;
		uint64 ActiveRequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(
					const_cast<DTexture2D*>(&Texture)));
				State && State->Texture.Get() == &Texture)
			{
				ActiveRequestId = State->ActiveRequestId;
				Status.bPending = ActiveRequestId != 0;
				Status.Phase = State->Terminal.Phase;
				Status.FailureMessage = State->Terminal.FailureMessage;
			}
		}
		if (ActiveRequestId != 0)
		{
			const FTexture2DCompilationDiagnostic Diagnostic = GetWorkDiagnostic(ActiveRequestId);
			Status.Phase = Diagnostic.Phase;
			Status.FailureMessage = FormatTexture2DCompilationError(Diagnostic.Error);
		}
		return Status;
	}

	auto FTextureCompilingManager::SubmitPlatformCache(DTexture& Texture,
		std::shared_ptr<const FTexturePlatformCacheInput> Input) -> bool
	{
		CheckGameThread();
		if (!CompilationState || !FAssetCompilingManager::Get().IsAcceptingRequests()
			|| !Input || !Input->Source.IsValid() || Input->Source.GetOwner()) return false;
		const FObjectKey Owner(&Texture);
		if (IsObjectKeyNull(Owner)) return false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (CompilationState->MutatingOwners.contains(Owner)) return false;
		}
		PrepareMutation(Texture);
		if (!IsValid(&Texture)) return false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets[Owner];
			require(State.ActiveRequestId == 0);
			State.Texture = TWeakObjectPtr<DTexture>(&Texture);
			State.SourceIdentity = Input->Source.GetIdentity();
			State.Terminal = {};
		}
		const uint64 Id = SubmitWork({.AssetIdentity = Texture.GetObjectPath(),
			.Build = {.Source = Input->Source.CopyTornOff()}, .Owner = Owner,
			.PlatformCache = std::move(Input)},
			[this](FTexture2DCompilationWorkResult&& Result) { ApplyCompletion(std::move(Result)); });
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets.at(Owner);
			State.ActiveRequestId = State.LastRequestId = Id;
			if (!Id)
			{
				State.Terminal.Phase = ETexture2DCompilationPhase::Failed;
				State.Terminal.Error = {.Code = ETexture2DCompilationError::AdmissionRejected,
					.ObjectPath = Texture.GetObjectPath()};
				State.Terminal.FailureMessage = FormatTexture2DCompilationError(State.Terminal.Error);
				CompilationState->RetainCompletedLocked(Owner);
			}
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

	auto FTextureCompilingManager::PrepareMutation(DTexture& Texture) -> void
	{
		CheckGameThread();
		if (!CompilationState) return;
		const FObjectKey Owner(&Texture);
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			require(CompilationState->MutatingOwners.emplace(Owner).second);
			if (const auto* State = CompilationState->FindLocked(Owner)) RequestId = State->ActiveRequestId;
		}
		struct FMutationGuard
		{
			std::shared_ptr<FCompilationState> State;
			FObjectKey Owner;
			~FMutationGuard() { std::lock_guard Lock(State->Mutex); State->MutatingOwners.erase(Owner); }
		} Guard{CompilationState, Owner};
		if (RequestId)
		{
			CancelWork(RequestId, ETexture2DCompilationError::CancelledForMutation);
			require(WaitForWork(RequestId, 300.0));
			PumpCompletions(1, {}, RequestId);
		}
		require(!HasPending(Texture));
		std::lock_guard Lock(CompilationState->Mutex);
		if (auto* State = CompilationState->FindLocked(Owner)) State->Terminal = {};
	}

	auto PrepareTextureForMutation(DTexture& Texture) -> void
	{
		if (const auto Manager = GetTextureCompilingManager()) Manager->PrepareMutation(Texture);
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
		bool bCanUsePlatformData = true;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(&Texture));
			if (State && State->Texture.Get() == &Texture)
			{
				RequestId = State->ActiveRequestId;
				bCanUsePlatformData = State->Terminal.AllowsPlatformData();
			}
		}
		if (RequestId == 0)
			return bCanUsePlatformData && Texture.HasPlatformData();
		if (!WaitForWork(RequestId, TimeoutSeconds)) return false;
		FAssetCompilingManager::Get().FinishCompilationForObject(Texture);
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(FObjectKey(&Texture)))
				bCanUsePlatformData = State->Terminal.AllowsPlatformData();
		}
		return !HasPending(Texture) && bCanUsePlatformData
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

	auto BuildTexture2DDetached(const FTexture2DBuildRequest& Request,
		const FTexture2DBuildExecutionControl* ExecutionControl)
		-> std::expected<FTexturePlatformData, FTextureBuildOperationError>
	{
		FTexturePlatformData Product;
		FTexture2DBuildInputIdentity Identity;
		const auto Built = BuildTexture2DPlatformData(Request, Product, Identity, ExecutionControl);
		if (Built) return Product;
		const auto& Error = Built.error();
		if (Error.Code == ETexture2DBuildError::Cancelled)
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Canceled});
		DURIN_ERROR_CATEGORY("Texture", "Detached Texture2D build failed: {}", FormatTexture2DBuildError(Error));
		if (Error.InputCause || Error.Code == ETexture2DBuildError::InvalidInput)
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::InvalidInput,
				FormatTexture2DBuildError(Error)});
		return std::unexpected(FTextureBuildOperationError{});
	}

	auto BuildTexture2DSynchronously(
		DTexture2D& Texture,
		FTexture2DBuildRequest Request,
		const FTexture2DResultApplicationContext& Context) -> std::expected<void, FTexture2DCompilationError>
	{
		CheckGameThread();
		Texture.BlockOnAnyAsyncBuild();
		FTexturePlatformData Product;
		FTexture2DBuildInputIdentity Identity;
		const std::expected<void, FTexture2DBuildError> BuildResult = BuildTexture2DPlatformData(
			Request, Product, Identity);
		if (!BuildResult)
		{
			if (BuildResult.error().Code != ETexture2DBuildError::Cancelled)
				DURIN_ERROR_CATEGORY("Texture", "Texture2D build failed for {}: {}",
					Texture.GetObjectPath(), FormatTexture2DBuildError(BuildResult.error()));
			return std::unexpected(TexturePrivate::MakeCompilationBuildFailure(BuildResult.error()));
		}
		return ApplyTexture2DBuildResult(Texture, Request.Source.GetIdentity(), Request.Settings,
			std::move(Product), Context);
	}

	namespace
	{
	auto ApplyTexture2DBuildResult(
		DTexture2D& Texture,
		FXxHash128 SourceIdentity,
		const FTexture2DBuildSettings& Settings,
		FTexturePlatformData Product,
		const FTexture2DResultApplicationContext& Context) -> std::expected<void, FTexture2DCompilationError>
	{
		CheckGameThread();
		if (!Texture.GetPackage())
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::MissingPackage,
				.ObjectPath = Texture.GetObjectPath()});
		}
		if (!Product.IsValid())
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidProduct,
				.ObjectPath = Texture.GetObjectPath()});
		}
		const FTextureSource& Source = Context.SourceReplacement
			? *Context.SourceReplacement : Texture.GetSource();
		if (!Source.IsValid() || Source.GetIdentity() != SourceIdentity)
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::SourceMismatch,
				.ObjectPath = Texture.GetObjectPath(), .ExpectedSourceIdentity = SourceIdentity,
				.ActualSourceIdentity = Source.GetIdentity()});
		}
		if (const auto Validation = ValidateTexture2DBuildSettings(Settings); !Validation)
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidSettings,
				.InputCause = Validation.error(), .ObjectPath = Texture.GetObjectPath()});
		auto PlatformData = std::make_unique<FTexturePlatformData>(
			std::move(Product));
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
		FTexture2DCompilationCompletion Completion) -> std::expected<void, FTexture2DCompilationError>
	{
		const auto Manager = GetTextureCompilingManager();
		if (!Manager)
		{
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::ManagerUnavailable,
				.ObjectPath = Texture.GetObjectPath()});
		}
		return Manager->Submit(
			Texture, std::move(Request), std::move(Completion));
	}

	auto AssetPrivate::GetTexture2DCompilationDiagnosticForTests(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->GetDiagnostic(Texture) : FTexture2DCompilationDiagnostic{};
	}

	auto GetTexture2DBuildStatus(const DTexture2D& Texture) -> FTexture2DBuildStatus
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->GetBuildStatus(Texture) : FTexture2DBuildStatus{};
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

#else
#include "Texture/TexturePlatformCache.h"
namespace Durin
{
	auto HasPendingTextureCompilation(const DTexture&) -> bool { return false; }
	auto FinishTextureCompilation(DTexture& Texture) -> bool { return Texture.HasPlatformData(); }
}
#endif
