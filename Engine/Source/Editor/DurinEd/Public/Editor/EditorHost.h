#pragma once

#include "CoreMinimal.h"
#include "Engine/HostStartup.h"

namespace Durin { class FViewportOutputReceipt; }

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

	// Summarizes whether the host bootstrap should continue, exit, or fail.
	enum class EBootstrapStepStatus : uint8
	{
		Pending,
		Ready,
		Failed,
	};

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
				|| To == EBootstrapState::Ready || To == EBootstrapState::Failed;
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
	class DEditorEngine;

	// Defines the host lifecycle required by the editor engine without naming its UI implementation.
	class IEditorHost : public IModuleInterface
	{
	public:
		virtual auto CreateEditorHost(
			DEditorEngine& Editor, std::shared_ptr<MWindow> StartupWindow, std::string InitialProjectError) -> void = 0;
		virtual auto DestroyEditorHost() -> void = 0;
		virtual auto IsShellPresented() const -> bool = 0;
		virtual auto IsScenePresented(const FViewportOutputReceipt& Receipt, uint64 Generation) const -> bool = 0;
		virtual auto PublishBootstrapState(Editor::Host::EBootstrapState State, std::string Error = {}) -> void = 0;
		virtual auto ActivateWorkspaces(std::string& OutError) -> bool = 0;
		virtual auto OpenDefaultDocument(std::string& OutError) -> EHostStartupState = 0;
		virtual auto AdvanceDocumentOperations() -> void = 0;
	};
}
