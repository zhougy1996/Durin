#include "Texture/Texture2DCompilation.h"

#include "Texture/Texture2DBuildProvider.h"

#include "Asset/AssetCompilingManager.h"
#include "Asset/Load.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/TextureCompilingManager.h"

namespace Durin
{
	struct FTextureCompilingManager::FCompilationState
	{
		struct FObjectHandleHash
		{
			auto operator()(FObjectHandle Handle) const noexcept -> size_t
			{
				return static_cast<size_t>((static_cast<uint64>(Handle.Generation) << 32)
					| static_cast<uint64>(Handle.Index));
			}
		};

		struct FAssetState
		{
			TWeakObjectPtr<DTexture2D> Texture;
			uint64 RequestSerial = 0;
			uint64 ActiveRequestId = 0;
			uint64 LastRequestId = 0;
			bool bLastRequestFailed = false;
			FTexture2DResultApplicationContext ResultApplicationContext;
			FTexture2DBuildInputIdentity InputIdentity;
			FTexture2DCompilationCompletion Completion;
		};

		auto FindLocked(FObjectHandle Owner) -> FAssetState*
		{
			const auto It = Assets.find(Owner);
			return It == Assets.end() ? nullptr : &It->second;
		}

		auto FindLocked(FObjectHandle Owner) const -> const FAssetState*
		{
			const auto It = Assets.find(Owner);
			return It == Assets.end() ? nullptr : &It->second;
		}

		mutable std::mutex Mutex;
		std::unordered_map<FObjectHandle, FAssetState, FObjectHandleHash> Assets;
		std::deque<FObjectHandle> CompletedOrder;
		static constexpr size_t MaximumRetainedAssetDiagnostics = 256;
		std::vector<FWeakObjectPtr> SuccessfullyAppliedTextures;

		auto RetainCompletedLocked(FObjectHandle Owner) -> void
		{
			CompletedOrder.erase(std::remove(
				CompletedOrder.begin(), CompletedOrder.end(), Owner), CompletedOrder.end());
			CompletedOrder.push_back(Owner);
			while (CompletedOrder.size() > MaximumRetainedAssetDiagnostics)
			{
				const FObjectHandle Oldest = CompletedOrder.front();
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
			FTexture2DImportedData ImportedData,
			const FTexture2DBuildSettings& Settings,
			FTexture2DBuildProduct Product,
			const FTexture2DResultApplicationContext& Context,
			std::string& OutError) -> bool;

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
				&& Expected.ImportedDataIdentity == Completed.ImportedDataIdentity
				&& Expected.Settings == Completed.Settings
				&& Expected.TargetPlatform == Completed.TargetPlatform
				&& Expected.TargetProfile == Completed.TargetProfile;
		}
	}

