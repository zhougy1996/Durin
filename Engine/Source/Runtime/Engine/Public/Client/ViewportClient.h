#pragma once

#include "EngineAPI.h"
namespace Durin
{
	struct FSceneView;

	class ENGINE_API FViewportClient
	{
	public:
		virtual ~FViewportClient();
		virtual auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool;
	};
}
