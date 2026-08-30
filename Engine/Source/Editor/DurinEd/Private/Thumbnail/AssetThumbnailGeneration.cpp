#include "Thumbnail/AssetThumbnailGeneration.h"

#include "Image/ImageEncoder.h"

namespace Durin::Editor
{
	struct FAssetThumbnailGeneration::FImpl
	{
		FImpl(
			FAssetThumbnailRequestQueue& InScheduler,
			FAssetThumbnailPoolStorageSettings StoreSettings,
			FAssetThumbnailBudgets InBudgets)
			: Scheduler(InScheduler)
			, Store(std::move(StoreSettings))
			, Budgets(InBudgets)
		{
		}

		FAssetThumbnailRequestQueue& Scheduler;
		FThumbnailObjectStore Store;
		FAssetThumbnailBudgets Budgets;
		FAssetThumbnailGenerationStats Stats;
		uint32 RendersStartedThisFrame = 0;

		auto Fail(
			const FAssetThumbnailJob& Job,
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

	FAssetThumbnailGeneration::FAssetThumbnailGeneration(
		FAssetThumbnailRequestQueue& Scheduler,
		FAssetThumbnailPoolStorageSettings StoreSettings,
		FAssetThumbnailBudgets Budgets
	)
		: Impl(std::make_unique<FImpl>(Scheduler, std::move(StoreSettings), Budgets))
	{
	}

	FAssetThumbnailGeneration::~FAssetThumbnailGeneration() = default;

	auto FAssetThumbnailGeneration::BeginFrame() -> void
	{
		Impl->RendersStartedThisFrame = 0;
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
		std::optional<FAssetThumbnailScheduledRequest> ScheduledJob = bGeneratedPixelsOnly
			? Impl->Scheduler.TakeNextGeneratedPixels()
			: Impl->Scheduler.TakeNext();
		if (!ScheduledJob) return Result;
		++Impl->Stats.Jobs;

		const EThumbnailObjectLoadResult LoadResult =
			Impl->Store.Load(ScheduledJob->CacheKey, Result.EncodedBytes);
		if (LoadResult == EThumbnailObjectLoadResult::Hit)
		{
			if (Impl->Scheduler.Transition(
					*ScheduledJob,
					EAssetThumbnailState::Loading,
					EAssetThumbnailState::Ready,
					ScheduledJob->GenerationRequest.AssetRevision,
					ScheduledJob->GenerationRequest.ResourceRevision))
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

	auto FAssetThumbnailGeneration::BeginRender(
		FAssetThumbnailJob& Job,
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

	auto FAssetThumbnailGeneration::CompleteRender(
		const FAssetThumbnailJob& Job,
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

	auto FAssetThumbnailGeneration::CompleteReadback(
		const FAssetThumbnailJob& Job,
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

	auto FAssetThumbnailGeneration::CompleteEncoding(
		const FAssetThumbnailJob& Job,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::span<const std::byte> EncodedBytes,
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

	auto FAssetThumbnailGeneration::CompletePixels(
		const FAssetThumbnailJob& Job,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::span<const std::byte> Pixels,
		uint32 Width,
		uint32 Height,
		std::string_view Error,
		std::function<std::string()> ValidateBeforePublication) -> bool
	{
		if (!Error.empty())
			return CompleteEncoding(Job, AssetRevision, ResourceRevision, {}, Error);
		std::vector<std::byte> EncodedBytes;
		if (!Image::EncodeRgba8Png(Pixels, Width, Height, EncodedBytes))
			return CompleteEncoding(
				Job,
				AssetRevision,
				ResourceRevision,
				{},
				"Rendered-thumbnail pixels do not match the requested RGBA8 output.");
		if (ValidateBeforePublication)
		{
			const std::string ValidationError = ValidateBeforePublication();
			if (!ValidationError.empty())
				return CompleteEncoding(
					Job, AssetRevision, ResourceRevision, {}, ValidationError);
		}
		return CompleteEncoding(
			Job, AssetRevision, ResourceRevision, EncodedBytes);
	}

	auto FAssetThumbnailGeneration::CompleteGeneratedPixels(
		FAssetThumbnailJob& Job,
		uint64 AssetRevision,
		std::span<const std::byte> Pixels,
		uint32 Width,
		uint32 Height,
		std::string_view Error) -> bool
	{
		if (Impl->Fail(Job, EAssetThumbnailState::Loading,
				AssetRevision, AssetRevision, Error)) return false;
		if (AssetRevision == 0
			|| !Impl->Scheduler.Transition(Job.ScheduledJob,
				EAssetThumbnailState::Loading, EAssetThumbnailState::Encoding,
				AssetRevision, AssetRevision)) return false;
		Job.AssetRevision = AssetRevision;
		Job.ResourceRevision = AssetRevision;
		return CompletePixels(Job, AssetRevision, AssetRevision,
			Pixels, Width, Height);
	}

	auto FAssetThumbnailGeneration::Cancel(const FAssetThumbnailJob& Job) -> void
	{
		if (!Job.ScheduledJob.GenerationRequest.Cancellation.IsCancelled())
		{
			Impl->Scheduler.Cancel(Job.ScheduledJob.GenerationRequest.KeyInput.Asset.VirtualPath);
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
		Impl->Store.Invalidate(CacheKey);
	}

	auto FAssetThumbnailGeneration::GetStats() const -> FAssetThumbnailGenerationStats
	{
		FAssetThumbnailGenerationStats Stats = Impl->Stats;
		Stats.Evictions = Impl->Store.GetStats().Evictions;
		return Stats;
	}
} // namespace Durin::Editor
