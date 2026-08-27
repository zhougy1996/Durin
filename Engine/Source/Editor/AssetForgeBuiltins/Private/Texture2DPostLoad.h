#pragma once

namespace Durin
{
	class DTexture2D;
}

namespace Durin::AssetForge::Builtins
{
	auto PostLoadTexture2DFeature(DTexture2D& Texture, std::string& OutError) -> bool;
}
