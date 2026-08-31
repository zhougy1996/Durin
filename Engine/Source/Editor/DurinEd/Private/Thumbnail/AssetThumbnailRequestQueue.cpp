#include "Thumbnail/AssetThumbnailRequestQueue.h"

#include "Thumbnail/AssetThumbnailKey.h"

namespace Durin::Editor
{
	struct FAssetThumbnailRequestQueue::FImpl
	{
		struct FEntry
		{
			FAssetThumbnailRequest Request;
			std::string CacheKey;
			EAssetThumbnailState State = EAssetThumbnailState::NotRequested;
			FAssetThumbnailGenerationRequest GenerationRequest;
			std::string Diagnostic;
			uint64 RendererGeneration = 0;
			bool bCaptured = false;
		};

		DThumbnailManager& Registry;
		FAssetThumbnailBudgets Budgets;
		std::unordered_map<std::string, FEntry> Entries;
		std::vector<FAssetThumbnailRequest> PendingRequests;
		std::vector<FAssetThumbnailScheduledRequest> CapturedQueue;
		bool bShuttingDown = false;

		auto RemovePendingRequest(const FPackagePath& AssetPath) -> void
		{
			std::erase_if(PendingRequests,
				[&AssetPath](const FAssetThumbnailRequest& Request) {
					return Request.Asset.VirtualPath == AssetPath;
				});
		}

		auto RemoveCapturedJob(const FPackagePath& AssetPath) -> void
		{
			std::erase_if(CapturedQueue,
				[&AssetPath](const FAssetThumbnailScheduledRequest& Job) {
					return Job.GenerationRequest.KeyInput.Asset.VirtualPath == AssetPath;
				});
		}

		auto CancelEntry(FEntry& Entry) -> void
		{
			if (Entry.bCaptured) Entry.GenerationRequest.Cancellation.Cancel();
			RemovePendingRequest(Entry.Request.Asset.VirtualPath);
			RemoveCapturedJob(Entry.Request.Asset.VirtualPath);
		}

		auto PromoteQueuedRequest(
			const FPackagePath& AssetPath,
			EAssetThumbnailPriority Priority) -> void
		{
			if (Priority != EAssetThumbnailPriority::Visible) return;
			for (FAssetThumbnailRequest& Pending : PendingRequests)
				if (Pending.Asset.VirtualPath == AssetPath)
					Pending.Priority = EAssetThumbnailPriority::Visible;
			for (FAssetThumbnailScheduledRequest& Captured : CapturedQueue)
				if (Captured.GenerationRequest.KeyInput.Asset.VirtualPath == AssetPath)
					Captured.Priority = EAssetThumbnailPriority::Visible;
		}

		auto CaptureNextPending() -> bool
		{
			if (PendingRequests.empty()) return false;
			auto Selected = std::ranges::find_if(
				PendingRequests,
				[](const FAssetThumbnailRequest& Request) {
					return Request.Priority == EAssetThumbnailPriority::Visible;
				});
			if (Selected == PendingRequests.end()) Selected = PendingRequests.begin();
			FAssetThumbnailRequest Request = std::move(*Selected);
			PendingRequests.erase(Selected);

			const std::string AssetPath = Request.Asset.VirtualPath.ToString();
			const auto Existing = Entries.find(AssetPath);
			if (Existing == Entries.end()) return false;
			FEntry& Entry = Existing->second;
			const FThumbnailRendererHandle CurrentRenderer =
				Registry.Find(Request.Asset.AssetClassName);
			if (Entry.State != EAssetThumbnailState::Queued
				|| Entry.bCaptured
				|| Entry.Request.RequestSerial != Request.RequestSerial
				|| Entry.Request.Asset != Request.Asset
				|| !CurrentRenderer
				|| CurrentRenderer.Generation != Entry.RendererGeneration)
				return false;

			FAssetThumbnailGenerationRequest GenerationRequest;
			FThumbnailRenderingInfo Registration;
			std::string Error;
			if (!Registry.Capture(Request, Entry.RendererGeneration,
					GenerationRequest, Registration, Error))
			{
				Entry.State = EAssetThumbnailState::Invalid;
				Entry.Diagnostic = Error.empty()
					? "The thumbnail renderer rejected the asset."
					: std::move(Error);
				return false;
			}

			GenerationRequest.KeyInput.Asset = Request.Asset;
			GenerationRequest.KeyInput.RendererName = Registration.RendererName;
			GenerationRequest.KeyInput.GeneratorSchemaVersion =
				Registration.GeneratorSchemaVersion;
			GenerationRequest.RendererGeneration = Entry.RendererGeneration;
			GenerationRequest.RequestSerial = Request.RequestSerial;
			Entry.CacheKey = BuildAssetThumbnailCacheKey(GenerationRequest.KeyInput);
			Entry.GenerationRequest = GenerationRequest;
			Entry.bCaptured = true;
			Entry.Diagnostic.clear();
			CapturedQueue.push_back({
				.CacheKey = Entry.CacheKey,
				.Priority = Request.Priority,
				.GenerationRequest = std::move(GenerationRequest)});
			return true;
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

	auto FAssetThumbnailRequestQueue::Request(
		const FAssetThumbnailRequest& Request,
		std::string& OutError) -> bool
	{
		if (Impl->bShuttingDown)
		{
			OutError = "Thumbnail requests are closed during shutdown.";
			return false;
		}
		const FThumbnailRendererHandle Handle =
			Impl->Registry.Find(Request.Asset.AssetClassName);
		if (!Handle)
		{
			OutError = std::format(
				"No thumbnail renderer is registered for asset class {}.",
				Request.Asset.AssetClassName);
			return false;
		}

		const std::string AssetPath = Request.Asset.VirtualPath.ToString();
		if (auto Existing = Impl->Entries.find(AssetPath);
			Existing != Impl->Entries.end())
		{
			FImpl::FEntry& Entry = Existing->second;
			if (Request.RequestSerial < Entry.Request.RequestSerial)
			{
				OutError = "A newer thumbnail request is already active for this asset.";
				return false;
			}
			if (Request.RequestSerial == Entry.Request.RequestSerial
				&& Request.Asset == Entry.Request.Asset
				&& Handle.Generation == Entry.RendererGeneration)
			{
				Impl->PromoteQueuedRequest(Request.Asset.VirtualPath, Request.Priority);
				OutError.clear();
				return true;
			}
			Impl->CancelEntry(Entry);
			Impl->Entries.erase(Existing);
		}

		if (Impl->PendingRequests.size() + Impl->CapturedQueue.size()
			>= Impl->Budgets.MaximumQueuedJobs)
		{
			OutError = "The thumbnail request queue budget is exhausted.";
			return false;
		}

		FImpl::FEntry Entry;
		Entry.Request = Request;
		Entry.State = EAssetThumbnailState::Queued;
		Entry.RendererGeneration = Handle.Generation;
		Impl->Entries.emplace(AssetPath, std::move(Entry));
		Impl->PendingRequests.push_back(Request);
		OutError.clear();
		return true;
	}

	auto FAssetThumbnailRequestQueue::Find(const FPackagePath& AssetPath) const
		-> FAssetThumbnailView
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return {};
		const FImpl::FEntry& Entry = It->second;
		return {
			.State = Entry.State,
			.Diagnostic = Entry.Diagnostic,
			.RequestSerial = Entry.Request.RequestSerial};
	}

