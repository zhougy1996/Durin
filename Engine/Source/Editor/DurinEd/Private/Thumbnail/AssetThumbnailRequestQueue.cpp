#include "Thumbnail/AssetThumbnailRequestQueue.h"

#include "Thumbnail/AssetThumbnailKey.h"

namespace Durin::Editor
{
	struct FAssetThumbnailRequestQueue::FImpl
	{
		struct FEntry
		{
			std::string CacheKey;
			EAssetThumbnailState State = EAssetThumbnailState::NotRequested;
			FAssetThumbnailGenerationRequest GenerationRequest;
			std::string Diagnostic;
			uint64 RequestSerial = 0;
		};

		DThumbnailManager& Registry;
		FAssetThumbnailBudgets Budgets;
		std::unordered_map<std::string, FEntry> Entries;
		std::vector<FAssetThumbnailScheduledRequest> Queue;
		bool bShuttingDown = false;

		auto RemoveQueuedJob(std::string_view CacheKey) -> void
		{
			std::erase_if(Queue, [CacheKey](const FAssetThumbnailScheduledRequest& Job) {
				return Job.CacheKey == CacheKey;
			});
		}
	};

	FAssetThumbnailRequestQueue::FAssetThumbnailRequestQueue(
		DThumbnailManager& Registry,
		FAssetThumbnailBudgets Budgets
	)
		: Impl(std::make_unique<FImpl>(FImpl{
			.Registry = Registry,
			.Budgets = Budgets}))
	{
	}

	FAssetThumbnailRequestQueue::~FAssetThumbnailRequestQueue()
	{
		Shutdown();
	}

