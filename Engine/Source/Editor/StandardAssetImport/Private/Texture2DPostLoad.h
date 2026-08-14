#pragma once

namespace Durin::Asset::Import::Standard
{
	auto RegisterTexture2DPostLoadPolicy() -> bool;
	auto UnregisterTexture2DPostLoadPolicy() -> void;
}
