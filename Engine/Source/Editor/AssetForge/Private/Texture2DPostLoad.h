#pragma once

namespace Durin
{
	class DTexture2D;
}

namespace Durin::Asset::Forge
{
	auto PostLoadTexture2DFeature(DTexture2D& Texture, std::string& OutError) -> bool;
	auto WaitForTexture2DInterchangeRecovery(
		DTexture2D& Texture, double TimeoutSeconds) -> bool;
}