	auto FAssetThumbnailRequestQueue::Request(
		const FAssetThumbnailRequest& Request,
		std::string& OutError
	) -> bool
	{
		if (Impl->bShuttingDown)
		{
			OutError = "Thumbnail requests are closed during shutdown.";
			return false;
		}
		const FThumbnailRendererHandle Handle = Impl->Registry.Find(Request.Asset.AssetClassName);
		if (!Handle)
		{
			OutError = std::format(
				"No thumbnail renderer is registered for asset class {}.",
				Request.Asset.AssetClassName);
			return false;
		}

		FAssetThumbnailGenerationRequest GenerationRequest;
		FThumbnailRenderingInfo Registration;
		if (!Impl->Registry.Capture(
				Request,
				Handle.Generation,
				GenerationRequest,
				Registration,
				OutError))
		{
			FImpl::FEntry& Entry = Impl->Entries[Request.Asset.VirtualPath.ToString()];
			if (Request.RequestSerial >= Entry.GenerationRequest.RequestSerial)
			{
				Entry.GenerationRequest.Cancellation.Cancel();
				Impl->RemoveQueuedJob(Entry.CacheKey);
				Entry = {};
				Entry.State = EAssetThumbnailState::Invalid;
				Entry.GenerationRequest.RequestSerial = Request.RequestSerial;
				Entry.Diagnostic = OutError.empty()
					? "The thumbnail renderer rejected the asset."
					: OutError;
				Entry.RequestSerial = Request.RequestSerial;
			}
			return false;
		}

		GenerationRequest.KeyInput.Asset = Request.Asset;
		GenerationRequest.KeyInput.RendererName = Registration.RendererName;
		GenerationRequest.KeyInput.GeneratorSchemaVersion = Registration.GeneratorSchemaVersion;
		GenerationRequest.RendererGeneration = Handle.Generation;
		GenerationRequest.RequestSerial = Request.RequestSerial;
		const std::string CacheKey = BuildAssetThumbnailCacheKey(GenerationRequest.KeyInput);
		const std::string AssetPath = Request.Asset.VirtualPath.ToString();
		auto Existing = Impl->Entries.find(AssetPath);
		if (Existing != Impl->Entries.end())
		{
			FImpl::FEntry& Entry = Existing->second;
			if (Request.RequestSerial < Entry.RequestSerial)
			{
				OutError = "A newer thumbnail request is already active for this asset.";
				return false;
			}
			if (Entry.CacheKey == CacheKey)
			{
				if (Request.RequestSerial == Entry.RequestSerial)
				{
					if (Entry.State == EAssetThumbnailState::Queued
						&& Request.Priority == EAssetThumbnailPriority::Visible)
					{
						for (FAssetThumbnailScheduledRequest& Job : Impl->Queue)
							if (Job.CacheKey == CacheKey)
								Job.Priority = EAssetThumbnailPriority::Visible;
					}
					OutError.clear();
					return true;
				}
				if (Entry.State == EAssetThumbnailState::Queued)
				{
					Entry.RequestSerial = Request.RequestSerial;
					Entry.GenerationRequest.RequestSerial = Request.RequestSerial;
					for (FAssetThumbnailScheduledRequest& Job : Impl->Queue)
					{
						if (Job.CacheKey != CacheKey) continue;
						Job.GenerationRequest.RequestSerial = Request.RequestSerial;
						if (Request.Priority == EAssetThumbnailPriority::Visible)
							Job.Priority = EAssetThumbnailPriority::Visible;
					}
					OutError.clear();
					return true;
				}
			}
			Entry.GenerationRequest.Cancellation.Cancel();
			Impl->RemoveQueuedJob(Entry.CacheKey);
			Impl->Entries.erase(Existing);
		}

		if (Impl->Queue.size() >= Impl->Budgets.MaximumQueuedJobs)
		{
			OutError = "The thumbnail request queue budget is exhausted.";
			return false;
		}

		FImpl::FEntry Entry;
		Entry.CacheKey = CacheKey;
		Entry.State = EAssetThumbnailState::Queued;
		Entry.GenerationRequest = GenerationRequest;
		Entry.RequestSerial = Request.RequestSerial;
		Impl->Entries.emplace(AssetPath, std::move(Entry));
		Impl->Queue.push_back({
			.CacheKey = CacheKey,
			.Priority = Request.Priority,
			.GenerationRequest = std::move(GenerationRequest)});
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailRequestQueue::Find(const FAssetPath& AssetPath) const -> FAssetThumbnailView
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return {};
		const FImpl::FEntry& Entry = It->second;
		return {
			.State = Entry.State,
			.Diagnostic = Entry.Diagnostic,
			.RequestSerial = Entry.RequestSerial};
	}

	auto FAssetThumbnailRequestQueue::TakeNext() -> std::optional<FAssetThumbnailScheduledRequest>
	{
		return TakeNext(false);
	}

	auto FAssetThumbnailRequestQueue::TakeNextGeneratedPixels()
		-> std::optional<FAssetThumbnailScheduledRequest>
	{
		return TakeNext(true);
	}

