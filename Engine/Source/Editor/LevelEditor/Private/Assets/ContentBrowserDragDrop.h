#pragma once

#include <array>

namespace Durin::Editor::Level
{
	inline constexpr const char* ContentBrowserAssetPayloadType = "DURIN_CONTENT_ASSET";

	// Carries fixed-size asset identity through an ImGui drag/drop payload.
	struct FContentBrowserAssetPayload
	{
		std::array<char, 256> AssetPath{};
		std::array<char, 192> AssetClassName{};
	};
}
