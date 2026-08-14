#pragma once

namespace Durin::Asset::Import::Standard
{
	auto RegisterTextureCubePostLoadPolicy() -> bool;
	auto UnregisterTextureCubePostLoadPolicy() -> void;
}
