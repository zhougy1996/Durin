#include "Misc/Build.h"
#if DURIN_WITH_EDITORONLY_DATA
#include "Texture/Texture2DCompilation.h"

#include "Texture/Texture2DBuild.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetImportData.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/TextureCompilingManager.h"

namespace Durin
{
	auto FormatTexture2DCompilationError(const FTexture2DCompilationError& Error) -> std::string
	{
		if (Error.InputCause) return FormatTexture2DInputError(*Error.InputCause);
		if (!Error.Diagnostic.empty()) return Error.Diagnostic;
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
		case ETexture2DCompilationError::SourceMismatch: return std::format("Texture2D build source mismatch: expected {}, actual {}.",
			Error.ExpectedSourceIdentity.ToString(), Error.ActualSourceIdentity.ToString());
		case ETexture2DCompilationError::InvalidSettings: return "Texture2D build settings are invalid.";
		case ETexture2DCompilationError::BuildFailed: return "Texture build failed.";
		}
		return {};
	}

	auto ReportTextureCompilationFailure(const FTexture2DCompilationError& Error) -> FTextureBuildOperationError
	{
		const auto Message = FormatTexture2DCompilationError(Error);
		if (Error.InputCause || Error.Code == ETexture2DCompilationError::InvalidSource
			|| Error.Code == ETexture2DCompilationError::MissingSourceIdentity || Error.Code == ETexture2DCompilationError::InvalidSettings
			|| Error.Code == ETexture2DCompilationError::MissingPackage || Error.Code == ETexture2DCompilationError::InvalidOwner)
			return {ETextureBuildOperationFailure::InvalidInput, Message};
		if (Error.Code == ETexture2DCompilationError::ManagerUnavailable || Error.Code == ETexture2DCompilationError::ManagerNotStarted)
			return {ETextureBuildOperationFailure::Unavailable};
		if (Error.Code == ETexture2DCompilationError::AdmissionRejected) return {ETextureBuildOperationFailure::Busy};
		DURIN_ERROR_CATEGORY("Texture", "Texture operation failed for {} (code {}): {}", Error.ObjectPath, int(Error.Code), Message);
		return {};
	}

	struct FTextureCompilingManager::FCompilationState
	{
		struct FAssetState
		{
			TWeakObjectPtr<DTexture> Texture;
			uint64 ActiveRequestId = 0;
			std::shared_ptr<FTexture2DCompilationProgress> Progress;
			FTexture2DResultApplicationContext ResultApplicationContext;
			FTextureCompilationCompletion Completion;
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
		FTextureCompilationCompletion Completion;
		std::shared_ptr<FTexture2DCompilationProgress> Progress;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto* State = CompilationState->FindLocked(Result.Owner);
			if (!State || State->ActiveRequestId != Result.RequestId) return;
			Progress = State->Progress;
			WeakTexture = State->Texture;
			ApplicationContext = std::move(State->ResultApplicationContext);
			State->ResultApplicationContext = {};
			Completion = std::move(State->Completion);
			State->ActiveRequestId = 0;
			CompilationState->RetainCompletedLocked(Result.Owner);
		}

		DTexture* Texture = WeakTexture.Get();
		// Work completion supplies a CPU product. Object completion includes validation and Apply.
		const auto Finalize = [&]() -> FTextureOperationResult {
			if (Result.Status != DerivedData::EStatus::Ok)
				return {.Status = Result.Status == DerivedData::EStatus::Canceled ? ETextureCompilationStatus::Canceled : ETextureCompilationStatus::Failed,
					.Diagnostic = std::move(Result.Diagnostic)};
			if (!Texture || Texture->IsPendingKill() || FObjectKey(Texture) != Result.Owner)
				return {.Diagnostic = "Texture compilation owner is no longer valid."};
			require(static_cast<bool>(Result.PlatformData) != static_cast<bool>(Result.PlatformCache));
			if (Result.PlatformCache)
			{
				if (Texture->GetSource().GetIdentity() != Result.SourceIdentity)
					return {.Diagnostic = "Texture build result does not match the source selected for commit."};
				Result.PlatformCache->Apply(*Texture);
			}
			else
			{
				auto Applied = Cast<DTexture2D>(Texture)->ApplyBuildResult(
					Result.SourceIdentity, Result.Settings,
					std::move(Result.PlatformData), ApplicationContext);
				if (!Applied)
				{
					return {.Diagnostic = FormatTextureBuildOperationError(Applied.error())};
				}
			}
			return {.Status = ETextureCompilationStatus::Succeeded};
		};
		auto Terminal = Finalize();
		const auto Phase = Terminal.Succeeded() ? ETexture2DCompilationPhase::Ready
			: Terminal.Status == ETextureCompilationStatus::Canceled
				? ETexture2DCompilationPhase::Canceled : ETexture2DCompilationPhase::Failed;
		{
			std::lock_guard Lock(Progress->Mutex);
			Progress->Diagnostic.Phase = Phase;
			Progress->Diagnostic.Diagnostic = Terminal.Diagnostic;
		}
		if (Terminal.Status == ETextureCompilationStatus::Failed)
			DURIN_ERROR("Texture compilation failed for {}: {}", Result.AssetIdentity, Terminal.Diagnostic);
		if (Terminal.Succeeded())
		{
			std::lock_guard Lock(CompilationState->Mutex);
			CompilationState->SuccessfullyAppliedTextures.emplace_back(Texture);
		}
		// Publish terminal state before user code can submit another request or shut down.
		if (Completion) {
			Terminal.Diagnostic = Terminal.Status == ETextureCompilationStatus::Failed ? "Texture build failed." : "";
			Completion(std::move(Terminal));
		}
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
			if (auto* Texture = Cast<DTexture>(Object)) Cancel(*Texture);
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
		FTextureCompilationCompletion Completion) -> std::expected<void, FTexture2DCompilationError>
	{
		CheckGameThread();
		if (!IsValid(&Texture))
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidOwner,
				.ObjectPath = Texture.GetObjectPath()});
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
		Texture.BlockOnAnyAsyncBuild();
		if (!IsValid(&Texture))
			return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidOwner,
				.ObjectPath = Identity});
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets[Owner];
			require(State.ActiveRequestId == 0);
			State.Texture = TWeakObjectPtr<DTexture>(&Texture);
			State.Progress = std::make_shared<FTexture2DCompilationProgress>();
			State.ResultApplicationContext = std::move(Request.ResultApplication);
			State.Completion = std::move(Completion);
		}
		const uint64 RequestId = SubmitWork({.AssetIdentity = Identity,
			.Progress = CompilationState->Assets.at(Owner).Progress,
			.Build = std::move(Request.Build), .Owner = Owner,
			.Priority = Request.Priority},
			[this](FTexture2DCompilationWorkResult&& Result) { ApplyCompletion(std::move(Result)); });
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets.at(Owner);
			State.ActiveRequestId = RequestId;
			if (!RequestId)
			{
				State.ResultApplicationContext = {};
				State.Completion = {};
				State.Progress->Diagnostic.Phase = ETexture2DCompilationPhase::Failed;
				State.Progress->Diagnostic.Diagnostic = "The Texture compiling manager rejected the request.";
				CompilationState->RetainCompletedLocked(Owner);
				return std::unexpected(FTexture2DCompilationError{.Code = ETexture2DCompilationError::AdmissionRejected, .ObjectPath = Identity});
			}
		}
		return {};
	}

	auto FTextureCompilingManager::GetDiagnostic(const DTexture2D& Texture) const
		-> FTexture2DCompilationDiagnostic
	{
		if (!CompilationState) return {};
		std::shared_ptr<FTexture2DCompilationProgress> Progress;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const auto* State = CompilationState->FindLocked(FObjectKey(const_cast<DTexture2D*>(&Texture)));
				State && State->Texture.Get() == &Texture) Progress = State->Progress;
		}
		return Progress ? Progress->Read() : FTexture2DCompilationDiagnostic{};
	}

	auto FTextureCompilingManager::SubmitPlatformCache(DTexture& Texture,
		std::shared_ptr<const FTexturePlatformCacheInput> Input) -> bool
	{
		CheckGameThread();
		if (!CompilationState || !FAssetCompilingManager::Get().IsAcceptingRequests()
			|| !IsValid(&Texture)
			|| !Input || !Input->Source.IsValid() || Input->Source.GetOwner()) return false;
		const FObjectKey Owner(&Texture);
		if (IsObjectKeyNull(Owner)) return false;
		Texture.BlockOnAnyAsyncBuild();
		if (!IsValid(&Texture)) return false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets[Owner];
			require(State.ActiveRequestId == 0);
			State.Texture = TWeakObjectPtr<DTexture>(&Texture);
			State.Progress = std::make_shared<FTexture2DCompilationProgress>();
		}
		const uint64 Id = SubmitWork({.AssetIdentity = Texture.GetObjectPath(),
			.Progress = CompilationState->Assets.at(Owner).Progress,
			.Owner = Owner,
			.PlatformCache = std::move(Input)},
			[this](FTexture2DCompilationWorkResult&& Result) { ApplyCompletion(std::move(Result)); });
		{
			std::lock_guard Lock(CompilationState->Mutex);
			auto& State = CompilationState->Assets.at(Owner);
			State.ActiveRequestId = Id;
			if (!Id)
			{
				State.Progress->Diagnostic.Phase = ETexture2DCompilationPhase::Failed;
				State.Progress->Diagnostic.Diagnostic = "The Texture compiling manager rejected the request.";
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

	auto FinishTextureCompilation(DTexture& Texture, double TimeoutSeconds) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->Wait(Texture, TimeoutSeconds) : Texture.HasPlatformData();
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
			// The key retains the object's serial even after its weak pointer expires at BeginDestroy.
			if (State) RequestId = State->ActiveRequestId;
		}
		return RequestId != 0 && CancelWork(RequestId);
	}

	auto FTextureCompilingManager::Wait(
		DTexture& Texture, double TimeoutSeconds) -> bool
	{
		uint64 RequestId = 0;
		if (CompilationState)
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const auto* State = CompilationState->FindLocked(FObjectKey(&Texture));
				State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		if (RequestId != 0)
		{
			if (!WaitForWork(RequestId, TimeoutSeconds)) return false;
			FAssetCompilingManager::Get().FinishCompilationForObject(Texture);
		}
		// Installed CPU data owns readiness; a retired request's diagnostic is not a second cache state.
		return !HasPending(Texture) && Texture.HasPlatformData();
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

	namespace
	{
		auto ReportTexture2DBuildFailure(const FTexture2DBuildError& Error) -> FTextureBuildOperationError
		{
			if (Error.Code == ETexture2DBuildError::Cancelled) return {ETextureBuildOperationFailure::Canceled};
			DURIN_ERROR_CATEGORY("Texture", "Texture2D build failed: {}", FormatTexture2DBuildError(Error));
			if (Error.InputCause || Error.Code == ETexture2DBuildError::InvalidInput)
				return {ETextureBuildOperationFailure::InvalidInput, FormatTexture2DBuildError(Error)};
			if (Error.Code == ETexture2DBuildError::ModuleUnavailable || Error.Code == ETexture2DBuildError::AuthoredBuildUnavailable)
				return {ETextureBuildOperationFailure::Unavailable};
			return {};
		}
	}
	auto BuildTexture2DDetached(const FTexture2DBuildRequest& Request)
		-> std::expected<FTexturePlatformData, FTextureBuildOperationError>
	{
		auto Built = BuildTexture2DPlatformData(Request);
		if (!Built) return std::unexpected(ReportTexture2DBuildFailure(Built.error()));
		return std::move(*Built);
	}

	auto DTexture2D::CachePlatformData(const FTexture2DBuildRequest& Request,
		const FTexture2DResultApplicationContext& Context) -> std::expected<void, FTextureBuildOperationError>
	{
		CheckGameThread();
		if (IsPendingKill())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		BlockOnAnyAsyncBuild();
		// A retired request's completion callback may have begun teardown.
		if (IsPendingKill())
			return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		auto Built = BuildTexture2DPlatformData(Request);
		if (!Built) return std::unexpected(ReportTexture2DBuildFailure(Built.error()));
		return ApplyBuildResult(Request.Source.GetIdentity(), Request.Settings,
			std::make_unique<FTexturePlatformData>(std::move(*Built)), Context);
	}

	auto DTexture2D::ApplyBuildResult(
		FXxHash128 SourceIdentity,
		const FTexture2DBuildSettings& Settings,
		std::unique_ptr<FTexturePlatformData> Product,
		const FTexture2DResultApplicationContext& Context) -> std::expected<void, FTextureBuildOperationError>
	{
		CheckGameThread();
		require(!HasPendingTextureCompilation(*this));
		if (!GetPackage())
		{
			return std::unexpected(ReportTextureCompilationFailure(FTexture2DCompilationError{.Code = ETexture2DCompilationError::MissingPackage,
				.ObjectPath = GetObjectPath()}));
		}
		if (!Product || !Product->IsValid())
		{
			return std::unexpected(ReportTextureCompilationFailure(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidProduct,
				.ObjectPath = GetObjectPath()}));
		}
		const FTextureSource& Source = Context.SourceReplacement
			? *Context.SourceReplacement : GetSource();
		if (!Source.IsValid() || Source.GetIdentity() != SourceIdentity)
		{
			return std::unexpected(ReportTextureCompilationFailure(FTexture2DCompilationError{.Code = ETexture2DCompilationError::SourceMismatch,
				.ObjectPath = GetObjectPath(), .ExpectedSourceIdentity = SourceIdentity,
				.ActualSourceIdentity = Source.GetIdentity()}));
		}
		if (const auto Validation = ValidateTexture2DBuildSettings(Settings); !Validation)
			return std::unexpected(ReportTextureCompilationFailure(FTexture2DCompilationError{.Code = ETexture2DCompilationError::InvalidSettings,
				.InputCause = Validation.error(), .ObjectPath = GetObjectPath()}));
		// Allocate the shared reader lifetime before changing any installed state.
		std::shared_ptr<FTexturePlatformData> Installed(std::move(Product));
		if (Context.SourceReplacement) AdoptBuildSource(*Context.SourceReplacement);
		Usage = Settings.Usage;
		bSRGB = ResolveTexture2DSRGB(Settings);
		MaxResolution = Settings.MaxResolution;
		CompressionQuality = Settings.CompressionQuality;
		AlphaMipMode = Settings.AlphaMipMode;
		AlphaCoverageThreshold = Settings.AlphaCoverageThreshold;
		InvalidateAuthoredBuild();
		PlatformData = std::move(Installed);
		UpdateResource();
		if (Context.bMarkPackageDirty) MarkPackageDirty();
		return {};

	}

	auto DTexture2D::BeginCachePlatformData(FTexture2DCompilationRequest Request,
		FTextureCompilationCompletion Completion) -> std::expected<void, FTextureBuildOperationError>
	{
		const auto Manager = GetTextureCompilingManager();
		if (!Manager) return std::unexpected(FTextureBuildOperationError{ETextureBuildOperationFailure::Unavailable});
		auto Submitted = Manager->Submit(*this, std::move(Request), std::move(Completion));
		if (!Submitted) return std::unexpected(ReportTextureCompilationFailure(Submitted.error()));
		return {};
	}

	auto AssetPrivate::GetTexture2DCompilationDiagnosticForTests(const DTexture2D& Texture)
		-> FTexture2DCompilationDiagnostic
	{
		const auto Manager = GetTextureCompilingManager();
		return Manager ? Manager->GetDiagnostic(Texture) : FTexture2DCompilationDiagnostic{};
	}

}

#else
#include "Texture/TexturePlatformCache.h"
namespace Durin
{
	auto HasPendingTextureCompilation(const DTexture&) -> bool { return false; }
	auto FinishTextureCompilation(DTexture& Texture, double) -> bool { return Texture.HasPlatformData(); }
}
#endif
