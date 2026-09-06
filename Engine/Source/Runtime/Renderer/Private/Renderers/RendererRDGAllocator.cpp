#include "Renderers/RendererRDGAllocator.h"

#include "RenderResourceCreation.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
#include "RenderingThread.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
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
			uint32 ObservationTag = 0;
		};

		using FTextureEntry = TEntry<FTextureDescriptorKey, FTextureRHIRef>;
		using FBufferEntry = TEntry<FBufferDescriptorKey, FBufferRHIRef>;
		std::vector<FTextureEntry> Textures;
		std::vector<FBufferEntry> Buffers;
		uint64 NextSequence = 0;
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
		State->NextSequence = 0;
		State->RetainedBytes = 0;
		State->RetainedResources = 0;
	}

	auto FRendererRDGAllocator::Allocate(
		std::span<const FRDGAllocationRequest> Requests,
		FRDGAllocatedResources& OutResources, std::string& OutError) -> bool
	{
		check(IsInRenderingThread());
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

		uint64 RequestedBytes = 0;
		for (const FRDGAllocationRequest& Request : Requests)
		{
			if (Request.Kind == ERDGResourceKind::Texture)
			{
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
					"RDGBudget", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				RequestedBytes = AddSaturated(RequestedBytes,
					GetLogicalTextureBytes(Desc));
			}
			else RequestedBytes = AddSaturated(
				RequestedBytes, Request.BufferDesc.Size);
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
		const uint64 FirstNewSequence = State->NextSequence;

		auto RemoveNewEntries = [&](auto& Entries, uint64 PreserveSequence) {
			std::erase_if(Entries, [&](const auto& Entry) {
				if (Entry.Sequence < FirstNewSequence
					|| Entry.Sequence == PreserveSequence) return false;
				if (Entry.Physical)
				{
					State->RetainedBytes -= Entry.LogicalBytes;
					--State->RetainedResources;
				}
				return true;
			});
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

		auto ReserveCandidate = [&](auto& Entries, const auto& Key,
			uint64 LogicalBytes, std::string_view Kind,
			const FRDGAllocationRequest& Request, auto CreatePhysical,
			auto AssignPhysical, FCandidate& Candidate) -> bool {
			auto It = std::ranges::find_if(Entries, [&](const auto& Entry) {
				return Entry.Key == Key && Entry.Physical
					&& !ActiveAllocationIds.contains(Entry.Sequence + 1);
			});
			Candidate.bReuseHit = It != Entries.end();
			if (Candidate.bReuseHit) ++State->ReuseHits;
			else ++State->ReuseMisses;
			if (It == Entries.end())
			{
				It = std::ranges::find_if(Entries, [&](const auto& Entry) {
					return Entry.Key == Key && !Entry.Physical;
				});
				const auto& Generation = Coordinator.GetGeneration_RenderThread();
				if (It != Entries.end() && It->FailedGeneration
					&& !HasSelectedRenderResourceGenerationChanged(
						*It->FailedGeneration, Generation,
						ERenderResourceGenerationDependency::Device
							| ERenderResourceGenerationDependency::Manual))
					return Fail("RDG " + std::string(Kind)
						+ " allocation remains unavailable for resource id="
						+ std::to_string(Request.ResourceId), It->Sequence);
				if (It == Entries.end())
					It = Entries.emplace(Entries.end(),
						typename std::remove_reference_t<decltype(Entries)>::value_type{
							Key, {}, State->NextSequence++, LogicalBytes, {},
							Request.ObservationTag});
				auto Physical = CreatePhysical();
				if (!Physical)
				{
					It->FailedGeneration = Generation;
					return Fail("RDG " + std::string(Kind)
						+ " allocation failed for resource id="
						+ std::to_string(Request.ResourceId), It->Sequence);
				}
				It->Physical = std::move(Physical);
				It->FailedGeneration.reset();
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

		for (const FRDGAllocationRequest& Request : Requests)
		{
			FCandidate Candidate{.ResourceId = Request.ResourceId,
				.bExtracted = Request.bExtracted};
			bool bReserved = false;
			if (Request.Kind == ERDGResourceKind::Texture)
			{
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
					"RDGTexture", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				bReserved = ReserveCandidate(State->Textures,
					MakeDescriptorKey(Desc), GetLogicalTextureBytes(Desc), "texture",
					Request, [&] { return RHICreateTexture(Desc); },
					[](FCandidate& OutCandidate, const FTextureRHIRef& Texture) {
						OutCandidate.Texture = Texture;
					}, Candidate);
			}
			else if (Request.Kind == ERDGResourceKind::Buffer)
			{
				const FBufferDescriptorKey Key{Request.BufferDesc.Size,
					Request.BufferDesc.Stride, Request.BufferDesc.Usage};
				bReserved = ReserveCandidate(State->Buffers, Key,
					Request.BufferDesc.Size, "buffer", Request,
					[&] { return RHICreateBuffer(FRHIBufferCreateDesc::Create(
						"RDGBuffer", Request.BufferDesc)); },
					[](FCandidate& OutCandidate, const FBufferRHIRef& Buffer) {
						OutCandidate.Buffer = Buffer;
					}, Candidate);
			}
			else return Fail("RDG allocator received a non-physical resource");
			if (!bReserved) return false;
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
			std::erase_if(Entries, [&](const auto& Entry) {
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

		while (State->RetainedBytes
			> FRendererRDGAllocationPolicy::MaximumRetainedBytes)
		{
			uint64 OldestSequence = std::numeric_limits<uint64>::max();
			bool bTexture = false;
			size_t OldestIndex = 0;
			auto SelectOldest = [&](const auto& Entries, bool bEntriesAreTextures) {
				for (size_t Index = 0; Index < Entries.size(); ++Index)
					if (Entries[Index].Physical
						&& !ActiveAllocationIds.contains(Entries[Index].Sequence + 1)
						&& Entries[Index].Sequence < OldestSequence)
					{
						OldestSequence = Entries[Index].Sequence;
						OldestIndex = Index;
						bTexture = bEntriesAreTextures;
					}
			};
			SelectOldest(State->Textures, true);
			SelectOldest(State->Buffers, false);
			if (OldestSequence == std::numeric_limits<uint64>::max()) break;
			if (bTexture)
			{
				State->RetainedBytes -= State->Textures[OldestIndex].LogicalBytes;
				State->Textures.erase(State->Textures.begin() + OldestIndex);
			}
			else
			{
				State->RetainedBytes -= State->Buffers[OldestIndex].LogicalBytes;
				State->Buffers.erase(State->Buffers.begin() + OldestIndex);
			}
			--State->RetainedResources;
			++State->Evictions;
		}
		PublishStatistics(RequestedBytes, static_cast<uint32>(Requests.size()));
		OutError.clear();
		return true;
	}
} // namespace Durin
