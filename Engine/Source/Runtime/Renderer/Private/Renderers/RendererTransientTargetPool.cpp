#include "Renderers/RendererTransientTargetPool.h"

#include "RenderResourceCreation.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderingThread.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace Durin
{
	namespace
	{
		struct FTextureKey
		{
			std::string Name;
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

			auto operator==(const FTextureKey&) const -> bool = default;
		};

		struct FBufferKey
		{
			uint32 Size = 0;
			uint32 Stride = 0;
			EBufferUsageFlags Usage = EBufferUsageFlags::None;
			auto operator==(const FBufferKey&) const -> bool = default;
		};

		auto MakeKey(const FRHITextureCreateDesc& Desc) -> FTextureKey
		{
			FTextureKey Key{
				.Name = Desc.DebugName != nullptr ? Desc.DebugName : "Transient",
				.Dimension = Desc.Dimension,
				.Flags = Desc.Flags,
				.Format = Desc.Format,
				.Extent = Desc.Extent,
				.Depth = Desc.Depth,
				.ArraySize = Desc.ArraySize,
				.NumMips = Desc.NumMips,
				.NumSamples = Desc.NumSamples,
				.ClearBinding = Desc.ClearValue.Binding
			};
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
	}

	struct FRendererTransientTargetPool::FState
	{
		struct FEntry
		{
			FEntry(FTextureKey InKey, uint64 InSequence, uint64 InLogicalBytes)
				: Key(std::move(InKey)), Sequence(InSequence),
				  LogicalBytes(InLogicalBytes),
				  Slot(ERenderResourceGenerationDependency::Device)
			{
			}

			FTextureKey Key;
			uint64 Sequence = 0;
			uint64 LogicalBytes = 0;
			TRenderResourceCreationSlot<FTextureRHIRef> Slot;
		};

		std::array<std::vector<FEntry>,
			static_cast<size_t>(ERendererTransientTargetGroup::Count)> Groups;
		struct FRDGTextureEntry
		{
			FTextureKey Key;
			FTextureRHIRef Texture;
			uint64 Sequence = 0;
			uint64 LogicalBytes = 0;
			std::optional<FRenderResourceGeneration> FailedGeneration;
			uint32 ObservationTag = 0;
		};
		struct FRDGBufferEntry
		{
			FBufferKey Key;
			FBufferRHIRef Buffer;
			uint64 Sequence = 0;
			uint64 LogicalBytes = 0;
			std::optional<FRenderResourceGeneration> FailedGeneration;
			uint32 ObservationTag = 0;
		};
		std::vector<FRDGTextureEntry> RDGTextures;
		std::vector<FRDGBufferEntry> RDGBuffers;
		uint64 NextSequence = 0;
		uint64 RDGPeakActiveBytes = 0;
		uint64 RDGReuseHits = 0;
		uint64 RDGReuseMisses = 0;
		uint64 RDGEvictions = 0;
		uint64 RDGFailures = 0;
	};

	FRendererTransientTargetPool::FRendererTransientTargetPool(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator), State(std::make_unique<FState>())
	{
	}

	FRendererTransientTargetPool::~FRendererTransientTargetPool() = default;

	auto FRendererTransientTargetPool::AcquireBundle_RenderThread(
		ERendererTransientTargetGroup Group,
		std::span<const FRHITextureCreateDesc> Descriptions,
		uint64 MaximumRetainedBytes) -> std::optional<FLease>
	{
		check(IsInRenderingThread());
		check(Group < ERendererTransientTargetGroup::Count);
		if (Descriptions.empty()) return FLease{};
		if (std::ranges::any_of(Descriptions, [](const auto& Desc) {
			return Desc.Extent.x <= 0 || Desc.Extent.y <= 0;
		})) return std::nullopt;
		auto& Entries = State->Groups[static_cast<size_t>(Group)];
		FLease Lease;
		Lease.Textures.reserve(Descriptions.size());
		std::vector<FTextureKey> ActiveKeys;
		ActiveKeys.reserve(Descriptions.size());
		std::vector<FTextureKey> NewKeys;
		NewKeys.reserve(Descriptions.size());
		auto EnforceBudget = [&](std::span<const FTextureKey> ProtectedKeys) {
			while (MaximumRetainedBytes != 0
				&& GetRetainedBytes_RenderThread(Group) > MaximumRetainedBytes)
			{
				auto Oldest = Entries.end();
				for (auto It = Entries.begin(); It != Entries.end(); ++It)
				{
					if (std::ranges::find(ProtectedKeys, It->Key)
						!= ProtectedKeys.end()) continue;
					if (Oldest == Entries.end()
						|| It->Sequence < Oldest->Sequence) Oldest = It;
				}
				if (Oldest == Entries.end()) break;
				Entries.erase(Oldest);
			}
		};
		for (const FRHITextureCreateDesc& Desc : Descriptions)
		{
			FTextureKey Key = MakeKey(Desc);
			auto It = std::ranges::find(Entries, Key, &FState::FEntry::Key);
			if (It == Entries.end())
			{
				It = Entries.emplace(Entries.end(), Key,
					State->NextSequence++, GetLogicalTextureBytes(Desc));
				NewKeys.push_back(Key);
			}
			using FResult = TRenderResourceCreateResult<FTextureRHIRef>;
			FTextureRHIRef* Texture = It->Slot.Resolve(
				Coordinator.GetGeneration_RenderThread(),
				[Desc, Key]() -> FResult {
					FTextureRHIRef Candidate = RHICreateTexture(Desc);
					return Candidate ? FResult::Success(std::move(Candidate))
						: FResult::Failure(MakeRendererResourceCreateError(
							ERenderResourceCreateErrorCategory::RHIResource,
							"RendererTransientTarget", Key.Name,
							"Texture creation returned null.",
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual));
				}, ReportRendererResourceCreateDiagnostic);
			if (Texture == nullptr || !*Texture)
			{
				std::erase_if(Entries, [&NewKeys, &Key](const FState::FEntry& Entry) {
					return Entry.Key != Key
						&& std::ranges::find(NewKeys, Entry.Key) != NewKeys.end();
				});
				EnforceBudget({});
				return std::nullopt;
			}
			Lease.ActiveBytes = AddSaturated(
				Lease.ActiveBytes, It->LogicalBytes);
			Lease.Textures.push_back(*Texture);
			ActiveKeys.push_back(std::move(Key));
		}

		EnforceBudget(ActiveKeys);
		return Lease;
	}

	auto FRendererTransientTargetPool::GetRetainedBytes_RenderThread(
		ERendererTransientTargetGroup Group) const -> uint64
	{
		check(Group < ERendererTransientTargetGroup::Count);
		uint64 Total = 0;
		for (const FState::FEntry& Entry :
			State->Groups[static_cast<size_t>(Group)])
		{
			const FTextureRHIRef* Texture = Entry.Slot.GetPayload();
			if (Texture != nullptr && *Texture)
				Total = AddSaturated(Total, Entry.LogicalBytes);
		}
		for (const auto& Entry : State->RDGTextures)
			if (Entry.Texture
				&& Entry.ObservationTag == static_cast<uint32>(Group))
				Total = AddSaturated(Total, Entry.LogicalBytes);
		return Total;
	}

	auto FRendererTransientTargetPool::GetTotalRetainedBytes_RenderThread() const
		-> uint64
	{
		uint64 Total = 0;
		for (const auto& Entries : State->Groups)
			for (const FState::FEntry& Entry : Entries)
			{
				const FTextureRHIRef* Texture = Entry.Slot.GetPayload();
				if (Texture != nullptr && *Texture)
					Total = AddSaturated(Total, Entry.LogicalBytes);
			}
		for (const auto& Entry : State->RDGTextures)
			if (Entry.Texture) Total = AddSaturated(Total, Entry.LogicalBytes);
		for (const auto& Entry : State->RDGBuffers)
			if (Entry.Buffer) Total = AddSaturated(Total, Entry.LogicalBytes);
		return Total;
	}

	auto FRendererTransientTargetPool::Release_RenderThread() -> void
	{
		check(IsInRenderingThread());
		for (auto& Entries : State->Groups) Entries.clear();
		State->RDGTextures.clear();
		State->RDGBuffers.clear();
		State->NextSequence = 0;
	}

	auto FRendererTransientTargetPool::Allocate(
		std::span<const FRDGAllocationRequest> Requests,
		FRDGAllocatedResources& OutResources, std::string& OutError) -> bool
	{
		check(IsInRenderingThread());
		constexpr uint64 MaximumRDGRetainedBytes = 640ull * 1024ull * 1024ull;
		auto PublishStatistics = [&](uint64 ActiveBytes, uint32 ActiveResources) {
			uint64 RetainedBytes = 0;
			uint32 RetainedResources = 0;
			for (const auto& Entry : State->RDGTextures)
				if (Entry.Texture)
				{
					RetainedBytes = AddSaturated(RetainedBytes, Entry.LogicalBytes);
					++RetainedResources;
				}
			for (const auto& Entry : State->RDGBuffers)
				if (Entry.Buffer)
				{
					RetainedBytes = AddSaturated(RetainedBytes, Entry.LogicalBytes);
					++RetainedResources;
				}
			State->RDGPeakActiveBytes = std::max(
				State->RDGPeakActiveBytes, ActiveBytes);
			OutResources.SetStatistics({
				.ActiveResources = ActiveResources,
				.RetainedResources = RetainedResources,
				.ActiveBytes = ActiveBytes,
				.RetainedBytes = RetainedBytes,
				.PeakActiveBytes = State->RDGPeakActiveBytes,
				.ReuseHits = State->RDGReuseHits,
				.ReuseMisses = State->RDGReuseMisses,
				.Evictions = State->RDGEvictions,
				.Failures = State->RDGFailures});
		};
		uint64 RequestedBytes = 0;
		for (const FRDGAllocationRequest& Request : Requests)
		{
			if (Request.Kind == ERenderGraphResourceKind::Texture)
			{
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
					"RDGBudget", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				RequestedBytes = AddSaturated(RequestedBytes,
					GetLogicalTextureBytes(Desc));
			}
			else RequestedBytes = AddSaturated(RequestedBytes,
				Request.BufferDesc.Size);
		}
		if (RequestedBytes > MaximumRDGRetainedBytes)
		{
			++State->RDGFailures;
			PublishStatistics(0, 0);
			OutError = "RDG retained allocation batch exceeds structural budget";
			return false;
		}
		struct FCandidate
		{
			uint32 ResourceId = 0;
			FTextureRHIRef Texture;
			FBufferRHIRef Buffer;
			uint64 AllocationId = 0;
			bool bReuseHit = false;
		};
		std::vector<FCandidate> Candidates;
		Candidates.reserve(Requests.size());
		const uint64 FirstNewSequence = State->NextSequence;
		auto Rollback = [&](uint64 PreserveSequence =
			std::numeric_limits<uint64>::max()) {
			std::erase_if(State->RDGTextures, [&](const auto& Entry) {
				return Entry.Sequence >= FirstNewSequence
					&& Entry.Sequence != PreserveSequence;
			});
			std::erase_if(State->RDGBuffers, [&](const auto& Entry) {
				return Entry.Sequence >= FirstNewSequence
					&& Entry.Sequence != PreserveSequence;
			});
		};
		for (const FRDGAllocationRequest& Request : Requests)
		{
			FCandidate Candidate{.ResourceId = Request.ResourceId};
			if (Request.Kind == ERenderGraphResourceKind::Texture)
			{
				FRHITextureCreateDesc Desc = FRHITextureCreateDesc::Create(
					"RDGTexture", Request.TextureDesc.Dimension);
				static_cast<FRHITextureDesc&>(Desc) = Request.TextureDesc;
				FTextureKey Key = MakeKey(Desc);
				Key.Name.clear();
				auto It = std::ranges::find_if(State->RDGTextures,
					[&](const FState::FRDGTextureEntry& Entry) {
						return Entry.Key == Key && Entry.Texture
							&& std::ranges::none_of(Candidates,
								[&](const FCandidate& Existing) {
									return Existing.AllocationId == Entry.Sequence + 1;
								});
					});
				Candidate.bReuseHit = It != State->RDGTextures.end();
				if (Candidate.bReuseHit) ++State->RDGReuseHits;
				else ++State->RDGReuseMisses;
				if (It == State->RDGTextures.end())
				{
					It = std::ranges::find_if(State->RDGTextures,
						[&](const FState::FRDGTextureEntry& Entry) {
							return Entry.Key == Key && !Entry.Texture;
						});
					const auto& Generation = Coordinator.GetGeneration_RenderThread();
					if (It != State->RDGTextures.end()
						&& It->FailedGeneration
						&& !HasSelectedRenderResourceGenerationChanged(
							*It->FailedGeneration, Generation,
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual))
					{
						Rollback(It->Sequence);
						++State->RDGFailures;
						PublishStatistics(0, 0);
						OutError = "RDG texture allocation remains unavailable for resource id="
							+ std::to_string(Request.ResourceId);
						return false;
					}
					if (It == State->RDGTextures.end())
						It = State->RDGTextures.emplace(State->RDGTextures.end(),
							FState::FRDGTextureEntry{Key, {}, State->NextSequence++,
								GetLogicalTextureBytes(Desc), {}, Request.ObservationTag});
					FTextureRHIRef Texture = RHICreateTexture(Desc);
					if (!Texture)
					{
						It->FailedGeneration = Generation;
						const uint64 FailedSequence = It->Sequence;
						Rollback(FailedSequence);
						++State->RDGFailures;
						PublishStatistics(0, 0);
						OutError = "RDG texture allocation failed for resource id="
							+ std::to_string(Request.ResourceId);
						return false;
					}
					It->Texture = std::move(Texture);
					It->FailedGeneration.reset();
				}
				Candidate.Texture = It->Texture;
				It->ObservationTag = Request.ObservationTag;
				Candidate.AllocationId = It->Sequence + 1;
			}
			else if (Request.Kind == ERenderGraphResourceKind::Buffer)
			{
				const FBufferKey Key{Request.BufferDesc.Size,
					Request.BufferDesc.Stride, Request.BufferDesc.Usage};
				auto It = std::ranges::find_if(State->RDGBuffers,
					[&](const FState::FRDGBufferEntry& Entry) {
						return Entry.Key == Key && Entry.Buffer
							&& std::ranges::none_of(Candidates,
								[&](const FCandidate& Existing) {
									return Existing.AllocationId == Entry.Sequence + 1;
								});
					});
				Candidate.bReuseHit = It != State->RDGBuffers.end();
				if (Candidate.bReuseHit) ++State->RDGReuseHits;
				else ++State->RDGReuseMisses;
				if (It == State->RDGBuffers.end())
				{
					It = std::ranges::find_if(State->RDGBuffers,
						[&](const FState::FRDGBufferEntry& Entry) {
							return Entry.Key == Key && !Entry.Buffer;
						});
					const auto& Generation = Coordinator.GetGeneration_RenderThread();
					if (It != State->RDGBuffers.end()
						&& It->FailedGeneration
						&& !HasSelectedRenderResourceGenerationChanged(
							*It->FailedGeneration, Generation,
							ERenderResourceGenerationDependency::Device
								| ERenderResourceGenerationDependency::Manual))
					{
						Rollback(It->Sequence);
						++State->RDGFailures;
						PublishStatistics(0, 0);
						OutError = "RDG buffer allocation remains unavailable for resource id="
							+ std::to_string(Request.ResourceId);
						return false;
					}
					if (It == State->RDGBuffers.end())
						It = State->RDGBuffers.emplace(State->RDGBuffers.end(),
							FState::FRDGBufferEntry{Key, {}, State->NextSequence++,
								Request.BufferDesc.Size, {}, Request.ObservationTag});
					FBufferRHIRef Buffer = RHICreateBuffer(
						FRHIBufferCreateDesc::Create("RDGBuffer", Request.BufferDesc));
					if (!Buffer)
					{
						It->FailedGeneration = Generation;
						const uint64 FailedSequence = It->Sequence;
						Rollback(FailedSequence);
						++State->RDGFailures;
						PublishStatistics(0, 0);
						OutError = "RDG buffer allocation failed for resource id="
							+ std::to_string(Request.ResourceId);
						return false;
					}
					It->Buffer = std::move(Buffer);
					It->FailedGeneration.reset();
				}
				Candidate.Buffer = It->Buffer;
				It->ObservationTag = Request.ObservationTag;
				Candidate.AllocationId = It->Sequence + 1;
			}
			else
			{
				Rollback();
				++State->RDGFailures;
				PublishStatistics(0, 0);
				OutError = "RDG allocator received a non-physical resource";
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
			{
				Rollback();
				++State->RDGFailures;
				PublishStatistics(0, 0);
				OutError = "RDG allocator could not publish resource id="
					+ std::to_string(Candidate.ResourceId);
				return false;
			}
		}
		auto RetainedBytes = [&] {
			uint64 Total = 0;
			for (const auto& Entry : State->RDGTextures)
				if (Entry.Texture)
					Total = AddSaturated(Total, Entry.LogicalBytes);
			for (const auto& Entry : State->RDGBuffers)
				if (Entry.Buffer)
					Total = AddSaturated(Total, Entry.LogicalBytes);
			return Total;
		};
		while (RetainedBytes() > MaximumRDGRetainedBytes)
		{
			uint64 OldestSequence = std::numeric_limits<uint64>::max();
			bool bTexture = false;
			size_t OldestIndex = 0;
			for (size_t Index = 0; Index < State->RDGTextures.size(); ++Index)
				if (State->RDGTextures[Index].Texture
					&& std::ranges::none_of(Candidates,
						[&](const FCandidate& Active) {
							return Active.AllocationId
								== State->RDGTextures[Index].Sequence + 1;
						})
					&& State->RDGTextures[Index].Sequence < OldestSequence)
				{
					OldestSequence = State->RDGTextures[Index].Sequence;
					OldestIndex = Index;
					bTexture = true;
				}
			for (size_t Index = 0; Index < State->RDGBuffers.size(); ++Index)
				if (State->RDGBuffers[Index].Buffer
					&& std::ranges::none_of(Candidates,
						[&](const FCandidate& Active) {
							return Active.AllocationId
								== State->RDGBuffers[Index].Sequence + 1;
						})
					&& State->RDGBuffers[Index].Sequence < OldestSequence)
				{
					OldestSequence = State->RDGBuffers[Index].Sequence;
					OldestIndex = Index;
					bTexture = false;
				}
			if (OldestSequence == std::numeric_limits<uint64>::max()) break;
			if (bTexture)
				State->RDGTextures.erase(State->RDGTextures.begin() + OldestIndex);
			else State->RDGBuffers.erase(State->RDGBuffers.begin() + OldestIndex);
			++State->RDGEvictions;
		}
		PublishStatistics(RequestedBytes, static_cast<uint32>(Requests.size()));
		OutError.clear();
		return true;
	}
} // namespace Durin
