#include "Thumbnail/AssetThumbnailGeneration.h"

#include "Image/ImageEncoder.h"
#include "Image/ImageDecoder.h"
#include "Threading/TaskComposition.h"

namespace Durin::Editor
{
	struct FAssetThumbnailGeneration::FImpl
	{
		// Only the serial cache task touches the store in background mode, including
		// its initial index scan. No task retains a renderer, session, or scheduler.
		struct FCacheState
		{
			FAssetThumbnailPoolStorageSettings Settings;
			std::unique_ptr<FThumbnailObjectStore> Store;
			auto GetStore() -> FThumbnailObjectStore&
			{
				if (!Store) Store = std::make_unique<FThumbnailObjectStore>(Settings);
				return *Store;
			}
		};
		struct FCacheResult
		{
			FByteBuffer Pixels;
			bool bHit = false;
			bool bSucceeded = false;
			bool bInvalid = false;
			bool bSkipped = false;
			uint64 Evictions = 0;
		};
		struct FCacheRead
		{
			FAssetThumbnailScheduledRequest Job;
			FCacheResult Result;
			uint64 ReservedBytes = 0;
			bool bStarted = false;
			bool bComplete = false;
		};
		struct FCacheWrite
		{
			std::string Key;
			FByteBuffer Bytes;
			FAssetThumbnailCancellation Cancellation;
			uint32 Width = 0;
			uint32 Height = 0;
		};

		FImpl(
			FAssetThumbnailRequestQueue& InScheduler,
			FAssetThumbnailPoolStorageSettings StoreSettings,
			FAssetThumbnailBudgets InBudgets,
			bool bInBackgroundCache)
			: Scheduler(InScheduler)
			, Cache(std::make_shared<FCacheState>(std::move(StoreSettings)))
			, Budgets(InBudgets)
			, bBackgroundCache(bInBackgroundCache)
		{
		}

		FAssetThumbnailRequestQueue& Scheduler;
		std::shared_ptr<FCacheState> Cache;
		FAssetThumbnailBudgets Budgets;
		FAssetThumbnailGenerationStats Stats;
		uint32 RendersStartedThisFrame = 0;
		bool bBackgroundCache = false;
		std::array<std::optional<FCacheRead>, 2> Reads;
		std::deque<FCacheWrite> Writes;
		Tasks::TTask<FCacheResult> CacheTask;
		std::optional<Tasks::FTaskGroup> CacheTasks;
		int32 ActiveRead = -1;
		uint64 ActiveWriteBytes = 0;
		uint64 RetainedBytes = 0;

		auto PumpCache() -> void
		{
			if (CacheTask.IsValid())
			{
				if (!CacheTask.IsCompleted()) return;
				FCacheResult Result;
				if (CacheTask.GetState() == ETaskState::Succeeded)
					Result = std::move(CacheTask).TakeResult();
				CacheTask = {};
				Stats.Evictions = std::max(Stats.Evictions, Result.Evictions);
				if (ActiveRead >= 0)
				{
					Reads[ActiveRead]->Result = std::move(Result);
					Reads[ActiveRead]->bComplete = true;
				}
				else
				{
					RetainedBytes -= ActiveWriteBytes;
					if (Result.bSucceeded) ++Stats.CacheWrites;
					else if (Result.bSkipped) ++Stats.CacheWritesSkipped;
					else ++Stats.CacheWriteFailures;
				}
			}
			for (int32 Index = 0; Index < 2; ++Index)
			{
				if (!Reads[Index] || Reads[Index]->bStarted) continue;
				FCacheRead& Read = *Reads[Index];
				const auto& Request = Read.Job.GenerationRequest;
				Read.bStarted = true;
				ActiveRead = Index;
				if (!CacheTasks) CacheTasks.emplace();
				CacheTask = Tasks::LaunchTask(*CacheTasks, Tasks::ETaskExecutor::BlockingIO,
					{.DebugName = "ReadAssetThumbnailCache"},
					[State = Cache, Key = Read.Job.CacheKey,
						Cancellation = Request.Cancellation, Output = Request.KeyInput.Output] {
						FCacheResult Result;
						if (Cancellation.IsCancelled()) return Result;
						auto& Store = State->GetStore();
						FByteBuffer Encoded;
						if (Store.Load(Key, Encoded) == EThumbnailObjectLoadResult::Hit)
						{
							std::string Error;
							const uint64 PixelCount = static_cast<uint64>(Output.Width) * Output.Height;
							if (auto Decoded = Image::DecodeImageFromMemory(Encoded,
									{.MaximumEncodedBytes = Encoded.size(), .MaximumDecodedPixels = PixelCount}); Decoded
								&& Decoded->Width == Output.Width && Decoded->Height == Output.Height
								&& Decoded->Pixels.size() == PixelCount * 4)
							{
								Result.bHit = true;
								Result.Pixels = std::move(Decoded->Pixels);
							}
							else
							{
								Store.Invalidate(Key);
								Result.bInvalid = true;
							}
						}
						Result.Evictions = Store.GetStats().Evictions;
						return Result;
					});
				return;
			}
			if (Writes.empty()) return;
			FCacheWrite Write = std::move(Writes.front());
			Writes.pop_front();
			ActiveRead = -1;
			ActiveWriteBytes = Write.Bytes.size();
			if (!CacheTasks) CacheTasks.emplace();
			CacheTask = Tasks::LaunchTask(*CacheTasks, Tasks::ETaskExecutor::BlockingIO,
				{.DebugName = "SaveAssetThumbnailCache"},
				[State = Cache, Write = std::move(Write)] {
					FCacheResult Result;
					if (Write.Cancellation.IsCancelled())
					{
						Result.bSkipped = true;
						return Result;
					}
					FByteBuffer Encoded;
					FByteView Bytes = Write.Bytes;
					if (Write.Width != 0)
					{
						if (!Image::EncodeRgba8Png(Bytes, Write.Width, Write.Height, Encoded)) return Result;
						Bytes = Encoded;
					}
					if (Write.Cancellation.IsCancelled())
					{
						Result.bSkipped = true;
						return Result;
					}
					auto& Store = State->GetStore();
					Result.bSucceeded = Store.Store(Write.Key, Bytes);
					Result.Evictions = Store.GetStats().Evictions;
					return Result;
				});
		}

