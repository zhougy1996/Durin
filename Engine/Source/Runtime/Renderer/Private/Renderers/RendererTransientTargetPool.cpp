#include "Renderers/RendererTransientTargetPool.h"

#include "RenderResourceCreation.h"
#include "Renderers/RendererResourceDiagnostics.h"
#include "Resources/RendererResourceCoordinator.h"
#include "RHI.h"
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
			std::string Group;
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

		auto MakeKey(std::string_view Group,
			const FRHITextureCreateDesc& Desc) -> FTextureKey
		{
			FTextureKey Key{
				.Group = std::string(Group),
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

		std::vector<FEntry> Entries;
		uint64 NextSequence = 0;
	};

	FRendererTransientTargetPool::FRendererTransientTargetPool(
		FRendererResourceCoordinator& InCoordinator)
		: Coordinator(InCoordinator), State(std::make_unique<FState>())
	{
	}

	FRendererTransientTargetPool::~FRendererTransientTargetPool() = default;

	auto FRendererTransientTargetPool::AcquireBundle_RenderThread(
		std::string_view Group,
		std::span<const FRHITextureCreateDesc> Descriptions,
		uint64 MaximumRetainedBytes) -> std::optional<FLease>
	{
		check(IsInRenderingThread());
		if (Descriptions.empty()) return FLease{};
		FLease Lease;
		Lease.Textures.reserve(Descriptions.size());
		std::vector<FTextureKey> ActiveKeys;
		ActiveKeys.reserve(Descriptions.size());
		for (const FRHITextureCreateDesc& Desc : Descriptions)
		{
			if (Desc.Extent.x <= 0 || Desc.Extent.y <= 0) return std::nullopt;
			FTextureKey Key = MakeKey(Group, Desc);
			auto It = std::ranges::find(State->Entries, Key, &FState::FEntry::Key);
			if (It == State->Entries.end())
				It = State->Entries.emplace(State->Entries.end(), Key,
					State->NextSequence++, GetLogicalTextureBytes(Desc));
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
			if (Texture == nullptr || !*Texture) return std::nullopt;
			Lease.ActiveBytes = AddSaturated(
				Lease.ActiveBytes, It->LogicalBytes);
			Lease.Textures.push_back(*Texture);
			ActiveKeys.push_back(std::move(Key));
		}

		while (MaximumRetainedBytes != 0
			&& GetRetainedBytes_RenderThread(Group) > MaximumRetainedBytes)
		{
			auto Oldest = State->Entries.end();
			for (auto It = State->Entries.begin(); It != State->Entries.end(); ++It)
			{
				if (It->Key.Group != Group
					|| std::ranges::find(ActiveKeys, It->Key) != ActiveKeys.end())
					continue;
				if (Oldest == State->Entries.end()
					|| It->Sequence < Oldest->Sequence) Oldest = It;
			}
			if (Oldest == State->Entries.end()) break;
			State->Entries.erase(Oldest);
		}
		return Lease;
	}

	auto FRendererTransientTargetPool::GetRetainedBytes_RenderThread(
		std::string_view Group) const -> uint64
	{
		uint64 Total = 0;
		for (const FState::FEntry& Entry : State->Entries)
		{
			if (Entry.Key.Group != Group) continue;
			const FTextureRHIRef* Texture = Entry.Slot.GetPayload();
			if (Texture != nullptr && *Texture)
				Total = AddSaturated(Total, Entry.LogicalBytes);
		}
		return Total;
	}

	auto FRendererTransientTargetPool::GetTotalRetainedBytes_RenderThread() const
		-> uint64
	{
		uint64 Total = 0;
		for (const FState::FEntry& Entry : State->Entries)
		{
			const FTextureRHIRef* Texture = Entry.Slot.GetPayload();
			if (Texture != nullptr && *Texture)
				Total = AddSaturated(Total, Entry.LogicalBytes);
		}
		return Total;
	}

	auto FRendererTransientTargetPool::Release_RenderThread() -> void
	{
		check(IsInRenderingThread());
		State->Entries.clear();
		State->NextSequence = 0;
	}
} // namespace Durin
