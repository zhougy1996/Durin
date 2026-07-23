#pragma once

#include <array>

namespace Durin
{
	inline constexpr const char* ContentBrowserAssetPayloadType = "DURIN_CONTENT_ASSET";

	struct FContentBrowserAssetPayload
	{
		std::array<char, 256> AssetPath{};
		std::array<char, 192> AssetClassName{};
	};
}
