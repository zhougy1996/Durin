#pragma once

namespace Durin
{
	// Describes the game-thread-owned, forward-only editor host bootstrap.
	enum class EEditorBootstrapState : uint8
	{
		ConstructingShell,
		WaitingForFirstPresent,
		LoadingWorkspace,
		WorkspaceReady,
		LoadingDefaultDocument,
		Ready,
		Failed,
	};

	// Publishes default-document readiness separately from workspace readiness.
	enum class EEditorDefaultDocumentState : uint8
	{
		NotApplicable,
		Pending,
		Loading,
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
			return To == EEditorBootstrapState::WorkspaceReady
				|| To == EEditorBootstrapState::Failed;
		case EEditorBootstrapState::WorkspaceReady:
			return To == EEditorBootstrapState::LoadingDefaultDocument;
		case EEditorBootstrapState::LoadingDefaultDocument:
			return To == EEditorBootstrapState::Ready;
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
		virtual auto GetDefaultDocumentState() const
			-> EEditorDefaultDocumentState = 0;
	};
}
