#include "Renderers/RendererRDGAllocator.h"

#include "RenderResourceCreation.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
#include "RenderingThread.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Durin
{
	namespace
	{
		struct FTextureDescriptorKey final
		{
			ETextureDimension Dimension = ETextureDimension::Texture2D;
			ETextureCreateFlags Flags = ETextureCreateFlags::None;
			EPixelFormat Format = EPixelFormat::Unknown;
			FIntPoint Extent{1, 1};
			uint16 Depth = 1;
			uint16 ArraySize = 1;
			uint8 NumMips = 1;
			uint8 NumSamples = 1;
			EClearBinding ClearBinding = EClearBinding::None;
			std::array<std::byte, sizeof(FClearValueBinding::FClearValue)>
				ClearValue{};

			auto operator==(const FTextureDescriptorKey&) const -> bool = default;
		};

		struct FBufferDescriptorKey final
		{
			uint32 Size = 0;
			uint32 Stride = 0;
			EBufferUsageFlags Usage = EBufferUsageFlags::None;

			auto operator==(const FBufferDescriptorKey&) const -> bool = default;
		};

		// Hash semantic fields only: descriptor struct padding is not identity.
		struct FDescriptorHash final
		{
			static auto Combine(size_t& Hash, uint64 Value) -> void
			{
				Hash ^= std::hash<uint64>{}(Value) + size_t(0x9e3779b9)
					+ (Hash << 6) + (Hash >> 2);
			}

			auto operator()(const FTextureDescriptorKey& Key) const -> size_t
			{
				size_t Hash = 0;
				for (uint64 Value : {uint64(Key.Dimension), uint64(Key.Flags),
					uint64(Key.Format), uint64(Key.Extent.x), uint64(Key.Extent.y),
					uint64(Key.Depth), uint64(Key.ArraySize), uint64(Key.NumMips),
					uint64(Key.NumSamples), uint64(Key.ClearBinding)})
					Combine(Hash, Value);
				for (std::byte Value : Key.ClearValue)
					Combine(Hash, std::to_integer<uint64>(Value));
				return Hash;
			}

			auto operator()(const FBufferDescriptorKey& Key) const -> size_t
			{
				size_t Hash = 0;
				Combine(Hash, Key.Size);
				Combine(Hash, Key.Stride);
				Combine(Hash, uint64(Key.Usage));
				return Hash;
			}
		};

		auto MakeDescriptorKey(const FRHITextureCreateDesc& Desc)
			-> FTextureDescriptorKey
		{
			FTextureDescriptorKey Key{
				.Dimension = Desc.Dimension,
				.Flags = Desc.Flags,
				.Format = Desc.Format,
				.Extent = Desc.Extent,
				.Depth = Desc.Depth,
				.ArraySize = Desc.ArraySize,
				.NumMips = Desc.NumMips,
				.NumSamples = Desc.NumSamples,
				.ClearBinding = Desc.ClearValue.Binding};
			std::memcpy(Key.ClearValue.data(), &Desc.ClearValue.ClearValue,
				Key.ClearValue.size());
			return Key;
		}

		auto AddSaturated(uint64 Left, uint64 Right) -> uint64
		{
			return Right > std::numeric_limits<uint64>::max() - Left
				? std::numeric_limits<uint64>::max() : Left + Right;
		}

		auto MultiplySaturated(uint64 Left, uint64 Right) -> uint64
		{
			return Left != 0 && Right > std::numeric_limits<uint64>::max() / Left
				? std::numeric_limits<uint64>::max() : Left * Right;
		}

		auto GetLogicalTextureBytes(const FRHITextureCreateDesc& Desc) -> uint64
		{
			uint32 Width = static_cast<uint32>(std::max(Desc.Extent.x, 1));
			uint32 Height = static_cast<uint32>(std::max(Desc.Extent.y, 1));
			uint32 Depth = std::max<uint16>(Desc.Depth, 1);
			uint64 Total = 0;
			for (uint32 Mip = 0; Mip < std::max<uint8>(Desc.NumMips, 1); ++Mip)
			{
				uint64 MipBytes = GetPixelFormatLayout(
					Desc.Format, Width, Height).DataSize;
				MipBytes = MultiplySaturated(MipBytes, Depth);
				MipBytes = MultiplySaturated(
					MipBytes, std::max<uint16>(Desc.ArraySize, 1));
				MipBytes = MultiplySaturated(
					MipBytes, std::max<uint8>(Desc.NumSamples, 1));
				Total = AddSaturated(Total, MipBytes);
				Width = std::max(Width / 2, 1u);
				Height = std::max(Height / 2, 1u);
				Depth = std::max(Depth / 2, 1u);
			}
			return Total;
		}
	} // namespace

	struct FRendererRDGAllocator::FState
	{
		template<typename Descriptor, typename Resource>
		struct TEntry final
		{
			Descriptor Key;
			Resource Physical;
			uint64 Sequence = 0;
			uint64 LogicalBytes = 0;
			std::optional<FRenderResourceGeneration> FailedGeneration;
			ERHIResourceCreationFailure Failure = ERHIResourceCreationFailure::None;
			uint32 RetryFailures = 0;
			std::chrono::steady_clock::time_point NextRetryTime{};
			uint32 ObservationTag = 0;
		};

		// Stable compaction preserves sequence order. All indices are rebuilt once
		// after compaction; ordered bucket members also support retrying old entries.
		template<typename Descriptor, typename Resource>
		struct TPool final
		{
			using FEntry = TEntry<Descriptor, Resource>;
			struct FBucket final
			{
				std::set<size_t> Physical;
				std::set<size_t> Empty;
				std::set<size_t> Failed;
			};
			std::vector<FEntry> Entries;
			std::unordered_map<Descriptor, FBucket, FDescriptorHash> Buckets;
			std::unordered_map<uint64, size_t> AllocationIndices;

			auto begin() { return Entries.begin(); }
			auto end() { return Entries.end(); }
			auto size() const -> size_t { return Entries.size(); }
			auto operator[](size_t Index) -> FEntry& { return Entries[Index]; }

			auto clear() -> void
			{
				Entries.clear();
				Buckets.clear();
				AllocationIndices.clear();
			}

			auto IndexEntry(size_t Index) -> void
			{
				const auto& Entry = Entries[Index];
				AllocationIndices.emplace(Entry.Sequence + 1, Index);
				auto& Bucket = Buckets[Entry.Key];
				auto& Members = Entry.Physical ? Bucket.Physical : Bucket.Empty;
				Members.insert(Members.end(), Index);
				if (!Entry.Physical && Entry.FailedGeneration)
					Bucket.Failed.insert(Bucket.Failed.end(), Index);
			}

			auto RebuildIndices() -> void
			{
				Buckets.clear();
				AllocationIndices.clear();
				for (size_t Index = 0; Index < Entries.size(); ++Index)
					IndexEntry(Index);
			}

			template<typename Predicate>
			auto EraseIf(Predicate Remove) -> void
			{
				if (std::erase_if(Entries, Remove) != 0) RebuildIndices();
			}

			auto Find(uint64 AllocationId) -> FEntry*
			{
				const auto It = AllocationIndices.find(AllocationId);
				return It == AllocationIndices.end() ? nullptr : &Entries[It->second];
			}

			auto FindEmpty(const Descriptor& Key) -> FEntry*
			{
				const auto It = Buckets.find(Key);
				return It == Buckets.end() || It->second.Empty.empty()
					? nullptr : &Entries[*It->second.Empty.begin()];
			}

			auto Add(FEntry Entry) -> FEntry*
			{
				Entries.push_back(std::move(Entry));
				IndexEntry(Entries.size() - 1);
				return &Entries.back();
			}

			auto Materialize(FEntry& Entry, Resource Physical) -> void
			{
				const size_t Index = AllocationIndices.at(Entry.Sequence + 1);
				auto& Bucket = Buckets.at(Entry.Key);
				Bucket.Empty.erase(Index);
				Bucket.Failed.erase(Index);
				Bucket.Physical.insert(Bucket.Physical.end(), Index);
				Entry.Physical = std::move(Physical);
			}
		};
		TPool<FTextureDescriptorKey, FTextureRHIRef> Textures;
		TPool<FBufferDescriptorKey, FBufferRHIRef> Buffers;
		std::optional<uint64> DeviceGeneration;
		uint64 NextSequence = 0;
		std::optional<FRenderResourceGeneration> RetryGeneration;
		std::chrono::steady_clock::time_point NextRetryTime{};
		bool bNeedsCollection = false;
		uint64 PeakActiveBytes = 0;
		uint64 ReuseHits = 0;
		uint64 ReuseMisses = 0;
		uint64 Evictions = 0;
		uint64 Failures = 0;
		uint64 RetainedBytes = 0;
		uint32 RetainedResources = 0;
	};

	FRendererRDGAllocator::FRendererRDGAllocator(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator), State(std::make_unique<FState>())
	{
	}

	FRendererRDGAllocator::~FRendererRDGAllocator() = default;

	auto FRendererRDGAllocator::GetObservedRetainedBytes_RenderThread(
		ERDGAllocationObservation Observation) const -> uint64
	{
		check(Observation < ERDGAllocationObservation::Count);
		uint64 Total = 0;
		for (const auto& Entry : State->Textures)
			if (Entry.Physical
				&& Entry.ObservationTag == static_cast<uint32>(Observation))
				Total = AddSaturated(Total, Entry.LogicalBytes);
		for (const auto& Entry : State->Buffers)
			if (Entry.Physical
				&& Entry.ObservationTag == static_cast<uint32>(Observation))
				Total = AddSaturated(Total, Entry.LogicalBytes);
		return Total;
	}

	auto FRendererRDGAllocator::Release_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Textures.clear();
		State->Buffers.clear();
		State->DeviceGeneration.reset();
		State->NextSequence = 0;
		State->RetryGeneration.reset();
		State->NextRetryTime = {};
		State->bNeedsCollection = false;
		State->RetainedBytes = 0;
		State->RetainedResources = 0;
	}

	auto FRendererRDGAllocator::Allocate(
		std::span<const FRDGAllocationRequest> Requests,
		FRDGAllocatedResources& OutResources, std::string& OutError) -> bool
	{
		check(IsInRenderingThread());
		const auto& Generation = Coordinator.GetGeneration_RenderThread();
		// Pool validity must not depend on the owner's invalidation callback.
		if (State->DeviceGeneration != Generation.Device)
		{
			Release_RenderThread();
			State->DeviceGeneration = Generation.Device;
		}
		const auto Now = std::chrono::steady_clock::now();
		constexpr auto RetryDependencies = ERenderResourceGenerationDependency::Device
			| ERenderResourceGenerationDependency::Manual;
		if (!State->RetryGeneration || HasSelectedRenderResourceGenerationChanged(
			*State->RetryGeneration, Generation, RetryDependencies))
		{
			State->NextRetryTime = {};
			State->RetryGeneration = Generation;
		}
		auto PublishStatistics = [&](uint64 ActiveBytes, uint32 ActiveResources) {
			State->PeakActiveBytes = std::max(State->PeakActiveBytes, ActiveBytes);
			OutResources.SetStatistics({
				.ActiveResources = ActiveResources,
				.RetainedResources = State->RetainedResources,
				.ActiveBytes = ActiveBytes,
				.RetainedBytes = State->RetainedBytes,
				.PeakActiveBytes = State->PeakActiveBytes,
				.ReuseHits = State->ReuseHits,
				.ReuseMisses = State->ReuseMisses,
				.Evictions = State->Evictions,
				.Failures = State->Failures});
		};

		std::vector<uint64> RequestLogicalBytes;
		RequestLogicalBytes.reserve(Requests.size());
		uint64 RequestedBytes = 0;
		for (const FRDGAllocationRequest& Request : Requests)
		{
			uint64 LogicalBytes = Request.BufferDesc.Size;
			if (Request.Kind == ERDGResourceKind::Texture)
			{
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
					"RDGBudget", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				LogicalBytes = GetLogicalTextureBytes(Desc);
			}
			RequestLogicalBytes.push_back(LogicalBytes);
			RequestedBytes = AddSaturated(RequestedBytes, LogicalBytes);
		}
		if (!FRendererRDGAllocationPolicy::IsBatchWithinStructuralBudget(
			RequestedBytes))
		{
			++State->Failures;
			PublishStatistics(0, 0);
			OutError = "RDG retained allocation batch exceeds structural budget";
			return false;
		}

		struct FCandidate final
		{
			uint32 ResourceId = 0;
			FTextureRHIRef Texture;
			FBufferRHIRef Buffer;
			uint64 AllocationId = 0;
			bool bReuseHit = false;
			bool bExtracted = false;
		};
		std::vector<FCandidate> Candidates;
		Candidates.reserve(Requests.size());
		std::unordered_set<uint64> ActiveAllocationIds;
		ActiveAllocationIds.reserve(Requests.size());
		std::unordered_set<uint64> CreatedAllocationIds;
		const uint64 FirstNewSequence = State->NextSequence;

		auto RemoveNewEntries = [&](auto& Entries, uint64 PreserveSequence) {
			// A successful retry can materialize an entry older than this batch.
			for (auto& Entry : Entries)
				if (Entry.Physical && CreatedAllocationIds.contains(Entry.Sequence + 1))
				{
					State->RetainedBytes -= Entry.LogicalBytes;
					--State->RetainedResources;
					Entry.Physical = {};
				}
			std::erase_if(Entries.Entries, [&](const auto& Entry) {
				return Entry.Sequence >= FirstNewSequence
					&& Entry.Sequence != PreserveSequence;
			});
			// Physical state can change even when no entry was removed.
			Entries.RebuildIndices();
		};
		auto Rollback = [&](uint64 PreserveSequence =
			std::numeric_limits<uint64>::max()) {
			RemoveNewEntries(State->Textures, PreserveSequence);
			RemoveNewEntries(State->Buffers, PreserveSequence);
		};
		auto Fail = [&](std::string Error, uint64 PreserveSequence =
			std::numeric_limits<uint64>::max()) {
			Rollback(PreserveSequence);
			++State->Failures;
			PublishStatistics(0, 0);
			OutError = std::move(Error);
			return false;
		};

		// Reserve the entire reusable set before eviction, including later requests
		// with duplicate descriptors. Allocation IDs survive vector compaction.
		std::vector<uint64> PlannedAllocationIds;
		PlannedAllocationIds.reserve(Requests.size());
		uint64 MissingBytes = 0;
		// Cursors live only during planning, before any bucket can be mutated.
		std::unordered_map<const void*, std::set<size_t>::const_iterator> BucketCursors;
		auto PlanCandidate = [&](const auto& Entries, const auto& Key,
			uint64 LogicalBytes) -> bool {
			const auto BucketIt = Entries.Buckets.find(Key);
			if (BucketIt != Entries.Buckets.end())
			{
				const auto& Bucket = BucketIt->second;
				auto& Cursor = BucketCursors.try_emplace(
					&Bucket, Bucket.Physical.begin()).first->second;
				if (Cursor != Bucket.Physical.end())
				{
					const uint64 Id = Entries.Entries[*Cursor++].Sequence + 1;
					PlannedAllocationIds.push_back(Id);
					ActiveAllocationIds.insert(Id);
					return true;
				}
				if (!Bucket.Failed.empty())
				{
					const auto& Failed = Entries.Entries[*Bucket.Failed.begin()];
					if (!HasSelectedRenderResourceGenerationChanged(
						*Failed.FailedGeneration, Generation, RetryDependencies)
						&& (Failed.Failure == ERHIResourceCreationFailure::UnsupportedDescriptor
							|| Now < Failed.NextRetryTime))
						return Fail("RDG allocation retry is suppressed for an unavailable descriptor");
				}
			}
			PlannedAllocationIds.push_back(0);
			MissingBytes = AddSaturated(MissingBytes, LogicalBytes);
			return true;
		};
		for (size_t RequestIndex = 0; RequestIndex < Requests.size(); ++RequestIndex)
		{
			const auto& Request = Requests[RequestIndex];
			const uint64 LogicalBytes = RequestLogicalBytes[RequestIndex];
			bool bPlanned = false;
			if (Request.Kind == ERDGResourceKind::Texture)
			{
				auto Desc = FRHITextureCreateDesc::Create("RDGPlan", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				bPlanned = PlanCandidate(State->Textures, MakeDescriptorKey(Desc),
					LogicalBytes);
			}
			else if (Request.Kind == ERDGResourceKind::Buffer)
				bPlanned = PlanCandidate(State->Buffers,
					FBufferDescriptorKey{Request.BufferDesc.Size, Request.BufferDesc.Stride,
						Request.BufferDesc.Usage}, LogicalBytes);
			else return Fail("RDG allocator received a non-physical resource");
			if (!bPlanned) return false;
		}
		if (MissingBytes != 0 && Now < State->NextRetryTime)
			return Fail("RDG allocation is waiting for the memory-pressure retry interval");

		auto EvictUntil = [&](uint64 Limit) {
			if (State->RetainedBytes <= Limit) return;
			size_t TextureIndex = 0;
			size_t BufferIndex = 0;
			std::optional<uint64> LastEvictedSequence;
			auto SkipReserved = [&](auto& Entries, size_t& Index) {
				while (Index < Entries.size()
					&& (!Entries[Index].Physical
						|| ActiveAllocationIds.contains(Entries[Index].Sequence + 1)))
					++Index;
			};
			auto Select = [&](const auto& Entry) {
				LastEvictedSequence = Entry.Sequence;
				State->RetainedBytes -= Entry.LogicalBytes;
				--State->RetainedResources;
				++State->Evictions;
			};
			// Both pools are sequence ordered. Merge once, then compact each pool
			// once; reserved and failed entries survive the selected prefix.
			while (State->RetainedBytes > Limit)
			{
				SkipReserved(State->Textures, TextureIndex);
				SkipReserved(State->Buffers, BufferIndex);
				if (TextureIndex == State->Textures.size()
					&& BufferIndex == State->Buffers.size()) break;
				if (BufferIndex == State->Buffers.size()
					|| (TextureIndex < State->Textures.size()
						&& State->Textures[TextureIndex].Sequence
							< State->Buffers[BufferIndex].Sequence))
					Select(State->Textures[TextureIndex++]);
				else
					Select(State->Buffers[BufferIndex++]);
			}
			if (LastEvictedSequence)
			{
				auto Remove = [&](const auto& Entry) {
					return Entry.Physical && Entry.Sequence <= *LastEvictedSequence
						&& !ActiveAllocationIds.contains(Entry.Sequence + 1);
				};
				State->Textures.EraseIf(Remove);
				State->Buffers.EraseIf(Remove);
			}
		};
		const auto PreviousEvictions = State->Evictions;
		EvictUntil(FRendererRDGAllocationPolicy::MaximumRetainedBytes - MissingBytes);
		if (MissingBytes != 0
			&& (State->Evictions != PreviousEvictions || State->bNeedsCollection))
		{
			// Erasing pool references alone does not free native allocations.
			// Only completed GPU deletions may be reclaimed here; never wait idle.
			GDynamicRHI->RHICollectCompletedResources();
			State->bNeedsCollection = false;
		}

		auto ReserveCandidate = [&](auto& Entries, const auto& Key,
			uint64 LogicalBytes, std::string_view Kind,
			const FRDGAllocationRequest& Request, auto CreatePhysical,
			auto AssignPhysical, FCandidate& Candidate) -> bool {
			auto* It = Entries.Find(Candidate.AllocationId);
			Candidate.bReuseHit = It != nullptr;
			if (Candidate.bReuseHit) ++State->ReuseHits;
			else ++State->ReuseMisses;
			if (!It)
			{
				It = Entries.FindEmpty(Key);
				if (!It)
					It = Entries.Add(
						typename std::remove_reference_t<decltype(Entries)>::FEntry{
							.Key = Key, .Sequence = State->NextSequence++,
							.LogicalBytes = LogicalBytes, .ObservationTag = Request.ObservationTag});
				if (It->FailedGeneration && HasSelectedRenderResourceGenerationChanged(
					*It->FailedGeneration, Generation, RetryDependencies))
					It->RetryFailures = 0;
				ERHIResourceCreationFailure Failure = ERHIResourceCreationFailure::Unknown;
				auto Physical = CreatePhysical(Failure);
				if (!Physical)
				{
					It->FailedGeneration = Generation;
					It->Failure = Failure;
					if (Failure != ERHIResourceCreationFailure::UnsupportedDescriptor)
					{
						It->RetryFailures = std::min(It->RetryFailures + 1, 6u);
						const auto Delay = std::chrono::milliseconds(
							std::min(100u << (It->RetryFailures - 1), 2000u));
						It->NextRetryTime = std::chrono::steady_clock::now() + Delay;
						State->NextRetryTime = It->NextRetryTime;
					}
					return Fail("RDG " + std::string(Kind)
						+ " allocation failed for resource id="
						+ std::to_string(Request.ResourceId), It->Sequence);
				}
				CreatedAllocationIds.insert(It->Sequence + 1);
				Entries.Materialize(*It, std::move(Physical));
				It->FailedGeneration.reset();
				It->RetryFailures = 0;
				It->Failure = ERHIResourceCreationFailure::None;
				State->RetainedBytes = AddSaturated(
					State->RetainedBytes, It->LogicalBytes);
				++State->RetainedResources;
			}
			It->ObservationTag = Request.ObservationTag;
			Candidate.AllocationId = It->Sequence + 1;
			ActiveAllocationIds.insert(Candidate.AllocationId);
			AssignPhysical(Candidate, It->Physical);
			return true;
		};

		for (size_t RequestIndex = 0; RequestIndex < Requests.size(); ++RequestIndex)
		{
			const auto& Request = Requests[RequestIndex];
			const uint64 LogicalBytes = RequestLogicalBytes[RequestIndex];
			FCandidate Candidate{.ResourceId = Request.ResourceId,
				.AllocationId = PlannedAllocationIds[RequestIndex],
				.bExtracted = Request.bExtracted};
			bool bReserved = false;
			if (Request.Kind == ERDGResourceKind::Texture)
			{
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
					"RDGTexture", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				bReserved = ReserveCandidate(State->Textures,
					MakeDescriptorKey(Desc), LogicalBytes, "texture",
					Request, [&](ERHIResourceCreationFailure& Failure) {
						return GDynamicRHI->RHITryCreateTexture(
							FRHICommandListImmediate::Get(), Desc, Failure);
					},
					[](FCandidate& OutCandidate, const FTextureRHIRef& Texture) {
						OutCandidate.Texture = Texture;
					}, Candidate);
			}
			else if (Request.Kind == ERDGResourceKind::Buffer)
			{
				const FBufferDescriptorKey Key{Request.BufferDesc.Size,
					Request.BufferDesc.Stride, Request.BufferDesc.Usage};
				bReserved = ReserveCandidate(State->Buffers, Key,
					LogicalBytes, "buffer", Request,
					[&](ERHIResourceCreationFailure& Failure) {
						return GDynamicRHI->RHITryCreateBuffer(FRHICommandListImmediate::Get(),
							FRHIBufferCreateDesc::Create("RDGBuffer", Request.BufferDesc), Failure);
					},
					[](FCandidate& OutCandidate, const FBufferRHIRef& Buffer) {
						OutCandidate.Buffer = Buffer;
					}, Candidate);
			}
			else return Fail("RDG allocator received a non-physical resource");
			if (!bReserved)
			{
				// Pressure may come from outside this pool even below its ceiling.
				// Drop idle cache now and collect retirement before the next attempt.
				EvictUntil(0);
				State->bNeedsCollection = true;
				PublishStatistics(0, 0);
				return false;
			}
			Candidates.push_back(std::move(Candidate));
		}

		for (auto& Candidate : Candidates)
		{
			const bool bPublished = Candidate.Texture
				? OutResources.SetTexture(Candidate.ResourceId,
					std::move(Candidate.Texture), Candidate.AllocationId,
					Candidate.bReuseHit ? "reuse-hit" : "reuse-miss")
				: OutResources.SetBuffer(Candidate.ResourceId,
					std::move(Candidate.Buffer), Candidate.AllocationId,
					Candidate.bReuseHit ? "reuse-hit" : "reuse-miss");
			if (!bPublished)
				return Fail("RDG allocator could not publish resource id="
					+ std::to_string(Candidate.ResourceId));
		}

		// Detach exports before any graph callback can allocate another batch.
		// Failure after this point releases through RHI ownership, never pool reuse.
		std::unordered_set<uint64> ExportedAllocationIds;
		for (const auto& Candidate : Candidates)
			if (Candidate.bExtracted)
				ExportedAllocationIds.insert(Candidate.AllocationId);
		auto DetachExports = [&](auto& Entries) {
			Entries.EraseIf([&](const auto& Entry) {
				if (!ExportedAllocationIds.contains(Entry.Sequence + 1)) return false;
				State->RetainedBytes -= Entry.LogicalBytes;
				--State->RetainedResources;
				return true;
			});
		};
		if (!ExportedAllocationIds.empty())
		{
			DetachExports(State->Textures);
			DetachExports(State->Buffers);
		}

		PublishStatistics(RequestedBytes, static_cast<uint32>(Requests.size()));
		OutError.clear();
		return true;
	}
} // namespace Durin
