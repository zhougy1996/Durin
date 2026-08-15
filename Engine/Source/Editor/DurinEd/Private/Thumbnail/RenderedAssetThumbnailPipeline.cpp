#include "Thumbnail/RenderedAssetThumbnailPipeline.h"

namespace Durin::Editor
{
	namespace
	{
		auto AppendBigEndian(std::vector<uint8>& Bytes, uint32 Value) -> void
		{
			Bytes.push_back(static_cast<uint8>(Value >> 24));
			Bytes.push_back(static_cast<uint8>(Value >> 16));
			Bytes.push_back(static_cast<uint8>(Value >> 8));
			Bytes.push_back(static_cast<uint8>(Value));
		}

		auto Crc32(std::span<const uint8> Bytes) -> uint32
		{
			uint32 Crc = 0xffffffffu;
			for (const uint8 Byte : Bytes)
			{
				Crc ^= Byte;
				for (uint32 Bit = 0; Bit < 8; ++Bit)
					Crc = (Crc >> 1) ^ (0xedb88320u & (0u - (Crc & 1u)));
			}
			return ~Crc;
		}

		auto WritePngChunk(
			std::vector<uint8>& Bytes,
			std::string_view Type,
			std::span<const uint8> Payload) -> void
		{
			AppendBigEndian(Bytes, static_cast<uint32>(Payload.size()));
			const size_t CrcStart = Bytes.size();
			Bytes.insert(Bytes.end(), Type.begin(), Type.end());
			Bytes.insert(Bytes.end(), Payload.begin(), Payload.end());
			AppendBigEndian(Bytes, Crc32(std::span(Bytes).subspan(CrcStart)));
		}

		auto EncodeRgbaPng(
			std::span<const uint8> Pixels,
			uint32 Width,
			uint32 Height,
			std::vector<uint8>& OutBytes) -> bool
		{
			const uint64 ExpectedBytes = static_cast<uint64>(Width) * Height * 4;
			if (Width == 0 || Height == 0 || Pixels.size() != ExpectedBytes) return false;

			std::vector<uint8> Scanlines;
			Scanlines.reserve(static_cast<size_t>(ExpectedBytes) + Height);
			const size_t RowBytes = static_cast<size_t>(Width) * 4;
			for (uint32 Y = 0; Y < Height; ++Y)
			{
				Scanlines.push_back(0);
				Scanlines.insert(
					Scanlines.end(),
					Pixels.begin() + static_cast<ptrdiff_t>(Y * RowBytes),
					Pixels.begin() + static_cast<ptrdiff_t>((Y + 1) * RowBytes));
			}

			std::vector<uint8> Deflate{0x78, 0x01};
			for (size_t Offset = 0; Offset < Scanlines.size();)
			{
				const uint16 BlockSize =
					static_cast<uint16>(std::min<size_t>(65'535, Scanlines.size() - Offset));
				const bool bFinal = Offset + BlockSize == Scanlines.size();
				Deflate.push_back(bFinal ? 1 : 0);
				Deflate.push_back(static_cast<uint8>(BlockSize));
				Deflate.push_back(static_cast<uint8>(BlockSize >> 8));
				const uint16 Inverse = static_cast<uint16>(~BlockSize);
				Deflate.push_back(static_cast<uint8>(Inverse));
				Deflate.push_back(static_cast<uint8>(Inverse >> 8));
				Deflate.insert(
					Deflate.end(),
					Scanlines.begin() + static_cast<ptrdiff_t>(Offset),
					Scanlines.begin() + static_cast<ptrdiff_t>(Offset + BlockSize));
				Offset += BlockSize;
			}
			uint32 S1 = 1;
			uint32 S2 = 0;
			for (const uint8 Byte : Scanlines)
			{
				S1 = (S1 + Byte) % 65'521;
				S2 = (S2 + S1) % 65'521;
			}
			AppendBigEndian(Deflate, (S2 << 16) | S1);

			OutBytes = {137, 80, 78, 71, 13, 10, 26, 10};
			std::vector<uint8> Header;
			AppendBigEndian(Header, Width);
			AppendBigEndian(Header, Height);
			Header.insert(Header.end(), {8, 6, 0, 0, 0});
			WritePngChunk(OutBytes, "IHDR", Header);
			WritePngChunk(OutBytes, "IDAT", Deflate);
			WritePngChunk(OutBytes, "IEND", {});
			return true;
		}
	}

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
		return StartNextDetailed().ColdJob;
	}

	auto FRenderedAssetThumbnailPipeline::StartNextDetailed()
		-> FRenderedAssetThumbnailStartResult
	{
		return StartNextDetailed(false);
	}

	auto FRenderedAssetThumbnailPipeline::StartNextGeneratedPixelsDetailed()
		-> FRenderedAssetThumbnailStartResult
	{
		return StartNextDetailed(true);
	}

	auto FRenderedAssetThumbnailPipeline::StartNextDetailed(
		bool bGeneratedPixelsOnly) -> FRenderedAssetThumbnailStartResult
	{
		FRenderedAssetThumbnailStartResult Result;
		std::optional<FAssetThumbnailScheduledJob> ScheduledJob = bGeneratedPixelsOnly
			? Impl->Scheduler.TakeNextGeneratedPixels()
			: Impl->Scheduler.TakeNext();
		if (!ScheduledJob) return Result;
		++Impl->Stats.Jobs;

		const EAssetThumbnailObjectLoadResult LoadResult =
			Impl->Store.Load(ScheduledJob->CacheKey, Result.EncodedBytes);
		if (LoadResult == EAssetThumbnailObjectLoadResult::Hit)
		{
			if (Impl->Scheduler.Transition(
					*ScheduledJob, EAssetThumbnailState::Loading, EAssetThumbnailState::Ready))
			{
				++Impl->Stats.DiskHits;
				Result.WarmJob = std::move(*ScheduledJob);
			}
			return Result;
		}

		++Impl->Stats.Loads;
		Result.ColdJob = FRenderedAssetThumbnailJob{
			.ScheduledJob = std::move(*ScheduledJob)};
		return Result;
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

	auto FRenderedAssetThumbnailPipeline::CompletePixels(
		const FRenderedAssetThumbnailJob& Job,
		uint64 AssetRevision,
		uint64 ResourceRevision,
		std::span<const uint8> Pixels,
		uint32 Width,
		uint32 Height,
		std::string_view Error,
		std::function<std::string()> ValidateBeforePublication) -> bool
	{
		if (!Error.empty())
			return CompleteEncoding(Job, AssetRevision, ResourceRevision, {}, Error);
		std::vector<uint8> EncodedBytes;
		if (!EncodeRgbaPng(Pixels, Width, Height, EncodedBytes))
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

	auto FRenderedAssetThumbnailPipeline::CompleteGeneratedPixels(
		FRenderedAssetThumbnailJob& Job,
		uint64 AssetRevision,
		std::span<const uint8> Pixels,
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

	auto FRenderedAssetThumbnailPipeline::InvalidatePersistentObject(
		std::string_view CacheKey) -> void
	{
		Impl->Store.Invalidate(CacheKey);
	}

	auto FRenderedAssetThumbnailPipeline::GetStats() const -> FRenderedAssetThumbnailPipelineStats
	{
		FRenderedAssetThumbnailPipelineStats Stats = Impl->Stats;
		Stats.Evictions = Impl->Store.GetStats().Evictions;
		return Stats;
	}
} // namespace Durin::Editor
