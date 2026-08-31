#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/AssetThumbnailGeneration.h"

#include "DynamicRHI.h"
#include "Image/ImageDecoder.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

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
		std::optional<FAssetThumbnailJob> ActiveJob;
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

		FImpl(
			DThumbnailManager& InService,
			FAssetThumbnailBudgets InBudgets,
			FAssetThumbnailPoolStorageSettings StoreSettings)
			: Manager(InService)
			, Budgets(InBudgets)
			, Scheduler(Manager, Budgets)
			, Pipeline(Scheduler, std::move(StoreSettings), Budgets)
		{
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

		auto ResetActive() -> void
		{
			if (!ActiveJob) return;
			ReleaseJob(*ActiveJob);
			if (ScenePool) ScenePool->Reset();
			ActiveJob.reset();
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
			FByteArray Pixels,
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
			auto SharedPixels = std::make_shared<FByteArray>(std::move(Pixels));
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
							GDynamicRHI->RHIUpdateTexture2D(
								CommandList,
								Result.Texture,
								0,
								0,
								Region,
								Width * 4,
								*SharedPixels);
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
				Pipeline.CompleteLoad(Job, 0,
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
				&& Pipeline.CompleteGeneratedPixels(Job, Generated->AssetRevision,
					Generated->Pixels, Generated->Width, Generated->Height))
				QueueUpload(Request, Generated->Pixels,
					Generated->Width, Generated->Height);
			else if (!Error.empty())
				Pipeline.CompleteLoad(Job, Generated->AssetRevision, Error);
		}

		auto DecodeAndQueueUpload(
			const FAssetThumbnailGenerationRequest& Request,
			std::span<const std::byte> EncodedBytes) -> bool
		{
			const uint64 ExpectedPixels =
				static_cast<uint64>(Request.KeyInput.Output.Width)
					* Request.KeyInput.Output.Height;
			uint64 ExpectedBytes = 0;
			Image::FDecodedImage Image;
			std::string Error;
			bool bDecoded = false;
			if (ExpectedPixels == 0
				|| ExpectedPixels > Budgets.CpuPixelBudgetBytes / 4)
			{
				Error = "The cached thumbnail exceeds the configured CPU pixel budget.";
			}
			else
			{
				ExpectedBytes = ExpectedPixels * 4;
				bDecoded = Image::DecodeImageFromMemory(
					EncodedBytes,
					Image,
					Error,
					{.MaximumEncodedBytes = EncodedBytes.size(),
					 .MaximumDecodedPixels = ExpectedPixels});
			}
			if (!bDecoded && Error.empty())
			{
				Error = "The cached thumbnail could not be decoded.";
			}
			else if (bDecoded && (Image.Width != Request.KeyInput.Output.Width
					|| Image.Height != Request.KeyInput.Output.Height
					|| Image.Pixels.size() != ExpectedBytes))
			{
				Error = "The cached thumbnail dimensions do not match its fixed output contract.";
			}
			if (!Error.empty())
			{
				if (auto It = Entries.find(Request.KeyInput.Asset.AssetPath);
					It != Entries.end())
				{
					It->second.Diagnostic = std::move(Error);
					It->second.bUploadFailed = true;
				}
				return false;
			}
			QueueUpload(
				Request, std::move(Image.Pixels), Image.Width, Image.Height);
			return true;
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
			if (!ActiveJob || ScenePool == nullptr) return;
			FByteArray Pixels;
			std::string Error;
			const EThumbnailCaptureState State =
				ScenePool->PollCapture(Pixels, Error);
			if (State == EThumbnailCaptureState::Rendering
				|| State == EThumbnailCaptureState::Idle)
				return;

			FAssetThumbnailJob& Job = *ActiveJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			IThumbnailRendererSession* Session =
				Request.GetRenderedSession();
			if (State == EThumbnailCaptureState::Failed)
			{
				Pipeline.CompleteRender(
					Job, Job.AssetRevision, Job.ResourceRevision, Error);
			}
			else if (Session == nullptr
				|| !Session->ValidateRevisions(
					Job.AssetRevision, Job.ResourceRevision, Error))
			{
				if (Error.empty()) Error = "The rendered-thumbnail renderer was removed.";
				Pipeline.CompleteRender(
					Job, Job.AssetRevision, Job.ResourceRevision, Error);
			}
			else if (Pipeline.CompleteRender(
					Job, Job.AssetRevision, Job.ResourceRevision)
				&& Pipeline.CompleteReadback(
					Job, Job.AssetRevision, Job.ResourceRevision)
				&& Pipeline.CompletePixels(
					Job,
					Job.AssetRevision,
					Job.ResourceRevision,
					Pixels,
					Request.KeyInput.Output.Width,
					Request.KeyInput.Output.Height,
					{},
					[Session,
						ExpectedAssetRevision = Job.AssetRevision,
						ExpectedResourceRevision = Job.ResourceRevision]() {
						std::string ValidationError;
						Session->ValidateRevisions(
							ExpectedAssetRevision,
							ExpectedResourceRevision,
							ValidationError);
						return ValidationError;
					}))
			{
				QueueUpload(
					Request,
					std::move(Pixels),
					Request.KeyInput.Output.Width,
					Request.KeyInput.Output.Height);
			}
			ResetActive();
		}

		auto ParkActive(
			const FThumbnailRendererSessionUpdate& Update) -> void
		{
			if (!ActiveJob) return;
			if (Budgets.MaximumParkedRenderedJobs == 0
				|| ParkedJobs.size() >= Budgets.MaximumParkedRenderedJobs)
			{
				Pipeline.BeginRender(
					*ActiveJob, false, Update.AssetRevision, 0,
					"The rendered-thumbnail parked-resource budget is exhausted.");
				ResetActive();
				return;
			}
			if (!Pipeline.BeginRender(
					*ActiveJob, false, Update.AssetRevision, 0))
			{
				Pipeline.Cancel(*ActiveJob);
				ResetActive();
				return;
			}
			ParkedJobs.push_back({
				.Job = std::move(*ActiveJob),
				.LastUpdate = Update,
				.FirstWaitFrame = FrameNumber,
				.NextPollFrame = FrameNumber
					+ std::max<uint32>(1, Budgets.ResourcePollIntervalFrames)});
			ActiveJob.reset();
			PeakParkedResourceWaits = std::max<uint64>(
				PeakParkedResourceWaits, ParkedJobs.size());
		}

		auto HandleActiveUpdate(
			const FThumbnailRendererSessionUpdate& Update) -> void
		{
			if (!ActiveJob) return;
			FAssetThumbnailJob& Job = *ActiveJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			IThumbnailRendererSession* Session =
				Request.GetRenderedSession();
			if (Session == nullptr || !IsCurrent(Request))
			{
				ResetActive();
				return;
			}
			if (Update.State == EThumbnailRendererSessionState::Failed)
			{
				Pipeline.BeginRender(
					Job,
					false,
					Update.AssetRevision,
					Update.ResourceRevision,
					Update.Diagnostic);
				ResetActive();
				return;
			}
			const bool bReady =
				Update.State == EThumbnailRendererSessionState::ReadyToRender;
			if (!bReady)
			{
				ParkActive(Update);
				return;
			}
			if (!Pipeline.BeginRender(
					Job,
					true,
					Update.AssetRevision,
					Update.ResourceRevision))
				return;
			std::string Error;
			if (!EnsureScene(Request.KeyInput.Output))
			{
				Error = ScenePool != nullptr
					? ScenePool->GetDiagnostic()
					: "The rendered-thumbnail scene is unavailable.";
			}
			else if (!Session->PreparePreview(*ScenePool, Error))
			{
				if (Error.empty()) Error = "The renderer could not prepare its preview.";
			}
			else
			{
				++PreviewSceneAssignments;
				if (!Session->ValidateRevisions(
						Job.AssetRevision, Job.ResourceRevision, Error)
					|| !ScenePool->BeginCapture(Error))
				{
					if (Error.empty()) Error = "The preview capture could not start.";
				}
				else
				{
					return;
				}
			}
			Pipeline.CompleteRender(
				Job, Job.AssetRevision, Job.ResourceRevision, Error);
			ResetActive();
		}

		auto PollActive() -> void
		{
			if (!ActiveJob) return;
			IThumbnailRendererSession* Session =
				ActiveJob->ScheduledJob.GenerationRequest.GetRenderedSession();
			if (Session == nullptr)
			{
				ResetActive();
				return;
			}
			HandleActiveUpdate(Session->PollResources());
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
						Parked.Job, false, Parked.Job.AssetRevision, 0,
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
							Parked.LastUpdate.AssetRevision,
							Parked.LastUpdate.ResourceRevision,
							Parked.LastUpdate.Diagnostic);
						bRelease = true;
					}
					else if (Parked.LastUpdate.State
						== EThumbnailRendererSessionState::ReadyToRender)
					{
						Parked.bReady = true;
					}
					else if (!Pipeline.BeginRender(
							Parked.Job, false,
							Parked.LastUpdate.AssetRevision, 0))
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
			if (ActiveJob || ParkedJobs.empty()) return;
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
			ActiveJob = std::move(Selected->Job);
			ParkedJobs.erase(Selected);
			HandleActiveUpdate(Update);
		}

		auto StartNext(bool bGeneratedPixelsOnly = false) -> void
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
				if (!DecodeAndQueueUpload(
						WarmJob.GenerationRequest, Start.EncodedBytes))
				{
					Pipeline.InvalidatePersistentObject(WarmJob.CacheKey);
					Pipeline.RecordRetry();
					Scheduler.Cancel(Path);
					std::string Error;
					if (Scheduler.Request({
							.Asset = WarmJob.GenerationRequest.KeyInput.Asset,
							.Priority = WarmJob.Priority,
							.RequestSerial =
								WarmJob.GenerationRequest.RequestSerial},
						Error))
					{
						if (auto It = Entries.find(Path); It != Entries.end())
						{
							It->second.Diagnostic.clear();
							It->second.bUploadFailed = false;
						}
					}
				}
				return;
			}
			if (!Start.ColdJob) return;
			if (bGeneratedPixelsOnly)
			{
				FAssetThumbnailJob Job = std::move(*Start.ColdJob);
				ProcessGeneratedPixels(Job);
				return;
			}
			ActiveJob = std::move(Start.ColdJob);
			FAssetThumbnailJob& Job = *ActiveJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			if (auto It = Entries.find(Request.KeyInput.Asset.AssetPath);
				It != Entries.end())
				It->second.bHasTransparency = Request.bHasTransparency;
			if (Request.GeneratedPixels)
			{
				ProcessGeneratedPixels(Job);
				ResetActive();
				return;
			}
			std::string Error;
			IThumbnailRendererSession* Session =
				Request.BeginRenderedSession(Error);
			if (Session == nullptr)
			{
				Pipeline.CompleteLoad(Job, 0, Error);
				ResetActive();
				return;
			}
			const FThumbnailRendererSessionUpdate Update = Session->Load();
			if (!Pipeline.CompleteLoad(
					Job, Update.AssetRevision, Update.Diagnostic))
			{
				ResetActive();
				return;
			}
			PollActive();
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
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
		Impl->DrainUploads();
		Impl->FinishCapture();
		Impl->Pipeline.BeginFrame();
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
		std::string Error;
		if (!Impl->Scheduler.Request({
				.Asset = Asset,
				.Priority = Priority,
				.RequestSerial = Entry.Serial},
			Error))
		{
			Entry.Diagnostic = std::move(Error);
		}
		else if (!Entry.bUploadFailed)
		{
			Entry.Diagnostic.clear();
		}
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
		if (Impl->ActiveJob)
			Impl->PollActive();
		Impl->PollParkedJobs();
		if (!Impl->ActiveJob)
			Impl->ActivateReadyParkedJob();
		if (!Impl->ActiveJob)
			Impl->StartNext();
		if (Impl->ActiveJob)
			Impl->StartNext(true);
		Impl->EvictToBudget();
	}

	auto FAssetThumbnailPool::CancelPendingRequests() -> void
	{
		if (Impl->ActiveJob) Impl->Pipeline.Cancel(*Impl->ActiveJob);
		for (const FImpl::FParkedJob& Parked : Impl->ParkedJobs)
			Impl->Pipeline.Cancel(Parked.Job);
		Impl->ResetActive();
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
			.bHasActiveJob = Impl->ActiveJob.has_value(),
			.bHasPreviewScene = Impl->ScenePool != nullptr};
	}
} // namespace Durin::Editor
