#include "VulkanViewCache.h"

#include "VulkanTexture.h"
#include "VulkanView.h"

namespace Durin::VulkanRHI
{
	namespace
	{
		auto HashCombine(size_t Seed, size_t Value) -> size_t
		{
			return Seed ^ (Value + 0x9e3779b9u + (Seed << 6u) + (Seed >> 2u));
		}

		template<typename MapType>
		auto EvictOldest(MapType& Entries, size_t MaximumEntries) -> void
		{
			if (Entries.size() < MaximumEntries) return;
			const auto Oldest = std::ranges::min_element(
				Entries, {}, [](const auto& Pair) { return Pair.second.LastUsedFrame; });
			if (Oldest != Entries.end()) Entries.erase(Oldest);
		}

		template<typename MapType>
		auto TrimInactive(
			MapType& Entries,
			uint64 FrameNumber,
			uint64 MaximumUnusedFrames) -> void
		{
			std::erase_if(Entries, [FrameNumber, MaximumUnusedFrames](const auto& Pair) {
				return FrameNumber > Pair.second.LastUsedFrame
					&& FrameNumber - Pair.second.LastUsedFrame > MaximumUnusedFrames;
			});
		}
	}

	FVulkanViewCache::~FVulkanViewCache()
	{
		check(BufferViews.empty() && TextureViews.empty());
	}

	auto FVulkanViewCache::FBufferKeyHash::operator()(
		const FBufferKey& Key) const -> size_t
	{
		size_t Hash = std::hash<const void*>{}(Key.Buffer);
		Hash = HashCombine(Hash, std::hash<uint64>{}(Key.Desc.Offset));
		Hash = HashCombine(Hash, std::hash<uint64>{}(Key.Desc.Size));
		Hash = HashCombine(Hash, static_cast<size_t>(Key.Desc.Type));
		return HashCombine(Hash, static_cast<size_t>(Key.Desc.Format));
	}

	auto FVulkanViewCache::FTextureKeyHash::operator()(
		const FTextureKey& Key) const -> size_t
	{
		size_t Hash = std::hash<const void*>{}(Key.Texture);
		Hash = HashCombine(Hash,
			std::hash<VkImage>{}(static_cast<VkImage>(Key.Image)));
		Hash = HashCombine(Hash, std::hash<uint64>{}(Key.BackingGeneration));
		Hash = HashCombine(Hash, static_cast<size_t>(Key.Desc.Usage));
		Hash = HashCombine(Hash, static_cast<size_t>(Key.Desc.Dimension));
		Hash = HashCombine(Hash, static_cast<size_t>(Key.Desc.Format));
		Hash = HashCombine(Hash, static_cast<size_t>(Key.Desc.Range.Aspects));
		Hash = HashCombine(Hash, Key.Desc.Range.FirstMip);
		Hash = HashCombine(Hash, Key.Desc.Range.NumMips);
		Hash = HashCombine(Hash, Key.Desc.Range.FirstArrayLayer);
		return HashCombine(Hash, Key.Desc.Range.NumArrayLayers);
	}

	auto FVulkanViewCache::FindBufferView(
		FRHIBuffer* Buffer,
		const FRHIBufferViewDesc& Desc,
		uint64 FrameNumber) -> FBufferViewRHIRef
	{
		std::lock_guard Lock(Mutex);
		const auto It = BufferViews.find({Buffer, Desc});
		if (It == BufferViews.end()) return nullptr;
		It->second.LastUsedFrame = FrameNumber;
		return It->second.View;
	}

	auto FVulkanViewCache::PublishBufferView(
		FBufferViewRHIRef Candidate,
		uint64 FrameNumber) -> FBufferViewRHIRef
	{
		check(Candidate);
		std::lock_guard Lock(Mutex);
		const FBufferKey Key{Candidate->GetBuffer(), Candidate->GetDesc()};
		if (auto It = BufferViews.find(Key); It != BufferViews.end())
		{
			It->second.LastUsedFrame = FrameNumber;
			return It->second.View;
		}
		EvictOldest(BufferViews, MaximumBufferViews);
		return BufferViews.emplace(Key,
			FEntry<FRHIBufferView>{Candidate, FrameNumber}).first->second.View;
	}

	auto FVulkanViewCache::FindTextureView(
		FRHITexture* Texture,
		const FRHITextureViewDesc& Desc,
		uint64 FrameNumber) -> FTextureViewRHIRef
	{
		std::lock_guard Lock(Mutex);
		const auto* VulkanTexture = static_cast<FVulkanTexture*>(Texture);
		const auto It = TextureViews.find({Texture, VulkanTexture->Image,
			VulkanTexture->GetViewBackingGeneration(), Desc});
		if (It == TextureViews.end()) return nullptr;
		It->second.LastUsedFrame = FrameNumber;
		return It->second.View;
	}

	auto FVulkanViewCache::PublishTextureView(
		FTextureViewRHIRef Candidate,
		uint64 FrameNumber) -> FTextureViewRHIRef
	{
		check(Candidate);
		std::lock_guard Lock(Mutex);
		const auto* VulkanView = static_cast<const FVulkanTextureView*>(
			Candidate.GetReference());
		const FTextureKey Key{Candidate->GetTexture(), VulkanView->GetSourceImage(),
			VulkanView->GetTextureViewBackingGeneration(), Candidate->GetDesc()};
		if (auto It = TextureViews.find(Key); It != TextureViews.end())
		{
			It->second.LastUsedFrame = FrameNumber;
			return It->second.View;
		}
		EvictOldest(TextureViews, MaximumTextureViews);
		return TextureViews.emplace(Key,
			FEntry<FRHITextureView>{Candidate, FrameNumber}).first->second.View;
	}

	auto FVulkanViewCache::Trim(uint64 FrameNumber) -> void
	{
		std::lock_guard Lock(Mutex);
		TrimInactive(BufferViews, FrameNumber, MaximumUnusedFrames);
		TrimInactive(TextureViews, FrameNumber, MaximumUnusedFrames);
	}

	auto FVulkanViewCache::Clear() -> void
	{
		std::lock_guard Lock(Mutex);
		BufferViews.clear();
		TextureViews.clear();
	}
}
