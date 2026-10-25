#pragma once

#include "CoreMinimal.h"

#include "SceneView.h"
#include "PixelFormat.h"
#include "RenderPipelineCreation.h"

namespace Durin
{
	// Zero is background. IDs are scoped to one immutable request, never object addresses.
	struct FHitProxyId
	{
		uint32 Value = 0;
		explicit operator bool() const { return Value != 0; }
		auto operator<=>(const FHitProxyId&) const = default;
	};

	struct FHitProxyPrimitive
	{
		uint64 PrimitiveId = 0;
		FHitProxyId Id;
	};

	struct FHitProxyOverlayVertex
	{
		FVector4f ClipPosition{0.f};
		float Distance = 0.f;
	};

	// Screen-expanded interaction geometry; foreground overlays draw after world depth.
	struct FHitProxyOverlay
	{
		FHitProxyId Id;
		std::array<FHitProxyOverlayVertex, 6> Vertices;
		bool bForeground = false;
		int32 Priority = 0;
	};

	// RG32_UINT stores the ID word followed by the bit-preserved IEEE 754 distance.
	struct FHitProxyPixel
	{
		static constexpr EPixelFormat Format = EPixelFormat::RG32_UINT;
		FHitProxyId Id;
		float Distance = 0.f;
	};
	static_assert(std::is_trivially_copyable_v<FHitProxyPixel> && std::is_standard_layout_v<FHitProxyPixel>
		&& sizeof(FHitProxyPixel) == 2 * sizeof(uint32) && offsetof(FHitProxyPixel, Distance) == sizeof(uint32));
	static_assert(sizeof(float) == sizeof(uint32) && std::numeric_limits<float>::is_iec559);

	// Validated immutable CPU pixels returned after synchronous full-viewport readback.
	class FHitProxyMap final
	{
	public:
		static auto IsValidExtent(uint64 Width, uint64 Height) -> bool
		{ return Width && Height && Width <= 8192 && Height <= 8192 && Width * Height <= 8ull * 1024 * 1024; }
		static auto Create(uint32 Width, uint32 Height, FByteBuffer Pixels) -> std::shared_ptr<const FHitProxyMap>
		{
			if (!IsValidExtent(Width, Height) || Pixels.size() != uint64(Width) * Height * sizeof(FHitProxyPixel)) return {};
			return std::shared_ptr<const FHitProxyMap>(new FHitProxyMap(Width, Height, std::move(Pixels)));
		}
		auto ReadPixel(uint32 X, uint32 Y, FHitProxyPixel& Out) const -> bool
		{
			if (X >= Width || Y >= Height) return false;
			std::memcpy(&Out, Pixels.data() + (uint64(Y) * Width + X) * sizeof(FHitProxyPixel), sizeof(FHitProxyPixel));
			return true;
		}
	private:
		FHitProxyMap(uint32 InWidth, uint32 InHeight, FByteBuffer InPixels)
			: Width(InWidth), Height(InHeight), Pixels(std::move(InPixels)) {}
		const uint32 Width, Height;
		const FByteBuffer Pixels;
	};

	struct FHitProxyMapCacheStatistics
	{
		uint64 RenderCount = 0;
		uint64 ReuseCount = 0;
	};

	// Entries are owned by the render thread; statistics may be sampled from any thread.
	class FHitProxyMapCache final
	{
	public:
		auto Find_RenderThread(const FXxHash128& Key) -> std::shared_ptr<const FHitProxyMap>
		{
			if (!Entry || Entry->Key != Key) return {};
			++ReuseCount;
			return Entry->Map;
		}
		auto Publish_RenderThread(FXxHash128 Key, std::shared_ptr<const FHitProxyMap> Map, bool bFrameDependent, std::vector<FTextureRHIRef> TextureLeases) -> void
		{
			check(Map);
			Entry = FEntry{Key, std::move(Map), bFrameDependent, std::move(TextureLeases)};
			++RenderCount;
		}
		auto Invalidate_RenderThread() -> void { Entry.reset(); Preparations.Reset(); }
		auto GetPreparations_RenderThread() -> FRenderPipelineRequests& { return Preparations; }
		auto IsFrameDependent_RenderThread() const -> bool { return Entry && Entry->bFrameDependent; }
		auto GetStatistics() const -> FHitProxyMapCacheStatistics
		{ return {RenderCount.load(), ReuseCount.load()}; }
	private:
		struct FEntry
		{
			FXxHash128 Key;
			std::shared_ptr<const FHitProxyMap> Map;
			bool bFrameDependent = false;
			// Keep cached native identities alive, preventing pointer reuse before invalidation.
			std::vector<FTextureRHIRef> TextureLeases;
		};
		std::optional<FEntry> Entry;
		FRenderPipelineRequests Preparations;
		std::atomic<uint64> RenderCount = 0;
		std::atomic<uint64> ReuseCount = 0;
	};

	// GPU input for a synchronous full-map query. IDs resolve through the caller's weak identity table.
	struct FHitProxyRenderRequest
	{
		FSceneView View;
		std::vector<FHitProxyPrimitive> Primitives;
		std::vector<FHitProxyOverlay> Overlays;
		bool bSceneGeometry = true;
		std::shared_ptr<FHitProxyMapCache> Cache;
		// Copies of an uncached query share its transient preparation window.
		std::shared_ptr<FRenderPipelineRequests> Preparations = std::make_shared<FRenderPipelineRequests>();
	};
}
