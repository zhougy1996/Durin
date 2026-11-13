#include "Thumbnail/AssetThumbnailGeneration.h"
#include "Thumbnail/ThumbnailRenderer.h"

#include "Image/ImageEncoder.h"
#include "Image/ImageDecoder.h"
#include "Threading/Task.h"

namespace Durin
{
	struct FAssetThumbnailGeneration::FImpl
	{
		static constexpr size_t MaximumCacheReads = 8;
		static constexpr size_t MaximumQueuedSaves = 8;
		// Only the serial cache task touches the store in background mode, including
		// its initial index scan. No task retains a renderer, session, or scheduler.
		struct FCacheState
		{
			FThumbnailObjectStoreSettings Settings;
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
			FThumbnailObjectStoreSettings StoreSettings,
			FAssetThumbnailBudgets InBudgets)
			: Scheduler(InScheduler)
			, Cache(std::make_shared<FCacheState>(std::move(StoreSettings)))
			, Budgets(InBudgets)
		{
			Cache->Settings.bBatchIndexWrites = true;
		}

		FAssetThumbnailRequestQueue& Scheduler;
		std::shared_ptr<FCacheState> Cache;
		FAssetThumbnailBudgets Budgets;
		FAssetThumbnailGenerationStats Stats;
		uint32 RendersStartedThisFrame = 0;
		std::deque<FCacheRead> Reads;
		// Deque entries stay stable until their completed result is consumed.
		// Workers capture values, never this game-thread-only observer.
		FCacheRead* ActiveRead = nullptr;
		std::deque<FCacheWrite> Writes;
		Tasks::TTask<FCacheResult> CacheTask;
		Tasks::TTask<FCacheWrite> EncodeTask;
		std::deque<FCacheWrite> PendingEncodes;
		uint64 EncodingBytes = 0;
		uint64 ActiveWriteBytes = 0;
		uint64 RetainedBytes = 0;
		bool bNeedsIndexFlush = false;
		bool bMaintenanceTask = false;

		auto PumpEncoding() -> void
		{
			if (EncodeTask.IsValid())
			{
				if (!EncodeTask.IsCompleted()) return;
				FCacheWrite Encoded;
				if (EncodeTask.GetState() == ETaskState::Succeeded)
					Encoded = std::move(EncodeTask.GetResult());
				EncodeTask = {};
				RetainedBytes -= EncodingBytes;
				if (Encoded.Cancellation.IsCancelled()) ++Stats.CacheWritesSkipped;
				else if (Encoded.Bytes.empty()) ++Stats.CacheWriteFailures;
				else if (Encoded.Bytes.size() > Budgets.CpuPixelBudgetBytes
					|| RetainedBytes > Budgets.CpuPixelBudgetBytes - Encoded.Bytes.size())
					++Stats.CacheWritesSkipped;
				else
				{
					RetainedBytes += Encoded.Bytes.size();
					Writes.push_back(std::move(Encoded));
				}
			}
			if (PendingEncodes.empty()) return;
			FCacheWrite Write = std::move(PendingEncodes.front());
			PendingEncodes.pop_front();
			EncodingBytes = Write.Bytes.size();
			EncodeTask = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker,
				{.DebugName = "EncodeAssetThumbnailCache"},
				[Write = std::move(Write)]() mutable {
					FByteBuffer Encoded;
					if (!Write.Cancellation.IsCancelled())
						(void)Image::EncodeRgba8Png(Write.Bytes, Write.Width, Write.Height, Encoded);
					Write.Bytes = std::move(Encoded);
					Write.Width = Write.Height = 0;
					return std::move(Write);
				});
		}

