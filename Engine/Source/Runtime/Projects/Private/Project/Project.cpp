#include "Project/Project.h"
#include "ProjectDescriptor.h"
#include "Interfaces/IProjectManager.h"
#include "ProjectContextInternal.h"
#include "Misc/FileHelper.h"

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Json/Json.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Project/ProjectHistory.h"
#include "Misc/StringHelper.h"

#ifdef _WIN32
	#include "Windows/WindowsPlatform.h"
#elif defined(__APPLE__)
	#include <fcntl.h>
	#include <sys/file.h>
	#include <unistd.h>
#endif

namespace Durin
{
	namespace
	{
		std::optional<std::vector<std::string>> GPendingEditorRelaunchArguments;
#ifdef _WIN32
		HANDLE GProjectEditMutex = nullptr;
#elif defined(__APPLE__)
		int GProjectEditFile = -1;
		pid_t GProjectEditOwnerProcess = 0;
#endif

		auto Normalize(const std::filesystem::path& Path) -> std::string
		{
			std::error_code Error;
			const std::filesystem::path Absolute = std::filesystem::absolute(Path, Error).lexically_normal();
			return (Error ? Path.lexically_normal() : Absolute).generic_string();
		}
		auto IsIdentifier(std::string_view Name) -> bool
		{
			const auto Letter=[](char C) { return (C>='a'&&C<='z')||(C>='A'&&C<='Z')||C=='_'; };
			return !Name.empty() && Letter(Name.front()) && std::ranges::all_of(Name,[&](char C) { return Letter(C)||(C>='0'&&C<='9'); });
		}
		auto ValidateObjectFields(FJsonNodeView Node, std::initializer_list<std::string_view> Allowed) -> bool
		{
			if (!Node.IsObject()) return false;
			std::set<std::string> Seen;
			bool Valid=true;
			Node.ForEachObjectMember([&](std::string_view Key,FJsonNodeView) {
				if (!Seen.insert(std::string(Key)).second || std::find(Allowed.begin(),Allowed.end(),Key)==Allowed.end()) Valid=false;
			});
			return Valid;
		}

		auto ReadModuleArray(
			FJsonNodeView Array,
			std::string_view FieldName,
			std::vector<std::string>& OutModules,
			std::unordered_set<std::string>& SeenModules,
			std::string* OutError
		) -> bool
		{
			if (!Array.IsArray())
			{
				if (OutError) *OutError = std::format(
					"Project descriptor field '{}' must be an array of module names.", FieldName);
				return false;
			}
			std::set<std::string> Local;
			for (size_t Index = 0; Index < Array.Num(); ++Index)
			{
				const FJsonNodeView Entry = Array.GetView(Index);
				if (!Entry.IsString() || !IsIdentifier(Entry.GetString()) || !Local.insert(StringUtils::FoldAscii(Entry.GetString())).second)
				{
					if (OutError) *OutError = std::format(
						"Project descriptor field '{}[{}]' must be a non-empty module name.",
						FieldName, Index);
					return false;
				}
				std::string ModuleName = Entry.GetString();
				if (SeenModules.insert(ModuleName).second)
					OutModules.push_back(std::move(ModuleName));
			}
			return true;
		}


	}

	auto IsValidProjectIdentifier(std::string_view Name) -> bool { return IsIdentifier(Name); }
	auto NormalizeProjectFile(std::string_view ProjectFile) -> std::string { return Normalize(ProjectFile); }