	auto FAssetThumbnailRequestQueue::TakeNext()
		-> std::optional<FAssetThumbnailScheduledRequest>
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
		if (Impl->bShuttingDown) return std::nullopt;
		auto IsEligible = [bGeneratedPixelsOnly](
			const FAssetThumbnailScheduledRequest& Job) {
			return !bGeneratedPixelsOnly
				|| Job.GenerationRequest.GeneratedPixels != nullptr;
		};
		auto SelectCaptured = [&]() {
			auto Selected = std::ranges::find_if(
				Impl->CapturedQueue,
				[&](const FAssetThumbnailScheduledRequest& Job) {
					return IsEligible(Job)
						&& Job.Priority == EAssetThumbnailPriority::Visible;
				});
			if (Selected == Impl->CapturedQueue.end())
				Selected = std::ranges::find_if(Impl->CapturedQueue, IsEligible);
			return Selected;
		};

		auto Selected = SelectCaptured();
		if (Selected == Impl->CapturedQueue.end())
		{
			// Capture may traverse registry dependencies and hash generation inputs.
			// Admit one uncaptured request per take so UI submission stays cheap.
			Impl->CaptureNextPending();
			Selected = SelectCaptured();
		}
		if (Selected == Impl->CapturedQueue.end()) return std::nullopt;

		FAssetThumbnailScheduledRequest Job = std::move(*Selected);
		Impl->CapturedQueue.erase(Selected);
		const std::string AssetPath =
			Job.GenerationRequest.KeyInput.Asset.VirtualPath.ToString();
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
		Entry->second.State = EAssetThumbnailState::Loading;
		return Job;
	}

	auto FAssetThumbnailRequestQueue::Transition(
		const FAssetThumbnailScheduledRequest& Job,
		EAssetThumbnailState ExpectedState,
		EAssetThumbnailState NextState,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::string_view Diagnostic) -> bool
	{
		if (Impl->bShuttingDown || Job.GenerationRequest.Cancellation.IsCancelled())
			return false;
		const std::string AssetPath =
			Job.GenerationRequest.KeyInput.Asset.VirtualPath.ToString();
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
			|| Entry.Request.Asset != Job.GenerationRequest.KeyInput.Asset
			|| (Entry.GenerationRequest.AssetRevision != 0
				&& Entry.GenerationRequest.AssetRevision != AssetRevision)
			|| (Entry.GenerationRequest.ResourceRevision != 0
				&& Entry.GenerationRequest.ResourceRevision != ResourceRevision))
			return false;

		if (AssetRevision != 0) Entry.GenerationRequest.AssetRevision = AssetRevision;
		if (ResourceRevision != 0)
			Entry.GenerationRequest.ResourceRevision = ResourceRevision;
		Entry.State = NextState;
		Entry.Diagnostic = std::string(Diagnostic);
		return true;
	}

	auto FAssetThumbnailRequestQueue::Cancel(const FPackagePath& AssetPath) -> void
	{
		const auto It = Impl->Entries.find(AssetPath.ToString());
		if (It == Impl->Entries.end()) return;
		Impl->CancelEntry(It->second);
		Impl->Entries.erase(It);
	}

	auto FAssetThumbnailRequestQueue::CancelAll() -> void
	{
		for (auto& [AssetPath, Entry] : Impl->Entries)
			if (Entry.bCaptured) Entry.GenerationRequest.Cancellation.Cancel();
		Impl->PendingRequests.clear();
		Impl->CapturedQueue.clear();
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
		return Impl->PendingRequests.size() + Impl->CapturedQueue.size();
	}

	auto FAssetThumbnailRequestQueue::IsShuttingDown() const -> bool
	{
		return Impl->bShuttingDown;
	}
} // namespace Durin::Editor
