#pragma once

namespace Durin
{
	class DTextureCube;
}

namespace Durin::Asset::Import::Standard
{
	auto PostLoadTextureCubeFeature(DTextureCube& Texture, std::string& OutError) -> bool;
}
