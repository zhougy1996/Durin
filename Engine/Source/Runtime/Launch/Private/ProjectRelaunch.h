#pragma once
#include "CoreMinimal.h"
#include "Project/ProjectError.h"
namespace Durin { auto LaunchPendingEditorRelaunch() -> std::expected<void, FProjectError>; }
