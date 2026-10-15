#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/AssetThumbnailGeneration.h"

#include "DynamicRHI.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "AssetRegistry/Catalog.h"

namespace Durin::Editor
{
	namespace
	{
		struct FAssetThumbnailUploadResult
		{
			FTopLevelAssetPath AssetPath;
			std::string AssetClassName;
			FAssetThumbnailCancellation Cancellation;
			uint64 RendererGeneration = 0;
			uint64 Serial = 0;
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Error;
		};

		struct FAssetThumbnailAsyncState
		{
			std::mutex Mutex;
			std::vector<FAssetThumbnailUploadResult> Uploads;
		};
	} // namespace

	struct FAssetThumbnailPool::FImpl
	{
		struct FParkedJob
		{
			FAssetThumbnailJob Job;
			FThumbnailRendererSessionUpdate LastUpdate;
			uint64 FirstWaitFrame = 0;
			uint64 NextPollFrame = 0;
			bool bReady = false;
		};

		struct FEntry
		{
			FAssetThumbnailPackageFingerprint Fingerprint;
			FTextureRHIRef Texture;
			uint64 Serial = 1;
			uint64 LastUsedFrame = 0;
			bool bVisible = false;
			bool bUploading = false;
			bool bUploadFailed = false;
			bool bHasTransparency = true;
			bool bBackgroundRequested = false;
			uint32 ReferencerCount = 0;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Diagnostic;
		};

		DThumbnailManager& Manager;
		FAssetThumbnailBudgets Budgets;
		FAssetThumbnailRequestQueue Scheduler;
		FAssetThumbnailGeneration Pipeline;
		std::unique_ptr<FThumbnailPreviewScenePool> ScenePool;
		std::optional<FAssetThumbnailOutputSettings> SceneOutput;
		std::unordered_map<FTopLevelAssetPath, FEntry> Entries;
		std::shared_ptr<FAssetThumbnailAsyncState> AsyncState =
			std::make_shared<FAssetThumbnailAsyncState>();
		std::optional<FAssetThumbnailJob> PreparingJob;
		// Preparation never owns the preview scene. A capture retains its session
		// until readback and revision validation finish, while other assets load.
		std::optional<FAssetThumbnailJob> CaptureJob;
		std::vector<FParkedJob> ParkedJobs;
		uint64 FrameNumber = 0;
		uint64 PreviewSceneCreations = 0;
		uint64 PreviewSceneAssignments = 0;
		uint64 UploadsQueued = 0;
		uint64 UploadsCompleted = 0;
		uint64 UploadFailures = 0;
		uint64 GpuEvictions = 0;
		uint64 PeakParkedResourceWaits = 0;
		uint64 ResourceWaitTimeouts = 0;
		uint64 CatalogRevision = 0;
		std::deque<std::string> BackgroundPackages;

		FImpl(
			DThumbnailManager& InService,
			FAssetThumbnailBudgets InBudgets,
			FAssetThumbnailPoolStorageSettings StoreSettings)
			: Manager(InService)
			, Budgets(InBudgets)
			, Scheduler(Manager, Budgets)
			, Pipeline(Scheduler, std::move(StoreSettings), Budgets, true)
		{
			if (Budgets.bPreGenerateOnAssetChanges) CatalogRevision = GetAssetCatalogRevision();
		}

		auto IsCurrent(const FAssetThumbnailGenerationRequest& Request) const -> bool
		{
			const FThumbnailRendererHandle Renderer =
				Manager.Find(Request.KeyInput.Asset.AssetClassName);
			return !Request.Cancellation.IsCancelled()
				&& Renderer.Generation == Request.RendererGeneration;
		}

		auto EnsureScene(const FAssetThumbnailOutputSettings& Output) -> bool
		{
			if (ScenePool != nullptr && SceneOutput && *SceneOutput == Output)
				return ScenePool->IsAvailable();
			ScenePool.reset();
			SceneOutput = Output;
			ScenePool = std::make_unique<FThumbnailPreviewScenePool>(
				Output, Budgets);
			++PreviewSceneCreations;
			return ScenePool->IsAvailable();
		}

		auto ReleaseJob(FAssetThumbnailJob& Job) -> void
		{
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			if (IThumbnailRendererSession* Session =
				Request.GetRenderedSession())
			{
				Session->ResetPreview();
			}
			Request.ReleaseRenderedSession();
		}

		auto ResetPreparing() -> void
		{
			if (!PreparingJob) return;
			ReleaseJob(*PreparingJob);
			PreparingJob.reset();
		}

