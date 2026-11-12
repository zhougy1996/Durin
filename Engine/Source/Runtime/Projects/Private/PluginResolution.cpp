#include "Plugins/PluginResolution.h"

#include "Json/Json.h"
#include "Project/Project.h"
#include "ProjectDescriptor.h"

namespace Durin
{
	namespace
	{
		using FNode = FJsonNodeView;
		const std::array<std::string, 4> DependencyFields = {
			"PublicDependencies", "PrivateDependencies", "OptionalPublicDependencies", "OptionalPrivateDependencies"};
		const std::array<std::string, 3> Phases = {"Providers", "EngineReady", "EditorReady"};

		auto Fold(std::string Value) -> std::string
		{
			for (char& C : Value) if (C >= 'A' && C <= 'Z') C += 'a' - 'A';
			return Value;
		}
		auto Identifier(std::string_view Value) -> bool
		{
			const auto Letter = [](char C) { return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || C == '_'; };
			return !Value.empty() && Letter(Value.front()) && std::ranges::all_of(Value,
				[&](char C) { return Letter(C) || (C >= '0' && C <= '9'); });
		}
		[[noreturn]] auto Fail(const std::filesystem::path& Path, const std::string& Message,
			const std::vector<std::string>& Chain = {}) -> void
		{
			std::string Error = Path.generic_string() + ": " + Message;
			if (!Chain.empty())
			{
				Error += " [chain ";
				for (size_t I = 0; I < Chain.size(); ++I) Error += (I ? " -> " : "") + Chain[I];
				Error += "]";
			}
			throw std::runtime_error(Error);
		}
		auto Hash(std::string_view Text) -> std::string
		{
			uint64 Value = 14695981039346656037ull;
			for (const unsigned char Byte : Text) Value = (Value ^ Byte) * 1099511628211ull;
			return std::format("{:016x}", Value);
		}
		auto Quote(std::string_view Text) -> std::string
		{
			std::string Result = "\"";
			for (const unsigned char C : Text)
			{
				switch (C)
				{
				case '"': Result += "\\\""; break;
				case '\\': Result += "\\\\"; break;
				case '\b': Result += "\\b"; break;
				case '\f': Result += "\\f"; break;
				case '\n': Result += "\\n"; break;
				case '\r': Result += "\\r"; break;
				case '\t': Result += "\\t"; break;
				default:
					if (C < 32) Result += std::format("\\u{:04x}", C);
					else Result += static_cast<char>(C);
				}
			}
			return Result + "\"";
		}
		auto Canonical(FNode Node) -> std::string
		{
			if (Node.IsString()) return Quote(Node.GetString());
			if (Node.IsBool()) return Node.GetBool() ? "true" : "false";
			if (Node.IsInt()) return std::to_string(Node.GetInt());
			if (Node.IsUInt()) return std::to_string(Node.GetUInt());
			std::string Result;
			if (Node.IsArray())
			{
				Result = "[";
				for (size_t I = 0; I < Node.Num(); ++I) Result += (I ? "," : "") + Canonical(Node.GetView(I));
				return Result + "]";
			}
			if (Node.IsObject())
			{
				std::map<std::string, FNode> Fields;
				Node.ForEachObjectMember([&](std::string_view Key, FNode Value) { Fields.emplace(Key, Value); });
				Result = "{";
				for (const auto& [Key, Value] : Fields)
				{
					if (Result.size() > 1) Result += ",";
					Result += Quote(Key) + ":" + Canonical(Value);
				}
				return Result + "}";
			}
			return "null";
		}
		auto Fields(FNode Node, std::initializer_list<std::string_view> Allowed, const std::filesystem::path& Path) -> void
		{
			if (!Node.IsObject()) Fail(Path, "descriptor must be an object");
			Node.ForEachObjectMember([&](std::string_view Key, FNode) {
				if (std::find(Allowed.begin(), Allowed.end(), Key) == Allowed.end()) Fail(Path, "unknown schema version or field: " + std::string(Key));
			});
		}
		auto RejectDuplicateFields(FNode Node, const std::filesystem::path& Path) -> void
		{
			if (Node.IsArray()) for (size_t I = 0; I < Node.Num(); ++I) RejectDuplicateFields(Node.GetView(I), Path);
			if (!Node.IsObject()) return;
			std::set<std::string> Seen;
			Node.ForEachObjectMember([&](std::string_view Key, FNode Value) {
				if (!Seen.insert(std::string(Key)).second) Fail(Path, "duplicate JSON field " + std::string(Key));
				RejectDuplicateFields(Value, Path);
			});
		}
		auto Text(FNode Node, std::string_view Key, const std::filesystem::path& Path,
			std::string Default = {}) -> std::string
		{
			const auto Value = Node.GetView(Key);
			if (!Value.IsValid() && !Default.empty()) return Default;
			if (!Value.IsString() || Value.GetString().empty()) Fail(Path, "invalid string: " + std::string(Key));
			return Value.GetString();
		}
		auto Ids(FNode Node, std::string_view Key, const std::filesystem::path& Path) -> std::vector<std::string>
		{
			const auto Value = Node.GetView(Key);
			if (!Value.IsValid()) return {};
			if (!Value.IsArray()) Fail(Path, "invalid identifier list: " + std::string(Key));
			std::set<std::string> Seen;
			std::vector<std::string> Result;
			for (size_t I = 0; I < Value.Num(); ++I)
			{
				const auto Item = Value.GetView(I);
				const auto Name = Item.GetString();
				if (!Item.IsString() || !Identifier(Name) || !Seen.insert(Fold(Name)).second)
					Fail(Path, "invalid identifier list: " + std::string(Key));
				Result.push_back(Name);
			}
			std::ranges::sort(Result, {}, [](const auto& V) { return Fold(V); });
			return Result;
		}
		auto Contained(const std::filesystem::path& Root, const std::string& Relative) -> std::filesystem::path
		{
			const auto Path = std::filesystem::path(Relative);
			if (Relative.empty() || Relative.find('\\') != std::string::npos || Relative.find(':') != std::string::npos
				|| Path.is_absolute() || std::ranges::any_of(Path, [](const auto& Part) { return Part == ".."; }))
				Fail(Root, "path escape or invalid relative path: " + Relative);
			const auto Result = std::filesystem::weakly_canonical(Root / Path);
			const auto Relation = Result.lexically_relative(Root);
			if (Relation.empty() || *Relation.begin() == "..") Fail(Root, "path escape: " + Relative);
			return Result;
		}
		struct FModule
		{
			std::string Name, Owner, Host, Phase;
			std::vector<std::string> Platforms;
			std::filesystem::path Path;
			FNode Data;
			std::string Logical;
		};
		struct FPlugin
		{
			std::string Name;
			std::filesystem::path Path;
			std::vector<std::string> Dependencies, Platforms;
		};
		struct FProject
		{
			std::filesystem::path Path;
			FNode Data;
			std::map<std::string, bool> Overrides;
			FProjectDescriptor Descriptor;
		};
		class FResolver
		{
		public:
			const FPluginResolveRequest& Request;
			FPluginResolution Result;
			std::vector<std::unique_ptr<FJsonDocument>> Documents;
			std::map<std::string, std::string> Inputs;
			std::map<std::string, FPlugin> Plugins;
			std::map<std::string, FModule> Modules;
			std::vector<FProject> Projects;
			std::map<std::string, bool> Explicit;
			std::map<std::string, std::set<std::string>> Reasons;
			std::set<std::string> Selected, Visiting, EditorOnly, Active, Done, WalkSeen, EnabledModules;
			std::vector<std::string> Order, ModuleOrder;

