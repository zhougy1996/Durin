#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/ToolkitManager.h"
#include "Toolkits/AssetEditor.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Misc/Paths.h"
#include "Yaml/Yaml.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/RegistryOperations.h"

namespace Durin
{
	namespace
	{
		auto ResolveEditorAsset(const FSoftObjectPath& Path) -> std::optional<FAssetOpenTarget>
		{
			const auto Resolved = ResolveAssetObjectPathForOperation(Path);
			if (!Resolved || !Resolved.FinalAssetData) return {};
			return FAssetOpenTarget{Resolved.FinalPath, Resolved.FinalAssetData->AssetClassName};
		}
	}

	struct DAssetEditorSubsystem::FLifecycleState
	{
		FLifecycleState()
		{
			RenameHandle = OnAssetRenamed().AddLambda([this](const FAssetData& Asset, const FSoftObjectPath& Old) {
				for (auto& Target : Recent)
					if (Target.AssetPath.GetAssetPath() == Old.GetAssetPath())
					{
						FSoftObjectPath Replacement;
						if (FSoftObjectPath::TryCreate(Asset.AssetPath, Target.AssetPath.GetSubobjectNames(), Replacement)) Target.AssetPath = Replacement;
					}
			});
		}
		~FLifecycleState() { OnAssetRenamed().Remove(RenameHandle); }
		FDelegateHandle RenameHandle;
		uint64 NextId = 1;
		std::map<uint64, std::function<void(const FAssetEditorEvent&)>> Subscribers;
		std::vector<FAssetOpenTarget> Recent;
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
		if (Event.InstanceId.empty()) if (const auto* Editor = Cast<DAssetEditor>(Event.Editor.ResolveObjectPtr())) Event.InstanceId = Editor->GetInstanceId();
		const auto State = Lifecycle;
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
	auto DAssetEditorSubsystem::GetRecentAssets() const -> std::vector<FAssetOpenTarget> { return Lifecycle ? Lifecycle->Recent : std::vector<FAssetOpenTarget>{}; }
	auto DAssetEditorSubsystem::CaptureEditorState() const -> std::vector<FAssetEditorRestoreEntry>
	{
		auto Toolkits = FToolkitManager::Get().GetToolkits(this);
		std::vector<FAssetEditorRestoreEntry> Result;
		for (const auto& Toolkit : Toolkits)
		{
			if (!Toolkit->SupportsRestore()) continue;
			for (auto* Object : Toolkit->GetObjectsToEdit())
			{
				FSoftObjectPath Path;
				if (FSoftObjectPath::TryCreate(Object->GetObjectPath(), Path) && Path.IsTopLevelAsset()
					&& FindAssetExact(Path.GetAssetPath(), EAssetQuerySource::OnDisk))
				{
					std::erase_if(Result, [&](const auto& Entry) {
						return Entry.AssetPath == Path && Entry.Method == Toolkit->GetOpenMethod();
					});
					Result.push_back({.AssetPath = Path, .Method = Toolkit->GetOpenMethod()});
				}
			}
		}
		return Result;
	}
	auto DAssetEditorSubsystem::RestoreEditorState(std::span<const FAssetEditorRestoreEntry> Entries) -> bool
	{
		bool Restored = true;
		for (const auto& Entry : Entries)
		{
			const auto Asset = ResolveEditorAsset(Entry.AssetPath);
			if (!Asset) { Restored = false; continue; }
			auto Opened = OpenAssets({.AssetEditors = this, .Assets = {*Asset}, .OpenMethod = Entry.Method,
				.OpenLocation = EAssetEditorOpenLocation::MainWindow, .bIsRestore = true});
			Restored = Opened && Restored;
		}
		return Restored;
	}
	namespace
	{
		auto StatePath(std::string Path) -> std::string
		{ return Path.empty() ? FPaths::LaunchConfigsDir() + "AssetEditorState/" + FXxHash128::HashBuffer(std::string_view(FPaths::ProjectFile())).ToString() + ".yaml" : std::move(Path); }
	}
	auto DAssetEditorSubsystem::SaveEditorState(std::string FilePath) const -> bool
	{
		FYamlDocument Document;
		auto Root = Document.GetMutableRoot();
		Root.SetChildValue("Version", 2);
		Root.SetChildValue("Project", FPaths::ProjectFile());
		auto EditorEntries = Root.AddSequence("OpenAssets");
		for (const auto& Entry : CaptureEditorState())
		{
			auto Node = EditorEntries.AppendMap();
			Node.SetChildValue("Method", Entry.Method == EAssetOpenMethod::View ? "View" : "Edit");
			Node.SetChildValue("Path", Entry.AssetPath.ToString());
		}
		auto Recent = Root.AddSequence("Recent");
		for (const auto& Target : GetRecentAssets())
		{ auto Node = Recent.AppendMap(); Node.SetChildValue("Path", Target.AssetPath.ToString()); }
		FilePath = StatePath(std::move(FilePath));
		std::error_code Error;
		std::filesystem::create_directories(std::filesystem::path(FilePath).parent_path(), Error);
		return !Error && static_cast<bool>(Document.SaveToFile(FilePath));
	}
	auto DAssetEditorSubsystem::LoadEditorState(std::string FilePath) -> bool
	{
		FYamlDocument Document;
		FilePath = StatePath(std::move(FilePath));
		if (!std::filesystem::exists(FilePath)) return true;
		if (!Document.LoadFromFile(FilePath)) return false;
		const auto Root = Document.GetRootView();
		if (Root.GetView("Project").GetString() != FPaths::ProjectFile()) return false;
		if (Root.GetView("Version").GetInt() != 2) return true;
		const auto Nodes = Root.GetView("OpenAssets");
		std::vector<FAssetEditorRestoreEntry> Entries;
		for (size_t I = 0; I < Nodes.Num(); ++I)
		{
			const auto Node = Nodes.GetView(I);
			const auto Method = Node.GetView("Method").GetString();
			const auto OpenMethod = Method == "View" ? EAssetOpenMethod::View : Method == "Edit" ? EAssetOpenMethod::Edit : static_cast<EAssetOpenMethod>(255);
			FSoftObjectPath Path;
			FSoftObjectPath::TryCreate(Node.GetView("Path").GetString(), Path);
			Entries.push_back({.AssetPath = Path, .Method = OpenMethod});
		}
		if (!Lifecycle) Lifecycle = std::make_shared<FLifecycleState>();
		Lifecycle->Recent.clear();
		const auto Recent = Root.GetView("Recent");
		for (size_t I = 0; I < std::min<size_t>(32, Recent.Num()); ++I)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Recent.GetView(I).GetView("Path").GetString(), Path)) continue;
			if (const auto Asset = ResolveEditorAsset(Path))
			{
				if (std::ranges::none_of(Lifecycle->Recent, [&](const auto& Target) { return Target.AssetPath == Asset->AssetPath; }))
					Lifecycle->Recent.push_back(*Asset);
			}
		}
		return RestoreEditorState(Entries);
	}
}
