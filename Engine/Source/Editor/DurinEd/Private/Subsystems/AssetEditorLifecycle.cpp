#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/ToolkitHost.h"
#include "DObject/Class.h"
#include "Misc/Paths.h"
#include "Misc/Time.h"
#include "Yaml/Yaml.h"
#include "Asset/MutationExtensions.h"
#include "Asset/RegistryOperations.h"

namespace Durin
{
	struct DAssetEditorSubsystem::FLifecycleState : IAssetMoveObserver
	{
		FLifecycleState() : MoveObserver(RegisterAssetMoveObserver(this)) {}
		~FLifecycleState() override { UnregisterAssetMoveObserver(MoveObserver); }
		auto OnAssetsRelocated(std::span<const FAssetRelocationMapping> Mappings) -> void override
		{
			for (auto& Target : Recent)
				for (const auto& Mapping : Mappings)
					if (Target.AssetPath.GetPackagePath() == Mapping.SourcePath)
					{
						FObjectPath Replacement;
						const auto Text = Target.AssetPath.ToString();
						if (FObjectPath::TryCreate(Mapping.DestinationPath.ToString() + Text.substr(Mapping.SourcePath.ToString().size()), Replacement)) Target.AssetPath = Replacement;
					}
		}
		FAssetMoveObserverHandle MoveObserver;
		uint64 NextId = 1;
		std::map<uint64, std::function<void(const FAssetEditorEvent&)>> Subscribers;
		std::vector<FAssetOpenTarget> Recent;
		FAssetEditorUsage Usage;
		std::unordered_map<FObjectKey, double, FObjectKeyHash> Started;
	};
	auto DAssetEditorSubsystem::Subscribe(std::function<void(const FAssetEditorEvent&)> Callback) -> FAssetEditorSubscription
	{
		if (!CanCall() || !Callback) return {};
		if (!Lifecycle) Lifecycle = std::make_shared<FLifecycleState>();
		const auto Id = Lifecycle->NextId++;
		Lifecycle->Subscribers.emplace(Id, std::move(Callback));
		return FAssetEditorSubscription([Weak = std::weak_ptr(Lifecycle), Id] {
			if (const auto State = Weak.lock()) State->Subscribers.erase(Id);
		});
	}
	auto DAssetEditorSubsystem::PublishEvent(FAssetEditorEvent Event) -> void
	{
		if (!Lifecycle) Lifecycle = std::make_shared<FLifecycleState>();
		const auto State = Lifecycle;
		if (Event.Type == EAssetEditorEvent::Opened)
		{
			++State->Usage.Opened;
			if (Event.Session.ResolveObjectPtr()) State->Started.try_emplace(Event.Session, FTime::Seconds());
		}
		else if (Event.Type == EAssetEditorEvent::Focused) ++State->Usage.Focused;
		else if (Event.Type == EAssetEditorEvent::Closed)
		{
			++State->Usage.Closed;
			if (const auto Found = State->Started.find(Event.Session); Found != State->Started.end())
			{ State->Usage.EditingSeconds += FTime::Seconds() - Found->second; State->Started.erase(Found); }
		}
		if ((Event.Type == EAssetEditorEvent::Opened || Event.Type == EAssetEditorEvent::Focused) && Event.Target.AssetPath.IsValid())
		{
			std::erase_if(State->Recent, [&](const auto& Target) { return Target.AssetPath == Event.Target.AssetPath; });
			State->Recent.insert(State->Recent.begin(), Event.Target);
			if (State->Recent.size() > 32) State->Recent.resize(32);
		}
		const auto Snapshot = State->Subscribers;
		for (const auto& [Id, Callback] : Snapshot)
			if (State->Subscribers.contains(Id)) DispatchEditorCallback([&] { Callback(Event); });
	}
	auto DAssetEditorSubsystem::GetUsage() const -> FAssetEditorUsage
	{
		auto Usage = Lifecycle ? Lifecycle->Usage : FAssetEditorUsage{};
		if (Lifecycle) for (const auto& [Key, Started] : Lifecycle->Started) Usage.EditingSeconds += FTime::Seconds() - Started;
		return Usage;
	}
	auto DAssetEditorSubsystem::GetRecentAssets() const -> std::vector<FAssetOpenTarget> { return Lifecycle ? Lifecycle->Recent : std::vector<FAssetOpenTarget>{}; }
	auto DAssetEditorSubsystem::SynchronizeSessionAssets(DAssetEditor& Editor) -> void
	{
		const auto Objects = Editor.GetObjectsToEdit();
		const auto Found = OpenedEditors.find(&Editor);
		const auto Previous = Found == OpenedEditors.end() ? std::vector<FObjectKey>{} : Found->second;
		for (const auto& Key : Previous)
			if (std::ranges::none_of(Objects, [&](auto* Object) { return FObjectKey(Object) == Key; }))
				if (auto* Object = Key.ResolveObjectPtr()) NotifyAssetClosed(Object, &Editor);
		for (auto* Object : Objects)
		{
			if (!OwnsAssetEditor(Editor) || Editor.IsClosed()) break;
			NotifyAssetOpened(Object, &Editor);
		}
	}
	auto DAssetEditorSubsystem::CaptureSessions() const -> std::vector<FAssetEditorRestoreEntry>
	{
		auto Sessions = GetOwnedAssetEditors();
		std::vector<FAssetEditorRestoreEntry> Result;
		for (const auto& Session : Sessions)
		{
			if (!Session->SupportsRestore()) continue;
			FAssetEditorRestoreEntry Entry{.Method = Session->GetOpenMethod(), .Mode = Session->GetToolkitMode(),
				.Role = std::string(Session->GetInstanceKey()), .bPrimary = Session->IsPrimaryEditor()};
			for (auto* Object : Session->GetObjectsToEdit())
			{
				const auto ClassName = Object->GetClass()->GetQualifiedName().ToString();
				FObjectPath Path;
				if (FObjectPath::TryCreate(Object->GetObjectPath(), Path)) Entry.Assets.push_back({Path, ClassName});
			}
			std::ranges::sort(Entry.Assets, {}, [](const auto& Target) { return Target.AssetPath.ToString(); });
			if (!Entry.Assets.empty()) Result.push_back(std::move(Entry));
		}
		return Result;
	}
	auto DAssetEditorSubsystem::RestoreSessions(std::span<const FAssetEditorRestoreEntry> Entries) -> FAssetOpenResult
	{
		FAssetOpenResult Result;
		for (const auto& Entry : Entries)
		{
			auto Opened = OpenAssets({.AssetEditors = this, .Assets = Entry.Assets, .OpenMethod = Entry.Method,
				.ToolkitMode = Entry.Mode, .Origin = "Restore", .SessionRole = Entry.Role, .bPrimary = Entry.bPrimary,
				.bCreateSingleSession = Entry.Assets.size() > 1, .bIsRestore = true});
			Result.Items.insert(Result.Items.end(), std::make_move_iterator(Opened.Items.begin()), std::make_move_iterator(Opened.Items.end()));
		}
		const auto Sessions = GetOwnedAssetEditors();
		if (!Sessions.empty() && Sessions.back()->SupportsRestore()) RestoreFocus = Sessions.back().Get();
		CompleteRestoreFocus();
		return Result;
	}
	auto DAssetEditorSubsystem::CompleteRestoreFocus() -> void
	{
		auto* Session = RestoreFocus.Get();
		if (!Session || !OwnsAssetEditor(*Session) || Session->IsClosed()) { RestoreFocus.Reset(); return; }
		for (const auto& Open : GetOwnedAssetEditors())
			if (const auto Host = Open->GetToolkitHost(); Host && !Host->IsReadyForFocus()) return;
		RestoreFocus.Reset();
		Session->FocusWindow();
	}
	namespace
	{
		auto StatePath(std::string Path) -> std::string
		{ return Path.empty() ? FPaths::LaunchConfigsDir() + "AssetEditorSessions/" + FXxHash128::HashBuffer(std::string_view(FPaths::ProjectFile())).ToString() + ".yaml" : std::move(Path); }
	}
	auto DAssetEditorSubsystem::SaveSessionState(std::string FilePath) const -> bool
	{
		FYamlDocument Document;
		auto Root = Document.GetMutableRoot();
		Root.SetChildValue("Version", 1);
		Root.SetChildValue("Project", FPaths::ProjectFile());
		auto Sessions = Root.AddSequence("Sessions");
		for (const auto& Entry : CaptureSessions())
		{
			auto Node = Sessions.AppendMap();
			Node.SetChildValue("Method", Entry.Method == EAssetOpenMethod::View ? "View" : "Edit");
			Node.SetChildValue("Mode", Entry.Mode == EToolkitMode::WorldCentric ? "WorldCentric" : "Standalone");
			Node.SetChildValue("Role", Entry.Role); Node.SetChildValue("Primary", Entry.bPrimary);
			auto Assets = Node.AddSequence("Assets");
			for (const auto& Target : Entry.Assets)
			{
				auto Asset = Assets.AppendMap();
				Asset.SetChildValue("Path", Target.AssetPath.ToString()); Asset.SetChildValue("Class", Target.AssetClassName);
			}
		}
		auto Recent = Root.AddSequence("Recent");
		for (const auto& Target : GetRecentAssets())
		{ auto Node = Recent.AppendMap(); Node.SetChildValue("Path", Target.AssetPath.ToString()); Node.SetChildValue("Class", Target.AssetClassName); }
		FilePath = StatePath(std::move(FilePath));
		std::error_code Error;
		std::filesystem::create_directories(std::filesystem::path(FilePath).parent_path(), Error);
		return !Error && static_cast<bool>(Document.SaveToFile(FilePath));
	}
	auto DAssetEditorSubsystem::LoadSessionState(std::string FilePath) -> FAssetOpenResult
	{
		FYamlDocument Document;
		FilePath = StatePath(std::move(FilePath));
		if (!std::filesystem::exists(FilePath)) return {};
		if (!Document.LoadFromFile(FilePath) || Document.GetRootView().GetView("Version").GetInt() != 1
			|| Document.GetRootView().GetView("Project").GetString() != FPaths::ProjectFile())
			return {{{.Status = EAssetOpenStatus::Rejected, .Message = "The session snapshot is invalid or belongs to another project."}}};
		const auto Root = Document.GetRootView();
		const auto Nodes = Root.GetView("Sessions");
		std::vector<FAssetEditorRestoreEntry> Entries;
		for (size_t I = 0; I < Nodes.Num(); ++I)
		{
			const auto Node = Nodes.GetView(I);
			const auto Method = Node.GetView("Method").GetString();
			const auto Mode = Node.GetView("Mode").GetString();
			FAssetEditorRestoreEntry Entry{.Method = Method == "View" ? EAssetOpenMethod::View : Method == "Edit" ? EAssetOpenMethod::Edit : static_cast<EAssetOpenMethod>(255),
				.Mode = Mode == "WorldCentric" ? EToolkitMode::WorldCentric : Mode == "Standalone" ? EToolkitMode::Standalone : static_cast<EToolkitMode>(255),
				.Role = Node.GetView("Role").GetString(), .bPrimary = Node.GetView("Primary").GetBool(true)};
			const auto Assets = Node.GetView("Assets");
			for (size_t J = 0; J < Assets.Num(); ++J)
			{
				FObjectPath Path; FObjectPath::TryCreate(Assets.GetView(J).GetView("Path").GetString(), Path);
				Entry.Assets.push_back({Path, Assets.GetView(J).GetView("Class").GetString()});
			}
			Entries.push_back(std::move(Entry));
		}
		if (!Lifecycle) Lifecycle = std::make_shared<FLifecycleState>();
		Lifecycle->Recent.clear();
		const auto Recent = Root.GetView("Recent");
		for (size_t I = 0; I < std::min<size_t>(32, Recent.Num()); ++I)
		{
			FObjectPath Path;
			if (FObjectPath::TryCreate(Recent.GetView(I).GetView("Path").GetString(), Path))
			{
				if (const auto Resolved = ResolveAssetObjectPathForOperation(Path)) Path = Resolved.FinalPath;
				if (std::ranges::none_of(Lifecycle->Recent, [&](const auto& Target) { return Target.AssetPath == Path; }))
					Lifecycle->Recent.push_back({Path, Recent.GetView(I).GetView("Class").GetString()});
			}
		}
		return RestoreSessions(Entries);
	}
}