		auto QueueWrite(const FAssetThumbnailJob& Job, FByteView Bytes,
			uint32 Width = 0, uint32 Height = 0) -> void
		{
			// Persistence is optional. A slow disk must not accumulate unlimited pixels
			// or hold back display and capture admission.
			if (Writes.size() >= 8 || Bytes.size() > Budgets.CpuPixelBudgetBytes
				|| RetainedBytes > Budgets.CpuPixelBudgetBytes - Bytes.size())
			{
				++Stats.CacheWritesSkipped;
				return;
			}
			RetainedBytes += Bytes.size();
			Writes.push_back({Job.ScheduledJob.CacheKey,
				FByteBuffer(Bytes.begin(), Bytes.end()),
				Job.ScheduledJob.GenerationRequest.Cancellation, Width, Height});
		}

		auto DrainCache() -> void
		{
			PumpCache();
			while (CacheTask.IsValid())
			{
				(void)WaitTask(CacheTask.GetCompletion().GetTaskHandle());
				PumpCache();
			}
		}

		auto Fail(
			const FAssetThumbnailJob& Job,
			EAssetThumbnailState ExpectedState,
			std::string_view Error) -> bool
		{
			if (Error.empty()) return false;
			if (!Scheduler.Transition(Job.ScheduledJob, ExpectedState, EAssetThumbnailState::Failed, Error))
				return false;
			++Stats.Failures;
			return true;
		}
	};

	FAssetThumbnailGeneration::FAssetThumbnailGeneration(
		FAssetThumbnailRequestQueue& Scheduler,
		FAssetThumbnailPoolStorageSettings StoreSettings,
		FAssetThumbnailBudgets Budgets,
		bool bBackgroundCache
	)
		: Impl(std::make_unique<FImpl>(Scheduler, std::move(StoreSettings), Budgets, bBackgroundCache))
	{
	}

	FAssetThumbnailGeneration::~FAssetThumbnailGeneration()
	{
		// Module teardown is the only production wait. UI frames merely poll.
		Impl->DrainCache();
		if (Impl->CacheTasks)
		{
			Impl->CacheTasks->Close();
			while (Impl->CacheTasks->WaitFor(60.0) == ETaskScopeWaitResult::TimedOut) {}
		}
	}

	auto FAssetThumbnailGeneration::WaitForCacheTasksForTesting() -> void
	{
		Impl->DrainCache();
	}

	auto FAssetThumbnailGeneration::BeginFrame() -> void
	{
		Impl->RendersStartedThisFrame = 0;
		if (Impl->bBackgroundCache) Impl->PumpCache();
	}

	auto FAssetThumbnailGeneration::StartNext() -> std::optional<FAssetThumbnailJob>
	{
		return StartNextDetailed().ColdJob;
	}

	auto FAssetThumbnailGeneration::StartNextDetailed()
		-> FAssetThumbnailStartResult
	{
		return StartNextDetailed(false);
	}

	auto FAssetThumbnailGeneration::StartNextGeneratedPixelsDetailed()
		-> FAssetThumbnailStartResult
	{
		return StartNextDetailed(true);
	}

