#pragma once

#include "RHIResources.h"

namespace Durin::VulkanRHI
{
	// Retains a bounded working set of automatically synthesized immutable views.
	class FVulkanViewCache
	{
	public:
		FVulkanViewCache() = default;
		~FVulkanViewCache();

		auto FindBufferView(FRHIBuffer* Buffer, const FRHIBufferViewDesc& Desc,
			uint64 FrameNumber) -> FBufferViewRHIRef;
		auto PublishBufferView(FBufferViewRHIRef Candidate, uint64 FrameNumber)
			-> FBufferViewRHIRef;
		auto FindTextureView(FRHITexture* Texture, const FRHITextureViewDesc& Desc,
			uint64 FrameNumber) -> FTextureViewRHIRef;
		auto PublishTextureView(FTextureViewRHIRef Candidate, uint64 FrameNumber)
			-> FTextureViewRHIRef;

		// Releases inactive entries without invalidating references retained by
		// already-recorded commands.
		auto Trim(uint64 FrameNumber) -> void;
		auto Clear() -> void;

	private:
		struct FBufferKey
		{
			FRHIBuffer* Buffer = nullptr;
			FRHIBufferViewDesc Desc{};
			auto operator==(const FBufferKey&) const -> bool = default;
		};

		struct FTextureKey
		{
			FRHITexture* Texture = nullptr;
			vk::Image Image{};
			uint64 BackingGeneration = 0;
			FRHITextureViewDesc Desc{};
			auto operator==(const FTextureKey&) const -> bool = default;
		};

		struct FBufferKeyHash
		{
			auto operator()(const FBufferKey& Key) const -> size_t;
		};

		struct FTextureKeyHash
		{
			auto operator()(const FTextureKey& Key) const -> size_t;
		};

		template<typename ViewType>
		struct FEntry
		{
			TRefCountPtr<ViewType> View;
			uint64 LastUsedFrame = 0;
		};

		static constexpr size_t MaximumBufferViews = 2048;
		static constexpr size_t MaximumTextureViews = 2048;
		static constexpr uint64 MaximumUnusedFrames = 120;

		std::mutex Mutex;
		std::unordered_map<FBufferKey, FEntry<FRHIBufferView>, FBufferKeyHash>
			BufferViews;
		std::unordered_map<FTextureKey, FEntry<FRHITextureView>, FTextureKeyHash>
			TextureViews;
	};
}
