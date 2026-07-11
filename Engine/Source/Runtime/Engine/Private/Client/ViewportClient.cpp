#include "Client/ViewportClient.h"

namespace Durin
{
	FViewportClient::~FViewportClient() = default;

	auto FViewportClient::CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		return false;
	}
}