		auto PumpCache() -> void
		{
			PumpEncoding();
			if (CacheTask.IsValid())
			{
				if (!CacheTask.IsCompleted()) return;
				FCacheResult Result;
				if (CacheTask.GetState() == ETaskState::Succeeded)
					Result = std::move(CacheTask.GetResult());
				CacheTask = {};
				Stats.Evictions = std::max(Stats.Evictions, Result.Evictions);
				if (ActiveRead)
				{
					ActiveRead->Result = std::move(Result);
					ActiveRead->bComplete = true;
					ActiveRead = nullptr;
				}
				else if (!bMaintenanceTask)
				{
					RetainedBytes -= ActiveWriteBytes;
					bNeedsIndexFlush |= Result.bSucceeded;
					if (Result.bSucceeded) ++Stats.CacheWrites;
					else if (Result.bSkipped) ++Stats.CacheWritesSkipped;
					else ++Stats.CacheWriteFailures;
				}
				bMaintenanceTask = false;
			}
			const auto Next = std::ranges::find_if(Reads, [](const FCacheRead& Read) { return !Read.bComplete; });
			if (Next != Reads.end())
			{
				ActiveRead = &*Next;
				FCacheRead& Read = *Next;
				const auto& Request = Read.Job.GenerationRequest;
				CacheTask = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO,
					{.DebugName = "ReadAssetThumbnailCache"},
					[State = Cache, Key = Read.Job.CacheKey,
						Cancellation = Request.Cancellation, Output = Request.KeyInput.Output] {
						FCacheResult Result;
						if (Cancellation.IsCancelled()) return Result;
						auto& Store = State->GetStore();
						FByteBuffer Encoded;
						if (Store.Load(Key, Encoded) == EThumbnailObjectLoadResult::Hit)
						{
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
			if (Writes.empty())
			{
				if (bNeedsIndexFlush && !EncodeTask.IsValid() && PendingEncodes.empty())
				{
					bNeedsIndexFlush = false;
					bMaintenanceTask = true;
					CacheTask = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO,
						{.DebugName = "FlushAssetThumbnailIndex"}, [State = Cache] {
							State->GetStore().Flush();
							return FCacheResult{};
						});
				}
				return;
			}
			FCacheWrite Write = std::move(Writes.front());
			Writes.pop_front();
			ActiveWriteBytes = Write.Bytes.size();
			CacheTask = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO,
				{.DebugName = "SaveAssetThumbnailCache"},
				[State = Cache, Write = std::move(Write)] {
					FCacheResult Result;
					if (Write.Cancellation.IsCancelled())
					{
						Result.bSkipped = true;
						return Result;
					}
					const FByteView Bytes = Write.Bytes;
					auto& Store = State->GetStore();
					Result.bSucceeded = Store.Store(Write.Key, Bytes);
					Result.Evictions = Store.GetStats().Evictions;
					return Result;
				});
		}

		auto QueueWrite(const FAssetThumbnailScheduledRequest& Job, FByteView Bytes,
			uint32 Width = 0, uint32 Height = 0) -> void
		{
			// Persistence is optional. A slow disk must not accumulate unlimited pixels
			// or hold back display and capture admission.
			if (Writes.size() + PendingEncodes.size() + (EncodeTask.IsValid() ? 1u : 0u) >= MaximumQueuedSaves
				|| Bytes.size() > Budgets.CpuPixelBudgetBytes
				|| RetainedBytes > Budgets.CpuPixelBudgetBytes - Bytes.size())
			{
				++Stats.CacheWritesSkipped;
				return;
			}
			RetainedBytes += Bytes.size();
			(Width != 0 ? PendingEncodes : Writes).push_back({Job.CacheKey,
				FByteBuffer(Bytes.begin(), Bytes.end()),
				Job.GenerationRequest.Cancellation, Width, Height});
		}

		auto DrainCache() -> void
		{
			PumpCache();
			while (CacheTask.IsValid() || EncodeTask.IsValid())
			{
				if (EncodeTask.IsValid()) (void)WaitTask(Tasks::FTask(EncodeTask));
				if (CacheTask.IsValid()) (void)WaitTask(Tasks::FTask(CacheTask));
				PumpCache();
			}
		}

		auto Fail(
			const FAssetThumbnailScheduledRequest& Job,
			EThumbnailJobState ExpectedState,
			std::string_view Error) -> bool
		{
			if (Error.empty()) return false;
			if (!Scheduler.Transition(Job, ExpectedState, EThumbnailJobState::Failed, Error))
				return false;
			++Stats.Failures;
			return true;
		}
	};

	FAssetThumbnailGeneration::FAssetThumbnailGeneration(
		FAssetThumbnailRequestQueue& Scheduler,
		FThumbnailObjectStoreSettings StoreSettings,
		FAssetThumbnailBudgets Budgets
	)
		: Impl(std::make_unique<FImpl>(Scheduler, std::move(StoreSettings), Budgets))
	{
	}

	FAssetThumbnailGeneration::~FAssetThumbnailGeneration()
	{
		// Module teardown is the only production wait. UI frames merely poll.
		Impl->DrainCache();
	}

	auto FAssetThumbnailGeneration::WaitForCacheTasksForTesting() -> void
	{
		Impl->DrainCache();
	}

	auto FAssetThumbnailGeneration::BeginFrame() -> void
	{
		Impl->RendersStartedThisFrame = 0;
		Impl->PumpCache();
	}

