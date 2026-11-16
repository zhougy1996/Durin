#include "Data/ContentBrowserNativeTransfer.h"
#include "ContentBrowserTransferHelpers.h"
#include "Misc/StringHelper.h"
#include "AssetTools/IAssetTools.h"

#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/FeedbackContext.h"

namespace Durin::ContentBrowser::AssetDataSource::Private
{
	using namespace ContentBrowser::Transfer;
	auto ExecuteAssetTransfer(std::vector<FNativeTransferInput> Inputs, std::string Destination, bool Move, std::optional<std::string> NewName)
		-> bool
	{
		struct FEntry
		{
			FPackagePath Source, Target;
			std::filesystem::path SourceFile, TargetFile;
			std::vector<std::pair<FTopLevelAssetPath, FTopLevelAssetPath>> Assets;
		};
		std::vector<FEntry> Entries;
		std::vector<std::filesystem::path> Folders;
		std::string Error;
		std::error_code Ec;
		if (!SafePath(Destination, true, &Error) || !std::filesystem::is_directory(Destination, Ec) || Ec)
			return Rejected(Inputs, Error.empty() ? "The destination is not an admitted directory." : Error);
		const auto Query = CaptureAssetQuerySnapshot();
		for (const auto& Input : Inputs)
		{
			const auto Source = std::filesystem::path(Input.Item.PhysicalPath).lexically_normal();
			if (std::ranges::any_of(Inputs, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder && !Same(Source, Other.Item.PhysicalPath)
					&& FPaths::IsLexicalDescendantPath(Source, Other.Item.PhysicalPath, true);
			})) continue;
			const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Source.generic_string());
			if (!Mount || !SafePath(Source, Move, &Error) || Same(Source, Mount.Mount->GetContentDir())) return Rejected(Inputs, Error.empty() ? "The source is outside an admitted directory." : Error);
			const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
			FTopLevelAssetPath Selected;
			if (!Folder && (Input.Item.Kind != EContentBrowserItemKind::Asset || !FTopLevelAssetPath::TryCreate(Input.Item.VirtualPath, Selected))) return Rejected(Inputs, "Asset transfer requires real assets or folders.");
			const auto TargetRoot = NewName ? Source.parent_path() / *NewName : std::filesystem::path(Destination) / Source.filename();
			for (const auto& Other : Inputs)
				if (!(Move && NewName && !Folder && Same(Source, Other.Item.PhysicalPath))
					&& (Same(TargetRoot, Other.Item.PhysicalPath) || FPaths::IsLexicalDescendantPath(TargetRoot, Other.Item.PhysicalPath, true))) return Rejected(Inputs, "A destination cannot be inside a selected source.");
			if (Folder)
			{
				if (!std::filesystem::is_directory(Source, Ec) || Ec) return Rejected(Inputs, "The selected folder is unavailable.");
				Folders.push_back(TargetRoot);
			}
			bool Found = Folder;
			for (const auto& [AssetPath, Asset] : Query.Assets)
			{
				const auto Path = Asset.AssetPath.GetPackagePath();
				const auto Physical = std::filesystem::path(ContentStorage::FContentMountPaths::VirtualToPhysical(Path.ToString() + ".dasset"));
				if (Folder ? !FPaths::IsLexicalDescendantPath(Physical, Source, true) : Asset.AssetPath != Selected) continue;
				if (!Folder && !Same(Physical, Source)) return Rejected(Inputs, "The source no longer matches its captured identity.");
				Found = true;
				if (std::ranges::any_of(Entries, [&](const auto& Entry) { return Entry.Assets.front().first == Asset.AssetPath; })) continue;
				if (Asset.IsRedirector()) return Rejected(Inputs, "Fix Up redirectors before transferring their folder.");
				const std::string Name = NewName && !Folder ? *NewName : std::string(Asset.AssetPath.GetAssetName());
				const auto TargetDirectory = Folder ? (TargetRoot / Physical.lexically_relative(Source)).parent_path() : std::filesystem::path(Destination);
				const auto Target = TargetDirectory / (Name + ".dasset");
				if (!SafePath(Physical, Move, &Error) || !SafePath(Target, true, &Error)) return Rejected(Inputs, Error);
				auto Virtual = ContentStorage::FContentMountPaths::PhysicalToVirtualDirectory(TargetDirectory.generic_string());
				if (Virtual.empty()) return Rejected(Inputs, "The target directory is not mounted.");
				if (!Virtual.ends_with('/')) Virtual += '/';
				Virtual += Name;
				FPackagePath TargetPath;
				FTopLevelAssetPath TargetAsset;
				if (!FPackagePath::TryCreate(Virtual, TargetPath) || !FTopLevelAssetPath::TryCreate(TargetPath, Name, TargetAsset)) return Rejected(Inputs, "The target object identity is invalid.");
				Entries.push_back({Path, TargetPath, Physical, Target, {{Asset.AssetPath, TargetAsset}}});
			}
			if (!Found) return Rejected(Inputs, "The selected asset is no longer available.");
		}
		std::ranges::sort(Entries, {}, [](const auto& Entry) { return Entry.Source.ToString(); });
		auto Validate = [&](std::string* Error) {
			if (!Admitted(Inputs, Error)) return false;
			for (const auto& Entry : Entries)
			{
				if (!FindAssetExact(Entry.Assets.front().first) || !SafePath(Entry.SourceFile, false, Error) || !SafePath(Entry.TargetFile, true, Error)) return false;
				std::error_code Ec;
				if (std::filesystem::is_directory(Entry.TargetFile, Ec)) { if (Error) *Error = "A directory occupies a package destination."; return false; }
				Ec.clear();
				if (std::filesystem::exists(Entry.TargetFile, Ec) && !FindPackageExact(Entry.Target))
				{ if (Error) *Error = "An unregistered file occupies a package destination."; return false; }
			}
			for (const auto& Folder : Folders)
			{
				std::error_code Ec;
				if (std::filesystem::exists(Folder, Ec) && !std::filesystem::is_directory(Folder, Ec)) { if (Error) *Error = "A file occupies a folder destination."; return false; }
			}
			return Admitted(Inputs, Error);
		};
		if (!Validate(&Error)) return Rejected(Inputs, Error);
		bool Success = false;
		bool Stopped = false;
		FContentChangeBatch Changes;
		for (const auto& Entry : Entries)
		{
			std::string Error;
			if (!Admitted(Inputs, &Error) || !SafePath(Entry.SourceFile, Move, &Error) || !SafePath(Entry.TargetFile, true, &Error))
			{ ReportDiagnostic(Inputs, ELogLevel::Error, Error); Stopped = true; break; }
			std::error_code Ec;
			if (!(Move && NewName && Entry.Source == Entry.Target) && (std::filesystem::exists(Entry.TargetFile, Ec) || Ec || FindPackageExact(Entry.Target) || FindResidentPackage(Entry.Target)))
			{ ReportDiagnostic(Inputs, ELogLevel::Error, "The destination package is already occupied."); continue; }
			std::vector<std::filesystem::path> MissingParents;
			for (auto Parent = Entry.TargetFile.parent_path(); !std::filesystem::exists(Parent, Ec) && !Ec && Parent != Parent.parent_path(); Parent = Parent.parent_path()) MissingParents.push_back(Parent);
			for (auto It = MissingParents.rbegin(); It != MissingParents.rend(); ++It)
			{
				if (!Admitted(Inputs, &Error) || !SafePath(*It, true, &Error)) { Stopped = true; break; }
				if (std::filesystem::create_directory(*It, Ec))
				{
					Changes.Changes.push_back({EContentChangeKind::Added, {}, It->generic_string(), {}, {}, true});
				}
				if (Ec) break;
			}
			if (Ec || Stopped)
			{ ReportDiagnostic(Inputs, ELogLevel::Error, "Could not create the package destination: " + Error + Ec.message()); if (Stopped) break; continue; }
			if (!Admitted(Inputs, &Error)) { ReportDiagnostic(Inputs, ELogLevel::Error, Error); Stopped = true; break; }
			if (Move)
			{
				std::vector<FAssetRenameData> Renames;
				std::vector<FStrongObjectPtr> Owners;
				for (const auto& [Source, Target] : Entry.Assets)
				{
					auto* Object = LoadObject(Source, nullptr);
					if (!Object) continue;
					Owners.emplace_back(Object);
					Renames.emplace_back(FSoftObjectPath(Source), FSoftObjectPath(Target));
				}
				if (!Admitted(Inputs, &Error)) { ReportDiagnostic(Inputs, ELogLevel::Error, Error); break; }
				auto& FallbackFeedback = FFeedbackContext::Get();
				FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
					if (!Inputs.empty() && Inputs.front().Policy->ReportDiagnostic) Inputs.front().Policy->ReportDiagnostic(Level, Message);
					else FallbackFeedback.Serialize(Level, Message);
				});
				FScopedFeedbackContext Scope(Feedback);
				const auto Handle = OnAssetRenamed().AddLambda([&](const FAssetData& Asset, const FSoftObjectPath& Old) {
					if (Old.GetAssetPath() != Entry.Assets.front().first) return;
					Success = true;
					Changes.Changes.push_back({EContentChangeKind::Renamed, Entry.SourceFile.generic_string(), Entry.TargetFile.generic_string(), Old.ToString(), Asset.AssetPath.ToString()});
				});
				if (!IAssetTools::Get().RenameAssets(Renames) && Success) Changes.bFullRefresh = true;
				OnAssetRenamed().Remove(Handle);
			}
			else
			{
				bool ReportedFailure = false;
				auto& FallbackFeedback = FFeedbackContext::Get();
				FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
					ReportedFailure |= Level >= ELogLevel::Error;
					if (!Inputs.empty() && Inputs.front().Policy->ReportDiagnostic) Inputs.front().Policy->ReportDiagnostic(Level, Message);
					else FallbackFeedback.Serialize(Level, Message);
				});
				const auto& [SourceAsset, TargetAsset] = Entry.Assets.front();
				FScopedFeedbackContext FeedbackScope(Feedback);
				std::optional<UnifiedError::FError> LoadError;
				auto* Source = LoadObject(SourceAsset, nullptr, nullptr, &LoadError);
				if (!Source) { Feedback.Error(LoadError ? LoadError->ToString() : "The copy source is unavailable."); continue; }
				const TStrongObjectPtr<DObject> SourceOwner(Source);
				if (!Admitted(Inputs, &Error)) { Feedback.Error(Error); Stopped = true; break; }
				const auto TargetPackage = Entry.Target.ToString();
				auto* Copy = IAssetTools::Get().DuplicateAsset(Entry.Target.GetPackageName(),
					TargetPackage.substr(0, TargetPackage.find_last_of('/')), Source);
				if (!Copy) { if (!ReportedFailure) ReportDiagnostic(Inputs, ELogLevel::Error, "Asset copy failed."); continue; }
				Success = true;
				Changes.Changes.push_back({EContentChangeKind::Added, {}, Entry.TargetFile.generic_string(), {}, Copy->GetObjectPath()});
			}
			if (Stopped) break;
		}
		for (const auto& Folder : Folders)
		{
			std::string Error;
			if (Stopped) break;
			if (!Admitted(Inputs, &Error)) { ReportDiagnostic(Inputs, ELogLevel::Error, Error); break; }
			std::error_code Ec;
			if (!SafePath(Folder, true, &Error)) { ReportDiagnostic(Inputs, ELogLevel::Error, Error); continue; }
			const bool Created = std::filesystem::create_directories(Folder, Ec);
			if (Ec) { ReportDiagnostic(Inputs, ELogLevel::Error, Ec.message()); continue; }
			Success = true;
			if (Created) Changes.Changes.push_back({EContentChangeKind::Added, {}, Folder.generic_string(), {}, {}, true});
		}
		Publish(Inputs, std::move(Changes));
		return Success;
	}
}
