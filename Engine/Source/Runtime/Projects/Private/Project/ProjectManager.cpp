#include "Interfaces/IProjectManager.h"
#include "Project/Project.h"
#include "ProjectContextInternal.h"
#include "Modules/ModuleManager.h"
#include "Misc/StringHelper.h"

namespace Durin
{
	namespace
	{
		class FProjectManager final : public IProjectManager
		{
			std::optional<FProjectDescriptor> Descriptor;
			std::optional<FProjectInfo> Active;
			mutable std::string LastError;
			bool bDirty = false;
			bool bModulesStarted = false;
		public:
			auto GetLastError() const -> const std::string& override { return LastError; }
			auto Fail(std::string Message,std::string* OutReason=nullptr) const -> bool
			{ LastError=std::move(Message); if (OutReason) *OutReason=LastError; return false; }
			auto LoadProjectFile(std::string_view InFile) -> bool override
			{
				if (bModulesStarted) return Fail("Changing the active project requires restart");
				FProjectDescriptor Candidate;
				if (!Candidate.Load(InFile,LastError)) return false;
				auto Committed=CommitProjectContext(InFile,Candidate);
				if (!Committed) return Fail(Committed.error());
				Active=std::move(*Committed);
				Descriptor = std::move(Candidate);
				bDirty = false;
				LastError.clear(); return true;
			}
			auto GetActiveProject() const -> const FProjectInfo* override { return Active ? &*Active : nullptr; }
			auto SetHistoryRecording(bool Enabled) -> void { if (Active) Active->bRecordInteractiveHistory=Enabled; }
			auto GetCurrentProject() const -> const FProjectDescriptor* override { return Descriptor ? &*Descriptor : nullptr; }
			auto ClearCurrentProject() -> void override { ClearProjectContext(); Descriptor.reset(); Active.reset(); bDirty=false; bModulesStarted=false; }
			auto LoadModulesForProject(ELoadingPhase Phase,const std::function<bool()>& IsCanceled) -> bool override
			{
				if (Phase!=ELoadingPhase::Providers) { LastError.clear(); return true; }
				const auto* Context=Durin::GetCurrentProject();
				if (!Context) { LastError.clear(); return true; }
				const auto Roots=Context->EnabledRootModules;
				for (const auto& Name : Roots)
				{
					if (IsCanceled && IsCanceled()) return Fail("Project module startup canceled");
					bModulesStarted=true;
					if (!FModuleManager::Get().LoadModule(FName(Name))) return Fail("Project module could not start: "+Name);
				}
				bModulesStarted=true;
				LastError.clear(); return true;
			}
			auto QueryStatusForProject(std::string_view InFile,FProjectStatus& OutStatus) const -> bool override
			{
				FProjectDescriptor Candidate;
				if (!Candidate.Load(InFile,LastError)) return false;
				OutStatus={Candidate.Name,NormalizeProjectFile(InFile)}; LastError.clear(); return true;
			}
			auto QueryStatusForCurrentProject(FProjectStatus& OutStatus) const -> bool override
			{
				if (!Descriptor) return Fail("No project loaded");
				OutStatus={Descriptor->Name,Active->ProjectFile}; LastError.clear(); return true;
			}
			auto IsCurrentProjectDirty() const -> bool override { return bDirty; }
			auto SaveCurrentProjectToDisk(std::string& OutReason) -> bool override
			{
				if (!Descriptor) return Fail("No project loaded",&OutReason);
				if (auto Ownership = AcquireProjectEditOwnership(); !Ownership) return Fail(Ownership.error().ToString(),&OutReason);
				if (!Descriptor->Save(Active->ProjectFile,LastError)) return Fail(LastError,&OutReason);
				bDirty = false;
				LastError.clear(); OutReason.clear(); return true;
			}
			auto SetPluginEnabled(std::string_view Name,bool bEnabled,std::string& OutReason) -> bool override
			{
				if (!Descriptor) return Fail("No project loaded",&OutReason);
				if (!IsValidProjectIdentifier(Name)) return Fail("Invalid plugin name",&OutReason);
				auto Key=std::string(Name);
				for (const auto& Entry : Descriptor->PluginOverrides)
					if (StringUtils::FoldAscii(Entry.first)==StringUtils::FoldAscii(Key)) { Key=Entry.first; break; }
				if (!Descriptor->PluginOverrides.contains(Key) || Descriptor->PluginOverrides.at(Key)!=bEnabled)
				{ Descriptor->PluginOverrides[Key]=bEnabled; bDirty=true; }
				LastError.clear(); OutReason.clear(); return true;
			}
			auto RemovePluginReference(std::string_view Name,std::string& OutReason) -> bool override
			{
				if (!Descriptor) return Fail("No project loaded",&OutReason);
				for (auto It=Descriptor->PluginOverrides.begin(); It!=Descriptor->PluginOverrides.end(); ++It)
					if (StringUtils::FoldAscii(It->first)==StringUtils::FoldAscii(std::string(Name)))
					{ Descriptor->PluginOverrides.erase(It); bDirty=true; break; }
				LastError.clear(); OutReason.clear(); return true;
			}
		};
	}

	namespace { auto Manager() -> FProjectManager& { static FProjectManager Instance; return Instance; } }
	auto IProjectManager::Get() -> IProjectManager& { return Manager(); }
	auto InitializeCurrentProject(const FProjectInitializationParams& Params) -> std::expected<void,FProjectError>
	{
		auto& Service=Manager();
		if (Params.bOpenProjectBrowser || Params.RequestedProjectFile.empty()) { Service.ClearCurrentProject(); return {}; }
		if (!Service.LoadProjectFile(Params.RequestedProjectFile)) return std::unexpected(FProjectError{EProjectError::InvalidDescriptor,Service.GetLastError()});
		Service.SetHistoryRecording(Params.bRecordInteractiveHistory); return {};
	}
}
