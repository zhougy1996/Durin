#pragma once

namespace Durin
{
	class DTexture2D;
}

namespace Durin::AssetForge::Builtins
{
	auto PostLoadTexture2DFeature(DTexture2D& Texture, std::string& OutError) -> bool;
	auto WaitForTexture2DImportRecovery(
		DTexture2D& Texture, double TimeoutSeconds) -> bool;
}