		auto ResetCapture() -> void
		{
			if (!CaptureJob) return;
			ReleaseJob(*CaptureJob);
			if (ScenePool) ScenePool->Reset();
			CaptureJob.reset();
		}

		auto ResetParkedJobs() -> void
		{
			for (FParkedJob& Parked : ParkedJobs) ReleaseJob(Parked.Job);
			ParkedJobs.clear();
		}

		auto UnregisterTexture(FEntry& Entry) -> void
		{
			if (Entry.Texture && Mona::GetActiveUIBackend())
				Mona::GetActiveUIBackend()->UnregisterTexture(Entry.Texture);
			Entry.Texture = nullptr;
		}

		auto QueueUpload(
			const FAssetThumbnailGenerationRequest& Request,
			FByteBuffer Pixels,
			uint32 Width,
			uint32 Height) -> void
		{
			const FTopLevelAssetPath& AssetPath = Request.KeyInput.Asset.AssetPath;
			auto It = Entries.find(AssetPath);
			if (It == Entries.end() || It->second.Serial != Request.RequestSerial
				|| !IsCurrent(Request))
				return;
			It->second.bUploading = true;
			It->second.bUploadFailed = false;
			It->second.bHasTransparency = Request.bHasTransparency;
			++UploadsQueued;
			const std::weak_ptr<FAssetThumbnailAsyncState> WeakState = AsyncState;
			auto SharedPixels = std::make_shared<FByteBuffer>(std::move(Pixels));
			const std::string AssetClassName = Request.KeyInput.Asset.AssetClassName;
			const FAssetThumbnailCancellation Cancellation = Request.Cancellation;
			const uint64 RendererGeneration = Request.RendererGeneration;
			const uint64 Serial = Request.RequestSerial;
			ENQUEUE_RENDER_COMMAND(UploadRenderedAssetThumbnail)(
				[WeakState,
					AssetPath,
					AssetClassName,
					Cancellation,
					RendererGeneration,
					Serial,
					SharedPixels,
					Width,
					Height](FRHICommandListImmediate& CommandList) {
					FAssetThumbnailUploadResult Result{
						.AssetPath = AssetPath,
						.AssetClassName = AssetClassName,
						.Cancellation = Cancellation,
						.RendererGeneration = RendererGeneration,
						.Serial = Serial,
						.Width = Width,
						.Height = Height};
					if (!Cancellation.IsCancelled())
					{
						FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D(
							"RenderedAssetThumbnail",
							Width,
							Height,
							EPixelFormat::SRGBA8_UNORM);
						Desc.AddFlags(ETextureCreateFlags::ShaderResource);
						Result.Texture = GDynamicRHI != nullptr
							? GDynamicRHI->RHICreateTexture(CommandList, Desc)
							: nullptr;
						if (Result.Texture != nullptr)
						{
							const FUpdateTextureRegion2D Region(
								0, 0, 0, 0, Width, Height);
							CommandList.UpdateTexture2D(Result.Texture,
								0,
								0,
								Region,
								Width * 4,
								*SharedPixels);
							CommandList.TransitionTextures(std::array{Durin::FRHITextureTransition{
								Result.Texture, {Durin::ERHITextureAspect::Color, 0, 1, 0, 1},
								Durin::ERHIAccess::TransferWrite, Durin::ERHIAccess::GraphicsShaderRead}});
						}
						else
						{
							Result.Error =
								"Unable to create the rendered-asset thumbnail UI texture.";
						}
					}
					if (const std::shared_ptr<FAssetThumbnailAsyncState> State =
						WeakState.lock())
					{
						std::lock_guard Lock(State->Mutex);
						State->Uploads.push_back(std::move(Result));
					}
				});
		}

		auto ProcessGeneratedPixels(FAssetThumbnailJob& Job) -> void
		{
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			if (auto It = Entries.find(Request.KeyInput.Asset.AssetPath);
				It != Entries.end())
				It->second.bHasTransparency = Request.bHasTransparency;
			const std::shared_ptr<const FAssetThumbnailGeneratedPixels> Generated =
				Request.GeneratedPixels;
			if (!Generated)
			{
				Pipeline.CompleteLoad(Job,
					"The generated-thumbnail fast lane received rendered work.");
				return;
			}
			const uint64 ExpectedBytes = static_cast<uint64>(Generated->Width)
				* Generated->Height * 4;
			std::string Error;
			if (Generated->Width != Request.KeyInput.Output.Width
				|| Generated->Height != Request.KeyInput.Output.Height
				|| Generated->Pixels.size() != ExpectedBytes
				|| ExpectedBytes > Budgets.CpuPixelBudgetBytes)
				Error = "Generated thumbnail pixels violate the requested output or CPU budget.";
			if (Error.empty()
				&& Pipeline.CompleteGeneratedPixels(Job,
					Generated->Pixels, Generated->Width, Generated->Height))
				QueueUpload(Request, Generated->Pixels,
					Generated->Width, Generated->Height);
			else if (!Error.empty())
				Pipeline.CompleteLoad(Job, Error);
		}

