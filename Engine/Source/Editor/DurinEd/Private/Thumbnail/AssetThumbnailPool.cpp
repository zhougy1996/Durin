#include "Thumbnail/AssetThumbnailPool.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "Thumbnail/AssetThumbnailCache.h"
#include "Thumbnail/AssetThumbnailRequestQueue.h"
#include "Thumbnail/AssetThumbnailKey.h"
#include "DObject/StrongObjectPtr.h"
#include "DObject/Class.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/ThumbnailCapture.h"
#include "Thumbnail/ThumbnailDrawContext.h"
#include "Engine/StreamableManager.h"

#include "DynamicRHI.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "Threading/Future.h"
#include "RenderingThread.h"
#include "AssetRegistry/Catalog.h"
#include "Misc/MountPaths.h"
#include "Misc/Paths.h"

namespace Durin
{
	namespace
	{
		struct FAssetThumbnailUploadResult
		{
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			std::string Error;
		};

	} // namespace

	struct FAssetThumbnailPool::FImpl
	{
		struct FThumbnailRenderRequest
		{
			std::string CacheKey;
			FAssetData Asset;
			FAssetThumbnailOutputSettings Output;
		};
		struct FRenderedJob
		{
			FThumbnailRenderRequest Work;
			TStrongObjectPtr<DThumbnailRenderer> Renderer;
			TStrongObjectPtr<DObject> LoadedOverride;
			std::shared_ptr<FStreamableHandle> Load;
			std::unique_ptr<FThumbnailDrawContext> Draw;

			FRenderedJob() = default;
			FRenderedJob(FRenderedJob&&) = default;
			auto operator=(FRenderedJob&& Other) -> FRenderedJob&
			{
				if (this == &Other) return *this;
				Draw.reset();
				if (Load) Load->CancelHandle();
				Work = std::move(Other.Work);
				Renderer = std::move(Other.Renderer);
				LoadedOverride = std::move(Other.LoadedOverride);
				Load = std::move(Other.Load);
				Draw = std::move(Other.Draw);
				return *this;
			}
			~FRenderedJob() { Draw.reset(); if (Load) Load->CancelHandle(); }
			auto IsReady() const -> bool
			{
				return Draw && Draw->Status == FThumbnailDrawContext::EState::Ready;
			}
		};
		struct FParkedJob
		{
			FRenderedJob Job;
			uint64 FirstWaitFrame = 0;
			uint64 NextPollFrame = 0;
		};

		struct FThumbnailInfo
		{
			FAssetData AssetData;
			std::vector<FPackagePath> Dependencies;
			FTextureRHIRef Texture;
			TFuture<FAssetThumbnailUploadResult> Upload;
			uint64 LastUsedFrame = 0;
			bool bVisible = false;

			EAssetThumbnailState State = EAssetThumbnailState::None;
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
		FAssetThumbnailCache Cache;
		FAssetThumbnailGenerationStats Stats;
		uint32 RendersStartedThisFrame = 0;
		struct FCacheJob
		{
			FThumbnailRenderRequest Work;
			TFuture<FThumbnailCacheReadResult> Read;
			uint64 Bytes = 0;
		};
		std::deque<FCacheJob> CacheJobs;
		std::unique_ptr<FThumbnailCapture> CaptureOutput;
		std::optional<FAssetThumbnailOutputSettings> CaptureSettings;
		std::unordered_map<FTopLevelAssetPath, FThumbnailInfo> Entries;
		std::optional<FRenderedJob> PreparingJob;
		// Only a ready draw reserves a scene. The pool retains its job through
		// readback and validation while other root assets continue loading.
		std::optional<FRenderedJob> CaptureJob;
		std::vector<FParkedJob> ParkedJobs;
		uint64 FrameNumber = 0;
		uint64 CaptureOutputCreations = 0;
		uint64 CapturesStarted = 0;
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
			FThumbnailObjectStoreSettings StoreSettings)
			: Manager(InService)
			, Budgets(InBudgets)
			, Scheduler(Budgets.MaximumQueuedJobs)
			, Cache(std::move(StoreSettings), Budgets.CpuPixelBudgetBytes)
		{
			CatalogRevision = GetAssetCatalogRevision();
		}

