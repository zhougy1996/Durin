#include "Thumbnail/AssetThumbnailRequestQueue.h"
#include "Thumbnail/ThumbnailRenderer.h"
#include "DObject/StrongObjectPtr.h"

#include "Thumbnail/AssetThumbnailKey.h"
#include "Thumbnail/ThumbnailManager.h"

namespace Durin
{
	struct FAssetThumbnailRequestQueue::FImpl
	{
		struct FEntry
		{
			FAssetThumbnailRequest Request;
			EThumbnailJobState State = EThumbnailJobState::NotRequested;
			FAssetThumbnailGenerationRequest GenerationRequest;
			std::string Diagnostic;
			bool bCaptured = false;
		};

		DThumbnailManager& Registry;
		FAssetThumbnailBudgets Budgets;
		std::unordered_map<std::string, FEntry> Entries;
		std::vector<FTopLevelAssetPath> PendingRequests;
		bool bShuttingDown = false;

		auto RemovePendingRequest(const FTopLevelAssetPath& AssetPath) -> void
		{
			std::erase(PendingRequests, AssetPath);
		}

		auto CancelEntry(FEntry& Entry) -> void
		{
			Entry.GenerationRequest.Cancellation.Cancel();
			RemovePendingRequest(Entry.Request.Asset.AssetPath);
		}

		auto PromoteQueuedRequest(
			const FTopLevelAssetPath& AssetPath,
			EAssetThumbnailPriority Priority) -> void
		{
			if (Priority != EAssetThumbnailPriority::Visible) return;
			const auto It = Entries.find(AssetPath.ToString());
			if (It != Entries.end() && It->second.State == EThumbnailJobState::Queued)
				It->second.Request.Priority = Priority;
		}

		auto CaptureNextPending() -> std::optional<FAssetThumbnailScheduledRequest>
		{
			if (PendingRequests.empty()) return std::nullopt;
			auto Selected = std::ranges::find_if(
				PendingRequests,
				[this](const FTopLevelAssetPath& Path) {
					const auto It = Entries.find(Path.ToString());
					return It != Entries.end() && It->second.Request.Priority == EAssetThumbnailPriority::Visible;
				});
			if (Selected == PendingRequests.end()) Selected = PendingRequests.begin();
			const FTopLevelAssetPath Path = *Selected;
			PendingRequests.erase(Selected);

			const std::string AssetPath = Path.ToString();
			const auto Existing = Entries.find(AssetPath);
			if (Existing == Entries.end()) return std::nullopt;
			FEntry& Entry = Existing->second;
			if (Entry.State != EThumbnailJobState::Queued || Entry.bCaptured) return std::nullopt;
			const auto Cancellation = Entry.GenerationRequest.Cancellation;
			const auto Request = Entry.Request;
			const TStrongObjectPtr<DThumbnailRenderer> Renderer(Registry.Find(Request.Asset.AssetClassName));
			if (!Renderer) return std::nullopt;

			auto Captured = Renderer->DescribeThumbnail(Request.Asset);
			if (Cancellation.IsCancelled()) return std::nullopt;
			if (!Captured)
			{
				Entry.State = EThumbnailJobState::Invalid;
				Entry.Diagnostic = Captured.error().empty()
					? "The thumbnail renderer rejected the asset."
					: std::move(Captured.error());
				return std::nullopt;
			}

			FAssetThumbnailGenerationRequest GenerationRequest{
				.KeyInput = Captured->MakeKeyInput(Request.Asset, Renderer->GetRegistration()),
				.Cancellation = Cancellation,
				.bHasTransparency = Captured->bHasTransparency};
			Entry.GenerationRequest = GenerationRequest;
			Entry.bCaptured = true;
			Entry.Diagnostic.clear();
			return FAssetThumbnailScheduledRequest{
				.CacheKey = BuildAssetThumbnailCacheKey(GenerationRequest.KeyInput),
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
		Impl->Registry.AttachQueue(this);
	}

	FAssetThumbnailRequestQueue::~FAssetThumbnailRequestQueue()
	{
		Impl->Registry.DetachQueue(this);
		Shutdown();
	}

	auto FAssetThumbnailRequestQueue::BeginFrame() -> void
	{
		// Visibility belongs to the current frame, not to an asset's first request.
		for (const auto& Path : Impl->PendingRequests)
			Impl->Entries.at(Path.ToString()).Request.Priority = EAssetThumbnailPriority::Prefetch;
	}

	auto FAssetThumbnailRequestQueue::Request(
		const FAssetThumbnailRequest& Request
	) -> EThumbnailRequestStatus
	{
		if (Impl->bShuttingDown)
		{
			return EThumbnailRequestStatus::ShuttingDown;
		}
		const auto* Renderer =
			Impl->Registry.Find(Request.Asset.AssetClassName);
		if (!Renderer)
		{
			return EThumbnailRequestStatus::Unsupported;
		}

		const std::string AssetPath = Request.Asset.AssetPath.ToString();
		if (auto Existing = Impl->Entries.find(AssetPath);
			Existing != Impl->Entries.end())
		{
			FImpl::FEntry& Entry = Existing->second;
			if (Request.Asset == Entry.Request.Asset)
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
		Impl->Entries.emplace(AssetPath, std::move(Entry));
		Impl->PendingRequests.push_back(Request.Asset.AssetPath);
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

	auto FAssetThumbnailRequestQueue::TakeNext() -> std::optional<FAssetThumbnailScheduledRequest>
	{
		if (Impl->bShuttingDown) return std::nullopt;
		auto Job = Impl->CaptureNextPending();
		if (!Job || Job->GenerationRequest.Cancellation.IsCancelled()) return std::nullopt;
		Impl->Entries.at(Job->GenerationRequest.KeyInput.Asset.AssetPath.ToString()).State = EThumbnailJobState::Loading;
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
		if (!Entry.bCaptured || Entry.State != ExpectedState) return false;
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

	auto FAssetThumbnailRequestQueue::CancelRenderer(std::string_view AssetClassName) -> void
	{
		for (auto It = Impl->Entries.begin(); It != Impl->Entries.end();)
			if (It->second.Request.Asset.AssetClassName == AssetClassName)
			{
				Impl->CancelEntry(It->second);
				It = Impl->Entries.erase(It);
			}
			else ++It;
	}

	auto FAssetThumbnailRequestQueue::CancelAll() -> void
	{
		for (auto& [AssetPath, Entry] : Impl->Entries)
			Entry.GenerationRequest.Cancellation.Cancel();
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
