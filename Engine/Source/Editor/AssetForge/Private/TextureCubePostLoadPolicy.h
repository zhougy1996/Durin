#pragma once

namespace Durin
{
	class DTextureCube;
}

namespace Durin::Asset::Forge
{
	auto PostLoadTextureCubeFeature(DTextureCube& Texture, std::string& OutError) -> bool;
}
