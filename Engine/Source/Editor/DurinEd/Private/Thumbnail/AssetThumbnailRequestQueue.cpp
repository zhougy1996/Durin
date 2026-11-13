#include "Thumbnail/AssetThumbnailRequestQueue.h"
#include "Thumbnail/ThumbnailRenderer.h"

#include "Thumbnail/AssetThumbnailKey.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin
{
	struct FAssetThumbnailRequestQueue::FImpl
	{
		struct FEntry
		{
			FAssetThumbnailRequest Request;
			std::string CacheKey;
			EThumbnailJobState State = EThumbnailJobState::NotRequested;
			FAssetThumbnailGenerationRequest GenerationRequest;
			std::string Diagnostic;
			uint64 RendererGeneration = 0;
			bool bCaptured = false;
		};

		DThumbnailManager& Registry;
		FAssetThumbnailBudgets Budgets;
		std::unordered_map<std::string, FEntry> Entries;
		std::vector<FAssetThumbnailRequest> PendingRequests;
		bool bShuttingDown = false;

		auto RemovePendingRequest(const FTopLevelAssetPath& AssetPath) -> void
		{
			std::erase_if(PendingRequests,
				[&AssetPath](const FAssetThumbnailRequest& Request) {
					return Request.Asset.AssetPath == AssetPath;
				});
		}

		auto CancelEntry(FEntry& Entry) -> void
		{
			if (Entry.bCaptured) Entry.GenerationRequest.Cancellation.Cancel();
			RemovePendingRequest(Entry.Request.Asset.AssetPath);
		}

		auto PromoteQueuedRequest(
			const FTopLevelAssetPath& AssetPath,
			EAssetThumbnailPriority Priority) -> void
		{
			if (Priority != EAssetThumbnailPriority::Visible) return;
			for (FAssetThumbnailRequest& Pending : PendingRequests)
				if (Pending.Asset.AssetPath == AssetPath)
					Pending.Priority = EAssetThumbnailPriority::Visible;
		}

		auto CaptureNextPending() -> std::optional<FAssetThumbnailScheduledRequest>
		{
			if (PendingRequests.empty()) return std::nullopt;
			auto Selected = std::ranges::find_if(
				PendingRequests,
				[](const FAssetThumbnailRequest& Request) {
					return Request.Priority == EAssetThumbnailPriority::Visible;
				});
			if (Selected == PendingRequests.end()) Selected = PendingRequests.begin();
			FAssetThumbnailRequest Request = std::move(*Selected);
			PendingRequests.erase(Selected);

			const std::string AssetPath = Request.Asset.AssetPath.ToString();
			const auto Existing = Entries.find(AssetPath);
			if (Existing == Entries.end()) return std::nullopt;
			FEntry& Entry = Existing->second;
			const FThumbnailRendererHandle CurrentRenderer =
				Registry.Find(Request.Asset.AssetClassName);
			if (Entry.State != EThumbnailJobState::Queued
				|| Entry.bCaptured
				|| Entry.Request.RequestSerial != Request.RequestSerial
				|| Entry.Request.Asset != Request.Asset
				|| !CurrentRenderer
				|| CurrentRenderer.Generation != Entry.RendererGeneration)
				return std::nullopt;

			auto Captured = Registry.Capture(Request, Entry.RendererGeneration);
			if (!Captured)
			{
				Entry.State = EThumbnailJobState::Invalid;
				Entry.Diagnostic = Captured.error().empty()
					? "The thumbnail renderer rejected the asset."
					: std::move(Captured.error());
				return std::nullopt;
			}

			auto GenerationRequest = std::move(*Captured);
			Entry.CacheKey = BuildAssetThumbnailCacheKey(GenerationRequest.KeyInput);
			Entry.GenerationRequest = GenerationRequest;
			Entry.bCaptured = true;
			Entry.Diagnostic.clear();
			return FAssetThumbnailScheduledRequest{
				.CacheKey = Entry.CacheKey,
				.Priority = Request.Priority,
				.GenerationRequest = std::move(GenerationRequest)};
		}
	};

	FAssetThumbnailRequestQueue::FAssetThumbnailRequestQueue(
		DThumbnailManager& Registry,
		FAssetThumbnailBudgets Budgets)
		: Impl(std::make_unique<FImpl>(FImpl{
			.Registry = Registry,
			.Budgets = Budgets}))
	{
	}

	FAssetThumbnailRequestQueue::~FAssetThumbnailRequestQueue()
	{
		Shutdown();
	}

	auto FAssetThumbnailRequestQueue::BeginFrame() -> void
	{
		// Visibility belongs to the current frame, not to an asset's first request.
		for (auto& Request : Impl->PendingRequests) Request.Priority = EAssetThumbnailPriority::Prefetch;
	}

	auto FAssetThumbnailRequestQueue::Request(
		const FAssetThumbnailRequest& Request
	) -> EThumbnailRequestStatus
	{
		if (Impl->bShuttingDown)
		{
			return EThumbnailRequestStatus::ShuttingDown;
		}
		const FThumbnailRendererHandle Handle =
			Impl->Registry.Find(Request.Asset.AssetClassName);
		if (!Handle)
		{
			return EThumbnailRequestStatus::Unsupported;
		}

		const std::string AssetPath = Request.Asset.AssetPath.ToString();
		if (auto Existing = Impl->Entries.find(AssetPath);
			Existing != Impl->Entries.end())
		{
			FImpl::FEntry& Entry = Existing->second;
			if (Request.RequestSerial < Entry.Request.RequestSerial)
			{
				return EThumbnailRequestStatus::Coalesced;
			}
			if (Request.RequestSerial == Entry.Request.RequestSerial
				&& Request.Asset == Entry.Request.Asset
				&& Handle.Generation == Entry.RendererGeneration)
			{
				Impl->PromoteQueuedRequest(Request.Asset.AssetPath, Request.Priority);
				return EThumbnailRequestStatus::Coalesced;
			}
			Impl->CancelEntry(Entry);
			Impl->Entries.erase(Existing);
		}

		if (Impl->PendingRequests.size()
			>= Impl->Budgets.MaximumQueuedJobs)
		{
			return EThumbnailRequestStatus::QueueFull;
		}

		FImpl::FEntry Entry;
		Entry.Request = Request;
		Entry.State = EThumbnailJobState::Queued;
		Entry.RendererGeneration = Handle.Generation;
		Impl->Entries.emplace(AssetPath, std::move(Entry));
		Impl->PendingRequests.push_back(Request);
		return EThumbnailRequestStatus::Accepted;
	}

	auto FAssetThumbnailRequestQueue::GetState(const FTopLevelAssetPath& AssetPath) const -> EThumbnailJobState
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		return It == Impl->Entries.end() ? EThumbnailJobState::NotRequested : It->second.State;
	}
	auto FAssetThumbnailRequestQueue::GetFailureReason(const FTopLevelAssetPath& AssetPath) const -> std::string_view
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		return It == Impl->Entries.end() ? std::string_view{} : It->second.Diagnostic;
	}

	auto FAssetThumbnailRequestQueue::TakeNext()
		-> std::optional<FAssetThumbnailScheduledRequest>
	{
		if (Impl->bShuttingDown) return std::nullopt;
		// Capture once at admission; there is no intermediate captured-work queue.
		auto Captured = Impl->CaptureNextPending();
		if (!Captured) return std::nullopt;
		FAssetThumbnailScheduledRequest Job = std::move(*Captured);
		const std::string AssetPath =
			Job.GenerationRequest.KeyInput.Asset.AssetPath.ToString();
		const auto Entry = Impl->Entries.find(AssetPath);
		const FThumbnailRendererHandle CurrentRenderer = Impl->Registry.Find(
			Job.GenerationRequest.KeyInput.Asset.AssetClassName);
		const bool bMatchesCurrentEntry = Entry != Impl->Entries.end()
			&& Entry->second.bCaptured
			&& Entry->second.CacheKey == Job.CacheKey
			&& Entry->second.Request.RequestSerial
				== Job.GenerationRequest.RequestSerial
			&& Entry->second.RendererGeneration
				== Job.GenerationRequest.RendererGeneration;
		if (!bMatchesCurrentEntry
			|| Job.GenerationRequest.Cancellation.IsCancelled()
			|| !CurrentRenderer
			|| CurrentRenderer.Generation != Job.GenerationRequest.RendererGeneration)
		{
			Job.GenerationRequest.Cancellation.Cancel();
			if (bMatchesCurrentEntry) Impl->Entries.erase(Entry);
			return std::nullopt;
		}
		Entry->second.State = EThumbnailJobState::Loading;
		return Job;
	}

	auto FAssetThumbnailRequestQueue::Transition(
		const FAssetThumbnailScheduledRequest& Job,
		EThumbnailJobState ExpectedState,
		EThumbnailJobState NextState,
		std::string_view Diagnostic) -> bool
	{
		if (Impl->bShuttingDown || Job.GenerationRequest.Cancellation.IsCancelled())
			return false;
		const std::string AssetPath =
			Job.GenerationRequest.KeyInput.Asset.AssetPath.ToString();
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return false;
		FImpl::FEntry& Entry = It->second;
		const FThumbnailRendererHandle CurrentRenderer =
			Impl->Registry.Find(Job.GenerationRequest.KeyInput.Asset.AssetClassName);
		if (!Entry.bCaptured
			|| Entry.CacheKey != Job.CacheKey
			|| Entry.State != ExpectedState
			|| Entry.Request.RequestSerial != Job.GenerationRequest.RequestSerial
			|| Entry.RendererGeneration != Job.GenerationRequest.RendererGeneration
			|| !CurrentRenderer
			|| CurrentRenderer.Generation != Job.GenerationRequest.RendererGeneration
			|| Entry.Request.Asset != Job.GenerationRequest.KeyInput.Asset)
			return false;
		Entry.State = NextState;
		Entry.Diagnostic = std::string(Diagnostic);
		return true;
	}

	auto FAssetThumbnailRequestQueue::Cancel(const FTopLevelAssetPath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return;
		Impl->CancelEntry(It->second);
		Impl->Entries.erase(It);
	}

	auto FAssetThumbnailRequestQueue::CancelRenderer(uint64 Generation) -> void
	{
		for (auto& [Path, Entry] : Impl->Entries)
			if (Entry.RendererGeneration == Generation) Impl->CancelEntry(Entry);
	}

	auto FAssetThumbnailRequestQueue::CancelAll() -> void
	{
		for (auto& [AssetPath, Entry] : Impl->Entries)
			if (Entry.bCaptured) Entry.GenerationRequest.Cancellation.Cancel();
		Impl->PendingRequests.clear();
		Impl->Entries.clear();
	}

	auto FAssetThumbnailRequestQueue::Shutdown() -> void
	{
		if (Impl->bShuttingDown) return;
		Impl->bShuttingDown = true;
		CancelAll();
	}

	auto FAssetThumbnailRequestQueue::NumQueued() const -> size_t
	{
		return Impl->PendingRequests.size();
	}

	auto FAssetThumbnailRequestQueue::IsShuttingDown() const -> bool
	{
		return Impl->bShuttingDown;
	}
} // namespace Durin
