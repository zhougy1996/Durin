#pragma once

namespace Durin
{
	struct FLevelEditorContext;

	// Defines one independently drawn panel within the level-editor workspace.
	class ILevelEditorPanel
	{
	public:
		virtual ~ILevelEditorPanel() = default;

		virtual auto GetWindowName() const -> const char* = 0;
		virtual auto TickWhenHidden() -> void {}
		virtual auto Draw(FLevelEditorContext& Context) -> void = 0;

		auto IsOpen() const -> bool { return bOpen; }
		auto SetOpen(bool bInOpen) -> void { bOpen = bInOpen; }
		auto GetOpenPtr() -> bool* { return &bOpen; }

	private:
		bool bOpen = true;
	};
} // namespace Durin