		auto CollectBackgroundPackages(FAssetThumbnailPool& Pool) -> void
		{
			const FContentChangeBatch Changes = CaptureAssetCatalogChanges(CatalogRevision);
			CatalogRevision = Changes.ToRevision;
			Pool.ApplyContentChanges(Changes);
			if (!Budgets.bPreGenerateOnAssetChanges) return;
			// Mounts and journal rollover must not generate thumbnails for a whole project.
			if (Changes.bFullRefresh)
			{
				BackgroundPackages.clear();
				return;
			}
			for (const FContentChange& Change : Changes.Changes)
			{
				if (Change.bDirectory || Change.NewAssetPath.empty()) continue;
				if (std::ranges::contains(BackgroundPackages, Change.NewAssetPath)) continue;
				if (BackgroundPackages.size() >= Budgets.MaximumQueuedJobs) break;
				BackgroundPackages.push_back(Change.NewAssetPath);
			}
		}

		auto AdmitBackgroundPackage(FAssetThumbnailPool& Pool) -> void
		{
			if (BackgroundPackages.empty()) return;
			std::string Path = std::move(BackgroundPackages.front());
			BackgroundPackages.pop_front();
			FPackagePath PackagePath;
			if (!FPackagePath::TryCreate(Path, PackagePath)) return;
			const auto Package = FindPackageExact(PackagePath);
			if (!Package) return;
			for (const FAssetData& Asset : Package->TopLevelAssets)
			{
				if (Asset.IsRedirector() || !Manager.Find(Asset.AssetClassName)) continue;
				Pool.Prefetch(Asset);
			}
		}

		auto EnsureCapture(const FAssetThumbnailOutputSettings& Output) -> bool
		{
			if (CaptureOutput != nullptr && CaptureSettings && *CaptureSettings == Output)
				return CaptureOutput->IsAvailable();
			CaptureOutput.reset();
			CaptureSettings = Output;
			CaptureOutput = std::make_unique<FThumbnailCapture>(Output);
			++CaptureOutputCreations;
			return CaptureOutput->IsAvailable();
		}

		auto ResetCapture() -> void
		{
			if (!CaptureJob) return;
			if (CaptureOutput) CaptureOutput->Reset();
			CaptureJob.reset();
		}

		auto FailThumbnail(const FTopLevelAssetPath& AssetPath, std::string_view Error) -> void
		{
			const auto It = Entries.find(AssetPath);
			if (It == Entries.end()) return;
			It->second.State = EAssetThumbnailState::Failed;
			It->second.Diagnostic = Error.empty() ? "The thumbnail renderer could not produce a preview." : std::string(Error);
			++Stats.Failures;
		}
		auto AcceptPixels(const FThumbnailRenderRequest& Work, FByteView Pixels, uint32 Width, uint32 Height, std::string_view Error) -> bool
		{
			const uint64 PixelCount = uint64(Width) * Height;
			const uint64 Bytes = PixelCount <= Budgets.CpuPixelBudgetBytes / 4 ? PixelCount * 4 : 0;
			if (Error.empty() && (!Bytes || Bytes > Budgets.CpuPixelBudgetBytes || Pixels.size() != Bytes)) Error = "Rendered-thumbnail pixels violate the RGBA8 output or CPU budget.";
			if (!Error.empty())
			{
				FailThumbnail(Work.Asset.AssetPath, Error);
				return false;
			}
			++Stats.Readbacks;
			uint64 PendingBytes = 0;
			for (const auto& Job : CacheJobs)
				PendingBytes += Job.Bytes;
			if (PendingBytes <= Budgets.CpuPixelBudgetBytes - Bytes
				&& Cache.GetRetainedBytes() <= Budgets.CpuPixelBudgetBytes - Bytes - PendingBytes)
				Cache.Save(Work.CacheKey, Pixels, Width, Height);
			return true;
		}
		auto RetireAssetWork(const FTopLevelAssetPath& AssetPath) -> void
		{
			if (CaptureJob && CaptureJob->Work.Asset.AssetPath == AssetPath) ResetCapture();
			if (PreparingJob && PreparingJob->Work.Asset.AssetPath == AssetPath) PreparingJob.reset();
			std::erase_if(ParkedJobs, [&](const FParkedJob& Parked) {
				return Parked.Job.Work.Asset.AssetPath == AssetPath;
			});
			std::erase_if(CacheJobs, [&](const FCacheJob& Job) {
				return Job.Work.Asset.AssetPath == AssetPath;
			});
			Scheduler.Remove(AssetPath);
			if (const auto It = Entries.find(AssetPath); It != Entries.end())
			{
				auto& Entry = It->second;
				if (Entry.State == EAssetThumbnailState::Pending) ++Stats.Cancellations;
				Entry.Upload.Reset();
				Entry.State = Entry.Texture ? EAssetThumbnailState::Ready : EAssetThumbnailState::None;
				Entry.Diagnostic.clear();
			}
		}

