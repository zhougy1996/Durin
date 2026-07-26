#include "Thumbnail/RenderedAssetThumbnailPipeline.h"

namespace Durin
{
	struct FRenderedAssetThumbnailPipeline::FImpl
	{
		FImpl(
			FAssetThumbnailScheduler& InScheduler,
			FAssetThumbnailObjectStoreSettings StoreSettings,
			FAssetThumbnailBudgets InBudgets)
			: Scheduler(InScheduler)
			, Store(std::move(StoreSettings))
			, Budgets(InBudgets)
		{
		}

		FAssetThumbnailScheduler& Scheduler;
		FAssetThumbnailObjectStore Store;
		FAssetThumbnailBudgets Budgets;
		FRenderedAssetThumbnailPipelineStats Stats;
		uint32 RendersStartedThisFrame = 0;

		auto Fail(
			const FRenderedAssetThumbnailJob& Job,
			EAssetThumbnailState ExpectedState,
			uint64 AssetRevision,
			uint64 ResourceRevision,
			std::string_view Error) -> bool
		{
			if (Error.empty()) return false;
			if (!Scheduler.Transition(Job.ScheduledJob, ExpectedState, EAssetThumbnailState::Failed,
					AssetRevision, ResourceRevision, Error))
				return false;
			++Stats.Failures;
			return true;
		}
	};

	FRenderedAssetThumbnailPipeline::FRenderedAssetThumbnailPipeline(
		FAssetThumbnailScheduler& Scheduler,
		FAssetThumbnailObjectStoreSettings StoreSettings,
		FAssetThumbnailBudgets Budgets
	)
		: Impl(std::make_unique<FImpl>(Scheduler, std::move(StoreSettings), Budgets))
	{
	}

	FRenderedAssetThumbnailPipeline::~FRenderedAssetThumbnailPipeline() = default;

	auto FRenderedAssetThumbnailPipeline::BeginFrame() -> void
	{
		Impl->RendersStartedThisFrame = 0;
	}

	auto FRenderedAssetThumbnailPipeline::StartNext() -> std::optional<FRenderedAssetThumbnailJob>
	{
		std::optional<FAssetThumbnailScheduledJob> ScheduledJob = Impl->Scheduler.TakeNext();
		if (!ScheduledJob) return std::nullopt;
		++Impl->Stats.Jobs;

		std::vector<uint8> EncodedBytes;
		const EAssetThumbnailObjectLoadResult LoadResult =
			Impl->Store.Load(ScheduledJob->CacheKey, EncodedBytes);
		if (LoadResult == EAssetThumbnailObjectLoadResult::Hit)
		{
			if (Impl->Scheduler.Transition(
					*ScheduledJob, EAssetThumbnailState::Loading, EAssetThumbnailState::Ready))
				++Impl->Stats.DiskHits;
			return std::nullopt;
		}

		++Impl->Stats.Loads;
		return FRenderedAssetThumbnailJob{.ScheduledJob = std::move(*ScheduledJob)};
	}

	auto FRenderedAssetThumbnailPipeline::CompleteLoad(
		FRenderedAssetThumbnailJob& Job,
		uint64 AssetRevision,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Loading, AssetRevision, 0, Error)) return false;
		if (AssetRevision == 0
			|| !Impl->Scheduler.Transition(Job.ScheduledJob, EAssetThumbnailState::Loading,
				EAssetThumbnailState::WaitingForResources, AssetRevision))
			return false;
		Job.AssetRevision = AssetRevision;
		return true;
	}

	auto FRenderedAssetThumbnailPipeline::BeginRender(
		FRenderedAssetThumbnailJob& Job,
		bool bResourcesReady,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::WaitingForResources,
				AssetRevision, ResourceRevision, Error))
			return false;
		if (AssetRevision == 0 || AssetRevision != Job.AssetRevision) return false;
		if (!bResourcesReady)
		{
			++Impl->Stats.ResourceWaits;
			return Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::WaitingForResources,
				EAssetThumbnailState::WaitingForResources,
				AssetRevision);
		}
		if (ResourceRevision == 0
			|| Impl->RendersStartedThisFrame >= Impl->Budgets.MaximumRendersPerFrame)
			return false;
		if (!Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::WaitingForResources,
				EAssetThumbnailState::Rendering,
				AssetRevision,
				ResourceRevision))
			return false;
		Job.ResourceRevision = ResourceRevision;
		++Impl->RendersStartedThisFrame;
		++Impl->Stats.Renders;
		return true;
	}

	auto FRenderedAssetThumbnailPipeline::CompleteRender(
		const FRenderedAssetThumbnailJob& Job,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Rendering,
				AssetRevision, ResourceRevision, Error))
			return false;
		return Impl->Scheduler.Transition(Job.ScheduledJob,
			EAssetThumbnailState::Rendering,
			EAssetThumbnailState::Readback,
			AssetRevision,
			ResourceRevision);
	}

	auto FRenderedAssetThumbnailPipeline::CompleteReadback(
		const FRenderedAssetThumbnailJob& Job,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Readback,
				AssetRevision, ResourceRevision, Error))
			return false;
		if (!Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::Readback,
				EAssetThumbnailState::Encoding,
				AssetRevision,
				ResourceRevision))
			return false;
		++Impl->Stats.Readbacks;
		return true;
	}

	auto FRenderedAssetThumbnailPipeline::CompleteEncoding(
		const FRenderedAssetThumbnailJob& Job,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::span<const uint8> EncodedBytes,
		std::string_view Error
	) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Encoding,
				AssetRevision, ResourceRevision, Error))
			return false;
		if (EncodedBytes.empty()
			|| !Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::Encoding,
				EAssetThumbnailState::Encoding,
				AssetRevision,
				ResourceRevision)
			|| !Impl->Store.Store(Job.ScheduledJob.CacheKey, EncodedBytes))
		{
			Impl->Fail(Job, EAssetThumbnailState::Encoding, AssetRevision, ResourceRevision,
				"Failed to atomically publish the encoded thumbnail.");
			return false;
		}
		return Impl->Scheduler.Transition(Job.ScheduledJob,
			EAssetThumbnailState::Encoding,
			EAssetThumbnailState::Ready,
			AssetRevision,
			ResourceRevision);
	}

	auto FRenderedAssetThumbnailPipeline::Cancel(const FRenderedAssetThumbnailJob& Job) -> void
	{
		if (!Job.ScheduledJob.GenerationRequest.Cancellation.IsCancelled())
		{
			Impl->Scheduler.Cancel(Job.ScheduledJob.GenerationRequest.KeyInput.Asset.VirtualPath);
			++Impl->Stats.Cancellations;
		}
	}

	auto FRenderedAssetThumbnailPipeline::RecordRetry() -> void
	{
		++Impl->Stats.Retries;
	}

	auto FRenderedAssetThumbnailPipeline::GetStats() const -> FRenderedAssetThumbnailPipelineStats
	{
		FRenderedAssetThumbnailPipelineStats Stats = Impl->Stats;
		Stats.Evictions = Impl->Store.GetStats().Evictions;
		return Stats;
	}
} // namespace Durin