	namespace
	{
		auto Fail(std::string& OutError,std::string Message) -> bool { OutError=std::move(Message); return false; }
	}
	auto FProjectDescriptor::Load(std::string_view File,std::string& OutFailReason) -> bool
	{
		FJsonDocument Source;
		if (auto Loaded=Source.LoadFromFile(std::string(File)); !Loaded) return Fail(OutFailReason,Loaded.error().ToString());
		return Read(Source,File,OutFailReason);
	}
	auto FProjectDescriptor::Read(const FJsonDocument& Source, std::string_view File, std::string& OutFailReason) -> bool
	{
		const auto Root=Source.GetRootView();
		if (!ValidateObjectFields(Root,{"$schema","ProjectName","ModuleDirs","BaseModules","ExtraModules","Tests","Mounts","PluginDependencies"}))
			return Fail(OutFailReason,"Invalid or duplicate project fields");
		const auto ProjectName = Root.GetView("ProjectName").GetString();
		if (!IsIdentifier(ProjectName)) return Fail(OutFailReason, "Project descriptor has no valid ProjectName");
		if (Root.Contains("$schema") && (!Root.GetView("$schema").IsString() || Root.GetView("$schema").GetString().empty()))
			return Fail(OutFailReason,"Invalid schema annotation");

		FProjectDescriptor Parsed;

		Parsed.Name = ProjectName;
		if (Root.Contains("PluginDependencies"))
		{
			std::unordered_set<std::string> Seen;
			if (!ReadModuleArray(Root.GetView("PluginDependencies"), "PluginDependencies", Parsed.PluginDependencies, Seen, &OutFailReason)) return false;
		}
		std::string Error;
		std::set<std::string> ModuleNames;
		const auto Directories = Root.GetView("ModuleDirs");
		if (Directories.IsValid() && !Directories.IsObject()) Error = "ModuleDirs must be an object";
		Directories.ForEachObjectMember([&](std::string_view Key, FJsonNodeView Value) {
			const auto Directory=std::filesystem::path(Value.GetString());
			if (!IsIdentifier(Key) || !ModuleNames.insert(StringUtils::FoldAscii(std::string(Key))).second || !Value.IsString() || Value.GetString().empty()
				|| Directory.is_absolute() || Directory.has_root_name() || std::ranges::any_of(Directory,[](const auto& Part) { return Part==".."; })) Error = "Invalid ModuleDirs entry";
			else { Parsed.ModuleDirs.emplace(Key, Value.GetString()); Parsed.ModuleOrder.emplace_back(Key); }
		});
		if (Root.Contains("$schema")) Parsed.Schema=Root.GetView("$schema").GetString();
		if (Root.Contains("BaseModules"))
		{
			Parsed.BaseModules.emplace(); std::unordered_set<std::string> Seen;
			if (!ReadModuleArray(Root.GetView("BaseModules"),"BaseModules",*Parsed.BaseModules,Seen,&Error))
				return Fail(OutFailReason,Error);
		}
		const auto Variants=Root.GetView("ExtraModules");
		if (Variants.IsValid() && !Variants.IsObject()) Error="ExtraModules must be an object";
		std::set<std::string> VariantNames;
		Variants.ForEachObjectMember([&](std::string_view Key,FJsonNodeView Value) {
			if (!IsIdentifier(Key) || !VariantNames.insert(StringUtils::FoldAscii(std::string(Key))).second || !ValidateObjectFields(Value,{"Modules"})) Error="Invalid runtime variant";
			std::vector<std::string> Names; std::unordered_set<std::string> Seen;
			if (Value.Contains("Modules") && !ReadModuleArray(Value.GetView("Modules"),"Modules",Names,Seen,&Error)) Error="Invalid runtime variant modules";
			Parsed.ExtraModules.emplace(Key,std::move(Names));
		});
		const auto Tests=Root.GetView("Tests");
		if (Tests.IsValid())
		{
			if (!ValidateObjectFields(Tests,{"Native"})) Error="Invalid Tests fields";
			if (Tests.Contains("Native"))
			{
				const auto Native=Tests.GetView("Native"); const auto Path=Native.GetView("Root").GetString();
				const std::filesystem::path Relative(Path);
				Parsed.NativeTestRoot=Path;
				if (!ValidateObjectFields(Native,{"Root"}) || Path.empty() || Relative.is_absolute() || Relative.has_root_name()
					|| Relative=="." || std::ranges::any_of(Relative,[](const auto& P) { return P==".."; })) Error="Invalid Tests.Native.Root";
			}
		}
		if (!ReadProjectMounts(Root, Normalize(File), Parsed.Mounts, &Error) || !Error.empty())
			return Fail(OutFailReason, Error);
		const auto Definitions=BuildProjectMountDefinitions(Normalize(File), Parsed.Mounts);
		if (!FMountPaths::ValidateMountDefinitions(Definitions, &Error))
			return Fail(OutFailReason, Error);
		for (const auto& Variant : {"DurinEditor", "DurinGame"})
			if (const auto Roots = Parsed.GetEnabledRootModules(Variant); !Roots) return Fail(OutFailReason,Roots.error());
		*this = std::move(Parsed);
		OutFailReason.clear(); return true;
	}

