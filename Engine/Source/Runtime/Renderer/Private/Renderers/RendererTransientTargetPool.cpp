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
		uint64 NextSequence = 0;
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
		return Total;
	}

	auto FRendererTransientTargetPool::Release_RenderThread() -> void
	{
		check(IsInRenderingThread());
		for (auto& Entries : State->Groups) Entries.clear();
		State->NextSequence = 0;
	}
} // namespace Durin
