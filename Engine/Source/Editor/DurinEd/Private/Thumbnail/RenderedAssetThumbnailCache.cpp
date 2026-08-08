#include "Thumbnail/RenderedAssetThumbnailCache.h"

#include "DynamicRHI.h"
#include "ImageDecoder.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		struct FRenderedThumbnailUploadResult
		{
			FAssetPath AssetPath;
			std::string AssetClassName;
			FAssetThumbnailCancellation Cancellation;
			uint64 ProviderGeneration = 0;
			uint64 Serial = 0;
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Error;
		};

		struct FRenderedThumbnailAsyncState
		{
			std::mutex Mutex;
			std::vector<FRenderedThumbnailUploadResult> Uploads;
		};
	} // namespace

	FRenderedAssetThumbnailService::FRenderedAssetThumbnailService() = default;

	FRenderedAssetThumbnailService::~FRenderedAssetThumbnailService()
	{
		Shutdown();
	}

	auto FRenderedAssetThumbnailService::RegisterScoped(
		std::unique_ptr<IAssetThumbnailProvider> Provider,
		std::string& OutError) -> FAssetThumbnailProviderRegistrationHandle
	{
		return Registry.RegisterScoped(std::move(Provider), OutError);
	}

	auto FRenderedAssetThumbnailService::Find(std::string_view AssetClassName) const
		-> FAssetThumbnailProviderHandle
	{
		return Registry.Find(AssetClassName);
	}

	auto FRenderedAssetThumbnailService::CaptureSourceImage(
		const Asset::FAssetData& Asset,
		FAssetThumbnailSourceImage& OutSource,
		std::string& OutError) const -> bool
	{
		return Registry.CaptureSourceImage(Asset, OutSource, OutError);
	}

	auto FRenderedAssetThumbnailService::UsesSourceImage(
		std::string_view AssetClassName) const -> bool
	{
		return Registry.UsesSourceImage(AssetClassName);
	}

	auto FRenderedAssetThumbnailService::Shutdown() -> void
	{
		Registry.Shutdown();
	}

	auto GetDefaultRenderedAssetThumbnailService()
		-> FRenderedAssetThumbnailService&
	{
		static FRenderedAssetThumbnailService Service;
		return Service;
	}

	struct FRenderedAssetThumbnailCache::FImpl
	{
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
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Diagnostic;
		};

		FRenderedAssetThumbnailService& Service;
		FAssetThumbnailBudgets Budgets;
		FAssetThumbnailScheduler Scheduler;
		FRenderedAssetThumbnailPipeline Pipeline;
		std::unique_ptr<FRenderedAssetThumbnailPreviewScenePool> ScenePool;
		std::optional<FAssetThumbnailOutputSettings> SceneOutput;
		std::unordered_map<FAssetPath, FEntry> Entries;
		std::shared_ptr<FRenderedThumbnailAsyncState> AsyncState =
			std::make_shared<FRenderedThumbnailAsyncState>();
		std::optional<FRenderedAssetThumbnailJob> ActiveJob;
		uint64 FrameNumber = 0;
		uint64 PreviewSceneCreations = 0;
		uint64 PreviewSceneAssignments = 0;
		uint64 UploadsQueued = 0;
		uint64 UploadsCompleted = 0;
		uint64 UploadFailures = 0;
		uint64 GpuEvictions = 0;

		FImpl(
			FRenderedAssetThumbnailService& InService,
			FAssetThumbnailBudgets InBudgets,
			FAssetThumbnailObjectStoreSettings StoreSettings)
			: Service(InService)
			, Budgets(InBudgets)
			, Scheduler(Service.Registry, Budgets)
			, Pipeline(Scheduler, std::move(StoreSettings), Budgets)
		{
		}

		auto IsCurrent(const FAssetThumbnailGenerationRequest& Request) const -> bool
		{
			const FAssetThumbnailProviderHandle Provider =
				Service.Find(Request.KeyInput.Asset.AssetClassName);
			return !Request.Cancellation.IsCancelled()
				&& Provider.Generation == Request.ProviderGeneration;
		}

		auto EnsureScene(const FAssetThumbnailOutputSettings& Output) -> bool
		{
			if (ScenePool != nullptr && SceneOutput && *SceneOutput == Output)
				return ScenePool->IsAvailable();
			ScenePool.reset();
			SceneOutput = Output;
			ScenePool = std::make_unique<FRenderedAssetThumbnailPreviewScenePool>(
				Output, Budgets);
			++PreviewSceneCreations;
			return ScenePool->IsAvailable();
		}

		auto ResetActive() -> void
		{
			if (!ActiveJob) return;
			FAssetThumbnailGenerationRequest& Request =
				ActiveJob->ScheduledJob.GenerationRequest;
			if (IRenderedAssetThumbnailGenerationSession* Session =
				Request.GetRenderedSession())
			{
				Session->ResetPreview();
			}
			if (ScenePool) ScenePool->Reset();
			Request.ReleaseRenderedSession();
			ActiveJob.reset();
		}

		auto UnregisterTexture(FEntry& Entry) -> void
		{
			if (Entry.Texture && Mona::GActiveUIBackend)
				Mona::GActiveUIBackend->UnregisterTexture(Entry.Texture);
			Entry.Texture = nullptr;
		}

		auto QueueUpload(
			const FAssetThumbnailGenerationRequest& Request,
			std::vector<uint8> Pixels,
			uint32 Width,
			uint32 Height) -> void
		{
			const FAssetPath& AssetPath = Request.KeyInput.Asset.VirtualPath;
			auto It = Entries.find(AssetPath);
			if (It == Entries.end() || It->second.Serial != Request.RequestSerial
				|| !IsCurrent(Request))
				return;
			It->second.bUploading = true;
			It->second.bUploadFailed = false;
			It->second.bHasTransparency = Request.bHasTransparency;
			++UploadsQueued;
			const std::weak_ptr<FRenderedThumbnailAsyncState> WeakState = AsyncState;
			auto SharedPixels = std::make_shared<std::vector<uint8>>(std::move(Pixels));
			const std::string AssetClassName = Request.KeyInput.Asset.AssetClassName;
			const FAssetThumbnailCancellation Cancellation = Request.Cancellation;
			const uint64 ProviderGeneration = Request.ProviderGeneration;
			const uint64 Serial = Request.RequestSerial;
			ENQUEUE_RENDER_COMMAND(UploadRenderedAssetThumbnail)(
				[WeakState,
					AssetPath,
					AssetClassName,
					Cancellation,
					ProviderGeneration,
					Serial,
					SharedPixels,
					Width,
					Height](FRHICommandListImmediate& CommandList) {
					FRenderedThumbnailUploadResult Result{
						.AssetPath = AssetPath,
						.AssetClassName = AssetClassName,
						.Cancellation = Cancellation,
						.ProviderGeneration = ProviderGeneration,
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
								SharedPixels->data());
						}
						else
						{
							Result.Error =
								"Unable to create the rendered-asset thumbnail UI texture.";
						}
					}
					if (const std::shared_ptr<FRenderedThumbnailAsyncState> State =
						WeakState.lock())
					{
						std::lock_guard Lock(State->Mutex);
						State->Uploads.push_back(std::move(Result));
					}
				});
		}

		auto DecodeAndQueueUpload(
			const FAssetThumbnailGenerationRequest& Request,
			std::span<const uint8> EncodedBytes) -> bool
		{
			Asset::FDecodedImage Image;
			std::string Error;
			if (!Asset::DecodeImageFromMemory(EncodedBytes, Image, Error))
			{
				if (auto It = Entries.find(Request.KeyInput.Asset.VirtualPath);
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
			std::vector<FRenderedThumbnailUploadResult> Results;
			{
				std::lock_guard Lock(AsyncState->Mutex);
				Results.swap(AsyncState->Uploads);
			}
			for (FRenderedThumbnailUploadResult& Result : Results)
			{
				auto It = Entries.find(Result.AssetPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial) continue;
				FEntry& Entry = It->second;
				Entry.bUploading = false;
				const FAssetThumbnailProviderHandle Provider =
					Service.Find(Result.AssetClassName);
				if (Result.Cancellation.IsCancelled()
					|| Provider.Generation != Result.ProviderGeneration)
					continue;
				if (Result.Texture == nullptr || !Mona::GActiveUIBackend)
				{
					Entry.Diagnostic = Result.Error.empty()
						? "The UI backend is unavailable for rendered-asset thumbnails."
						: std::move(Result.Error);
					Entry.bUploadFailed = true;
					++UploadFailures;
					continue;
				}
				UnregisterTexture(Entry);
				Mona::GActiveUIBackend->RegisterTexture(Result.Texture);
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
			std::vector<uint8> Pixels;
			std::string Error;
			const ERenderedAssetThumbnailCaptureState State =
				ScenePool->PollCapture(Pixels, Error);
			if (State == ERenderedAssetThumbnailCaptureState::Rendering
				|| State == ERenderedAssetThumbnailCaptureState::Idle)
				return;

			FRenderedAssetThumbnailJob& Job = *ActiveJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			IRenderedAssetThumbnailGenerationSession* Session =
				Request.GetRenderedSession();
			if (State == ERenderedAssetThumbnailCaptureState::Failed)
			{
				Pipeline.CompleteRender(
					Job, Job.AssetRevision, Job.ResourceRevision, Error);
			}
			else if (Session == nullptr
				|| !Session->ValidateRevisions(
					Job.AssetRevision, Job.ResourceRevision, Error))
			{
				if (Error.empty()) Error = "The rendered-thumbnail provider was removed.";
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

		auto PollActive() -> void
		{
			if (!ActiveJob) return;
			FRenderedAssetThumbnailJob& Job = *ActiveJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			IRenderedAssetThumbnailGenerationSession* Session =
				Request.GetRenderedSession();
			if (Session == nullptr || !IsCurrent(Request))
			{
				ResetActive();
				return;
			}
			FRenderedAssetThumbnailSessionUpdate Update = Session->PollResources();
			if (Update.State == ERenderedAssetThumbnailSessionState::Failed)
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
				Update.State == ERenderedAssetThumbnailSessionState::ReadyToRender;
			if (!Pipeline.BeginRender(
					Job,
					bReady,
					Update.AssetRevision,
					Update.ResourceRevision))
				return;
			if (!bReady) return;
			std::string Error;
			if (!EnsureScene(Request.KeyInput.Output))
			{
				Error = ScenePool != nullptr
					? ScenePool->GetDiagnostic()
					: "The rendered-thumbnail scene is unavailable.";
			}
			else if (!Session->PreparePreview(*ScenePool, Error))
			{
				if (Error.empty()) Error = "The provider could not prepare its preview.";
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

		auto StartNext() -> void
		{
			FRenderedAssetThumbnailStartResult Start = Pipeline.StartNextDetailed();
			if (Start.WarmJob)
			{
				FAssetThumbnailScheduledJob& WarmJob = *Start.WarmJob;
				const FAssetPath& Path =
					WarmJob.GenerationRequest.KeyInput.Asset.VirtualPath;
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
			ActiveJob = std::move(Start.ColdJob);
			FRenderedAssetThumbnailJob& Job = *ActiveJob;
			FAssetThumbnailGenerationRequest& Request =
				Job.ScheduledJob.GenerationRequest;
			if (auto It = Entries.find(Request.KeyInput.Asset.VirtualPath);
				It != Entries.end())
				It->second.bHasTransparency = Request.bHasTransparency;
			std::string Error;
			IRenderedAssetThumbnailGenerationSession* Session =
				Request.BeginRenderedSession(Error);
			if (Session == nullptr)
			{
				Pipeline.CompleteLoad(Job, 0, Error);
				ResetActive();
				return;
			}
			const FRenderedAssetThumbnailSessionUpdate Update = Session->Load();
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
			std::vector<FAssetThumbnailBudgetEntry> BudgetEntries;
			BudgetEntries.reserve(Entries.size());
			for (const auto& [Path, Entry] : Entries)
			{
				BudgetEntries.push_back({
					.Key = Path.ToString(),
					.Bytes = Entry.Texture
						? static_cast<uint64>(Entry.Width) * Entry.Height * 4
						: 0,
					.LastUsed = Entry.LastUsedFrame,
					.bPinned = Entry.bVisible});
			}
			for (const std::string& Key : SelectAssetThumbnailBudgetEvictions(
					BudgetEntries, Budgets.GpuTextureBudgetBytes))
			{
				FAssetPath Path;
				if (!FAssetPath::TryCreate(Key, Path)) continue;
				if (auto It = Entries.find(Path); It != Entries.end())
				{
					UnregisterTexture(It->second);
					Entries.erase(It);
					Scheduler.Cancel(Path);
					++GpuEvictions;
				}
			}
		}
	};

	FRenderedAssetThumbnailCache::FRenderedAssetThumbnailCache(
		FAssetThumbnailBudgets Budgets,
		FAssetThumbnailObjectStoreSettings StoreSettings)
		: FRenderedAssetThumbnailCache(
			GetDefaultRenderedAssetThumbnailService(),
			Budgets,
			std::move(StoreSettings))
	{
	}

	FRenderedAssetThumbnailCache::FRenderedAssetThumbnailCache(
		FRenderedAssetThumbnailService& Service,
		FAssetThumbnailBudgets Budgets,
		FAssetThumbnailObjectStoreSettings StoreSettings)
		: Impl(std::make_unique<FImpl>(
			Service, Budgets, std::move(StoreSettings)))
	{
	}

	FRenderedAssetThumbnailCache::~FRenderedAssetThumbnailCache()
	{
		Clear();
		Impl->ScenePool.reset();
		Impl->AsyncState.reset();
	}

	auto FRenderedAssetThumbnailCache::BeginFrame() -> void
	{
		++Impl->FrameNumber;
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
		Impl->DrainUploads();
		Impl->FinishCapture();
		Impl->Pipeline.BeginFrame();
	}

	auto FRenderedAssetThumbnailCache::Request(
		const FAssetThumbnailPackageFingerprint& Asset,
		EAssetThumbnailPriority Priority) -> void
	{
		if (!Asset.VirtualPath.IsValid()) return;
		auto [It, bInserted] = Impl->Entries.try_emplace(Asset.VirtualPath);
		FImpl::FEntry& Entry = It->second;
		if (bInserted)
			Entry.Fingerprint = Asset;
		else if (Entry.Fingerprint != Asset)
		{
			const uint64 NextSerial = Entry.Serial + 1;
			Impl->Scheduler.Cancel(Asset.VirtualPath);
			Impl->UnregisterTexture(Entry);
			Entry = {};
			Entry.Fingerprint = Asset;
			Entry.Serial = NextSerial;
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

	auto FRenderedAssetThumbnailCache::Find(const FAssetPath& AssetPath) const
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

	auto FRenderedAssetThumbnailCache::EndFrame() -> void
	{
		if (Impl->ActiveJob)
			Impl->PollActive();
		else
			Impl->StartNext();
		Impl->EvictToBudget();
	}

	auto FRenderedAssetThumbnailCache::CancelPendingRequests() -> void
	{
		if (Impl->ActiveJob) Impl->Pipeline.Cancel(*Impl->ActiveJob);
		Impl->ResetActive();
		Impl->Scheduler.CancelAll();
		for (auto& [Path, Entry] : Impl->Entries)
		{
			++Entry.Serial;
			Entry.bUploading = false;
			Entry.bUploadFailed = false;
			Entry.Diagnostic.clear();
		}
	}

	auto FRenderedAssetThumbnailCache::Clear() -> void
	{
		CancelPendingRequests();
		for (auto& [Path, Entry] : Impl->Entries)
			Impl->UnregisterTexture(Entry);
		Impl->Entries.clear();
	}

	auto FRenderedAssetThumbnailCache::GetStats() const
		-> FRenderedAssetThumbnailCacheStats
	{
		uint64 LiveGpuTextures = 0;
		for (const auto& [Path, Entry] : Impl->Entries)
			LiveGpuTextures += Entry.Texture != nullptr ? 1u : 0u;
		return {
			.Pipeline = Impl->Pipeline.GetStats(),
			.PreviewSceneCreations = Impl->PreviewSceneCreations,
			.PreviewSceneAssignments = Impl->PreviewSceneAssignments,
			.UploadsQueued = Impl->UploadsQueued,
			.UploadsCompleted = Impl->UploadsCompleted,
			.UploadFailures = Impl->UploadFailures,
			.GpuEvictions = Impl->GpuEvictions,
			.LiveGpuTextures = LiveGpuTextures,
			.bHasActiveJob = Impl->ActiveJob.has_value(),
			.bHasPreviewScene = Impl->ScenePool != nullptr};
	}
} // namespace Durin