		auto DrainUploads() -> void
		{
			std::vector<FAssetThumbnailUploadResult> Results;
			{
				std::lock_guard Lock(AsyncState->Mutex);
				Results.swap(AsyncState->Uploads);
			}
			for (FAssetThumbnailUploadResult& Result : Results)
			{
				auto It = Entries.find(Result.AssetPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial) continue;
				FEntry& Entry = It->second;
				Entry.bUploading = false;
				const FThumbnailRendererHandle Renderer =
					Manager.Find(Result.AssetClassName);
				if (Result.Cancellation.IsCancelled()
					|| Renderer.Generation != Result.RendererGeneration)
					continue;
				if (Result.Texture == nullptr || !Mona::GetActiveUIBackend())
				{
					Entry.Diagnostic = Result.Error.empty()
						? "The UI backend is unavailable for rendered-asset thumbnails."
						: std::move(Result.Error);
					Entry.bUploadFailed = true;
					++UploadFailures;
					continue;
				}
				UnregisterTexture(Entry);
				Mona::GetActiveUIBackend()->RegisterTexture(Result.Texture);
				Entry.Texture = std::move(Result.Texture);
				Entry.Width = Result.Width;
				Entry.Height = Result.Height;
				Entry.Diagnostic.clear();
				Entry.bUploadFailed = false;
				++UploadsCompleted;
			}
		}

		auto FinishCapture() -> void
		{
			if (!CaptureJob || ScenePool == nullptr) return;
			if (!IsCurrent(CaptureJob->ScheduledJob.GenerationRequest))
			{
				ResetCapture();
				return;
			}
			auto Capture = ScenePool->PollCapture();
			if (Capture && !Capture->has_value()) return;

			FAssetThumbnailJob& Job = *CaptureJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			IThumbnailRendererSession* Session =
				Request.GetRenderedSession();
			if (!Capture)
			{
				Pipeline.CompleteRender(
					Job, Capture.error());
			}
			else if (auto Validation = Session ? Session->ValidatePreparedInput()
				: std::expected<void, std::string>(std::unexpected("The rendered-thumbnail renderer was removed.")); !Validation)
			{
				Pipeline.CompleteRender(Job, Validation.error().empty()
					? "The rendered-thumbnail input is no longer valid." : Validation.error());
			}
			else if (Pipeline.CompleteRender(
					Job)
				&& Pipeline.CompleteReadback(
					Job)
				&& Pipeline.CompletePixels(
					Job,
					**Capture,
					Request.KeyInput.Output.Width,
					Request.KeyInput.Output.Height,
					{},
					[Session]() {
						auto Validation = Session->ValidatePreparedInput();
						if (Validation) return std::string{};
						// CompletePixels uses an empty diagnostic to indicate success.
						return Validation.error().empty()
							? std::string("The rendered-thumbnail input is no longer valid.")
							: std::move(Validation.error());
					}))
			{
				QueueUpload(
					Request,
					std::move(**Capture),
					Request.KeyInput.Output.Width,
					Request.KeyInput.Output.Height);
			}
			ResetCapture();
		}

		auto ParkPreparing(
			const FThumbnailRendererSessionUpdate& Update) -> void
		{
			if (!PreparingJob) return;
			if (Budgets.MaximumParkedRenderedJobs == 0
				|| ParkedJobs.size() >= Budgets.MaximumParkedRenderedJobs)
			{
				Pipeline.BeginRender(
					*PreparingJob, false,
					"The rendered-thumbnail parked-resource budget is exhausted.");
				ResetPreparing();
				return;
			}
			if (!Pipeline.BeginRender(
					*PreparingJob, false))
			{
				Pipeline.Cancel(*PreparingJob);
				ResetPreparing();
				return;
			}
			ParkedJobs.push_back({
				.Job = std::move(*PreparingJob),
				.LastUpdate = Update,
				.FirstWaitFrame = FrameNumber,
				.NextPollFrame = FrameNumber
					+ std::max<uint32>(1, Budgets.ResourcePollIntervalFrames),
				.bReady = Update.State == EThumbnailRendererSessionState::ReadyToRender});
			PreparingJob.reset();
			PeakParkedResourceWaits = std::max<uint64>(
				PeakParkedResourceWaits, ParkedJobs.size());
		}

