#pragma once

namespace Durin::Editor::Host
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

	// Summarizes whether the host bootstrap should continue, exit, or fail.
	enum class EBootstrapStepStatus : uint8
	{
		Pending,
		Ready,
		Failed,
	};

	// Carries the observable state of one editor-host bootstrap step.
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

	// Defines the host lifecycle required by the editor engine without naming its UI implementation.
	class IEditorHost : public IModuleInterface
	{
	public:
		virtual auto CreateEditorHost(
			std::shared_ptr<MWindow> StartupWindow) -> void = 0;
		virtual auto DestroyEditorHost() -> void = 0;
		virtual auto AdvanceBootstrap(bool bFirstPresentAvailable)
			-> Editor::Host::FBootstrapProgress = 0;
		virtual auto GetBootstrapProgress() const
			-> Editor::Host::FBootstrapProgress = 0;
		virtual auto GetBootstrapState() const
			-> Editor::Host::EBootstrapState = 0;
		virtual auto GetDefaultDocumentState() const
			-> Editor::Host::EDefaultDocumentState = 0;
	};
}
