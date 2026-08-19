#pragma once

namespace Durin
{
	class DTexture2D;
}

namespace Durin::Asset::Forge
{
	auto PostLoadTexture2DFeature(DTexture2D& Texture, std::string& OutError) -> bool;
}
