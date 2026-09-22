#pragma once

#include "SceneView.h"
#include "RHITextureReadback.h"

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

	// Detached GPU input. The caller retains the corresponding weak identity table.
	struct FHitProxyRenderRequest
	{
		FSceneView View;
		std::vector<FHitProxyPrimitive> Primitives;
		std::vector<FHitProxyOverlay> Overlays;
		bool bSceneGeometry = true;
		std::shared_ptr<FRHITextureReadback> Readback;
	};
}
