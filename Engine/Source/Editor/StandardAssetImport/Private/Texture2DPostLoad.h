#pragma once

namespace Durin::Asset::Import
{
	auto RegisterTexture2DPostLoadPolicy() -> bool;
	auto UnregisterTexture2DPostLoadPolicy() -> void;
}