	auto FAssetThumbnailGeneration::StartNextDetailed(
		bool bGeneratedPixelsOnly) -> FAssetThumbnailStartResult
	{
		FAssetThumbnailStartResult Result;
		if (Impl->bBackgroundCache)
		{
			Impl->PumpCache();
			const int32 Index = bGeneratedPixelsOnly
				? 1 : (Impl->Reads[1] && Impl->Reads[1]->bComplete ? 1 : 0);
			auto& Slot = Impl->Reads[Index];
			if (Slot)
			{
				if (!Slot->bComplete) return Result;
				FImpl::FCacheRead Read = std::move(*Slot);
				Slot.reset();
				Impl->RetainedBytes -= Read.ReservedBytes;
				if (!Impl->Scheduler.Transition(Read.Job, EAssetThumbnailState::Loading,
						Read.Result.bHit ? EAssetThumbnailState::Ready : EAssetThumbnailState::Loading)) return Result;
				if (Read.Result.bHit)
				{
					++Impl->Stats.DiskHits;
					Result.Pixels = std::move(Read.Result.Pixels);
					Result.WarmJob = std::move(Read.Job);
				}
				else
				{
					if (Read.Result.bInvalid) ++Impl->Stats.Retries;
					++Impl->Stats.Loads;
					Result.ColdJob = FAssetThumbnailJob{.ScheduledJob = std::move(Read.Job)};
				}
				return Result;
			}
			auto Job = bGeneratedPixelsOnly
				? Impl->Scheduler.TakeNextGeneratedPixels() : Impl->Scheduler.TakeNext();
			if (!Job) return Result;
			++Impl->Stats.Jobs;
			const auto& Output = Job->GenerationRequest.KeyInput.Output;
			const uint64 PixelCount = static_cast<uint64>(Output.Width) * Output.Height;
			if (PixelCount == 0 || PixelCount > Impl->Budgets.CpuPixelBudgetBytes / 4
				|| Impl->RetainedBytes > Impl->Budgets.CpuPixelBudgetBytes - PixelCount * 4)
			{
				// Skip optional cache reuse under pressure; rendering still validates output.
				++Impl->Stats.Loads;
				Result.ColdJob = FAssetThumbnailJob{.ScheduledJob = std::move(*Job)};
				return Result;
			}
			Impl->RetainedBytes += PixelCount * 4;
			Slot = FImpl::FCacheRead{.Job = std::move(*Job), .ReservedBytes = PixelCount * 4};
			Impl->PumpCache();
			return Result;
		}
		std::optional<FAssetThumbnailScheduledRequest> ScheduledJob = bGeneratedPixelsOnly
			? Impl->Scheduler.TakeNextGeneratedPixels()
			: Impl->Scheduler.TakeNext();
		if (!ScheduledJob) return Result;
		++Impl->Stats.Jobs;

		const EThumbnailObjectLoadResult LoadResult =
			Impl->Cache->GetStore().Load(ScheduledJob->CacheKey, Result.EncodedBytes);
		if (LoadResult == EThumbnailObjectLoadResult::Hit)
		{
			if (Impl->Scheduler.Transition(
					*ScheduledJob,
					EAssetThumbnailState::Loading,
					EAssetThumbnailState::Ready))
			{
				++Impl->Stats.DiskHits;
				Result.WarmJob = std::move(*ScheduledJob);
			}
			return Result;
		}

		++Impl->Stats.Loads;
		Result.ColdJob = FAssetThumbnailJob{
			.ScheduledJob = std::move(*ScheduledJob)};
		return Result;
	}