			explicit FResolver(const FPluginResolveRequest& InRequest) : Request(InRequest) {}
			auto Read(const std::filesystem::path& Path, const std::string& Logical) -> FNode
			{
				std::ifstream Stream(Path, std::ios::binary);
				if (!Stream) Fail(Path, "descriptor file does not exist or cannot be read");
				const std::string Raw{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
				auto Document = std::make_unique<FJsonDocument>();
				if (const auto Parsed = Document->Parse(Raw); !Parsed) Fail(Path, Parsed.error().Message);
				const auto Node = Document->GetRootView();
				if (!Node.IsObject()) Fail(Path, "descriptor must be an object");
				RejectDuplicateFields(Node, Path);
				Inputs[Logical] = Hash(Raw);
				Result.Locations[Logical] = Path;
				Documents.push_back(std::move(Document));
				return Node;
			}
			auto RenameInput(const std::string& Key) -> void
			{
				Inputs[Key] = Inputs.at("Pending"); Inputs.erase("Pending");
				Result.Locations[Key] = Result.Locations.at("Pending"); Result.Locations.erase("Pending");
			}
			auto AddModule(const std::filesystem::path& Path, const std::string& Logical, const std::string& Name,
				const std::string& Owner = {}, const std::string& Host = "Runtime",
				const std::string& Phase = "Providers", std::vector<std::string> Platforms = {}) -> void
			{
				if (!Identifier(Name)) Fail(Path, "invalid module " + Name);
				const auto Key = Fold(Name);
				if (Modules.contains(Key)) Fail(Path, "duplicate module " + Name + " also in " + Modules.at(Key).Path.generic_string());
				FNode Data;
				if (Owner.empty())
				{
					Data=Read(Path,Logical);
					if (Text(Data,"ModuleName",Path)!=Name) Fail(Path,"ModuleName mismatch: "+Name);
					for (const auto& Field:DependencyFields) (void)Ids(Data,Field,Path);
				}
				Modules.emplace(Key,FModule{Name,Owner,Host,Phase,std::move(Platforms),Path,Data,Logical});
			}
			auto Discover(const std::filesystem::path& Directory, const std::filesystem::path& SearchRoot) -> void
			{
				if (!std::filesystem::exists(Directory)) return;
				if (std::filesystem::is_symlink(Directory) || std::filesystem::absolute(Directory).lexically_normal() != std::filesystem::weakly_canonical(Directory))
					Fail(Directory, "symbolic plugin directories are unsupported");
				const auto Relative = std::filesystem::weakly_canonical(Directory).lexically_relative(std::filesystem::weakly_canonical(SearchRoot));
				if (Relative.empty() || *Relative.begin() == "..") Fail(Directory, "path escape");
				std::vector<std::filesystem::path> Entries, Descriptors;
				for (const auto& Entry : std::filesystem::directory_iterator(Directory)) Entries.push_back(Entry.path());
				std::ranges::sort(Entries);
				for (const auto& Entry : Entries)
					if (Entry.extension() == ".dplugin" && std::filesystem::is_regular_file(Entry)) Descriptors.push_back(Entry);
				if (Descriptors.size() > 1) Fail(Directory, "multiple plugin descriptors");
				if (Descriptors.empty())
				{
					for (const auto& Entry : Entries) if (std::filesystem::is_directory(Entry)) Discover(Entry, SearchRoot);
					return;
				}
				const auto Path = Descriptors.front();
				if (std::filesystem::weakly_canonical(Path).parent_path() != std::filesystem::weakly_canonical(Directory)) Fail(Path, "path escape");
				const auto Data = Read(Path, "Pending");
				Fields(Data, {"$schema", "SchemaVersion", "Id", "DisplayName", "Version", "Dependencies", "Platforms", "Content", "Shaders", "Configs", "Modules"}, Path);
				const auto Version = Data.GetView("SchemaVersion");
				if ((!Version.IsInt() && !Version.IsUInt()) || Version.GetUInt() != 1) Fail(Path, "unknown schema version or field");
				const auto Name = Text(Data, "Id", Path);
				if (!Identifier(Name) || Plugins.contains(Fold(Name))) Fail(Path, "invalid or duplicate plugin " + Name);
				const auto Logical = "Plugin/" + Name;
				RenameInput(Logical);
				for (const auto& Field : {"$schema", "DisplayName", "Version"}) if (Data.Contains(Field)) (void)Text(Data, Field, Path);
				for (const auto& Field : {"Content", "Shaders", "Configs"})
					if (Data.Contains(Field)) (void)Contained(std::filesystem::weakly_canonical(Directory), Text(Data, Field, Path));
				Plugins.emplace(Fold(Name), FPlugin{Name, Path, Ids(Data, "Dependencies", Path), Ids(Data, "Platforms", Path)});
				const auto List = Data.GetView("Modules");
				if (List.IsValid() && !List.IsArray()) Fail(Path, "Modules must be an array");
				for (size_t I = 0; I < List.Num(); ++I)
				{
					const auto Module = List.GetView(I);
					Fields(Module, {"Name", "Path", "Host", "Phase", "Platforms"}, Path);
					const auto ModName = Text(Module, "Name", Path);
					const auto Host = Text(Module, "Host", Path, "Runtime"), Phase = Text(Module, "Phase", Path, "Providers");
					if ((Host != "Runtime" && Host != "Editor") || std::ranges::find(Phases, Phase) == Phases.end()
						|| (Host == "Runtime" && Phase == "EditorReady")) Fail(Path, "invalid host/phase");
					const auto ModPath = Contained(std::filesystem::weakly_canonical(Directory), Text(Module, "Path", Path)) / (ModName + ".dmodule");
					AddModule(ModPath, Logical + "/" + ModName, ModName, Name, Host, Phase, Ids(Module, "Platforms", Path));
				}

			}
			auto Visit(const std::string& Key, std::vector<std::string> Chain) -> void
			{
				if (!Plugins.contains(Key)) Fail(Request.ProjectFile, "missing plugin descriptor " + Key, Chain);
				const auto& Plugin = Plugins.at(Key);
				if (Explicit.contains(Key) && !Explicit.at(Key)) Fail(Plugin.Path, "explicitly disabled plugin", Chain);
				if (!Plugin.Platforms.empty() && std::ranges::find(Plugin.Platforms, Request.Platform) == Plugin.Platforms.end()) Fail(Plugin.Path, "ineligible plugin platform", Chain);
				if (Visiting.contains(Key)) Fail(Plugin.Path, "plugin dependency cycle", Chain);
				if (Selected.contains(Key)) return;
				Visiting.insert(Key);
				for (const auto& Dep : Plugin.Dependencies)
				{
					Reasons[Fold(Dep)].insert("RequiredBy:" + Plugin.Name);
					auto Next = Chain; Next.push_back(Dep); Visit(Fold(Dep), std::move(Next));
				}
				Visiting.erase(Key); Selected.insert(Key); Order.push_back(Key);
			}
			auto Edges(const FModule& Module, bool bIncludeOptional = true, bool bConditionalRuntime = false) -> std::vector<std::string>
			{
				std::vector<std::string> ResultEdges;
				for (const auto& Field : DependencyFields)
					for (const auto& Dep : Ids(Module.Data, Field, Module.Path))
						if (!Field.starts_with("Optional") || (bIncludeOptional && EnabledModules.contains(Fold(Dep)) && Eligible(Modules.at(Fold(Dep)))
							&& !(bConditionalRuntime && Modules.at(Fold(Dep)).Owner.empty() && EditorOnly.contains(Fold(Dep)))))
							ResultEdges.push_back(Fold(Dep));
				return ResultEdges;
			}
			auto ProjectRoots(const std::string& Variant) -> std::set<std::string>
			{
				std::set<std::string> Roots;
				for (const auto& Project : Projects)
				{
					if (Project.Path != std::filesystem::weakly_canonical(Request.ProjectFile) && Project.Path != std::filesystem::weakly_canonical(Request.EngineRoot / "Engine.dproject")) continue;
					const auto ProjectModules = Project.Descriptor.GetEnabledRootModules(Variant);
					if (!ProjectModules) Fail(Project.Path, ProjectModules.error());
					for (const auto& Root : *ProjectModules) Roots.insert(Fold(Root));
				}
				return Roots;
			}
			auto Reachable(const std::string& Variant) -> std::set<std::string>
			{
				std::set<std::string> Seen;
				std::function<void(const std::string&)> Walk = [&](const std::string& Key) {
					if (!Modules.contains(Key) || !Seen.insert(Key).second) return;
					for (const auto& Dep : Edges(Modules.at(Key), false)) Walk(Dep);
				};
				for (const auto& Root : ProjectRoots(Variant)) Walk(Root);
				return Seen;
			}
			auto Eligible(const FModule& Module) const -> bool
			{
				return (Module.Owner.empty() || Selected.contains(Fold(Module.Owner)))
					&& (Module.Platforms.empty() || std::ranges::find(Module.Platforms, Request.Platform) != Module.Platforms.end())
					&& (Request.RuntimeVariant == "DurinEditor" || Module.Host != "Editor");
			}
			auto PhaseIndex(const std::string& Phase) const -> size_t
			{
				return static_cast<size_t>(std::ranges::find(Phases, Phase) - Phases.begin());
			}
			auto WalkModule(const std::string& Key, std::vector<std::string> Chain,
				std::set<std::string> Stack, const FModule& Source) -> void
			{
				if (!Modules.contains(Key)) Fail(Source.Path, "missing module " + Key, Chain);
				const auto& Module = Modules.at(Key);
				if (!Eligible(Module)) Fail(Module.Path, "disabled or ineligible module", Chain);
				if (!Stack.insert(Key).second) Fail(Module.Path, "module dependency cycle", Chain);
				if (!WalkSeen.insert(Key).second) return;
				// Preserve existing Runtime modules' conditional editor implementation.
				// Plugin-owned optional edges still obey their declared host contract.
				auto Dependencies = Edges(Module, true, Source.Host == "Runtime" && Module.Owner.empty());
				std::ranges::sort(Dependencies);
				for (const auto& Dep : Dependencies)
				{
					auto Next = Chain;
					Next.push_back(Modules.contains(Dep) ? Modules.at(Dep).Name : Dep);
					if (Modules.contains(Dep))
					{
						const auto& Target = Modules.at(Dep);
						if (Source.Host == "Runtime" && (Target.Host == "Editor" || (Target.Owner.empty() && EditorOnly.contains(Dep))))
							Fail(Module.Path, "Runtime-to-Editor dependency", Next);
						if (!Target.Owner.empty() && !Module.Owner.empty() && Fold(Module.Owner) != Fold(Target.Owner))
						{
							bool bDeclared = false;
							for (const auto& PluginDep : Plugins.at(Fold(Module.Owner)).Dependencies)
								if (Fold(PluginDep) == Fold(Target.Owner)) bDeclared = true;
							if (!bDeclared) Fail(Module.Path, "undeclared plugin dependency", Next);
						}
						if (!Target.Owner.empty() && PhaseIndex(Source.Phase) < PhaseIndex(Target.Phase)) Fail(Module.Path, "invalid phase edge", Next);
					}
					WalkModule(Dep, std::move(Next), Stack, Source);
				}
				if (Active.contains(Key) && Done.insert(Key).second) ModuleOrder.push_back(Key);
			}
			auto Run() -> FPluginResolution
			{
				if ((Request.RuntimeVariant != "DurinEditor" && Request.RuntimeVariant != "DurinGame") || !Identifier(Request.Platform))
					throw std::runtime_error("invalid target");
				const auto EngineRoot = std::filesystem::weakly_canonical(Request.EngineRoot);
				const auto ProjectFile = std::filesystem::weakly_canonical(Request.ProjectFile);
				std::set<std::filesystem::path> Files;
				for (const auto& File : Request.ProjectFiles) Files.insert(std::filesystem::weakly_canonical(File));
				Files.insert(EngineRoot / "Engine.dproject"); Files.insert(ProjectFile);
				std::set<std::string> Names;
				for (const auto& File : Files)
				{
					const auto Data = Read(File, "Pending");
					FProjectDescriptor Descriptor;
					std::string DescriptorError;
					if (!Descriptor.Read(*Documents.back(), File.generic_string(), DescriptorError)) Fail(File, DescriptorError);
					const auto Name = Descriptor.Name;
					if (!Identifier(Name) || !Names.insert(Fold(Name)).second) Fail(File, "invalid or duplicate project " + Name);
					RenameInput("Project/" + Name);
					const auto Directories = Descriptor.ModuleDirs;
					const auto Overrides = Descriptor.PluginOverrides;
					Projects.push_back({File, Data, Overrides, std::move(Descriptor)});
					for (const auto& [ModName, Directory] : Directories)
						AddModule(Contained(File.parent_path(), Directory) / (ModName + ".dmodule"), "Project/" + Name + "/" + ModName, ModName);
				}
				std::set<std::filesystem::path> Roots = {EngineRoot / "Plugins", ProjectFile.parent_path() / "Plugins"};
				for (const auto& Root : Roots) Discover(Root, Root);
				for (const auto& Project : Projects) if (Project.Path == ProjectFile)
					for (const auto& [Name, Enabled] : Project.Overrides) Explicit.emplace(Fold(Name), Enabled);
				for (const auto& [Key, Enabled] : Explicit)
				{
					if (!Plugins.contains(Key)) Fail(ProjectFile, "missing plugin descriptor " + Key, {Key});
					if (Enabled) { Reasons[Key].insert("Explicit"); Visit(Key, {Plugins.at(Key).Name}); }
				}
				for (auto& [Key,Module]:Modules) if (!Module.Owner.empty() && Eligible(Module))
				{
					const auto OwnerRoot = Plugins.at(Fold(Module.Owner)).Path.parent_path();
					Module.Path = Contained(OwnerRoot,Module.Path.lexically_relative(OwnerRoot).generic_string());
					Module.Data=Read(Module.Path,Module.Logical);
					if (Text(Module.Data,"ModuleName",Module.Path)!=Module.Name) Fail(Module.Path,"ModuleName mismatch: "+Module.Name);
					for (const auto& Field:DependencyFields) (void)Ids(Module.Data,Field,Module.Path);
				}
				EditorOnly = Reachable("DurinEditor");
				for (const auto& Key : Reachable("DurinGame")) EditorOnly.erase(Key);
				for (const auto& [Key, Module] : Modules) if (!Module.Owner.empty() && Eligible(Module)) Active.insert(Key);
				std::function<void(const std::string&)> EnableModule = [&](const std::string& Key) {
					if (!Modules.contains(Key) || !EnabledModules.insert(Key).second) return;
					for (const auto& Dep : Edges(Modules.at(Key), false)) EnableModule(Dep);
				};
				for (const auto& Key : Active) EnableModule(Key);
				for (const auto& Key : Reachable(Request.RuntimeVariant)) EnableModule(Key);
				std::vector<std::string> Sorted(Active.begin(), Active.end());
				std::ranges::sort(Sorted, [&](const auto& A, const auto& B) {
					return std::pair(PhaseIndex(Modules.at(A).Phase), A) < std::pair(PhaseIndex(Modules.at(B).Phase), B);
				});
				for (const auto& Key : Sorted)
				{
					WalkSeen.clear(); WalkModule(Key, {Modules.at(Key).Name}, {}, Modules.at(Key));
				}
				if (std::ranges::any_of(Modules, [](const auto& Pair) { return !Pair.second.Owner.empty(); }))
					for (const auto& Key : ProjectRoots(Request.RuntimeVariant))
					{
						if (!Modules.contains(Key)) Fail(ProjectFile, "missing module " + Key, {Key});
						auto Source = Modules.at(Key);
						Source.Host = Request.RuntimeVariant == "DurinEditor" ? "Editor" : "Runtime";
						WalkSeen.clear(); WalkModule(Key, {Source.Name}, {}, Source);
					}
				FJsonDocument Manifest;
				auto Root = Manifest.GetMutableRoot(); Root.EnsureObject();
				Root.SetChildValue("SchemaVersion", 1);
				Root.SetChildValue("Platform", Request.Platform);
				Root.SetChildValue("RuntimeVariant", Request.RuntimeVariant);
				auto PluginList = Root.AddArray("Plugins");
				for (const auto& Key : Order)
				{
					Result.Plugins.push_back(Plugins.at(Key).Name);
					auto Item = PluginList.AppendObject(); Item.SetChildValue("Id", Plugins.at(Key).Name);
					auto List = Item.AddArray("Reasons"); for (const auto& Reason : Reasons[Key]) List.AppendValue(Reason);
				}
				auto ModuleList = Root.AddArray("Modules");
				for (const auto& Key : ModuleOrder)
				{
					const auto& Module = Modules.at(Key);
					Result.Modules.push_back({Module.Name,Module.Owner,Module.Host,Module.Phase});
					auto Item = ModuleList.AppendObject(); Item.SetChildValue("Name", Module.Name);
					Item.SetChildValue("Owner", Module.Owner); Item.SetChildValue("Host", Module.Host); Item.SetChildValue("Phase", Module.Phase);
				}
				auto InputList = Root.AddArray("Inputs");
				for (const auto& [Path, Value] : Inputs)
				{
					auto Item = InputList.AppendObject(); Item.SetChildValue("Path", Path); Item.SetChildValue("Hash", Value);
				}
				Root.SetChildValue("Fingerprint", Hash(Canonical(Manifest.GetRootView())));
				Result.ManifestJson = Canonical(Manifest.GetRootView());
				return std::move(Result);
			}
		};
	}

	auto ResolvePlugins(const FPluginResolveRequest& Request) -> std::expected<FPluginResolution, std::string>
	{
		try { return FResolver(Request).Run(); }
		catch (const std::exception& Error) { return std::unexpected(Error.what()); }
	}
}