		auto HandlePreparedUpdate(
			const FThumbnailRendererSessionUpdate& Update) -> void
		{
			if (!PreparingJob) return;
			FAssetThumbnailJob& Job = *PreparingJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			IThumbnailRendererSession* Session =
				Request.GetRenderedSession();
			if (Session == nullptr || !IsCurrent(Request))
			{
				ResetPreparing();
				return;
			}
			if (Update.State == EThumbnailRendererSessionState::Failed)
			{
				Pipeline.BeginRender(
					Job,
					false,
					Update.Diagnostic);
				ResetPreparing();
				return;
			}
			const bool bReady =
				Update.State == EThumbnailRendererSessionState::ReadyToRender;
			if (!bReady || CaptureJob)
			{
				ParkPreparing(Update);
				return;
			}
			if (!Pipeline.BeginRender(
					Job,
					true))
				return;
			std::string Error;
			if (!EnsureScene(Request.KeyInput.Output))
			{
				Error = ScenePool != nullptr
					? ScenePool->GetDiagnostic()
					: "The rendered-thumbnail scene is unavailable.";
			}
			else if (auto Prepared = Session->PreparePreview(*ScenePool); !Prepared)
			{
				Error = Prepared.error().empty()
					? "The renderer could not prepare its preview." : std::move(Prepared.error());
			}
			else
			{
				++PreviewSceneAssignments;
				if (auto Capture = Session->ValidatePreparedInput().and_then(
					[this] { return ScenePool->BeginCapture(); }); !Capture)
				{
					Error = Capture.error().empty()
						? "The preview capture could not start." : std::move(Capture.error());
				}
				else
				{
					CaptureJob = std::move(PreparingJob);
					PreparingJob.reset();
					return;
				}
			}
			Pipeline.CompleteRender(
				Job, Error);
			if (ScenePool) ScenePool->Reset();
			ResetPreparing();
		}

		auto PollPreparing() -> void
		{
			if (!PreparingJob) return;
			IThumbnailRendererSession* Session =
				PreparingJob->ScheduledJob.GenerationRequest.GetRenderedSession();
			if (Session == nullptr)
			{
				ResetPreparing();
				return;
			}
			HandlePreparedUpdate(Session->PollResources());
		}

		auto PollParkedJobs() -> void
		{
			const uint64 PollInterval =
				std::max<uint32>(1, Budgets.ResourcePollIntervalFrames);
			for (size_t Index = 0; Index < ParkedJobs.size();)
			{
				FParkedJob& Parked = ParkedJobs[Index];
				FAssetThumbnailGenerationRequest& Request =
					Parked.Job.ScheduledJob.GenerationRequest;
				IThumbnailRendererSession* Session =
					Request.GetRenderedSession();
				const bool bTimedOut = !Parked.bReady
					&& Budgets.MaximumResourceWaitFrames != 0
					&& FrameNumber - Parked.FirstWaitFrame
						>= Budgets.MaximumResourceWaitFrames;
				bool bRelease = Session == nullptr || !IsCurrent(Request);
				if (!bRelease && bTimedOut)
				{
					Pipeline.BeginRender(
						Parked.Job, false,
						"Timed out waiting for rendered-thumbnail resources.");
					++ResourceWaitTimeouts;
					bRelease = true;
				}
				if (!bRelease && !Parked.bReady
					&& FrameNumber >= Parked.NextPollFrame)
				{
					Parked.LastUpdate = Session->PollResources();
					Parked.NextPollFrame = FrameNumber + PollInterval;
					if (Parked.LastUpdate.State
						== EThumbnailRendererSessionState::Failed)
					{
						Pipeline.BeginRender(
							Parked.Job, false,
							Parked.LastUpdate.Diagnostic);
						bRelease = true;
					}
					else if (Parked.LastUpdate.State
						== EThumbnailRendererSessionState::ReadyToRender)
					{
						Parked.bReady = true;
					}
					else if (!Pipeline.BeginRender(
							Parked.Job, false))
					{
						Pipeline.Cancel(Parked.Job);
						bRelease = true;
					}
				}
				if (bRelease)
				{
					ReleaseJob(Parked.Job);
					ParkedJobs.erase(ParkedJobs.begin()
						+ static_cast<ptrdiff_t>(Index));
					continue;
				}
				++Index;
			}
		}

