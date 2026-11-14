#include "Thumbnail/AssetThumbnailCache.h"

#include "Image/ImageEncoder.h"
#include "Image/ImageDecoder.h"
#include "Threading/Task.h"

namespace Durin
{
	struct FAssetThumbnailCache::FImpl
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
			bool bSucceeded = false;
			bool bInvalid = false;
			uint64 Evictions = 0;
		};
		struct FCacheRead
		{
			std::string Key;
			FAssetThumbnailOutputSettings Output;
			std::shared_ptr<TPromise<FThumbnailCacheReadResult>> Promise;
			uint64 ReservedBytes = 0;
		};

		struct FCacheWrite
		{
			std::string Key;
			FByteBuffer Bytes;
			uint32 Width = 0;
			uint32 Height = 0;
		};

		FImpl(FThumbnailObjectStoreSettings Settings, uint64 InBudget)
			: Cache(std::make_shared<FCacheState>(std::move(Settings)))
			, CpuBudgetBytes(InBudget)
		{
			Cache->Settings.bBatchIndexWrites = true;
		}

		std::shared_ptr<FCacheState> Cache;
		uint64 CpuBudgetBytes;
		FThumbnailCacheStats Stats;
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
				if (Encoded.Bytes.empty())
					++Stats.CacheWriteFailures;
				else if (Encoded.Bytes.size() > CpuBudgetBytes
						 || RetainedBytes > CpuBudgetBytes - Encoded.Bytes.size())
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
			EncodeTask = Tasks::LaunchTask(Tasks::ETaskExecutor::Worker, {.DebugName = "EncodeAssetThumbnailCache"}, [Write = std::move(Write)]() mutable {
				FByteBuffer Encoded;
				(void)Image::EncodeRgba8Png(Write.Bytes, Write.Width, Write.Height, Encoded);
				Write.Bytes = std::move(Encoded);
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
					(void)ActiveRead->Promise->TrySetValue({std::move(Result.Pixels), Result.bInvalid});
					RetainedBytes -= ActiveRead->ReservedBytes;
					Reads.pop_front();
					ActiveRead = nullptr;
				}
				else if (!bMaintenanceTask)
				{
					RetainedBytes -= ActiveWriteBytes;
					bNeedsIndexFlush |= Result.bSucceeded;
					if (Result.bSucceeded)
						++Stats.CacheWrites;
					else
						++Stats.CacheWriteFailures;
				}
				bMaintenanceTask = false;
			}
			const auto Next = Reads.begin();
			if (Next != Reads.end())
			{
				ActiveRead = &*Next;
				FCacheRead& Read = *Next;
				CacheTask = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {.DebugName = "ReadAssetThumbnailCache"}, [State = Cache, Key = Read.Key, Output = Read.Output] {
					FCacheResult Result;
					auto& Store = State->GetStore();
					FByteBuffer Encoded;
					if (Store.Load(Key, Encoded) == EThumbnailObjectLoadResult::Hit)
					{
						const uint64 PixelCount = static_cast<uint64>(Output.Width) * Output.Height;
						if (auto Decoded = Image::DecodeImageFromMemory(Encoded, {.MaximumEncodedBytes = Encoded.size(), .MaximumDecodedPixels = PixelCount}); Decoded
																																							   && Decoded->Width == Output.Width && Decoded->Height == Output.Height
																																							   && Decoded->Pixels.size() == PixelCount * 4)
						{
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
					CacheTask = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {.DebugName = "FlushAssetThumbnailIndex"}, [State = Cache] {
						State->GetStore().Flush();
						return FCacheResult{};
					});
				}
				return;
			}
			FCacheWrite Write = std::move(Writes.front());
			Writes.pop_front();
			ActiveWriteBytes = Write.Bytes.size();
			CacheTask = Tasks::LaunchTask(Tasks::ETaskExecutor::BlockingIO, {.DebugName = "SaveAssetThumbnailCache"}, [State = Cache, Write = std::move(Write)] {
				FCacheResult Result;
				const FByteView Bytes = Write.Bytes;
				auto& Store = State->GetStore();
				Result.bSucceeded = Store.Store(Write.Key, Bytes);
				Result.Evictions = Store.GetStats().Evictions;
				return Result;
			});
		}

		auto QueueWrite(std::string Key, FByteView Bytes, uint32 Width, uint32 Height) -> void
		{
			// Persistence is optional. A slow disk must not accumulate unlimited pixels
			// or hold back display and capture admission.
			if (Writes.size() + PendingEncodes.size() + (EncodeTask.IsValid() ? 1u : 0u) >= MaximumQueuedSaves
				|| Bytes.size() > CpuBudgetBytes
				|| RetainedBytes > CpuBudgetBytes - Bytes.size())
			{
				++Stats.CacheWritesSkipped;
				return;
			}
			RetainedBytes += Bytes.size();
			PendingEncodes.push_back({std::move(Key), FByteBuffer(Bytes.begin(), Bytes.end()), Width, Height});
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
	};

	FAssetThumbnailCache::FAssetThumbnailCache(FThumbnailObjectStoreSettings Settings, uint64 Budget)
		: Impl(std::make_unique<FImpl>(std::move(Settings), Budget))
	{
	}
	FAssetThumbnailCache::~FAssetThumbnailCache() { Impl->DrainCache(); }
	auto FAssetThumbnailCache::Tick() -> void { Impl->PumpCache(); }
	auto FAssetThumbnailCache::WaitForTasksForTesting() -> void { Impl->DrainCache(); }
	auto FAssetThumbnailCache::GetRetainedBytes() const -> uint64 { return Impl->RetainedBytes; }
	auto FAssetThumbnailCache::GetStats() const -> FThumbnailCacheStats { return Impl->Stats; }
	auto FAssetThumbnailCache::Load(std::string Key, FAssetThumbnailOutputSettings Output) -> TFuture<FThumbnailCacheReadResult>
	{
		auto Promise = std::make_shared<TPromise<FThumbnailCacheReadResult>>();
		auto Future = Promise->GetFuture();
		const uint64 PixelCount = uint64(Output.Width) * Output.Height;
		const uint64 Bytes = PixelCount <= Impl->CpuBudgetBytes / 4 ? PixelCount * 4 : 0;
		if (!Bytes || Bytes > Impl->CpuBudgetBytes || Impl->RetainedBytes > Impl->CpuBudgetBytes - Bytes || Impl->Reads.size() >= FImpl::MaximumCacheReads)
			(void)Promise->TrySetValue({});
		else
		{
			Impl->RetainedBytes += Bytes;
			Impl->Reads.push_back({std::move(Key), Output, std::move(Promise), Bytes});
			Impl->PumpCache();
		}
		return Future;
	}
	auto FAssetThumbnailCache::Save(std::string Key, FByteView Pixels, uint32 Width, uint32 Height) -> void
	{
		Impl->QueueWrite(std::move(Key), Pixels, Width, Height);
	}
} // namespace Durin