	auto FTextureCompilingManager::Start(std::string* OutError) -> bool
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
		TWeakObjectPtr<DTexture2D> WeakTexture;
		FTexture2DResultApplicationContext ResultApplicationContext;
		FTexture2DCompilationCompletion Completion;
		bool bInputMismatch = false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(Result.Owner);
			if (!State || State->RequestSerial != Result.RequestSerial
				|| State->ActiveRequestId != Result.RequestId) return;
			bInputMismatch = Result.Phase == ETexture2DCompilationPhase::UploadPending
				&& !MatchesRequestedInput(State->InputIdentity, Result.InputIdentity);
			WeakTexture = State->Texture;
			ResultApplicationContext = State->ResultApplicationContext;
			Completion = std::move(State->Completion);
			State->ActiveRequestId = 0;
			State->LastRequestId = Result.RequestId;
			State->bLastRequestFailed =
				Result.Phase == ETexture2DCompilationPhase::Failed || bInputMismatch;
			CompilationState->RetainCompletedLocked(Result.Owner);
		}
		DTexture2D* Texture = WeakTexture.Get();
		if (!Texture || MakeObjectHandle(Texture) != Result.Owner)
		{
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Diagnostic = "The Texture2D compilation target is unavailable."});
			return;
		}
		if (bInputMismatch)
		{
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Diagnostic = "The Texture2D build input identity changed before result application."});
			return;
		}
		if (Result.Phase != ETexture2DCompilationPhase::UploadPending
			|| !Result.ImportedData || !Result.PlatformData)
		{
			if (Completion) Completion({
				.Status = Result.Phase == ETexture2DCompilationPhase::Cancelled
					? ETexture2DCompilationStatus::Canceled : ETexture2DCompilationStatus::Failed,
				.Diagnostic = Result.Error.empty()
					? "The Texture2D compilation did not produce an applicable product."
					: std::move(Result.Error)});
			return;
		}

		const FTexture2DBuildSettings Settings{
			.Usage = Result.Settings.Usage,
			.CompressionQuality = Result.Settings.CompressionQuality,
			.AlphaMipMode = Result.Settings.AlphaMipMode,
			.AlphaCoverageThreshold = Result.Settings.AlphaCoverageThreshold,
			.MaxResolution = Result.Settings.MaxResolution,
			.bSRGB = Result.Settings.bSRGB};
		FTexture2DBuildProduct Product{
			.PlatformData = std::move(*Result.PlatformData),
			.DerivedDataKey = std::move(Result.DerivedDataKey),
			.PersistenceDiagnostic = std::move(Result.PersistenceDiagnostic),
			.Origin = Result.Origin};
		std::string Error;
		if (!ApplyTexture2DBuildResult(*Texture, std::move(*Result.ImportedData), Settings,
			std::move(Product), ResultApplicationContext, Error))
		{
			{
				std::lock_guard Lock(CompilationState->Mutex);
				if (FCompilationState::FAssetState* State =
					CompilationState->FindLocked(Result.Owner))
					State->bLastRequestFailed = true;
			}
			DURIN_ERROR("Texture2D compilation result application failed for {}: {}",
				Result.AssetIdentity, Error);
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Diagnostic = std::move(Error)});
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
		if (Completion) Completion({.Status = ETexture2DCompilationStatus::Succeeded});
	}

	auto FTextureCompilingManager::PumpCompletions(uint32 MaximumCount)
		-> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Result;
		Result.ProcessedCompletionCount = PumpWorkCompletions(MaximumCount);
		std::lock_guard Lock(CompilationState->Mutex);
		Result.SuccessfullyCompiledAssets =
			std::move(CompilationState->SuccessfullyAppliedTextures);
		CompilationState->SuccessfullyAppliedTextures.clear();
		return Result;
	}

	auto FTextureCompilingManager::ProcessAsyncTasks(
		const FAssetCompileProcessParams& Params) -> FAssetCompileProcessResult
	{
		return PumpCompletions(Params.MaximumCompletions);
	}

	auto FTextureCompilingManager::FinishCompilationForObjects(
		std::span<DObject* const> Objects) -> FAssetCompileProcessResult
	{
		FAssetCompileProcessResult Aggregate;
		for (DObject* Object : Objects)
		{
			auto* Texture = Cast<DTexture2D>(Object);
			if (!IsValid(Texture)) continue;
			uint64 RequestId = 0;
			const FObjectHandle Owner = MakeObjectHandle(Texture);
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
				Aggregate, PumpCompletions(std::numeric_limits<uint32>::max()));
		}
		return Aggregate;
	}

	auto FTextureCompilingManager::MarkCompilationAsCanceled(
		std::span<DObject* const> Objects) -> void
	{
		for (DObject* Object : Objects)
			if (auto* Texture = Cast<DTexture2D>(Object); IsValid(Texture)) Cancel(*Texture);
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
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		CheckGameThread();
		if (!Request.Build.ImportedData.IsValid())
		{
			OutError = "Texture2D compilation submission requires valid normalized source pixels.";
			return false;
		}
		if (!FAssetCompilingManager::Get().IsAcceptingRequests())
		{
			OutError = "The Texture compiling manager is unavailable.";
			return false;
		}
		if (!CompilationState)
		{
			OutError = "The Texture compiling manager has not started.";
			return false;
		}

		const std::string Identity = Texture.GetObjectPath();
		const FObjectHandle Owner = MakeObjectHandle(&Texture);
		if (IsObjectHandleNull(Owner))
		{
			OutError = "Texture2D compilation submission requires a live object handle.";
			return false;
		}
		const FTexture2DBuildSettings Settings = Request.Build.Settings;
		const bool bSourceDecoderInvoked =
			Request.ResultApplication.bSourceDecoderInvoked;
		const bool bSRGB = ResolveTexture2DSRGB(Settings);
		const FXxHash128 ImportedDataIdentity =
			Request.Build.ImportedData.GetIdentity();
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
			State.Texture = TWeakObjectPtr<DTexture2D>(&Texture);
			State.ActiveRequestId = 0;
			State.bLastRequestFailed = false;
			State.ResultApplicationContext = std::move(Request.ResultApplication);
			State.InputIdentity = {
					.ImportedDataIdentity = ImportedDataIdentity,
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

		const uint32 Width = Request.Build.ImportedData.Width;
		const uint32 Height = Request.Build.ImportedData.Height;
		const uint64 RequestId = SubmitWork({
			.AssetIdentity = Identity,
			.ImportedData = std::move(Request.Build.ImportedData),
			.ImportedDataIdentity = ImportedDataIdentity,
			.Settings = {
				.Usage = Settings.Usage,
				.bSRGB = bSRGB,
				.MaxResolution = Settings.MaxResolution,
				.CompressionQuality = Settings.CompressionQuality,
				.AlphaMipMode = Settings.AlphaMipMode,
				.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold},
			.Owner = Owner,
			.RequestSerial = RequestSerial,
			.EstimatedWidth = Width,
			.EstimatedHeight = Height,
			.Priority = Request.Priority,
			.TargetPlatform = Request.Build.TargetPlatform,
			.TargetProfile = Request.Build.TargetProfile,
			.bPersistDerivedData = Request.Build.bPersistDerivedData,
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
					State->bLastRequestFailed = true;
					CompilationState->RetainCompletedLocked(Owner);
				}
			}
			if (SupersededCompletion) SupersededCompletion({
				.Status = ETexture2DCompilationStatus::Superseded,
				.Diagnostic = "The Texture2D compilation was superseded by a newer request."});
			OutError = "The Texture compiling manager rejected the request.";
			return false;
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
			.Diagnostic = "The Texture2D compilation was superseded by a newer request."});
		OutError.clear();
		return true;
	}

	auto FTextureCompilingManager::GetDiagnostic(const DTexture2D& Texture) const
		-> FTexture2DCompilationDiagnostic
	{
		if (!CompilationState) return {};
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			if (const FCompilationState::FAssetState* State =
				CompilationState->FindLocked(MakeObjectHandle(
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

	auto FTextureCompilingManager::HasPending(const DTexture2D& Texture) const -> bool
	{
		if (!CompilationState) return false;
		std::lock_guard Lock(CompilationState->Mutex);
		const FCompilationState::FAssetState* State =
			CompilationState->FindLocked(MakeObjectHandle(
				const_cast<DTexture2D*>(&Texture)));
		return State && State->Texture.Get() == &Texture && State->ActiveRequestId != 0;
	}

	auto FTextureCompilingManager::Cancel(DTexture2D& Texture) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(MakeObjectHandle(&Texture));
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		return RequestId != 0 && CancelWork(RequestId);
	}

	auto FTextureCompilingManager::Wait(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		bool bLastRequestFailed = false;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(MakeObjectHandle(&Texture));
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
				CompilationState->FindLocked(MakeObjectHandle(&Texture)))
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
		const FTexture2DResultApplicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FTexture2DBuildProduct Product;
		FTexture2DBuildInputIdentity Identity;
		const FTexture2DBuildResult BuildResult = InvokeTexture2DBuildProvider(
			Request, Product, Identity);
		if (!BuildResult)
		{
			OutError = BuildResult.Diagnostic;
			return false;
		}
		return ApplyTexture2DBuildResult(Texture, std::move(Request.ImportedData),
			Request.Settings, std::move(Product), Context, OutError);
	}

	namespace
	{
	auto ApplyTexture2DBuildResult(
		DTexture2D& Texture,
		FTexture2DImportedData ImportedData,
		const FTexture2DBuildSettings& Settings,
		FTexture2DBuildProduct Product,
		const FTexture2DResultApplicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Texture.GetPackage())
		{
			OutError = "Texture2D result application requires a package.";
			return false;
		}
		if (!ImportedData.IsValid() || !Product.PlatformData.IsValid()
			|| Product.DerivedDataKey.empty())
		{
			OutError = "Texture2D result application requires a complete detached product.";
			return false;
		}
		if (!ValidateTexture2DBuildSettings(Settings, OutError)) return false;
		auto PlatformData = std::make_unique<FTexturePlatformData>(
			std::move(Product.PlatformData));
		if (!Texture.SetSourceData(ImportedData, OutError)
			|| !Texture.SetBuildSettings(Settings.Usage, ResolveTexture2DSRGB(Settings),
				Settings.MaxResolution, Settings.CompressionQuality,
				Settings.AlphaMipMode, Settings.AlphaCoverageThreshold, OutError)
			|| !Texture.SetPlatformData(std::move(PlatformData), OutError)) return false;
		Texture.UpdateResource();
		if (Context.bMarkPackageDirty) Texture.MarkPackageDirty();
		if (Context.bReportLoadMutation)
		{
			ReportAssetLoadMutation(&Texture,
				"Engine.Texture2D.SourceIdentity",
				"Texture source identity metadata was reconciled by an uncooked post-load build.");
		}
		OutError.clear();
		return true;
	}
	}

	auto SubmitTexture2DCompilation(
		DTexture2D& Texture,
		FTexture2DCompilationRequest Request,
		std::string& OutError,
		FTexture2DCompilationCompletion Completion) -> bool
	{
		const auto Manager = GetTextureCompilingManager();
		if (!Manager)
		{
			OutError = "The Texture compiling manager is unavailable.";
			return false;
		}
		return Manager->Submit(
			Texture, std::move(Request), OutError, std::move(Completion));
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
