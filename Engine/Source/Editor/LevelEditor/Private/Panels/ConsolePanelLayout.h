#pragma once

namespace Durin::Editor::Level
{
	struct FConsoleVisibleRange
	{
		size_t Begin = 0;
		size_t End = 0;
	};

	// Resolves the records intersecting a variable-height scrolling viewport.
	// Offsets contains one entry per record plus the total height sentinel.
	inline auto ResolveConsoleVisibleRange(
		std::span<const float> Offsets, float ScrollY, float ViewportHeight) -> FConsoleVisibleRange
	{
		if (Offsets.size() < 2) return {};
		const size_t RecordCount = Offsets.size() - 1;
		const float ClipMin = std::max(0.0f, ScrollY);
		const float ClipMax = ClipMin + std::max(0.0f, ViewportHeight);
		auto BeginIt = std::upper_bound(Offsets.begin(), Offsets.end(), ClipMin);
		size_t Begin = BeginIt == Offsets.begin()
			? 0
			: static_cast<size_t>(std::distance(Offsets.begin(), BeginIt) - 1);
		Begin = Begin > 0 ? Begin - 1 : 0;
		auto EndIt = std::lower_bound(Offsets.begin(), Offsets.end(), ClipMax);
		size_t End = std::min(RecordCount,
			static_cast<size_t>(std::distance(Offsets.begin(), EndIt)) + 1);
		return {std::min(Begin, RecordCount), std::max(std::min(End, RecordCount), Begin)};
	}
} // namespace Durin::Editor::Level