	auto FAssetThumbnailGeneration::PollNext() -> FAssetThumbnailStartResult
	{
		FAssetThumbnailStartResult Result;
		Impl->PumpCache();
		auto& Reads = Impl->Reads;
		if (!Reads.empty() && Reads.front().bComplete)
		{
			auto Read = std::move(Reads.front());
			Reads.pop_front();
			Result.bProgressed = true;
			Impl->RetainedBytes -= Read.ReservedBytes;
			if (!Impl->Scheduler.Transition(Read.Job, EThumbnailJobState::Loading,
					Read.Result.bHit ? EThumbnailJobState::PixelsReady : EThumbnailJobState::Loading)) return Result;
			if (Read.Result.bHit)
			{
				++Impl->Stats.DiskHits;
				Result.Pixels = std::move(Read.Result.Pixels);
				Result.Job = std::move(Read.Job);
			}
			else
			{
				if (Read.Result.bInvalid) ++Impl->Stats.Retries;
				++Impl->Stats.Loads;
				Result.Job = std::move(Read.Job);
			}
			return Result;
		}
		if (Reads.size() >= FImpl::MaximumCacheReads) return Result;
		auto Job = Impl->Scheduler.TakeNext();
		if (!Job) return Result;
		Result.bProgressed = true;
		++Impl->Stats.Jobs;
		const auto& Output = Job->GenerationRequest.KeyInput.Output;
		const uint64 PixelCount = static_cast<uint64>(Output.Width) * Output.Height;
		if (PixelCount == 0 || PixelCount > Impl->Budgets.CpuPixelBudgetBytes / 4
			|| Impl->RetainedBytes > Impl->Budgets.CpuPixelBudgetBytes - PixelCount * 4)
		{
			++Impl->Stats.Loads;
			Result.Job = std::move(*Job);
			return Result;
		}
		Impl->RetainedBytes += PixelCount * 4;
		Reads.push_back({.Job = std::move(*Job), .ReservedBytes = PixelCount * 4});
		Impl->PumpCache();
		return Result;
	}

	auto FAssetThumbnailGeneration::CompleteLoad(
		FAssetThumbnailScheduledRequest& Job,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EThumbnailJobState::Loading, Error)) return false;
		if (!Impl->Scheduler.Transition(Job, EThumbnailJobState::Loading,
				EThumbnailJobState::WaitingForResources))
			return false;

		return true;
	}

	auto FAssetThumbnailGeneration::BeginRender(
		FAssetThumbnailScheduledRequest& Job,
		bool bResourcesReady,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EThumbnailJobState::WaitingForResources, Error))
			return false;
		if (!bResourcesReady)
		{
			++Impl->Stats.ResourceWaits;
			return Impl->Scheduler.Transition(Job,
				EThumbnailJobState::WaitingForResources,
				EThumbnailJobState::WaitingForResources);
		}
		if (Impl->RendersStartedThisFrame >= Impl->Budgets.MaximumRendersPerFrame)
			return false;
		if (!Impl->Scheduler.Transition(Job,
				EThumbnailJobState::WaitingForResources,
				EThumbnailJobState::Rendering))
			return false;

		++Impl->RendersStartedThisFrame;
		++Impl->Stats.Renders;
		return true;
	}

	auto FAssetThumbnailGeneration::CompleteCapture(
		const FAssetThumbnailScheduledRequest& Job,
		FByteView Pixels, uint32 Width, uint32 Height, std::string_view Error) -> bool
	{
		if (Impl->Fail(Job, EThumbnailJobState::Rendering, Error)) return false;
		const uint64 PixelCount = static_cast<uint64>(Width) * Height;
		if (PixelCount == 0 || PixelCount > Impl->Budgets.CpuPixelBudgetBytes / 4 || Pixels.size() != PixelCount * 4)
			Error = "Rendered-thumbnail pixels violate the RGBA8 output or CPU budget.";
		if (Impl->Fail(Job, EThumbnailJobState::Rendering, Error)) return false;
		if (!Impl->Scheduler.Transition(Job, EThumbnailJobState::Rendering, EThumbnailJobState::PixelsReady)) return false;
		++Impl->Stats.Readbacks;
		Impl->QueueWrite(Job, Pixels, Width, Height);
		return true;
	}

	auto FAssetThumbnailGeneration::Cancel(const FAssetThumbnailScheduledRequest& Job) -> void
	{
		if (!Job.GenerationRequest.Cancellation.IsCancelled())
		{
			Impl->Scheduler.Cancel(Job.GenerationRequest.KeyInput.Asset.AssetPath);
			++Impl->Stats.Cancellations;
		}
	}

	auto FAssetThumbnailGeneration::RecordRetry() -> void
	{
		++Impl->Stats.Retries;
	}

	auto FAssetThumbnailGeneration::GetStats() const -> FAssetThumbnailGenerationStats
	{
		return Impl->Stats;
	}
} // namespace Durin
