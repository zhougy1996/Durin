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

	enum class EEditorBootstrapStepStatus : uint8
	{
		Pending,
		Ready,
		Failed,
	};

	struct FEditorBootstrapProgress
	{
		EEditorBootstrapState State = EEditorBootstrapState::ConstructingShell;
		EEditorDefaultDocumentState DefaultDocumentState =
			EEditorDefaultDocumentState::NotApplicable;
		EEditorBootstrapStepStatus Status = EEditorBootstrapStepStatus::Pending;
		uint8 PhaseIndex = 0;
		uint8 PhaseCount = 3;
		std::string Message;
	};

	constexpr auto GetEditorBootstrapPhaseIndex(EEditorBootstrapState State)
		-> uint8
	{
		switch (State)
		{
		case EEditorBootstrapState::ConstructingShell:
		case EEditorBootstrapState::WaitingForFirstPresent:
			return 1;
		case EEditorBootstrapState::LoadingWorkspace:
		case EEditorBootstrapState::WorkspaceReady:
			return 2;
		case EEditorBootstrapState::LoadingDefaultDocument:
		case EEditorBootstrapState::Ready:
			return 3;
		case EEditorBootstrapState::Failed:
			return 0;
		}
		return 0;
	}

	constexpr auto GetEditorBootstrapStepStatus(EEditorBootstrapState State)
		-> EEditorBootstrapStepStatus
	{
		if (State == EEditorBootstrapState::Ready)
			return EEditorBootstrapStepStatus::Ready;
		if (State == EEditorBootstrapState::Failed)
			return EEditorBootstrapStepStatus::Failed;
		return EEditorBootstrapStepStatus::Pending;
	}

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
		virtual auto AdvanceDefaultMainFrameBootstrap(bool bFirstPresentAvailable)
			-> FEditorBootstrapProgress = 0;
		virtual auto GetDefaultMainFrameBootstrapProgress() const
			-> FEditorBootstrapProgress = 0;
		virtual auto GetDefaultMainFrameBootstrapState() const
			-> EEditorBootstrapState = 0;
		virtual auto GetDefaultDocumentState() const
			-> EEditorDefaultDocumentState = 0;
	};
}
