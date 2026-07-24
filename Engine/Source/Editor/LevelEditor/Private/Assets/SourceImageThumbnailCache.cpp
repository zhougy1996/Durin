#include "Assets/SourceImageThumbnailCache.h"

#include "Assets/SourceImageThumbnailDecoder.h"
#include "Assets/SourceImageThumbnailDiskCache.h"

#include "DynamicRHI.h"
#include "MonaCoreGlobals.h"
#include "MonaUIBackend.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Threading/Task.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 ThumbnailMaximumDimension = 256;
		constexpr uint32 MaximumConcurrentDecodes = 4;
		constexpr uint32 MaximumUploadsPerFrame = 2;
		constexpr uint64 ThumbnailMemoryBudget = 64ull * 1024ull * 1024ull;

		// Transfers decoded pixels from a worker to the game-thread cache.
		struct FDecodeResult
		{
			std::string PhysicalPath;
			uint64 Serial = 0;
			FDecodedSourceImageThumbnail Thumbnail;
			std::string Error;
			bool bSucceeded = false;
		};

		// Transfers an uploaded texture and byte cost into a cache entry.
		struct FUploadResult
		{
			std::string PhysicalPath;
			uint64 Serial = 0;
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			bool bHasTransparency = false;
		};

		// Keeps worker queues alive independently from the public cache object.
		struct FAsyncThumbnailState
		{
			std::mutex Mutex;
			std::vector<FDecodeResult> DecodedResults;
			std::vector<FUploadResult> UploadedResults;
			std::shared_ptr<FSourceImageThumbnailDiskCache> DiskCache = std::make_shared<FSourceImageThumbnailDiskCache>();
		};
	} // namespace

	// Owns asynchronous thumbnail state and coordinates decode and upload queues.
	struct FSourceImageThumbnailCache::FImpl
	{
		// Owns one thumbnail's state, texture, revision, and recency metadata.
		struct FEntry
		{
			ESourceImageThumbnailState State = ESourceImageThumbnailState::NotRequested;
			uintmax_t FileSize = 0;
			std::filesystem::file_time_type LastWriteTime{};
			uint64 Serial = 1;
			uint64 LastUsedFrame = 0;
			uint64 RequestedFrame = 0;
			FTextureRHIRef Texture;
			uint32 Width = 0;
			uint32 Height = 0;
			bool bHasTransparency = false;
			bool bVisible = false;
			std::string Error;
		};

		// Captures the source identity required by one queued decode request.
		struct FPendingRequest
		{
			std::string PhysicalPath;
			bool bVisible = false;
		};

		std::unordered_map<std::string, FEntry> Entries;
		std::vector<FPendingRequest> PendingRequests;
		std::vector<FDecodeResult> PendingUploads;
		std::shared_ptr<FAsyncThumbnailState> AsyncState = std::make_shared<FAsyncThumbnailState>();
		uint64 FrameNumber = 0;
		uint32 ActiveDecodeCount = 0;

		auto UnregisterTexture(FEntry& Entry) -> void
		{
			if (Entry.Texture && Mona::GActiveUIBackend)
				Mona::GActiveUIBackend->UnregisterTexture(Entry.Texture);
			Entry.Texture = nullptr;
		}

		auto ResetEntry(FEntry& Entry, uintmax_t FileSize, const std::filesystem::file_time_type& LastWriteTime) -> void
		{
			UnregisterTexture(Entry);
			Entry.State = ESourceImageThumbnailState::NotRequested;
			Entry.FileSize = FileSize;
			Entry.LastWriteTime = LastWriteTime;
			++Entry.Serial;
			Entry.Width = 0;
			Entry.Height = 0;
			Entry.bHasTransparency = false;
			Entry.Error.clear();
		}

		auto DrainUploadResults() -> void
		{
			std::vector<FUploadResult> Results;
			{
				std::lock_guard Lock(AsyncState->Mutex);
				Results.swap(AsyncState->UploadedResults);
			}
			for (FUploadResult& Result : Results)
			{
				auto It = Entries.find(Result.PhysicalPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial || It->second.State != ESourceImageThumbnailState::Uploading)
					continue;
				FEntry& Entry = It->second;
				if (!Result.Texture || !Mona::GActiveUIBackend)
				{
					Entry.State = ESourceImageThumbnailState::Failed;
					Entry.Error = "Unable to create the preview texture.";
					continue;
				}
				Mona::GActiveUIBackend->RegisterTexture(Result.Texture);
				Entry.Texture = std::move(Result.Texture);
				Entry.Width = Result.Width;
				Entry.Height = Result.Height;
				Entry.bHasTransparency = Result.bHasTransparency;
				Entry.State = ESourceImageThumbnailState::Ready;
				Entry.LastUsedFrame = FrameNumber;
			}
		}

		auto DrainDecodeResults() -> void
		{
			std::vector<FDecodeResult> Results;
			{
				std::lock_guard Lock(AsyncState->Mutex);
				Results.swap(AsyncState->DecodedResults);
			}
			for (FDecodeResult& Result : Results)
			{
				if (ActiveDecodeCount > 0) --ActiveDecodeCount;
				auto It = Entries.find(Result.PhysicalPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial || It->second.State != ESourceImageThumbnailState::Decoding)
					continue;
				if (!Result.bSucceeded)
				{
					It->second.State = ESourceImageThumbnailState::Failed;
					It->second.Error = std::move(Result.Error);
					continue;
				}
				PendingUploads.push_back(std::move(Result));
			}
		}

		auto SubmitUploads() -> void
		{
			const uint32 UploadCount = std::min<uint32>(MaximumUploadsPerFrame, static_cast<uint32>(PendingUploads.size()));
			for (uint32 Index = 0; Index < UploadCount; ++Index)
			{
				FDecodeResult Result = std::move(PendingUploads.front());
				PendingUploads.erase(PendingUploads.begin());
				auto It = Entries.find(Result.PhysicalPath);
				if (It == Entries.end() || It->second.Serial != Result.Serial || It->second.State != ESourceImageThumbnailState::Decoding)
					continue;
				It->second.State = ESourceImageThumbnailState::Uploading;
				const std::weak_ptr<FAsyncThumbnailState> WeakState = AsyncState;
				const std::string PhysicalPath = std::move(Result.PhysicalPath);
				const uint64 Serial = Result.Serial;
				const uint32 Width = Result.Thumbnail.Width;
				const uint32 Height = Result.Thumbnail.Height;
				const bool bHasTransparency = Result.Thumbnail.bHasTransparency;
				auto Pixels = std::make_shared<std::vector<uint8>>(std::move(Result.Thumbnail.Pixels));
				ENQUEUE_RENDER_COMMAND(UploadSourceImageThumbnail)([WeakState, PhysicalPath, Serial, Width, Height, bHasTransparency, Pixels](FRHICommandListImmediate& CommandList) {
					FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create2D("SourceImageThumbnail", Width, Height, EPixelFormat::SRGBA8_UNORM);
					Desc.AddFlags(ETextureCreateFlags::ShaderResource);
					FTextureRHIRef Texture = GDynamicRHI->RHICreateTexture(CommandList, Desc);
					if (Texture)
					{
						const FUpdateTextureRegion2D Region(0, 0, 0, 0, Width, Height);
						GDynamicRHI->RHIUpdateTexture2D(CommandList, Texture, 0, 0, Region, Width * 4, Pixels->data());
					}
					if (const std::shared_ptr<FAsyncThumbnailState> State = WeakState.lock())
					{
						std::lock_guard Lock(State->Mutex);
						State->UploadedResults.push_back({PhysicalPath, Serial, std::move(Texture), Width, Height, bHasTransparency});
					}
				});
			}
		}

		auto LaunchPendingDecodes() -> void
		{
			std::ranges::stable_sort(PendingRequests, [](const FPendingRequest& A, const FPendingRequest& B) { return A.bVisible && !B.bVisible; });
			for (const FPendingRequest& Request : PendingRequests)
			{
				if (ActiveDecodeCount >= MaximumConcurrentDecodes) break;
				auto It = Entries.find(Request.PhysicalPath);
				if (It == Entries.end() || It->second.State != ESourceImageThumbnailState::Queued) continue;
				FEntry& Entry = It->second;
				Entry.State = ESourceImageThumbnailState::Decoding;
				const uint64 Serial = Entry.Serial;
				const uintmax_t FileSize = Entry.FileSize;
				const std::filesystem::file_time_type LastWriteTime = Entry.LastWriteTime;
				const std::string PhysicalPath = Request.PhysicalPath;
				const std::weak_ptr<FAsyncThumbnailState> WeakState = AsyncState;
				const std::shared_ptr<FSourceImageThumbnailDiskCache> DiskCache = AsyncState->DiskCache;
				const FTaskHandle Task = LaunchTask("DecodeSourceImageThumbnail", [WeakState, DiskCache, PhysicalPath, FileSize, LastWriteTime, Serial] {
					FDecodeResult Result;
					Result.PhysicalPath = PhysicalPath;
					Result.Serial = Serial;
					Result.bSucceeded = DiskCache->LoadOrGenerate(PhysicalPath, FileSize, LastWriteTime, Result.Thumbnail, Result.Error);
					if (const std::shared_ptr<FAsyncThumbnailState> State = WeakState.lock())
					{
						std::lock_guard Lock(State->Mutex);
						State->DecodedResults.push_back(std::move(Result));
					}
				});
				if (Task.IsValid())
					++ActiveDecodeCount;
				else
				{
					Entry.State = ESourceImageThumbnailState::Failed;
					Entry.Error = "The background task queue is unavailable.";
				}
			}
			PendingRequests.clear();
		}

		auto EvictToBudget() -> void
		{
			auto EntryBytes = [](const FEntry& Entry) -> uint64 { return Entry.Texture ? static_cast<uint64>(Entry.Width) * Entry.Height * 4 : 0; };
			uint64 TotalBytes = 0;
			for (const auto& [Path, Entry] : Entries) TotalBytes += EntryBytes(Entry);
			while (TotalBytes > ThumbnailMemoryBudget)
			{
				auto Candidate = Entries.end();
				for (auto It = Entries.begin(); It != Entries.end(); ++It)
				{
					if (!It->second.Texture || It->second.bVisible) continue;
					if (Candidate == Entries.end() || It->second.LastUsedFrame < Candidate->second.LastUsedFrame) Candidate = It;
				}
				if (Candidate == Entries.end()) break;
				TotalBytes -= EntryBytes(Candidate->second);
				UnregisterTexture(Candidate->second);
				Entries.erase(Candidate);
			}
		}
	};

	FSourceImageThumbnailCache::FSourceImageThumbnailCache()
		: Impl(std::make_unique<FImpl>())
	{
	}

	FSourceImageThumbnailCache::~FSourceImageThumbnailCache()
	{
		Clear();
		Impl->AsyncState.reset();
	}

	auto FSourceImageThumbnailCache::BeginFrame() -> void
	{
		++Impl->FrameNumber;
		for (auto& [Path, Entry] : Impl->Entries) Entry.bVisible = false;
		Impl->DrainUploadResults();
		Impl->DrainDecodeResults();
		Impl->SubmitUploads();
	}

	auto FSourceImageThumbnailCache::Request(std::string_view PhysicalPath, uintmax_t FileSize, const std::filesystem::file_time_type& LastWriteTime, bool bVisible) -> void
	{
		const std::string Key(PhysicalPath);
		auto [It, bInserted] = Impl->Entries.try_emplace(Key);
		FImpl::FEntry& Entry = It->second;
		if (bInserted)
		{
			Entry.FileSize = FileSize;
			Entry.LastWriteTime = LastWriteTime;
		}
		else if (Entry.FileSize != FileSize || Entry.LastWriteTime != LastWriteTime)
			Impl->ResetEntry(Entry, FileSize, LastWriteTime);
		Entry.bVisible |= bVisible;
		Entry.LastUsedFrame = Impl->FrameNumber;
		if (Entry.RequestedFrame == Impl->FrameNumber) return;
		Entry.RequestedFrame = Impl->FrameNumber;
		if (Entry.State == ESourceImageThumbnailState::NotRequested)
		{
			Entry.State = ESourceImageThumbnailState::Queued;
			Impl->PendingRequests.push_back({Key, bVisible});
		}
		else if (Entry.State == ESourceImageThumbnailState::Queued && bVisible)
		{
			auto Pending = std::ranges::find_if(Impl->PendingRequests, [&](const FImpl::FPendingRequest& Request) { return Request.PhysicalPath == Key; });
			if (Pending != Impl->PendingRequests.end())
				Pending->bVisible = true;
			else
				Impl->PendingRequests.push_back({Key, true});
		}
		else if (Entry.State == ESourceImageThumbnailState::Queued)
		{
			const auto Pending = std::ranges::find_if(Impl->PendingRequests, [&](const FImpl::FPendingRequest& Request) { return Request.PhysicalPath == Key; });
			if (Pending == Impl->PendingRequests.end()) Impl->PendingRequests.push_back({Key, false});
		}
	}

	auto FSourceImageThumbnailCache::Find(std::string_view PhysicalPath) const -> FSourceImageThumbnailView
	{
		const auto It = Impl->Entries.find(std::string(PhysicalPath));
		if (It == Impl->Entries.end()) return {};
		const FImpl::FEntry& Entry = It->second;
		return {Entry.State, Entry.Texture, Entry.Width, Entry.Height, Entry.bHasTransparency, Entry.Error};
	}

	auto FSourceImageThumbnailCache::EndFrame() -> void
	{
		Impl->LaunchPendingDecodes();
		Impl->EvictToBudget();
	}

	auto FSourceImageThumbnailCache::CancelPendingRequests() -> void
	{
		Impl->PendingRequests.clear();
		Impl->PendingUploads.clear();
		for (auto& [Path, Entry] : Impl->Entries)
			if (Entry.State == ESourceImageThumbnailState::Queued || Entry.State == ESourceImageThumbnailState::Decoding || Entry.State == ESourceImageThumbnailState::Uploading)
			{
				Entry.State = ESourceImageThumbnailState::NotRequested;
				++Entry.Serial;
			}
	}

	auto FSourceImageThumbnailCache::Clear() -> void
	{
		CancelPendingRequests();
		for (auto& [Path, Entry] : Impl->Entries) Impl->UnregisterTexture(Entry);
		Impl->Entries.clear();
	}
} // namespace Durin