	auto FProjectDescriptor::GetEnabledRootModules(std::string_view Variant) const -> std::expected<std::vector<std::string>,std::string>
	{
		std::vector<std::string> Roots;
		std::set<std::string> Seen;
		auto Append=[&](const std::string& Name) { if (Seen.insert(Name).second) Roots.push_back(Name); };
		if (BaseModules) for (const auto& Name:*BaseModules) Append(Name);
		else
		{
			for (const auto& Name:ModuleOrder) if (ModuleDirs.contains(Name)) Append(Name);
			for (const auto& [Name,Path]:ModuleDirs) Append(Name);
		}
		if (const auto It=ExtraModules.find(std::string(Variant)); It!=ExtraModules.end()) for (const auto& Name:It->second) Append(Name);
		for (const auto& Name:Roots) if (!ModuleDirs.contains(Name)) return std::unexpected("Undefined project root module: "+Name);
		return Roots;
	}
	auto FProjectDescriptor::Save(std::string_view File,std::string& OutFailReason) const -> bool
	{
		FJsonDocument Output; auto Root=Output.GetMutableRoot(); Root.EnsureObject();
		Root.SetChildValue("ProjectName",Name);
		if (!Schema.empty()) Root.SetChildValue("$schema",Schema);
		auto Directories=Root.AddObject("ModuleDirs"); std::set<std::string> Written;
		for (const auto& Key:ModuleOrder) if (auto It=ModuleDirs.find(Key); It!=ModuleDirs.end()) { Directories.SetChildValue(Key,It->second); Written.insert(Key); }
		for (const auto& [Key,Path]:ModuleDirs) if (!Written.contains(Key)) Directories.SetChildValue(Key,Path);
		if (BaseModules) { auto List=Root.AddArray("BaseModules"); for (const auto& Item:*BaseModules) List.AppendValue(Item); }
		auto Variants=Root.AddObject("ExtraModules");
		for (const auto& [Key,Names]:ExtraModules) { auto List=Variants.AddObject(Key).AddArray("Modules"); for (const auto& Item:Names) List.AppendValue(Item); }
		if (NativeTestRoot) Root.AddObject("Tests").AddObject("Native").SetChildValue("Root",*NativeTestRoot);
		auto Plugins=Root.AddArray("PluginDependencies"); for (const auto& Name:PluginDependencies) Plugins.AppendValue(Name);
		auto MountList=Root.AddArray("Mounts");
		const auto OwnerRoot=std::filesystem::path(Normalize(File)).parent_path();
		for (const auto& Mount:Mounts)
		{
			auto Item=MountList.AppendObject(); Item.SetChildValue("VirtualRoot",Mount.VirtualRoot);
			Item.SetChildValue("Owner",Mount.Owner==EMountOwner::Extension ? "Extension" : "ExternalSources");
			if (Mount.Owner!=EMountOwner::Extension && Mount.Owner!=EMountOwner::ExternalSources) return Fail(OutFailReason,"Invalid authored mount owner");
			Item.SetChildValue("Root",Mount.Root.lexically_relative(OwnerRoot).generic_string());
			Item.SetChildValue("ContentPath",Mount.ContentPath.generic_string()); Item.SetChildValue("AutoScan",Mount.bAutoScan);
			Item.SetChildValue("ContentWritable",Mount.bContentWritable); auto Dependencies=Item.AddArray("Dependencies");
			for (const auto& Dependency:Mount.Dependencies) Dependencies.AppendValue(Dependency);
		}
		FProjectDescriptor Validated; if (!Validated.Read(Output,File,OutFailReason)) return false;
		const auto Text=Output.ToString();
		if (auto Saved=FFileHelper::SaveArrayToFileAtomically(FByteView(reinterpret_cast<const std::byte*>(Text.data()),Text.size()),std::string(File)); !Saved)
			return Fail(OutFailReason,Saved.error().ToString());
		OutFailReason.clear(); return true;
	}

