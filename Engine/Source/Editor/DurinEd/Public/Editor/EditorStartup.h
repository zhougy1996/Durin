#pragma once

#include "CoreMinimal.h"
#include "DurinEdAPI.h"

namespace Durin { class FSoftObjectPath; struct FProjectInfo; }

namespace Durin::EditorStartup
{
	// Empty OutPath means no startup level; validation never loads or activates assets.
	DURINED_API auto ResolveStartupLevel(const FProjectInfo& Project, FSoftObjectPath& OutPath, std::string& OutError) -> bool;

}