		auto UnregisterTexture(FThumbnailInfo& Entry) -> void
		{
			if (Entry.Texture && Mona::GetActiveUIBackend())
				Mona::GetActiveUIBackend()->UnregisterTexture(Entry.Texture);
			Entry.Texture = nullptr;
			if (Entry.State == EAssetThumbnailState::Ready) Entry.State = EAssetThumbnailState::None;
		}

		auto QueueUpload(
			const FThumbnailRenderRequest& Request,
			FByteBuffer Pixels,
			uint32 Width,
			uint32 Height) -> void
		{
			const FTopLevelAssetPath& AssetPath = Request.Asset.AssetPath;
			auto It = Entries.find(AssetPath);
			if (It == Entries.end()) return;

			It->second.State = EAssetThumbnailState::Pending;
			It->second.bHasTransparency = false;
			for (size_t Offset = 3; Offset < Pixels.size(); Offset += 4)
				if (Pixels[Offset] != std::byte{255})
				{
					It->second.bHasTransparency = true;
					break;
				}
			++UploadsQueued;
			auto Promise = std::make_shared<TPromise<FAssetThumbnailUploadResult>>();
			It->second.Upload = Promise->GetFuture();
			ENQUEUE_RENDER_COMMAND(UploadRenderedAssetThumbnail)(
				[Promise, Pixels = std::move(Pixels), Width, Height](FRHICommandListImmediate& CommandList) {
					FAssetThumbnailUploadResult Result{.Width = Width, .Height = Height};

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
							Pixels);
						CommandList.TransitionTextures(std::array{FRHITextureTransition{
							Result.Texture, {ERHITextureAspect::Color, 0, 1, 0, 1},
							ERHIAccess::TransferWrite, ERHIAccess::GraphicsShaderRead}});
					}
					else
					{
						Result.Error =
							"Unable to create the rendered-asset thumbnail UI texture.";
					}
					(void)Promise->TrySetValue(std::move(Result));
				});
		}

		auto DrainUploads() -> void
		{
			for (auto& [Path, Entry] : Entries)
			{
				if (!Entry.Upload.IsReady()) continue;
				FAssetThumbnailUploadResult Result;
				if (Entry.Upload.GetState() == EFutureState::Succeeded) Result = Entry.Upload.Consume();
				else
				{
					Entry.Upload.Reset();
					Result.Error = "The thumbnail upload did not complete.";
				}

				if (Result.Texture == nullptr || !Mona::GetActiveUIBackend())
				{
					Entry.Diagnostic = Result.Error.empty()
						? "The UI backend is unavailable for rendered-asset thumbnails."
						: std::move(Result.Error);
					Entry.State = EAssetThumbnailState::Failed;
					++UploadFailures;
					continue;
				}
				UnregisterTexture(Entry);
				Mona::GetActiveUIBackend()->RegisterTexture(Result.Texture);
				Entry.Texture = std::move(Result.Texture);
				Entry.Width = Result.Width;
				Entry.Height = Result.Height;
				Entry.Diagnostic.clear();
				Entry.State = EAssetThumbnailState::Ready;
				++UploadsCompleted;
			}
		}

		auto FinishCapture() -> void
		{
			if (!CaptureJob || CaptureOutput == nullptr) return;
			auto Capture = CaptureOutput->PollCapture();
			if (Capture && !Capture->has_value()) return;

			FRenderedJob& Job = *CaptureJob;
			FThumbnailRenderRequest& Request =
				Job.Work;
			FThumbnailDrawContext* Draw = Job.Draw.get();
			std::string Error;
			if (!Capture) Error = Capture.error();
			else if (!Draw) Error = "The rendered-thumbnail renderer was removed.";
			else if (auto Valid = Draw->Validate(); !Valid)
				Error = Valid.error();
			if (AcceptPixels(Job.Work, Capture ? FByteView(**Capture) : FByteView{},
				Request.Output.Width, Request.Output.Height, Error))
			{
				QueueUpload(Request, std::move(**Capture), Request.Output.Width, Request.Output.Height);
			}
			ResetCapture();
		}

		auto ParkPreparing() -> void
		{
			if (!PreparingJob) return;
			if (Budgets.MaximumParkedRenderedJobs == 0
				|| ParkedJobs.size() >= Budgets.MaximumParkedRenderedJobs)
			{
				FailThumbnail(PreparingJob->Work.Asset.AssetPath,
					"The rendered-thumbnail parked-resource budget is exhausted.");
				PreparingJob.reset();
				return;
			}
			++Stats.ResourceWaits;
			ParkedJobs.push_back({
				.Job = std::move(*PreparingJob),
				.FirstWaitFrame = FrameNumber,
				.NextPollFrame = FrameNumber
					+ std::max<uint32>(1, Budgets.ResourcePollIntervalFrames)});
			PreparingJob.reset();
			PeakParkedResourceWaits = std::max<uint64>(
				PeakParkedResourceWaits, ParkedJobs.size());
		}

		auto HandlePreparedUpdate() -> void
		{
			if (!PreparingJob) return;
			FRenderedJob& Job = *PreparingJob;
			FThumbnailRenderRequest& Request =
				Job.Work;
			FThumbnailDrawContext* Draw = Job.Draw.get();
			const bool bReady = Job.IsReady();
			if (!bReady || CaptureJob)
			{
				ParkPreparing();
				return;
			}
			if (RendersStartedThisFrame >= Budgets.MaximumRendersPerFrame) return;
			++RendersStartedThisFrame;
			++Stats.Renders;
			std::string Error;
			if (!EnsureCapture(Request.Output))
			{
				Error = CaptureOutput != nullptr
					? CaptureOutput->GetDiagnostic()
					: "The thumbnail capture output is unavailable.";
			}
			else if (auto Prepared = Draw->BindCapture(*CaptureOutput); !Prepared)
			{
				Error = std::move(Prepared.error());
			}
			else
			{
				if (auto Capture = CaptureOutput->BeginCapture(); !Capture)
				{
					Error = std::move(Capture.error());
				}
				else
				{
					++CapturesStarted;
					CaptureJob = std::move(PreparingJob);
					PreparingJob.reset();
					return;
				}
			}
			AcceptPixels(Job.Work, {}, 0, 0, Error);
			if (CaptureOutput) CaptureOutput->Reset();
			PreparingJob.reset();
		}

		// Root loading and renderer preparation belong to the pool, alongside admission.
		// False consumes a terminal failure; readiness lives only in the context.
		auto UpdateDraw(FRenderedJob& Job, bool bAllowDraw) -> bool
		{
			auto Fail = [&](std::string Error) {
				FailThumbnail(Job.Work.Asset.AssetPath, std::move(Error));
				return false;
			};
			if (!Job.Draw)
			{
				DObject* Object = Job.LoadedOverride.Get();
				if (!Object)
				{
					if (!Job.Load) return Fail("The thumbnail asset load is unavailable.");
					if (!Job.Load->IsComplete()) return true;
					if (Job.Load->HasError()) return Fail(Job.Load->GetError()->ToString());
					Object = Job.Load->GetLoadedAsset();
					if (!Object || Object->GetClass()->GetQualifiedName().ToString() != Job.Work.Asset.AssetClassName)
						return Fail("The thumbnail asset does not match its catalog class.");
				}
				if (!Job.Renderer->CanVisualizeAsset(Object)) return Fail("The renderer cannot visualize this asset.");
				Job.Draw = std::make_unique<FThumbnailDrawContext>(Object);
			}
			if (!Job.IsReady() && bAllowDraw)
			{
				DObject* Object = Job.LoadedOverride ? Job.LoadedOverride.Get() : Job.Load->GetLoadedAsset();
				Job.Renderer->Draw(Object, *Job.Draw);
			}
			if (Job.Draw->Status == FThumbnailDrawContext::EState::Failed) return Fail(Job.Draw->Diagnostic);
			return true;
		}

		auto PollPreparing() -> void
		{
			if (!PreparingJob) return;
			if (!UpdateDraw(*PreparingJob, !CaptureJob)) { PreparingJob.reset(); return; }
			HandlePreparedUpdate();
		}

		auto PollParkedJobs() -> void
		{
			const uint64 PollInterval =
				std::max<uint32>(1, Budgets.ResourcePollIntervalFrames);
			bool bDrawReserved = PreparingJob.has_value() || CaptureJob.has_value()
				|| std::ranges::any_of(ParkedJobs, [](const auto& Job) { return Job.Job.IsReady(); });
			for (size_t Index = 0; Index < ParkedJobs.size();)
			{
				FParkedJob& Parked = ParkedJobs[Index];
				const bool bTimedOut = !Parked.Job.IsReady()
					&& Budgets.MaximumResourceWaitFrames != 0
					&& FrameNumber - Parked.FirstWaitFrame
						>= Budgets.MaximumResourceWaitFrames;
				bool bRelease = false;
				if (bTimedOut)
				{
					FailThumbnail(Parked.Job.Work.Asset.AssetPath,
						"Timed out waiting for rendered-thumbnail resources.");
					++ResourceWaitTimeouts;
					bRelease = true;
				}
				if (!bRelease && !Parked.Job.IsReady()
					&& FrameNumber >= Parked.NextPollFrame)
				{
					bRelease = !UpdateDraw(Parked.Job, !bDrawReserved);
					Parked.NextPollFrame = FrameNumber + PollInterval;
					if (Parked.Job.IsReady())
					{
						bDrawReserved = true;
					}
					else if (!bRelease)
						++Stats.ResourceWaits;
				}
				if (bRelease)
				{

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
					Parked.Job.Work.Asset.AssetPath);
				return It != Entries.end() && It->second.bVisible;
			};
			auto Selected = std::ranges::find_if(
				ParkedJobs,
				[&](const FParkedJob& Parked) {
					return Parked.Job.IsReady() && IsVisible(Parked);
				});
			if (Selected == ParkedJobs.end())
				Selected = std::ranges::find_if(
					ParkedJobs,
					[](const FParkedJob& Parked) { return Parked.Job.IsReady(); });
			if (Selected == ParkedJobs.end()) return;
			PreparingJob = std::move(Selected->Job);
			ParkedJobs.erase(Selected);
			HandlePreparedUpdate();
		}

		auto StartNext() -> bool
		{
			Cache.Tick();
			if (CacheJobs.empty() || (CacheJobs.front().Read.IsValid() && !CacheJobs.front().Read.IsReady()))
			{
				if (CacheJobs.size() >= 8) return false;
				auto Request = Scheduler.TakeNext();
				if (!Request) return false;
				const TStrongObjectPtr<DThumbnailRenderer> Renderer(Manager.Find(Request->Asset.AssetClassName));
				if (!Renderer)
				{
					if (const auto It = Entries.find(Request->Asset.AssetPath); It != Entries.end())
						It->second.State = EAssetThumbnailState::None;
					return true;
				}
				auto Captured = CaptureThumbnailKeyInput(Request->Asset, Renderer->GetClass());
				if (!Captured)
				{
					FailThumbnail(Request->Asset.AssetPath, Captured.error());
					return true;
				}
				auto& Input = *Captured;
				FThumbnailRenderRequest Work{
					.CacheKey = BuildAssetThumbnailCacheKey(Input),
					.Asset = Request->Asset,
					.Output = Input.Output};
				++Stats.Jobs;
				if (auto It = Entries.find(Input.Asset.AssetPath); It != Entries.end())
				{
					It->second.Dependencies.clear();
					for (const auto& Package : Input.Packages)
						if (Package.PackagePath != Input.Asset.AssetPath.GetPackagePath())
							It->second.Dependencies.push_back(Package.PackagePath);
				}
				const uint64 PixelCount = uint64(Input.Output.Width) * Input.Output.Height;
				const uint64 Bytes = PixelCount <= Budgets.CpuPixelBudgetBytes / 4 ? PixelCount * 4 : 0;
				uint64 Retained = 0;
				for (const auto& Job : CacheJobs)
					Retained += Job.Bytes;
				TFuture<FThumbnailCacheReadResult> Read;
				if (Bytes && Retained <= Budgets.CpuPixelBudgetBytes - Bytes
					&& Cache.GetRetainedBytes() <= Budgets.CpuPixelBudgetBytes - Bytes - Retained)
					Read = Cache.Load(Work.CacheKey, Input.Output);
				const uint64 ReservedBytes = Read.IsValid() ? Bytes : 0;
				CacheJobs.push_back({std::move(Work), std::move(Read), ReservedBytes});
				return true;
			}
			auto Pending = std::move(CacheJobs.front());
			CacheJobs.pop_front();
			auto Work = std::move(Pending.Work);
			FThumbnailCacheReadResult Result;
			if (Pending.Read.GetState() == EFutureState::Succeeded) Result = Pending.Read.Consume();
			if (!Result.Pixels.empty())
			{
				++Stats.DiskHits;
				QueueUpload(Work, std::move(Result.Pixels), Work.Output.Width, Work.Output.Height);
				return true;
			}
			if (Result.bInvalid) ++Stats.Retries;
			++Stats.Loads;
			PreparingJob.emplace();
			PreparingJob->Work = std::move(Work);
			FRenderedJob& Job = *PreparingJob;
			auto& Request = Job.Work;
			Job.Renderer = TStrongObjectPtr<DThumbnailRenderer>(Manager.Find(Request.Asset.AssetClassName));
			if (!Job.Renderer)
			{
				FailThumbnail(Request.Asset.AssetPath, "The thumbnail renderer was removed.");
				PreparingJob.reset();
				return true;
			}
			Job.LoadedOverride = TStrongObjectPtr<DObject>(Manager.GetLoadedObjectOverride(Request.Asset.AssetClassName));
			if (!Job.LoadedOverride) Job.Load = GetStreamableManager().RequestAsyncLoad(Request.Asset.AssetPath);
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
					RetireAssetWork(Path);
					Entries.erase(It);
					++GpuEvictions;
				}
			}

			BudgetEntries.clear();
			BudgetEntries.reserve(Entries.size());
			for (const auto& [Path, Entry] : Entries)
			{
				const bool bActive = Entry.State == EAssetThumbnailState::Pending;
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
					RetireAssetWork(Path);
					Entries.erase(It);
					GpuEvictions += bHadTexture ? 1u : 0u;
				}
			}
		}
	};

	FAssetThumbnailPool::FAssetThumbnailPool(
		FAssetThumbnailBudgets Budgets,
		FThumbnailObjectStoreSettings StoreSettings)
		: FAssetThumbnailPool(
			GetDefaultThumbnailManager(),
			Budgets,
			std::move(StoreSettings))
	{
	}

	FAssetThumbnailPool::FAssetThumbnailPool(
		DThumbnailManager& Manager,
		FAssetThumbnailBudgets Budgets,
		FThumbnailObjectStoreSettings StoreSettings)
		: Impl(std::make_unique<FImpl>(
			Manager, Budgets, std::move(StoreSettings)))
	{
		Manager.AttachPool(this);
	}

	auto FAssetThumbnailPool::InvalidateRenderer(std::string_view AssetClassName) -> void
	{
		for (const auto& [Path, Entry] : Impl->Entries)
			if (Entry.AssetData.AssetClassName == AssetClassName) Impl->RetireAssetWork(Path);
	}

	FAssetThumbnailPool::~FAssetThumbnailPool()
	{
		Impl->Manager.DetachPool(this);
		Clear();
		Impl->CaptureOutput.reset();
	}

	auto FAssetThumbnailPool::Prefetch(const FAssetData& Asset) -> void
	{
		AccessTexture(Asset, EAssetThumbnailPriority::Prefetch);
		if (const auto It = Impl->Entries.find(Asset.AssetPath); It != Impl->Entries.end())
			It->second.bBackgroundRequested = true;
	}

	auto FAssetThumbnailPool::AccessTexture(
		const FAssetData& Asset,
		EAssetThumbnailPriority Priority) -> FAssetThumbnailView
	{
		if (!Asset.AssetPath.IsValid()) return {};
		// Apply already-published changes before accepting fresh UI demand.
		if (Impl->CatalogRevision != GetAssetCatalogRevision()) Impl->CollectBackgroundPackages(*this);
		auto [It, bInserted] = Impl->Entries.try_emplace(Asset.AssetPath);
		FImpl::FThumbnailInfo& Entry = It->second;
		if (bInserted)
			Entry.AssetData = Asset;
		else if (Entry.AssetData != Asset)
		{
			const uint32 ReferencerCount = Entry.ReferencerCount;
			Impl->RetireAssetWork(Asset.AssetPath);
			Impl->UnregisterTexture(Entry);
			Entry = {};
			Entry.AssetData = Asset;
			Entry.ReferencerCount = ReferencerCount;
		}
		Entry.bVisible |= Priority == EAssetThumbnailPriority::Visible;
		Entry.LastUsedFrame = Impl->FrameNumber;
		if (Entry.Texture)
		{
			Entry.Diagnostic.clear();
			return Find(Asset.AssetPath);
		}
		if (Entry.State == EAssetThumbnailState::Pending)
		{
			if (Priority == EAssetThumbnailPriority::Visible) Impl->Scheduler.Prioritize(Asset.AssetPath);
		}
		else if (Entry.State == EAssetThumbnailState::None && !Impl->Manager.IsShuttingDown()
			&& Impl->Manager.Find(Asset.AssetClassName))
		{
			if (Impl->Scheduler.Request({.Asset = Asset, .Priority = Priority}))
				Entry.State = EAssetThumbnailState::Pending;
		}
		return Find(Asset.AssetPath);
	}

	auto FAssetThumbnailPool::Find(const FTopLevelAssetPath& AssetPath) const -> FAssetThumbnailView
	{
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return {};
		const auto& Entry = It->second;
		FAssetThumbnailView View;
		View.State = Entry.State;
		View.Texture = Entry.Texture;
		View.Width = Entry.Width;
		View.Height = Entry.Height;
		View.bHasTransparency = Entry.bHasTransparency;
		View.bShowTransparencyGrid = false;
		if (View.State == EAssetThumbnailState::Failed) View.Diagnostic = Entry.Diagnostic;
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
		// Directory navigation releases cold work while keeping cached pixels.
		Impl->RetireAssetWork(AssetPath);
	}

	auto FAssetThumbnailPool::Refresh(const FTopLevelAssetPath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return;
		Impl->RetireAssetWork(AssetPath);
		Impl->UnregisterTexture(It->second);

		It->second.State = EAssetThumbnailState::None;
		It->second.Diagnostic.clear();
	}

	auto FAssetThumbnailPool::ApplyContentChanges(const FContentChangeBatch& Changes) -> void
	{
		if (!Changes.bFullRefresh && Changes.Changes.empty()) return;
		auto AffectsPackage = [&](const FPackagePath& Package) {
			if (!Package.IsValid()) return false;
			const auto Virtual = Package.ToString();
			const auto Physical = FMountPaths::ResolveAssetPath(Virtual);
			const auto Filename = Physical ? std::filesystem::path(Physical->PhysicalPath.generic_string() + ".dasset") : std::filesystem::path{};
			return std::ranges::any_of(Changes.Changes, [&](const auto& Change) {
				for (const auto* Changed : {&Change.OldAssetPath, &Change.NewAssetPath})
				{
					std::string_view Path = *Changed;
					while (Path.size() > 1 && Path.ends_with('/')) Path.remove_suffix(1);
					if (!Path.empty() && (Virtual == Path || (Path.starts_with(Virtual) && Path.size() > Virtual.size() && Path[Virtual.size()] == '.')
						|| (Change.bDirectory && Virtual.starts_with(Path) && (Path == "/" || (Virtual.size() > Path.size() && Virtual[Path.size()] == '/'))))) return true;
				}
				if (Physical)
					for (const auto* Changed : {&Change.OldPhysicalPath, &Change.NewPhysicalPath})
					{
						if (Changed->empty()) continue;
						std::filesystem::path Relative;
						if (FPaths::TryMakeLexicalRelativePath(Filename, std::filesystem::path(*Changed), Relative)
							&& (Relative.empty() || (Change.bDirectory && FPaths::IsLexicalDescendantPath(Filename, std::filesystem::path(*Changed), true)))) return true;
					}
				return false;
			});
		};
		for (auto& [Path, Entry] : Impl->Entries)
			if (Changes.bFullRefresh || AffectsPackage(Path.GetPackagePath())
				|| std::ranges::any_of(Entry.Dependencies, [&](const auto& Dependency) { return AffectsPackage(Dependency); }))
				Refresh(Path);
	}

	auto FAssetThumbnailPool::Tick(bool bAllowNewAdmissions) -> void
	{
		++Impl->FrameNumber;
		Impl->CollectBackgroundPackages(*this);
		Impl->DrainUploads();
		Impl->FinishCapture();
		Impl->RendersStartedThisFrame = 0;
		Impl->Cache.Tick();

		const auto StartTime = std::chrono::steady_clock::now();
		if (Impl->PreparingJob)
			Impl->PollPreparing();
		Impl->PollParkedJobs();
		if (!Impl->PreparingJob)
			Impl->ActivateReadyParkedJob();
		for (uint32 Admission = 0; bAllowNewAdmissions && Admission < Impl->Budgets.MaximumAdmissionsPerFrame; ++Admission)
		{
			if (std::chrono::duration<double>(std::chrono::steady_clock::now() - StartTime).count()
				>= Impl->Budgets.FrameTimeAllowanceSeconds) break;
			Impl->AdmitBackgroundPackage(*this);
			// Leave a bounded slot for the next cold result; pressure defers work
			// rather than turning otherwise valid assets into permanent failures.
			const bool bPreparationFull = Impl->PreparingJob.has_value()
				|| Impl->ParkedJobs.size() >= Impl->Budgets.MaximumParkedRenderedJobs;
			if (bPreparationFull || !Impl->StartNext()) break;
		}
		Impl->EvictToBudget();

		Impl->Scheduler.BeginFrame();
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
	}

	auto FAssetThumbnailPool::CancelPendingRequests() -> void
	{
		Impl->BackgroundPackages.clear();
		for (auto& [Path, Entry] : Impl->Entries)
		{
			Impl->RetireAssetWork(Path);
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
		auto GenerationStats = Impl->Stats;
		const auto CacheStats = Impl->Cache.GetStats();
		GenerationStats.Evictions = CacheStats.Evictions;
		GenerationStats.CacheWrites = CacheStats.CacheWrites;
		GenerationStats.CacheWriteFailures = CacheStats.CacheWriteFailures;
		GenerationStats.CacheWritesSkipped = CacheStats.CacheWritesSkipped;
		return {
			.Generation = GenerationStats,
			.CaptureOutputCreations = Impl->CaptureOutputCreations,
			.CapturesStarted = Impl->CapturesStarted,
			.UploadsQueued = Impl->UploadsQueued,
			.UploadsCompleted = Impl->UploadsCompleted,
			.UploadFailures = Impl->UploadFailures,
			.GpuEvictions = Impl->GpuEvictions,
			.LiveGpuTextures = LiveGpuTextures,
			.ParkedResourceWaits = Impl->ParkedJobs.size(),
			.PeakParkedResourceWaits = Impl->PeakParkedResourceWaits,
			.ResourceWaitTimeouts = Impl->ResourceWaitTimeouts,
			.QueuedJobs = Impl->Scheduler.Num(),
			.RetainedEntries = Impl->Entries.size(),
			.PinnedEntries = PinnedEntries,
			.Referencers = Referencers,
			.bHasActiveJob = Impl->PreparingJob.has_value() || Impl->CaptureJob.has_value(),
			.bHasCaptureOutput = Impl->CaptureOutput != nullptr};
	}
} // namespace Durin
