#include "Data/ContentBrowserNativeTransfer.h"
#include "Operations/ContentBrowserTransferPaths.h"
#include "AssetTools/IAssetTools.h"
#include "AssetTools/Relocation.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "Misc/FeedbackContext.h"

namespace Durin::ContentBrowser::Private
{
	using namespace TransferPaths;
	auto PrepareAssetTransfer(std::vector<FNativeTransferInput> Inputs, std::string Destination, bool Move, std::optional<std::string> NewName)
		-> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		struct FEntry
		{
			FPackagePath Source, Target;
			std::filesystem::path SourceFile, TargetFile;
			std::vector<std::pair<FContentBrowserItemContributionKey, FContentBrowserItemContributionKey>> Identities;
			FTopLevelAssetPath SourceAsset, TargetAsset;
		};
		std::vector<FEntry> Entries;
		std::vector<FContentBrowserItemContributionKey> Keys;
		std::vector<FContentBrowserTransferDestination> Claims;
		std::vector<std::pair<std::filesystem::path, std::pair<FContentBrowserItemContributionKey, FContentBrowserItemContributionKey>>> Folders;
		std::string Error;
		std::error_code Ec;
		if (!SafePath(Destination, true, &Error) || !std::filesystem::is_directory(Destination, Ec) || Ec)
			return Rejected(std::move(Inputs), Error.empty() ? "The destination is not an admitted directory." : Error);
		const auto Catalog = CaptureAssetCatalogSnapshot();
		for (const auto& Input : Inputs)
		{
			Keys.push_back(Input.Identity);
			const auto Source = std::filesystem::path(Input.Item.PhysicalPath).lexically_normal();
			if (std::ranges::any_of(Inputs, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder && !Same(Source, Other.Item.PhysicalPath)
					&& FPaths::IsLexicalDescendantPath(Source, Other.Item.PhysicalPath, true);
			})) continue;
			const auto Mount = FContentBrowserPaths::ResolveMountPath(Source.generic_string());
			if (!Mount || !SafePath(Source, Move, &Error) || Same(Source, Mount.Mount->GetContentDir()))
				return Rejected(std::move(Inputs), Error.empty() ? "The asset source is outside an admitted directory." : Error);
			const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
			if (!Folder && Input.Item.Kind != EContentBrowserItemKind::Asset) return Rejected(std::move(Inputs), "Asset transfer requires real assets or folders.");
			const auto TargetRoot = NewName ? Source.parent_path() / *NewName : std::filesystem::path(Destination) / Source.filename();
			if (NewName && Folder && (std::filesystem::exists(TargetRoot, Ec) || Ec)) return Rejected(std::move(Inputs), "The rename destination is already occupied.");
			for (const auto& Other : Inputs)
				if (Same(TargetRoot, Other.Item.PhysicalPath) || FPaths::IsLexicalDescendantPath(TargetRoot, Other.Item.PhysicalPath, true))
					return Rejected(std::move(Inputs), "A destination cannot be inside a selected source.");
			if (Folder)
			{
				if (!std::filesystem::is_directory(Source, Ec) || Ec) return Rejected(std::move(Inputs), "The selected folder is unavailable.");
				const auto NewVirtual = FContentBrowserPaths::ResolveMountPath(TargetRoot.generic_string()).VirtualPath;
				Folders.push_back({TargetRoot, {Input.Identity, KeyAt(Input.Identity, NewVirtual, true)}});
				Claims.push_back({Claim(TargetRoot), true});
			}
			bool Found = Folder;
			for (const auto& [Path, Package] : Catalog.Assets)
			{
				const auto Physical = std::filesystem::path(Package.PhysicalPath);
				if (Folder ? !FPaths::IsLexicalDescendantPath(Physical, Source, true) : Path != Input.Item.PackagePath) continue;
				if (!Folder && !Same(Physical, Source)) return Rejected(std::move(Inputs), "The asset no longer matches its captured file.");
				Found = true;
				if (std::ranges::any_of(Entries, [&](const auto& Entry) { return Entry.Source == Path; })) continue;
				if (Package.TopLevelAssets.empty() || std::ranges::any_of(Package.TopLevelAssets, &FAssetData::IsRedirector))
					return Rejected(std::move(Inputs), "Fix Up redirectors before transferring their folder.");
				if (!Move && Package.TopLevelAssets.size() != 1) return Rejected(std::move(Inputs), "Package copy requires one real top-level asset.");
				const auto Target = Folder ? TargetRoot / Physical.lexically_relative(Source) : NewName ? Physical.parent_path() / (*NewName + ".dasset") : std::filesystem::path(Destination) / Physical.filename();
				if (NewName && !Folder && (std::filesystem::exists(Target, Ec) || Ec)) return Rejected(std::move(Inputs), "The rename destination is already occupied.");
				if (!SafePath(Physical, Move, &Error) || !SafePath(Target, true, &Error)) return Rejected(std::move(Inputs), Error);
				auto Virtual = FContentBrowserPaths::ResolveMountPath(Target.generic_string()).VirtualPath;
				if (!Virtual.ends_with(".dasset")) return Rejected(std::move(Inputs), "The target package extension is invalid.");
				Virtual.resize(Virtual.size() - 7);
				FPackagePath TargetPath;
				if (!FPackagePath::TryCreate(Virtual, TargetPath)) return Rejected(std::move(Inputs), "The target package path is invalid.");
				FEntry Entry{Path, TargetPath, Physical, Target};
				for (const auto& Asset : Package.TopLevelAssets)
				{
					FTopLevelAssetPath NewAsset;
					if (!FTopLevelAssetPath::TryCreate(TargetPath, Asset.AssetPath.GetAssetName(), NewAsset)) return Rejected(std::move(Inputs), "The target asset path is invalid.");
					Entry.Identities.push_back({KeyAt(Input.Identity, Asset.AssetPath.ToString()), KeyAt(Input.Identity, NewAsset.ToString())});
					Entry.SourceAsset = Asset.AssetPath; Entry.TargetAsset = NewAsset;
				}
				Entries.push_back(std::move(Entry));
				Claims.push_back({Claim(Target), false});
				for (auto Parent = Target.parent_path(); !Same(Parent, Destination) && Parent != Parent.parent_path(); Parent = Parent.parent_path())
					Claims.push_back({Claim(Parent), true});
				auto Bulk = Target; Bulk.replace_extension(".dbulk");
				Claims.push_back({Claim(Bulk), false});
			}
			if (!Found) return Rejected(std::move(Inputs), "The selected asset is no longer registered.");
		}
		std::ranges::sort(Entries, {}, [](const auto& Entry) { return Entry.Source.ToString(); });
		auto Validate = [Inputs, Entries, Folders](std::string* Error) {
			if (!Admitted(Inputs, Error)) return false;
			for (const auto& Entry : Entries)
			{
				if (!FindPackageExact(Entry.Source) || !SafePath(Entry.SourceFile, false, Error) || !SafePath(Entry.TargetFile, true, Error)) return false;
				std::error_code Ec;
				if (std::filesystem::is_directory(Entry.TargetFile, Ec)) { if (Error) *Error = "A directory occupies a package destination."; return false; }
				Ec.clear();
				if (std::filesystem::exists(Entry.TargetFile, Ec) && !FindPackageExact(Entry.Target))
				{ if (Error) *Error = "An unregistered file occupies a package destination."; return false; }
			}
			for (const auto& [Folder, Identity] : Folders)
			{
				std::error_code Ec;
				if (std::filesystem::exists(Folder, Ec) && !std::filesystem::is_directory(Folder, Ec)) { if (Error) *Error = "A file occupies a folder destination."; return false; }
			}
			return Admitted(Inputs, Error);
		};
		return std::make_unique<FContentBrowserItemDataTransferContext>(std::move(Keys), std::move(Claims), Validate,
			[Inputs = std::move(Inputs), Entries = std::move(Entries), Folders = std::move(Folders), Move](const auto& Cancelled) {
				FContentBrowserOperationResult Result;
				FContentChangeBatch Changes;
				for (const auto& Entry : Entries)
				{
					std::string Error;
					if (Cancelled()) Result.bCancelled = true;
					if (!Admitted(Inputs, &Error)) Result.bStopped = true;
					if (Cancelled()) Result.bCancelled = true;
					auto Add = [&](EContentBrowserEffectStatus Status, const std::string& Message = std::string{}) {
						for (const auto& Identity : Entry.Identities)
							Result.Effects.push_back({Identity.first, Status == EContentBrowserEffectStatus::Committed ? std::optional(Identity.second) : std::nullopt, Status, Message});
					};
					if (Result.bStopped || Result.bCancelled) { Add(EContentBrowserEffectStatus::Unattempted, "Asset transfer stopped before this package."); continue; }
					if (!SafePath(Entry.SourceFile, Move, &Error) || !SafePath(Entry.TargetFile, true, &Error)) { Add(EContentBrowserEffectStatus::Failed, Error); Result.bStopped = true; continue; }
					std::error_code Ec;
					if (std::filesystem::exists(Entry.TargetFile, Ec) || Ec || FindPackageExact(Entry.Target) || FindResidentPackage(Entry.Target))
					{ Add(EContentBrowserEffectStatus::Failed, "The destination package is already occupied."); continue; }
					std::vector<std::filesystem::path> MissingParents;
					for (auto Parent = Entry.TargetFile.parent_path(); !std::filesystem::exists(Parent, Ec) && !Ec && Parent != Parent.parent_path(); Parent = Parent.parent_path()) MissingParents.push_back(Parent);
					for (auto It = MissingParents.rbegin(); It != MissingParents.rend(); ++It)
					{
						if (!Admitted(Inputs, &Error) || Cancelled() || !SafePath(*It, true, &Error)) { Result.bStopped = true; break; }
						if (std::filesystem::create_directory(*It, Ec))
						{
							const auto OldParent = (Entry.SourceFile.parent_path() / It->lexically_relative(Entry.TargetFile.parent_path())).lexically_normal();
							const auto OldKey = KeyAt(Entry.Identities.front().first, FContentBrowserPaths::ResolveMountPath(OldParent.generic_string()).VirtualPath, true);
							const auto NewKey = KeyAt(Entry.Identities.front().first, FContentBrowserPaths::ResolveMountPath(It->generic_string()).VirtualPath, true);
							Result.Effects.push_back({OldKey, NewKey, EContentBrowserEffectStatus::Committed});
							Changes.Changes.push_back({EContentChangeKind::Added, {}, It->generic_string(), {}, {}, true});
						}
						if (Ec) break;
					}
					Result.bCancelled |= Cancelled();
					if (Ec || Result.bStopped || Result.bCancelled) { Add(Result.bCancelled ? EContentBrowserEffectStatus::Unattempted : EContentBrowserEffectStatus::Failed, "Could not create the package destination: " + Error + Ec.message()); continue; }
					if (!Admitted(Inputs, &Error) || Cancelled()) { Add(EContentBrowserEffectStatus::Unattempted, Error); Result.bStopped = !Cancelled(); Result.bCancelled |= Cancelled(); continue; }
					if (Move)
					{
						const FAssetRelocationMapping Mapping{Entry.Source, Entry.Target};
						const auto Native = RelocateAssets(std::span{&Mapping, 1});
						const bool Committed = Native.Result || Native.Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending;
						if (Committed)
						{
							Add(EContentBrowserEffectStatus::Committed);
							for (const auto& Identity : Entry.Identities) Changes.Changes.push_back({EContentChangeKind::Renamed, Entry.SourceFile.generic_string(), Entry.TargetFile.generic_string(), Identity.first.Item.GetVirtualPath(), Identity.second.Item.GetVirtualPath()});
						}
						if (!Native.Result)
						{
							const auto FirstFailure = Result.Effects.size();
							Add(EContentBrowserEffectStatus::Failed, Native.Result.Message);
							for (size_t I = FirstFailure; I < Result.Effects.size(); ++I)
							{
								for (const auto& Path : Native.BackupLocations) Result.Effects[I].RecoveryLocations.push_back(Path.generic_string());
								for (const auto& Path : Native.AffectedFiles) Result.Effects[I].RecoveryLocations.push_back(Path.generic_string());
							}
							Result.bStopped = !Committed && Native.Result.Effect != EAssetWriteEffect::None;
							Changes.bFullRefresh |= Native.Result.Effect != EAssetWriteEffect::None;
						}
					}
					else
					{
						FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
							if (Level >= ELogLevel::Error) Error += std::string(Message) + "\n";
							if (Inputs.front().Policy->ReportDiagnostic) Inputs.front().Policy->ReportDiagnostic(Level, Message);
						});
						auto* Copy = IAssetTools::Get().DuplicateAsset({.SourcePath = Entry.SourceAsset, .DestinationDirectory = Entry.Target.ToString().substr(0, Entry.Target.ToString().find_last_of('/')), .DestinationPath = Entry.TargetAsset}, &Feedback);
						if (!Copy) { Add(EContentBrowserEffectStatus::Failed, Error.empty() ? "Asset copy failed." : Error); continue; }
						Add(EContentBrowserEffectStatus::Committed); // The duplicated resident asset now exists even if saving is interrupted.
						bool Committed = false;
						const bool MaySave = Admitted(Inputs, &Error);
						Result.bCancelled |= Cancelled();
						const bool Saved = !Cancelled() && MaySave && IAssetTools::Get().SaveAssets({.AssetPaths = {Entry.Target}, .OnSaved = [&](const FPackagePath&) { Committed = true; }}, &Feedback);
						if (Committed) { Changes.Changes.push_back({EContentChangeKind::Added, {}, Entry.TargetFile.generic_string(), {}, Entry.TargetAsset.ToString()}); }
						if (!Saved)
						{
							Add(EContentBrowserEffectStatus::Failed, Error.empty() ? "The copied package needs save/recovery inspection." : Error);
							Result.Effects.back().RecoveryLocations = {Entry.TargetFile.generic_string()};
							Result.bStopped = !Committed;
							Changes.bFullRefresh = true;
						}
					}
				}
				for (const auto& [Folder, Identity] : Folders)
				{
					std::string Error;
					if (Result.bStopped || Result.bCancelled || Cancelled() || !Admitted(Inputs, &Error) || Cancelled())
					{ Result.Add(Identity.first, EContentBrowserEffectStatus::Unattempted, "Folder transfer did not complete."); continue; }
					std::error_code Ec;
					if (!SafePath(Folder, true, &Error)) { Result.Add(Identity.first, EContentBrowserEffectStatus::Failed, Error); continue; }
					const bool Created = std::filesystem::create_directories(Folder, Ec);
					if (Ec) { Result.Add(Identity.first, EContentBrowserEffectStatus::Failed, Ec.message()); continue; }
					if (Created) Result.Effects.push_back({Identity.first, Identity.second, EContentBrowserEffectStatus::Committed});
					else
					{
						const bool HasChildren = std::ranges::any_of(Result.Effects, [&](const auto& Effect) { return Effect.Status == EContentBrowserEffectStatus::Committed && Effect.NewIdentity && Effect.NewIdentity->Item.GetVirtualPath().starts_with(Identity.second.Item.GetVirtualPath() + "/"); });
						if (HasChildren) Result.Effects.push_back({Identity.first, Identity.second, EContentBrowserEffectStatus::Committed});
						else Result.bNoChanges = true;
					}
					if (Created) Changes.Changes.push_back({EContentChangeKind::Added, {}, Folder.generic_string(), {}, {}, true});
				}
				Publish(Inputs, std::move(Changes));
				return Result;
			});
	}
}
