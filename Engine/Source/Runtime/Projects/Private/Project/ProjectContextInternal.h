#pragma once

#include "CoreMinimal.h"
#include "ProjectDescriptor.h"
#include "Project/Project.h"

namespace Durin
{
	auto IsValidProjectIdentifier(std::string_view Name) -> bool;
	auto ReadProjectMounts(FJsonNodeView Root, std::string_view File, std::vector<FMountPoint>& Definitions, std::string* OutError) -> bool;
	auto BuildProjectMountDefinitions(std::string_view File, const std::vector<FMountPoint>& Additional) -> std::vector<FMountPoint>;
	auto CommitProjectContext(std::string_view File, const FProjectDescriptor& Descriptor) -> std::expected<FProjectInfo, std::string>;
	auto ClearProjectContext() -> void;
}
