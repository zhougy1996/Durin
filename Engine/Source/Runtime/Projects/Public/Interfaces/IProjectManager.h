#pragma once

#include "CoreMinimal.h"
#include "ProjectDescriptor.h"

namespace Durin
{
	struct FProjectInfo;
	enum class ELoadingPhase : uint8 { Providers, EngineReady, EditorReady };
	class IProjectManager
	{
	public:
		virtual ~IProjectManager() = default;
		virtual auto GetLastError() const -> const std::string& = 0;
		PROJECTS_API static auto Get() -> IProjectManager&;
		virtual auto LoadProjectFile(std::string_view File) -> bool = 0;
		virtual auto GetCurrentProject() const -> const FProjectDescriptor* = 0;
		virtual auto GetActiveProject() const -> const FProjectInfo* = 0;
		virtual auto ClearCurrentProject() -> void = 0;
		virtual auto LoadModulesForProject(ELoadingPhase Phase = ELoadingPhase::Providers, const std::function<bool()>& IsCanceled = {}) -> bool = 0;
		virtual auto QueryStatusForProject(std::string_view File, FProjectStatus& OutStatus) const -> bool = 0;
		virtual auto QueryStatusForCurrentProject(FProjectStatus& OutStatus) const -> bool = 0;
		virtual auto IsCurrentProjectDirty() const -> bool = 0;
		virtual auto SaveCurrentProjectToDisk(std::string& OutFailReason) -> bool = 0;
		virtual auto SetPluginEnabled(std::string_view Name, bool bEnabled, std::string& OutFailReason) -> bool = 0;
		virtual auto RemovePluginReference(std::string_view Name, std::string& OutFailReason) -> bool = 0;
	};
}