		auto ActivateReadyParkedJob() -> void
		{
			if (PreparingJob || CaptureJob || ParkedJobs.empty()) return;
			auto IsVisible = [this](const FParkedJob& Parked) {
				const auto It = Entries.find(
					Parked.Job.ScheduledJob.GenerationRequest.KeyInput.Asset.AssetPath);
				return It != Entries.end() && It->second.bVisible;
			};
			auto Selected = std::ranges::find_if(
				ParkedJobs,
				[&](const FParkedJob& Parked) {
					return Parked.bReady && IsVisible(Parked);
				});
			if (Selected == ParkedJobs.end())
				Selected = std::ranges::find_if(
					ParkedJobs,
					[](const FParkedJob& Parked) { return Parked.bReady; });
			if (Selected == ParkedJobs.end()) return;
			FThumbnailRendererSessionUpdate Update = Selected->LastUpdate;
			PreparingJob = std::move(Selected->Job);
			ParkedJobs.erase(Selected);
			HandlePreparedUpdate(Update);
		}

		auto StartNext(bool bGeneratedPixelsOnly = false) -> bool
		{
			FAssetThumbnailStartResult Start = bGeneratedPixelsOnly
				? Pipeline.StartNextGeneratedPixelsDetailed()
				: Pipeline.StartNextDetailed();
			if (Start.WarmJob)
			{
				FAssetThumbnailScheduledRequest& WarmJob = *Start.WarmJob;
				const FTopLevelAssetPath& Path =
					WarmJob.GenerationRequest.KeyInput.Asset.AssetPath;
				if (auto It = Entries.find(Path); It != Entries.end())
					It->second.bHasTransparency =
						WarmJob.GenerationRequest.bHasTransparency;
				QueueUpload(WarmJob.GenerationRequest, std::move(Start.Pixels),
					WarmJob.GenerationRequest.KeyInput.Output.Width,
					WarmJob.GenerationRequest.KeyInput.Output.Height);
				return true;
			}
			if (!Start.ColdJob) return Start.bProgressed;
			if (bGeneratedPixelsOnly)
			{
				FAssetThumbnailJob Job = std::move(*Start.ColdJob);
				ProcessGeneratedPixels(Job);
				return true;
			}
			PreparingJob = std::move(Start.ColdJob);
			FAssetThumbnailJob& Job = *PreparingJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			if (auto It = Entries.find(Request.KeyInput.Asset.AssetPath);
				It != Entries.end())
				It->second.bHasTransparency = Request.bHasTransparency;
			if (Request.GeneratedPixels)
			{
				ProcessGeneratedPixels(Job);
				ResetPreparing();
				return true;
			}
			auto Session = Request.BeginRenderedSession();
			if (!Session)
			{
				Pipeline.CompleteLoad(Job, Session.error());
				ResetPreparing();
				return true;
			}
			const FThumbnailRendererSessionUpdate Update = (*Session)->Load();
			if (!Pipeline.CompleteLoad(
					Job, Update.Diagnostic))
			{
				ResetPreparing();
				return true;
			}
			PollPreparing();
			return true;
		}

