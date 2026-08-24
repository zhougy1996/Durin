#pragma once

namespace Durin
{
	class DTextureCube;
}

namespace Durin::AssetForge::Builtins
{
	auto PostLoadTextureCubeFeature(DTextureCube& Texture, std::string& OutError) -> bool;
}
