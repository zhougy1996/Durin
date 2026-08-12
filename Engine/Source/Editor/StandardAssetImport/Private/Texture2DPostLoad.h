#pragma once

namespace Durin::StandardAssetImport
{
	auto RegisterTexture2DPostLoadPolicy() -> bool;
	auto UnregisterTexture2DPostLoadPolicy() -> void;
}