		auto EvictToBudget() -> void
		{
			std::vector<FThumbnailBudgetEntry> BudgetEntries;
			BudgetEntries.reserve(Entries.size());
			for (const auto& [Path, Entry] : Entries)
			{
				BudgetEntries.push_back({
					.Key = Path.ToString(),
					.Bytes = Entry.Texture
						? static_cast<uint64>(Entry.Width) * Entry.Height * 4
						: 0,
					.LastUsed = Entry.LastUsedFrame,
					.bPinned = Entry.bVisible || Entry.ReferencerCount != 0});
			}
			for (const std::string& Key : SelectThumbnailBudgetEvictions(
					BudgetEntries, Budgets.GpuTextureBudgetBytes))
			{
				FTopLevelAssetPath Path;
				if (!FTopLevelAssetPath::TryCreate(Key, Path)) continue;
				if (auto It = Entries.find(Path); It != Entries.end())
				{
					UnregisterTexture(It->second);
					Entries.erase(It);
					Scheduler.Cancel(Path);
					++GpuEvictions;
				}
			}

			BudgetEntries.clear();
			BudgetEntries.reserve(Entries.size());
			for (const auto& [Path, Entry] : Entries)
			{
				const EAssetThumbnailState State = Scheduler.Find(Path).State;
				const bool bActive = Entry.bUploading
					|| State == EAssetThumbnailState::Queued
					|| State == EAssetThumbnailState::Loading
					|| State == EAssetThumbnailState::WaitingForResources
					|| State == EAssetThumbnailState::Rendering
					|| State == EAssetThumbnailState::Readback
					|| State == EAssetThumbnailState::Encoding
					|| State == EAssetThumbnailState::Uploading;
				BudgetEntries.push_back({
					.Key = Path.ToString(),
					.Bytes = 1,
					.LastUsed = Entry.LastUsedFrame,
					.bPinned = Entry.bVisible || Entry.ReferencerCount != 0 || bActive});
			}
			for (const std::string& Key : SelectThumbnailBudgetEvictions(
					BudgetEntries, Budgets.MaximumRetainedEntries))
			{
				FTopLevelAssetPath Path;
				if (!FTopLevelAssetPath::TryCreate(Key, Path)) continue;
				if (auto It = Entries.find(Path); It != Entries.end())
				{
					const bool bHadTexture = It->second.Texture != nullptr;
					UnregisterTexture(It->second);
					Entries.erase(It);
					Scheduler.Cancel(Path);
					GpuEvictions += bHadTexture ? 1u : 0u;
				}
			}
		}
	};

	FAssetThumbnailPool::FAssetThumbnailPool(
		FAssetThumbnailBudgets Budgets,
		FAssetThumbnailPoolStorageSettings StoreSettings)
		: FAssetThumbnailPool(
			GetDefaultThumbnailManager(),
			Budgets,
			std::move(StoreSettings))
	{
	}

	FAssetThumbnailPool::FAssetThumbnailPool(
		DThumbnailManager& Manager,
		FAssetThumbnailBudgets Budgets,
		FAssetThumbnailPoolStorageSettings StoreSettings)
		: Impl(std::make_unique<FImpl>(
			Manager, Budgets, std::move(StoreSettings)))
	{
	}

	FAssetThumbnailPool::~FAssetThumbnailPool()
	{
		Clear();
		Impl->ScenePool.reset();
		Impl->AsyncState.reset();
	}

	auto FAssetThumbnailPool::BeginFrame() -> void
	{
		++Impl->FrameNumber;
		Impl->Scheduler.BeginFrame();
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
		Impl->DrainUploads();
		Impl->FinishCapture();
		Impl->Pipeline.BeginFrame();
		if (Impl->Budgets.bPreGenerateOnAssetChanges)
		{
			const FContentChangeBatch Changes = CaptureAssetCatalogChanges(Impl->CatalogRevision);
			Impl->CatalogRevision = Changes.ToRevision;
			// A full refresh may describe an entire project mount. Never load all
			// assets merely because a project opened or the journal rolled over.
			if (Changes.bFullRefresh) Impl->BackgroundPackages.clear();
			else for (const FContentChange& Change : Changes.Changes)
			{
				if (Change.bDirectory || Change.NewAssetPath.empty()) continue;
				if (std::ranges::contains(Impl->BackgroundPackages, Change.NewAssetPath)) continue;
				if (Impl->BackgroundPackages.size() >= Impl->Budgets.MaximumQueuedJobs) break;
				Impl->BackgroundPackages.push_back(Change.NewAssetPath);
			}
		}
	}

	auto FAssetThumbnailPool::Prefetch(const FAssetThumbnailPackageFingerprint& Asset) -> void
	{
		Request(Asset, EAssetThumbnailPriority::Prefetch);
		if (const auto It = Impl->Entries.find(Asset.AssetPath); It != Impl->Entries.end())
			It->second.bBackgroundRequested = true;
	}

