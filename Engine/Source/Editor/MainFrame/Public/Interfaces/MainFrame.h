#pragma once

namespace Durin::Editor::MainFrame
{
	// Describes the game-thread-owned, forward-only editor host bootstrap.
	enum class EBootstrapState : uint8
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
	enum class EDefaultDocumentState : uint8
	{
		NotApplicable,
		Pending,
		Loading,
		Ready,
		Failed,
	};

	enum class EBootstrapStepStatus : uint8
	{
		Pending,
		Ready,
		Failed,
	};

	struct FBootstrapProgress
	{
		EBootstrapState State = EBootstrapState::ConstructingShell;
		EDefaultDocumentState DefaultDocumentState =
			EDefaultDocumentState::NotApplicable;
		EBootstrapStepStatus Status = EBootstrapStepStatus::Pending;
		uint8 PhaseIndex = 0;
		uint8 PhaseCount = 3;
		std::string Message;
	};

	constexpr auto GetBootstrapPhaseIndex(EBootstrapState State)
		-> uint8
	{
		switch (State)
		{
		case EBootstrapState::ConstructingShell:
		case EBootstrapState::WaitingForFirstPresent:
			return 1;
		case EBootstrapState::LoadingWorkspace:
		case EBootstrapState::WorkspaceReady:
			return 2;
		case EBootstrapState::LoadingDefaultDocument:
		case EBootstrapState::Ready:
			return 3;
		case EBootstrapState::Failed:
			return 0;
		}
		return 0;
	}

	constexpr auto GetBootstrapStepStatus(EBootstrapState State)
		-> EBootstrapStepStatus
	{
		if (State == EBootstrapState::Ready)
			return EBootstrapStepStatus::Ready;
		if (State == EBootstrapState::Failed)
			return EBootstrapStepStatus::Failed;
		return EBootstrapStepStatus::Pending;
	}

	constexpr auto IsValidBootstrapTransition(
		EBootstrapState From,
		EBootstrapState To) -> bool
	{
		switch (From)
		{
		case EBootstrapState::ConstructingShell:
			return To == EBootstrapState::WaitingForFirstPresent;
		case EBootstrapState::WaitingForFirstPresent:
			return To == EBootstrapState::LoadingWorkspace
				|| To == EBootstrapState::Ready;
		case EBootstrapState::LoadingWorkspace:
			return To == EBootstrapState::WorkspaceReady
				|| To == EBootstrapState::Failed;
		case EBootstrapState::WorkspaceReady:
			return To == EBootstrapState::LoadingDefaultDocument;
		case EBootstrapState::LoadingDefaultDocument:
			return To == EBootstrapState::Ready
				|| To == EBootstrapState::Failed;
		case EBootstrapState::Ready:
		case EBootstrapState::Failed:
			return false;
		}
		return false;
	}

}

namespace Durin
{
	class MWindow;

	// Defines the module boundary that hosts editor workspaces in the main frame.
	class IMainFrameModule : public IModuleInterface
	{
	public:
		virtual auto CreateDefaultFrame(
			std::shared_ptr<MWindow> StartupWindow) -> void = 0;
		virtual auto DestroyDefaultFrame() -> void = 0;
		virtual auto AdvanceDefaultBootstrap(bool bFirstPresentAvailable)
			-> Editor::MainFrame::FBootstrapProgress = 0;
		virtual auto GetDefaultBootstrapProgress() const
			-> Editor::MainFrame::FBootstrapProgress = 0;
		virtual auto GetDefaultBootstrapState() const
			-> Editor::MainFrame::EBootstrapState = 0;
		virtual auto GetDefaultDocumentState() const
			-> Editor::MainFrame::EDefaultDocumentState = 0;
	};
}
