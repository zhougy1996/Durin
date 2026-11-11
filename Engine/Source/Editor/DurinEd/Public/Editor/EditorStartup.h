#pragma once

#include "CoreMinimal.h"
#include "Engine/HostStartup.h"
#include "DurinEdAPI.h"

namespace Durin { class FSoftObjectPath; struct FProjectInfo; }

namespace Durin::EditorStartup
{
	// Empty OutPath means no startup level; validation never loads or activates assets.
	DURINED_API auto ResolveStartupLevel(const FProjectInfo& Project, FSoftObjectPath& OutPath, std::string& OutError) -> bool;

	// Describes the game-thread-owned, forward-only editor startup sequence.
	enum class EBootstrapState : uint8
	{
		InitializingProject,
		LoadingWorkspace,
		Ready,
		Failed,
	};

	constexpr auto GetBootstrapStepStatus(EBootstrapState State)
		-> EHostStartupState
	{
		if (State == EBootstrapState::Ready)
			return EHostStartupState::Ready;
		if (State == EBootstrapState::Failed)
			return EHostStartupState::Failed;
		return EHostStartupState::Pending;
	}

	constexpr auto IsValidBootstrapTransition(
		EBootstrapState From,
		EBootstrapState To) -> bool
	{
		switch (From)
		{
		case EBootstrapState::InitializingProject:
			return To == EBootstrapState::LoadingWorkspace
				|| To == EBootstrapState::Ready || To == EBootstrapState::Failed;
		case EBootstrapState::LoadingWorkspace:
			return To == EBootstrapState::Ready
				|| To == EBootstrapState::Failed;
		case EBootstrapState::Ready:
		case EBootstrapState::Failed:
			return false;
		}
		return false;
	}
}