	auto FAssetThumbnailPool::Request(
		const FAssetThumbnailPackageFingerprint& Asset,
		EAssetThumbnailPriority Priority) -> void
	{
		if (!Asset.AssetPath.IsValid()) return;
		auto [It, bInserted] = Impl->Entries.try_emplace(Asset.AssetPath);
		FImpl::FEntry& Entry = It->second;
		if (bInserted)
			Entry.Fingerprint = Asset;
		else if (Entry.Fingerprint != Asset)
		{
			const uint64 NextSerial = Entry.Serial + 1;
			const uint32 ReferencerCount = Entry.ReferencerCount;
			Impl->Scheduler.Cancel(Asset.AssetPath);
			Impl->UnregisterTexture(Entry);
			Entry = {};
			Entry.Fingerprint = Asset;
			Entry.Serial = NextSerial;
			Entry.ReferencerCount = ReferencerCount;
		}
		Entry.bVisible |= Priority == EAssetThumbnailPriority::Visible;
		Entry.LastUsedFrame = Impl->FrameNumber;
		if (Entry.Texture)
		{
			Entry.Diagnostic.clear();
			return;
		}
		const auto Admission = Impl->Scheduler.Request({.Asset = Asset, .Priority = Priority, .RequestSerial = Entry.Serial});
		// Queue pressure is retried by subsequent visible requests. Old requests
		// and shutdown need no diagnostic; unsupported assets retain a placeholder.
		if (Admission == EThumbnailRequestStatus::Unsupported)
			Entry.Diagnostic = "No thumbnail renderer is registered for this asset class.";
		else if (!Entry.bUploadFailed)
			Entry.Diagnostic.clear();
	}

	auto FAssetThumbnailPool::Find(const FTopLevelAssetPath& AssetPath) const
		-> FAssetThumbnailView
	{
		FAssetThumbnailView View = Impl->Scheduler.Find(AssetPath);
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return View;
		const FImpl::FEntry& Entry = It->second;
		View.Texture = Entry.Texture;
		View.Width = Entry.Width;
		View.Height = Entry.Height;
		View.bHasTransparency = Entry.bHasTransparency;
		View.bShowTransparencyGrid = false;
		if (Entry.Texture)
			View.State = EAssetThumbnailState::Ready;
		else if (View.State == EAssetThumbnailState::Ready && Entry.bUploading)
			View.State = EAssetThumbnailState::Uploading;
		else if (View.State == EAssetThumbnailState::Ready && Entry.bUploadFailed)
			View.State = EAssetThumbnailState::Failed;
		if (!Entry.Diagnostic.empty()) View.Diagnostic = Entry.Diagnostic;
		return View;
	}

	auto FAssetThumbnailPool::AddReferencer(const FTopLevelAssetPath& AssetPath) -> void
	{
		if (!AssetPath.IsValid()) return;
		auto [It, Inserted] = Impl->Entries.try_emplace(AssetPath);
		(void)Inserted;
		++It->second.ReferencerCount;
	}

