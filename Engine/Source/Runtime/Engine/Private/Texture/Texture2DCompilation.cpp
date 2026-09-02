#include "Texture/Texture2DCompilation.h"

#include "Texture/Texture2DBuildProvider.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/DObjectGlobals.h"
#include "Threading/RunnableThread.h"
#include "Texture/Texture2DCompilationDomain.h"

namespace Durin
{
	struct FTexture2DCompilationDomain::FCompilationState
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
			FTexture2DPublicationContext PublicationContext;
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
		std::vector<FWeakObjectPtr> SuccessfullyPublishedTextures;
	};

	namespace
	{
		std::mutex GTexture2DCompilationDomainMutex;
		std::shared_ptr<FTexture2DCompilationDomain> GTexture2DCompilationDomain;

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
		return static_cast<uint64>(CompilationState->Assets.size());
	}

	auto FTexture2DCompilationDomain::ApplyCompletion(
		FTexture2DCompilationWorkResult&& Result) -> void
	{
		CheckGameThread();
		TWeakObjectPtr<DTexture2D> WeakTexture;
		FTexture2DPublicationContext PublicationContext;
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
			PublicationContext = State->PublicationContext;
			Completion = std::move(State->Completion);
			CompilationState->Assets.erase(Result.Owner);
		}
		DTexture2D* Texture = WeakTexture.Get();
		if (!Texture || MakeObjectHandle(Texture) != Result.Owner
			|| Texture->CompilationRequestSerial != Result.RequestSerial)
		{
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Diagnostic = "The Texture2D compilation target is unavailable."});
			return;
		}
		Texture->CompilationLastRequestId = Result.RequestId;
		Texture->bCompilationLastRequestFailed =
			Result.Phase == ETexture2DCompilationPhase::Failed || bInputMismatch;
		if (bInputMismatch)
		{
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Diagnostic = "The Texture2D build input identity changed before publication."});
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
		if (!PublishTexture2DProduct(*Texture, std::move(*Result.SourceData), Settings,
			std::move(Product), PublicationContext, Error))
		{
			Texture->bCompilationLastRequestFailed = true;
			DURIN_ERROR("Texture2D compilation publication failed for {}: {}",
				Result.AssetIdentity, Error);
			if (Completion) Completion({
				.Status = ETexture2DCompilationStatus::Failed,
				.Diagnostic = std::move(Error)});
			return;
		}
		Texture->bCompilationLastRequestFailed = false;
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
		if (!Request.Build.SourceData.IsValid())
		{
			OutError = "Texture2D compilation submission requires valid normalized source pixels.";
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
		const FObjectHandle Owner = MakeObjectHandle(&Texture);
		if (IsObjectHandleNull(Owner))
		{
			OutError = "Texture2D compilation submission requires a live object handle.";
			return false;
		}
		const FTexture2DBuildSettings Settings = Request.Build.Settings;
		const bool bSRGB = ResolveTexture2DSRGB(Settings);
		const FXxHash128 ImportedDataIdentity =
			Request.Build.SourceData.GetImportedDataIdentity();
		uint64 RequestSerial = ++Texture.CompilationRequestSerial;
		if (RequestSerial == 0) RequestSerial = ++Texture.CompilationRequestSerial;
		uint64 PreviousRequestId = 0;
		FTexture2DCompilationCompletion SupersededCompletion;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState& State = CompilationState->Assets[Owner];
			PreviousRequestId = State.ActiveRequestId;
			if (PreviousRequestId != 0) SupersededCompletion = std::move(State.Completion);
			State = {
				.Texture = TWeakObjectPtr<DTexture2D>(&Texture),
				.RequestSerial = RequestSerial,
				.PublicationContext = std::move(Request.Publication),
				.InputIdentity = {
					.ImportedDataIdentity = ImportedDataIdentity,
					.Settings = {
						.Usage = Settings.Usage,
						.CompressionQuality = Settings.CompressionQuality,
						.AlphaMipMode = Settings.AlphaMipMode,
						.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
						.MaxResolution = Settings.MaxResolution,
						.bSRGB = bSRGB},
					.TargetPlatform = Request.Build.TargetPlatform,
					.TargetProfile = Request.Build.TargetProfile},
				.Completion = std::move(Completion)};
		}
		if (PreviousRequestId != 0) CancelWork(PreviousRequestId);

		const uint32 Width = Request.Build.SourceData.Width;
		const uint32 Height = Request.Build.SourceData.Height;
		const uint64 RequestId = SubmitWork({
			.AssetIdentity = Identity,
			.SourceData = std::move(Request.Build.SourceData),
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
			.bPersistDerivedData = Request.Build.bPersistDerivedData},
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
					CompilationState->Assets.erase(Owner);
			}
			if (SupersededCompletion) SupersededCompletion({
				.Status = ETexture2DCompilationStatus::Superseded,
				.Diagnostic = "The Texture2D compilation was superseded by a newer request."});
			OutError = "The Texture2D compilation domain rejected the request.";
			return false;
		}
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State = CompilationState->FindLocked(Owner);
			if (State && State->RequestSerial == RequestSerial)
			{
				State->ActiveRequestId = RequestId;
				Texture.CompilationLastRequestId = RequestId;
				Texture.bCompilationLastRequestFailed = false;
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
				CompilationState->FindLocked(MakeObjectHandle(
					const_cast<DTexture2D*>(&Texture))))
				RequestId = State->ActiveRequestId;
		}
		if (RequestId == 0) RequestId = Texture.CompilationLastRequestId;
		return RequestId != 0 ? GetWorkDiagnostic(RequestId) : FTexture2DCompilationDiagnostic{};
	}

	auto FTexture2DCompilationDomain::GetManagerDiagnostics() const
		-> FTexture2DCompilationManagerDiagnostics
	{
		FTexture2DCompilationManagerDiagnostics Result = GetWorkManagerDiagnostics();
		if (!CompilationState) return Result;
		std::lock_guard Lock(CompilationState->Mutex);
		Result.ActiveRecordCount = CompilationState->Assets.size();
		return Result;
	}

	auto FTexture2DCompilationDomain::HasPending(const DTexture2D& Texture) const -> bool
	{
		if (!CompilationState) return false;
		std::lock_guard Lock(CompilationState->Mutex);
		const FCompilationState::FAssetState* State =
			CompilationState->FindLocked(MakeObjectHandle(
				const_cast<DTexture2D*>(&Texture)));
		return State && State->Texture.Get() == &Texture && State->ActiveRequestId != 0;
	}

	auto FTexture2DCompilationDomain::Cancel(DTexture2D& Texture) -> bool
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

	auto FTexture2DCompilationDomain::Wait(
		DTexture2D& Texture, double TimeoutSeconds) -> bool
	{
		if (!CompilationState) return false;
		uint64 RequestId = 0;
		{
			std::lock_guard Lock(CompilationState->Mutex);
			FCompilationState::FAssetState* State =
				CompilationState->FindLocked(MakeObjectHandle(&Texture));
			if (State && State->Texture.Get() == &Texture) RequestId = State->ActiveRequestId;
		}
		if (RequestId == 0)
			return !Texture.bCompilationLastRequestFailed
				&& Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
		if (!WaitForWork(RequestId, TimeoutSeconds)) return false;
		FAssetCompilingManager::Get().FinishCompilationForObject(Texture);
		return !HasPending(Texture) && !Texture.bCompilationLastRequestFailed
			&& Texture.GetBuildStatus() == ETextureBuildStatus::Ready;
	}

	namespace AssetPrivate
	{
		auto CreateTexture2DCompilationDomain() -> std::shared_ptr<IAssetCompilationDomain>
		{
			CheckGameThread();
			std::lock_guard Lock(GTexture2DCompilationDomainMutex);
			if (!GTexture2DCompilationDomain)
				GTexture2DCompilationDomain = std::make_shared<FTexture2DCompilationDomain>();
			return GTexture2DCompilationDomain;
		}

		auto ReleaseTexture2DCompilationDomain() -> void
		{
			CheckGameThread();
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

	auto BuildTexture2DSynchronously(
		DTexture2D& Texture,
		FTexture2DBuildRequest Request,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		FTexture2DBuildProduct Product;
		FTexture2DBuildInputIdentity Identity;
		if (!InvokeTexture2DBuildProvider(
			Request, Product, Identity, OutError)) return false;
		return PublishTexture2DProduct(Texture, std::move(Request.SourceData),
			Request.Settings, std::move(Product), Context, OutError);
	}

	auto PublishTexture2DProduct(
		DTexture2D& Texture,
		FTextureSourceData SourceData,
		const FTexture2DBuildSettings& Settings,
		FTexture2DBuildProduct Product,
		const FTexture2DPublicationContext& Context,
		std::string& OutError) -> bool
	{
		CheckGameThread();
		if (!Texture.GetPackage())
		{
			OutError = "Texture2D product publication requires a package.";
			return false;
		}
		if (!SourceData.IsValid() || !Product.PlatformData.IsValid()
			|| Product.DerivedDataKey.empty())
		{
			OutError = "Texture2D product publication requires a complete detached product.";
			return false;
		}
		if (!ValidateTexture2DBuildSettings(Settings, OutError)) return false;
		return Texture.PublishImportedState({
			.SourceData = std::make_unique<FTextureSourceData>(std::move(SourceData)),
			.PlatformData = std::make_unique<FTexturePlatformData>(std::move(Product.PlatformData)),
			.DerivedDataKey = std::move(Product.DerivedDataKey),
			.BuildDiagnostic = std::move(Product.PersistenceDiagnostic),
			.Usage = Settings.Usage,
			.bSRGB = ResolveTexture2DSRGB(Settings),
			.MaxResolution = Settings.MaxResolution,
			.CompressionQuality = Settings.CompressionQuality,
			.AlphaMipMode = Settings.AlphaMipMode,
			.AlphaCoverageThreshold = Settings.AlphaCoverageThreshold,
			.bMarkPackageDirty = Context.bMarkPackageDirty,
			.bReportLoadMutation = Context.bReportLoadMutation,
			.bSourceDecoderInvoked = Context.bSourceDecoderInvoked,
			.bLoadedFromDerivedDataCache =
				Product.Origin == ETexture2DBuildProductOrigin::CacheHit}, OutError);
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

	auto GetTexture2DCompilationManagerDiagnostics()
		-> FTexture2DCompilationManagerDiagnostics
	{
		const auto Domain = GetTexture2DCompilationDomain();
		return Domain ? Domain->GetManagerDiagnostics()
			: FTexture2DCompilationManagerDiagnostics{};
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
