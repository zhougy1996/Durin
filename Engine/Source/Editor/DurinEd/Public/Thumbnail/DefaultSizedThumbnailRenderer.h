#pragma once

#include "Thumbnail/ThumbnailManager.h"

namespace Durin::Editor
{
	// Supplies the fixed production output selected by the thumbnail contract.
	// Requested UI dimensions are presentation-only and never expand cache identity.
	class DDefaultSizedThumbnailRenderer : public DThumbnailRenderer
	{
	public:
		static constexpr uint32 DefaultWidth = 256;
		static constexpr uint32 DefaultHeight = 256;

	protected:
		static auto MakeDefaultOutput() -> FAssetThumbnailOutputSettings
		{
			return {.Width = DefaultWidth, .Height = DefaultHeight};
		}
	};
}