	auto FAssetThumbnailPool::RemoveReferencer(const FTopLevelAssetPath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end() || It->second.ReferencerCount == 0) return;
		--It->second.ReferencerCount;
		if (It->second.ReferencerCount != 0) return;
		if (It->second.bBackgroundRequested) return;
		// Directory navigation releases cold requests but retains cached pixels.
		Impl->Scheduler.Cancel(AssetPath);
		if (Impl->PreparingJob
			&& Impl->PreparingJob->ScheduledJob.GenerationRequest.KeyInput.Asset.AssetPath == AssetPath)
			Impl->ResetPreparing();
		if (Impl->CaptureJob
			&& Impl->CaptureJob->ScheduledJob.GenerationRequest.KeyInput.Asset.AssetPath == AssetPath)
			Impl->ResetCapture();
		std::erase_if(Impl->ParkedJobs, [&](FImpl::FParkedJob& Parked) {
			if (Parked.Job.ScheduledJob.GenerationRequest.KeyInput.Asset.AssetPath != AssetPath) return false;
			Impl->ReleaseJob(Parked.Job);
			return true;
		});
		++It->second.Serial;
		It->second.bUploading = false;
	}

	auto FAssetThumbnailPool::Refresh(const FTopLevelAssetPath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return;
		const uint32 ReferencerCount = It->second.ReferencerCount;
		Impl->Scheduler.Cancel(AssetPath);
		Impl->UnregisterTexture(It->second);
		It->second.Texture = nullptr;
		It->second.bUploading = false;
		It->second.bUploadFailed = false;
		It->second.Diagnostic.clear();
		++It->second.Serial;
		It->second.ReferencerCount = ReferencerCount;
	}

	auto FAssetThumbnailPool::EndFrame() -> void
	{
		const auto StartTime = std::chrono::steady_clock::now();
		if (Impl->PreparingJob)
			Impl->PollPreparing();
		Impl->PollParkedJobs();
		if (!Impl->PreparingJob)
			Impl->ActivateReadyParkedJob();
		for (uint32 Admission = 0; Admission < Impl->Budgets.MaximumAdmissionsPerFrame; ++Admission)
		{
			if (std::chrono::duration<double>(std::chrono::steady_clock::now() - StartTime).count()
				>= Impl->Budgets.FrameTimeAllowanceSeconds) break;
			if (!Impl->BackgroundPackages.empty())
			{
				std::string Path = std::move(Impl->BackgroundPackages.front());
				Impl->BackgroundPackages.pop_front();
				FPackagePath PackagePath;
				if (FPackagePath::TryCreate(Path, PackagePath))
				{
					const auto Package = FindAssetExact(PackagePath);
					if (Package) for (const FTopLevelAssetData& Asset : Package->TopLevelAssets)
					{
						if (Asset.IsRedirector() || !Impl->Manager.Find(Asset.AssetClassName)) continue;
						Prefetch({.AssetPath = Asset.AssetPath, .PackagePath = PackagePath,
							.AssetClassName = Asset.AssetClassName, .PackageFormatVersion = Package->FormatVersion,
							.FileSize = static_cast<uint64>(Package->FileSize), .LastWriteTimeTicks = Package->LastWriteTimeTicks});
					}
				}
			}
			// Leave a bounded slot for the next cold result; pressure defers work
			// rather than turning otherwise valid assets into permanent failures.
			const bool bPreparationFull = Impl->PreparingJob.has_value()
				|| Impl->ParkedJobs.size() >= Impl->Budgets.MaximumParkedRenderedJobs;
			if (!Impl->StartNext(bPreparationFull)) break;
		}
		Impl->EvictToBudget();
	}

	auto FAssetThumbnailPool::CancelPendingRequests() -> void
	{
		Impl->BackgroundPackages.clear();
		if (Impl->PreparingJob) Impl->Pipeline.Cancel(*Impl->PreparingJob);
		if (Impl->CaptureJob) Impl->Pipeline.Cancel(*Impl->CaptureJob);
		for (const FImpl::FParkedJob& Parked : Impl->ParkedJobs)
			Impl->Pipeline.Cancel(Parked.Job);
		Impl->ResetPreparing();
		Impl->ResetCapture();
		Impl->ResetParkedJobs();
		Impl->Scheduler.CancelAll();
		for (auto& [Path, Entry] : Impl->Entries)
		{
			++Entry.Serial;
			Entry.bUploading = false;
			Entry.bUploadFailed = false;
			Entry.Diagnostic.clear();
		}
	}

	auto FAssetThumbnailPool::Clear() -> void
	{
		CancelPendingRequests();
		for (auto& [Path, Entry] : Impl->Entries)
			Impl->UnregisterTexture(Entry);
		Impl->Entries.clear();
	}

	auto FAssetThumbnailPool::GetStats() const
		-> FAssetThumbnailPoolStats
	{
		uint64 LiveGpuTextures = 0;
		uint64 PinnedEntries = 0;
		uint64 Referencers = 0;
		for (const auto& [Path, Entry] : Impl->Entries)
		{
			LiveGpuTextures += Entry.Texture != nullptr ? 1u : 0u;
			PinnedEntries += Entry.ReferencerCount != 0 ? 1u : 0u;
			Referencers += Entry.ReferencerCount;
		}
		return {
			.Generation = Impl->Pipeline.GetStats(),
			.PreviewSceneCreations = Impl->PreviewSceneCreations,
			.PreviewSceneAssignments = Impl->PreviewSceneAssignments,
			.UploadsQueued = Impl->UploadsQueued,
			.UploadsCompleted = Impl->UploadsCompleted,
			.UploadFailures = Impl->UploadFailures,
			.GpuEvictions = Impl->GpuEvictions,
			.LiveGpuTextures = LiveGpuTextures,
			.ParkedResourceWaits = Impl->ParkedJobs.size(),
			.PeakParkedResourceWaits = Impl->PeakParkedResourceWaits,
			.ResourceWaitTimeouts = Impl->ResourceWaitTimeouts,
			.QueuedJobs = Impl->Scheduler.NumQueued(),
			.RetainedEntries = Impl->Entries.size(),
			.PinnedEntries = PinnedEntries,
			.Referencers = Referencers,
			.bHasActiveJob = Impl->PreparingJob.has_value() || Impl->CaptureJob.has_value(),
			.bHasPreviewScene = Impl->ScenePool != nullptr};
	}
} // namespace Durin::Editor