	auto PluginStartupConfigFile(const std::filesystem::path& ProjectFile) -> std::filesystem::path
	{ return ProjectFile.parent_path() / "Configs/Plugins.json"; }
	auto ReadPluginStartupSnapshot(const std::filesystem::path& ProjectFile) -> std::expected<std::string, std::string>
	{
		const auto File = PluginStartupConfigFile(ProjectFile);
		std::error_code Error;
		const bool Exists = std::filesystem::exists(File, Error);
		if (Error) return std::unexpected("Cannot inspect plugin startup configuration: " + File.generic_string());
		if (!Exists) return std::string{};
		const auto Bytes = FFileHelper::LoadFileToArray(File);
		if (!Bytes) return std::unexpected("Cannot read plugin startup configuration: " + File.generic_string());
		return std::string(reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
	}
	auto LoadPluginStartupOverrides(const std::filesystem::path& ProjectFile) -> std::expected<std::map<std::string, bool>, std::string>
	{
		const auto Snapshot = ReadPluginStartupSnapshot(ProjectFile);
		if (!Snapshot) return std::unexpected(Snapshot.error());
		if (Snapshot->empty() && !std::filesystem::exists(PluginStartupConfigFile(ProjectFile))) return std::map<std::string, bool>{};
		FJsonDocument Document;
		if (!Document.Parse(*Snapshot) || !ValidateObjectFields(Document.GetRootView(), {"Plugins"}))
			return std::unexpected("Invalid plugin startup configuration: " + PluginStartupConfigFile(ProjectFile).generic_string());
		return ReadProjectPluginOverrides(Document.GetRootView());
	}

	auto ReadProjectPluginOverrides(FJsonNodeView Root)
		-> std::expected<std::map<std::string, bool>, std::string>
	{
		std::map<std::string, bool> Result;
		const auto Plugins = Root.GetView("Plugins");
		if (!Plugins.IsValid()) return Result;
		if (!Plugins.IsObject()) return std::unexpected("Plugins must be an object of plugin IDs and boolean overrides");
		std::string Error;
		std::set<std::string> Seen;
		Plugins.ForEachObjectMember([&](std::string_view Key, FJsonNodeView Value) {
			std::string Folded(Key);
			for (char& C : Folded) if (C >= 'A' && C <= 'Z') C += 'a' - 'A';
			if (!IsIdentifier(Key) || !Value.IsBool() || !Seen.insert(Folded).second)
				Error = "Plugins contains an invalid or duplicate override: " + std::string(Key);
			else Result.emplace(Key, Value.GetBool());
		});
		if (!Error.empty()) return std::unexpected(Error);
		return Result;
	}
	auto GetCurrentProject() -> const FProjectInfo* { return IProjectManager::Get().GetActiveProject(); }
	auto HasCurrentProject() -> bool { return GetCurrentProject()!=nullptr; }


	auto CommitProjectContext(std::string_view File, const FProjectDescriptor& Descriptor) -> std::expected<FProjectInfo, std::string>
	{
		const auto Normalized = Normalize(File);
		auto Roots = Descriptor.GetEnabledRootModules(DURIN_RUNTIME_VARIANT);
		if (!Roots) return std::unexpected(Roots.error());
		std::string Error;
		FProjectInfo Info;
		Info.Name=Descriptor.Name; Info.ProjectFile=Normalized;
		Info.ProjectDir=std::filesystem::path(Normalized).parent_path().generic_string()+"/";
		Info.ContentDir=Info.ProjectDir+"Content/"; Info.MountRoot="/Game/";
		Info.EnabledRootModules=std::move(*Roots); Info.PluginDependencies=Descriptor.PluginDependencies;
		Info.bRecordInteractiveHistory=false;
		if (!FPaths::SetProjectFile(Normalized, &Error))
			return std::unexpected(Error);
		if (GetCurrentProject() && GetCurrentProject()->ProjectFile != Normalized) ReleaseProjectEditOwnership();
		FMountPaths::SetProjectMountDefinitions(Descriptor.Mounts);
		return Info;
	}
	auto ClearProjectContext() -> void
	{
		ReleaseProjectEditOwnership();
		FPaths::SetProjectFile({}); FMountPaths::SetProjectMountDefinitions({});
	}

	auto RelaunchEditorForProject(std::string_view ProjectFile) -> void
	{
		std::vector<std::string> Arguments;
		if (ProjectFile.empty()) Arguments.emplace_back("--project-browser");
		else
		{
			const auto Path = std::filesystem::path(Normalize(ProjectFile)).generic_u8string();
			Arguments.push_back("--project=" + std::string(reinterpret_cast<const char*>(Path.data()), Path.size()));
		}
		GPendingEditorRelaunchArguments = std::move(Arguments);
		RequestEngineExit();
	}

	auto TakePendingEditorRelaunchArguments() -> std::optional<std::vector<std::string>>
	{ return std::exchange(GPendingEditorRelaunchArguments, std::nullopt); }

	auto AcquireProjectEditOwnership() -> std::expected<void, FProjectError>
	{
		const auto* Project=GetCurrentProject();
		if (!Project) return {};
#ifdef _WIN32
		if (GProjectEditMutex) return {};
		uint64 Hash = 14695981039346656037ull;
		for (unsigned char Character : Project->ProjectFile)
		{
			Hash ^= static_cast<unsigned char>(
				StringUtils::ToLowerAscii(static_cast<char>(Character)));
			Hash *= 1099511628211ull;
		}
		const std::wstring Name = std::format(
			L"Local\\DurinProjectAuthoring_{:016x}", Hash);
		HANDLE Mutex = CreateMutexW(nullptr, TRUE, Name.c_str());
		if (!Mutex)
		{
			return std::unexpected(FProjectError{EProjectError::EditOwnership, std::format(
				"Could not create project edit ownership (Windows error {}).",
				GetLastError())});
		}
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(Mutex);
			return std::unexpected(FProjectError{EProjectError::EditOwnership, std::format(
				"Another Editor process already owns project '{}'.",
				Project->Name)});
		}
		GProjectEditMutex = Mutex;
#elif defined(__APPLE__)
		if (GProjectEditFile >= 0
			&& GProjectEditOwnerProcess == getpid()) return {};
		if (GProjectEditFile >= 0)
		{
			close(GProjectEditFile);
			GProjectEditFile = -1;
		}
		std::error_code CanonicalError;
		const std::string Identity = std::filesystem::weakly_canonical(
			Project->ProjectFile, CanonicalError).generic_string();
		const std::string_view IdentityPath = CanonicalError
			? std::string_view(Project->ProjectFile) : std::string_view(Identity);
		uint64 Hash = 14695981039346656037ull;
		for (const unsigned char Character : IdentityPath)
		{
			Hash ^= Character;
			Hash *= 1099511628211ull;
		}
		const std::filesystem::path LockPath = std::filesystem::temp_directory_path()
			/ std::format("DurinProjectAuthoring-{:016x}.lock", Hash);
		const int File = open(LockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (File < 0)
		{
			return std::unexpected(FProjectError{EProjectError::EditOwnership, std::format(
				"Could not create project edit lock '{}': macOS error {} ({}).",
				LockPath.generic_string(), errno, std::generic_category().message(errno))});
		}
		if (flock(File, LOCK_EX | LOCK_NB) != 0)
		{
			const int Error = errno;
			close(File);
			return std::unexpected(FProjectError{EProjectError::EditOwnership, Error == EWOULDBLOCK
				? std::format("Another Editor process already owns project '{}'.",
					Project->Name)
				: std::format("Could not acquire project edit lock: macOS error {} ({}).",
					Error, std::generic_category().message(Error))});
		}
		const std::string Owner = std::format("pid={}\nproject={}\n",
			getpid(), Project->ProjectFile);
		(void)ftruncate(File, 0);
		(void)write(File, Owner.data(), Owner.size());
		GProjectEditFile = File;
		GProjectEditOwnerProcess = getpid();
#endif
		return {};
	}

	auto ReleaseProjectEditOwnership() -> void
	{
#ifdef _WIN32
		if (!GProjectEditMutex) return;
		ReleaseMutex(GProjectEditMutex);
		CloseHandle(GProjectEditMutex);
		GProjectEditMutex = nullptr;
#elif defined(__APPLE__)
		if (GProjectEditFile < 0) return;
		(void)flock(GProjectEditFile, LOCK_UN);
		close(GProjectEditFile);
		GProjectEditFile = -1;
		GProjectEditOwnerProcess = 0;
#endif
	}
}