	auto FAssetThumbnailRequestQueue::TakeNext(bool bGeneratedPixelsOnly)
		-> std::optional<FAssetThumbnailScheduledRequest>
	{
		if (Impl->bShuttingDown || Impl->Queue.empty()) return std::nullopt;
		auto IsEligible = [bGeneratedPixelsOnly](
			const FAssetThumbnailScheduledRequest& Job) {
			return !bGeneratedPixelsOnly || Job.GenerationRequest.GeneratedPixels != nullptr;
		};
		auto Selected = std::ranges::find_if(
			Impl->Queue,
			[&](const FAssetThumbnailScheduledRequest& Job) {
				return IsEligible(Job)
					&& Job.Priority == EAssetThumbnailPriority::Visible;
			});
		if (Selected == Impl->Queue.end())
			Selected = std::ranges::find_if(Impl->Queue, IsEligible);
		if (Selected == Impl->Queue.end()) return std::nullopt;
		FAssetThumbnailScheduledRequest Job = std::move(*Selected);
		Impl->Queue.erase(Selected);
		const std::string AssetPath = Job.GenerationRequest.KeyInput.Asset.VirtualPath.ToString();
		const auto Entry = Impl->Entries.find(AssetPath);
		const FThumbnailRendererHandle CurrentRenderer = Impl->Registry.Find(
			Job.GenerationRequest.KeyInput.Asset.AssetClassName);
		const bool bMatchesCurrentEntry = Entry != Impl->Entries.end()
			&& Entry->second.CacheKey == Job.CacheKey
			&& Entry->second.GenerationRequest.RequestSerial
				== Job.GenerationRequest.RequestSerial
			&& Entry->second.GenerationRequest.RendererGeneration
				== Job.GenerationRequest.RendererGeneration;
		if (!bMatchesCurrentEntry
			|| Job.GenerationRequest.Cancellation.IsCancelled()
			|| !CurrentRenderer
			|| CurrentRenderer.Generation != Job.GenerationRequest.RendererGeneration)
		{
			Job.GenerationRequest.Cancellation.Cancel();
			if (bMatchesCurrentEntry) Impl->Entries.erase(Entry);
			return TakeNext(bGeneratedPixelsOnly);
		}
		Entry->second.State = EAssetThumbnailState::Loading;
		return Job;
	}

	auto FAssetThumbnailRequestQueue::Transition(
		const FAssetThumbnailScheduledRequest& Job,
		EAssetThumbnailState ExpectedState,
		EAssetThumbnailState NextState,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::string_view Diagnostic
	) -> bool
	{
		if (Impl->bShuttingDown || Job.GenerationRequest.Cancellation.IsCancelled()) return false;
		const std::string AssetPath = Job.GenerationRequest.KeyInput.Asset.VirtualPath.ToString();
		const auto It = Impl->Entries.find(AssetPath);
		if (It == Impl->Entries.end()) return false;
		FImpl::FEntry& Entry = It->second;
		const FThumbnailRendererHandle CurrentRenderer =
			Impl->Registry.Find(Job.GenerationRequest.KeyInput.Asset.AssetClassName);
		if (Entry.CacheKey != Job.CacheKey
			|| Entry.State != ExpectedState
			|| Entry.RequestSerial != Job.GenerationRequest.RequestSerial
			|| Entry.GenerationRequest.RendererGeneration != Job.GenerationRequest.RendererGeneration
			|| !CurrentRenderer
			|| CurrentRenderer.Generation != Job.GenerationRequest.RendererGeneration
			|| Entry.GenerationRequest.KeyInput.Asset != Job.GenerationRequest.KeyInput.Asset
			|| (Entry.GenerationRequest.AssetRevision != 0
				&& Entry.GenerationRequest.AssetRevision != AssetRevision)
			|| (Entry.GenerationRequest.ResourceRevision != 0
				&& Entry.GenerationRequest.ResourceRevision != ResourceRevision))
			return false;

		if (AssetRevision != 0) Entry.GenerationRequest.AssetRevision = AssetRevision;
		if (ResourceRevision != 0) Entry.GenerationRequest.ResourceRevision = ResourceRevision;
		Entry.State = NextState;
		Entry.Diagnostic = std::string(Diagnostic);
		return true;
	}

	auto FAssetThumbnailRequestQueue::Cancel(const FAssetPath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return;
		It->second.GenerationRequest.Cancellation.Cancel();
		Impl->RemoveQueuedJob(It->second.CacheKey);
		Impl->Entries.erase(It);
	}

	auto FAssetThumbnailRequestQueue::CancelAll() -> void
	{
		for (auto& [AssetPath, Entry] : Impl->Entries)
			Entry.GenerationRequest.Cancellation.Cancel();
		Impl->Queue.clear();
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
		return Impl->Queue.size();
	}

	auto FAssetThumbnailRequestQueue::IsShuttingDown() const -> bool
	{
		return Impl->bShuttingDown;
	}

} // namespace Durin::Editor