	auto FAssetThumbnailGeneration::CompleteLoad(
		FAssetThumbnailJob& Job,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Loading, Error)) return false;
		if (!Impl->Scheduler.Transition(Job.ScheduledJob, EAssetThumbnailState::Loading,
				EAssetThumbnailState::WaitingForResources))
			return false;

		return true;
	}

	auto FAssetThumbnailGeneration::BeginRender(
		FAssetThumbnailJob& Job,
		bool bResourcesReady,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::WaitingForResources, Error))
			return false;
		if (!bResourcesReady)
		{
			++Impl->Stats.ResourceWaits;
			return Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::WaitingForResources,
				EAssetThumbnailState::WaitingForResources);
		}
		if (Impl->RendersStartedThisFrame >= Impl->Budgets.MaximumRendersPerFrame)
			return false;
		if (!Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::WaitingForResources,
				EAssetThumbnailState::Rendering))
			return false;

		++Impl->RendersStartedThisFrame;
		++Impl->Stats.Renders;
		return true;
	}

	auto FAssetThumbnailGeneration::CompleteRender(
		const FAssetThumbnailJob& Job,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Rendering, Error))
			return false;
		return Impl->Scheduler.Transition(Job.ScheduledJob,
			EAssetThumbnailState::Rendering,
			EAssetThumbnailState::Readback);
	}

	auto FAssetThumbnailGeneration::CompleteReadback(
		const FAssetThumbnailJob& Job,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Readback, Error))
			return false;
		if (!Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::Readback,
				EAssetThumbnailState::Encoding))
			return false;
		++Impl->Stats.Readbacks;
		return true;
	}

	auto FAssetThumbnailGeneration::CompleteEncoding(
		const FAssetThumbnailJob& Job,
		FByteView EncodedBytes,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Encoding, Error))
			return false;
		if (Impl->bBackgroundCache && !EncodedBytes.empty())
		{
			if (!Impl->Scheduler.Transition(Job.ScheduledJob, EAssetThumbnailState::Encoding,
					EAssetThumbnailState::Ready)) return false;
			Impl->QueueWrite(Job, EncodedBytes);
			return true;
		}
		if (EncodedBytes.empty()
			|| !Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::Encoding,
				EAssetThumbnailState::Encoding)
			|| !Impl->Cache->GetStore().Store(Job.ScheduledJob.CacheKey, EncodedBytes))
		{
			Impl->Fail(Job, EAssetThumbnailState::Encoding,
				"Failed to atomically publish the encoded thumbnail.");
			return false;
		}
		return Impl->Scheduler.Transition(Job.ScheduledJob,
			EAssetThumbnailState::Encoding,
			EAssetThumbnailState::Ready);
	}

	auto FAssetThumbnailGeneration::CompletePixels(
		const FAssetThumbnailJob& Job,
		FByteView Pixels,
		uint32 Width,
		uint32 Height,
		std::string_view Error,
		std::function<std::string()> ValidateBeforePublication) -> bool
	{
		if (!Error.empty())
			return CompleteEncoding(Job, {}, Error);
		if (Impl->bBackgroundCache)
		{
			const uint64 PixelCount = static_cast<uint64>(Width) * Height;
			if (PixelCount == 0 || PixelCount > Impl->Budgets.CpuPixelBudgetBytes / 4
				|| Pixels.size() != PixelCount * 4)
				return CompleteEncoding(Job, {},
					"Rendered-thumbnail pixels violate the RGBA8 output or CPU budget.");
			if (ValidateBeforePublication)
			{
				const std::string ValidationError = ValidateBeforePublication();
				if (!ValidationError.empty())
					return CompleteEncoding(Job, {}, ValidationError);
			}
			if (!Impl->Scheduler.Transition(Job.ScheduledJob, EAssetThumbnailState::Encoding,
					EAssetThumbnailState::Ready)) return false;
			Impl->QueueWrite(Job, Pixels, Width, Height);
			return true;
		}
		FByteBuffer EncodedBytes;
		if (!Image::EncodeRgba8Png(Pixels, Width, Height, EncodedBytes))
			return CompleteEncoding(
				Job,
				{},
				"Rendered-thumbnail pixels do not match the requested RGBA8 output.");
		if (ValidateBeforePublication)
		{
			const std::string ValidationError = ValidateBeforePublication();
			if (!ValidationError.empty())
				return CompleteEncoding(
					Job, {}, ValidationError);
		}
		return CompleteEncoding(
			Job, EncodedBytes);
	}

	auto FAssetThumbnailGeneration::CompleteGeneratedPixels(
		FAssetThumbnailJob& Job,
		FByteView Pixels,
		uint32 Width,
		uint32 Height,
		std::string_view Error) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Loading, Error)) return false;
		if (!Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::Loading, EAssetThumbnailState::Encoding)) return false;

		return CompletePixels(Job,
			Pixels, Width, Height);
	}

	auto FAssetThumbnailGeneration::Cancel(const FAssetThumbnailJob& Job) -> void
	{
		if (!Job.ScheduledJob.GenerationRequest.Cancellation.IsCancelled())
		{
			Impl->Scheduler.Cancel(Job.ScheduledJob.GenerationRequest.KeyInput.Asset.AssetPath);
			++Impl->Stats.Cancellations;
		}
	}

	auto FAssetThumbnailGeneration::RecordRetry() -> void
	{
		++Impl->Stats.Retries;
	}

	auto FAssetThumbnailGeneration::InvalidatePersistentObject(
		std::string_view CacheKey) -> void
	{
		require(!Impl->bBackgroundCache);
		Impl->Cache->GetStore().Invalidate(CacheKey);
	}

	auto FAssetThumbnailGeneration::GetStats() const -> FAssetThumbnailGenerationStats
	{
		FAssetThumbnailGenerationStats Stats = Impl->Stats;
		if (!Impl->bBackgroundCache && Impl->Cache->Store)
			Stats.Evictions = Impl->Cache->Store->GetStats().Evictions;
		return Stats;
	}
} // namespace Durin::Editor
