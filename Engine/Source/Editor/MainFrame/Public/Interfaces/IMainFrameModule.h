#pragma once

namespace Durin
{
	// Describes the game-thread-owned, forward-only editor host bootstrap.
	enum class EEditorBootstrapState : uint8
	{
		ConstructingShell,
		WaitingForFirstPresent,
		LoadingWorkspace,
		Ready,
		Failed,
	};

	constexpr auto IsValidEditorBootstrapTransition(
		EEditorBootstrapState From,
		EEditorBootstrapState To) -> bool
	{
		switch (From)
		{
		case EEditorBootstrapState::ConstructingShell:
			return To == EEditorBootstrapState::WaitingForFirstPresent;
		case EEditorBootstrapState::WaitingForFirstPresent:
			return To == EEditorBootstrapState::LoadingWorkspace
				|| To == EEditorBootstrapState::Ready;
		case EEditorBootstrapState::LoadingWorkspace:
			return To == EEditorBootstrapState::Ready
				|| To == EEditorBootstrapState::Failed;
		case EEditorBootstrapState::Ready:
		case EEditorBootstrapState::Failed:
			return false;
		}
		return false;
	}

	// Defines the module boundary that hosts editor workspaces in the main frame.
	class IMainFrameModule : public IModuleInterface
	{
	public:
		virtual auto CreateDefaultMainFrame() -> void = 0;
		virtual auto DestroyDefaultMainFrame() -> void = 0;
		virtual auto TickDefaultMainFrameBootstrap() -> void = 0;
		virtual auto GetDefaultMainFrameBootstrapState() const
			-> EEditorBootstrapState = 0;
	};
}
