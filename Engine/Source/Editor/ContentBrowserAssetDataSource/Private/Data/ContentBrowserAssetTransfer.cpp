#include "Data/ContentBrowserNativeTransfer.h"
#include "ContentBrowserTransferHelpers.h"
#include "Misc/StringHelper.h"
#include "AssetTools/IAssetTools.h"
#include "AssetTools/Relocation.h"
#include "AssetRegistry/Catalog.h"
#include "Asset/Load.h"
#include "DObject/Package.h"
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
		const auto Catalog = CaptureAssetCatalogSnapshot();
		for (const auto& Input : Inputs)
		{
			const auto Source = std::filesystem::path(Input.Item.PhysicalPath).lexically_normal();
			if (std::ranges::any_of(Inputs, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder && !Same(Source, Other.Item.PhysicalPath)
					&& FPaths::IsLexicalDescendantPath(Source, Other.Item.PhysicalPath, true);
			})) continue;
			const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Source.generic_string());
			if (!Mount || !SafePath(Source, Move, &Error) || Same(Source, Mount.Mount->GetContentDir()))
				return Rejected(Inputs, Error.empty() ? "The asset source is outside an admitted directory." : Error);
			const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
			if (!Folder && Input.Item.Kind != EContentBrowserItemKind::Asset) return Rejected(Inputs, "Asset transfer requires real assets or folders.");
			const auto TargetRoot = NewName ? Source.parent_path() / *NewName : std::filesystem::path(Destination) / Source.filename();

			for (const auto& Other : Inputs)
				if (Same(TargetRoot, Other.Item.PhysicalPath) || FPaths::IsLexicalDescendantPath(TargetRoot, Other.Item.PhysicalPath, true))
					return Rejected(Inputs, "A destination cannot be inside a selected source.");
			if (Folder)
			{
				if (!std::filesystem::is_directory(Source, Ec) || Ec) return Rejected(Inputs, "The selected folder is unavailable.");
				Folders.push_back(TargetRoot);
			}
			bool Found = Folder;
			for (const auto& [Path, Package] : Catalog.Assets)
			{
				const auto Physical = std::filesystem::path(Package.PhysicalPath);
				if (Folder ? !FPaths::IsLexicalDescendantPath(Physical, Source, true) : Path != Input.Item.PackagePath) continue;
				if (!Folder && !Same(Physical, Source)) return Rejected(Inputs, "The asset no longer matches its captured file.");
				Found = true;
				if (std::ranges::any_of(Entries, [&](const auto& Entry) { return Entry.Source == Path; })) continue;
				if (Package.TopLevelAssets.empty() || std::ranges::any_of(Package.TopLevelAssets, &FAssetData::IsRedirector))
					return Rejected(Inputs, "Fix Up redirectors before transferring their folder.");
				if (!Move && Package.TopLevelAssets.size() != 1) return Rejected(Inputs, "Package copy requires one real top-level asset.");
				const auto Target = Folder ? TargetRoot / Physical.lexically_relative(Source) : NewName ? Physical.parent_path() / (*NewName + ".dasset") : std::filesystem::path(Destination) / Physical.filename();
				if (NewName && !Folder && (std::filesystem::exists(Target, Ec) || Ec)) return Rejected(Inputs, "The rename destination is already occupied.");
				if (!SafePath(Physical, Move, &Error) || !SafePath(Target, true, &Error)) return Rejected(Inputs, Error);
				auto Virtual = ContentStorage::FContentMountPaths::ResolveMountPath(Target.generic_string()).VirtualPath;
				if (!Virtual.ends_with(".dasset")) return Rejected(Inputs, "The target package extension is invalid.");
				Virtual.resize(Virtual.size() - 7);
				FPackagePath TargetPath;
				if (!FPackagePath::TryCreate(Virtual, TargetPath)) return Rejected(Inputs, "The target package path is invalid.");
				FEntry Entry{Path, TargetPath, Physical, Target};
				for (const auto& Asset : Package.TopLevelAssets)
				{
					FTopLevelAssetPath NewAsset;
					if (!FTopLevelAssetPath::TryCreate(TargetPath, Asset.AssetPath.GetAssetName(), NewAsset)) return Rejected(Inputs, "The target asset path is invalid.");
					Entry.Assets.emplace_back(Asset.AssetPath, NewAsset);
				}
				Entries.push_back(std::move(Entry));
			}
			if (!Found) return Rejected(Inputs, "The selected asset is no longer registered.");
		}
		std::ranges::sort(Entries, {}, [](const auto& Entry) { return Entry.Source.ToString(); });
		auto Validate = [&](std::string* Error) {
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
			if (std::filesystem::exists(Entry.TargetFile, Ec) || Ec || FindPackageExact(Entry.Target) || FindResidentPackage(Entry.Target))
			{ ReportDiagnostic(Inputs, ELogLevel::Error, "The destination package is already occupied."); continue; }
			std::vector<std::filesystem::path> MissingParents;
			for (auto Parent = Entry.TargetFile.parent_path(); !std::filesystem::exists(Parent, Ec) && !Ec && Parent != Parent.parent_path(); Parent = Parent.parent_path()) MissingParents.push_back(Parent);
			for (auto It = MissingParents.rbegin(); It != MissingParents.rend(); ++It)
			{
				if (!Admitted(Inputs, &Error) || !SafePath(*It, true, &Error)) { Stopped = true; break; }
				if (std::filesystem::create_directory(*It, Ec))
				{
					Success = true;
					Changes.Changes.push_back({EContentChangeKind::Added, {}, It->generic_string(), {}, {}, true});
				}
				if (Ec) break;
			}
			if (Ec || Stopped)
			{ ReportDiagnostic(Inputs, ELogLevel::Error, "Could not create the package destination: " + Error + Ec.message()); if (Stopped) break; continue; }
			if (!Admitted(Inputs, &Error)) { ReportDiagnostic(Inputs, ELogLevel::Error, Error); Stopped = true; break; }
			if (Move)
			{
				const FAssetRelocationMapping Mapping{Entry.Source, Entry.Target};
				const auto Native = RelocateAssets(std::span{&Mapping, 1});
				const bool Committed = Native.Result || Native.Result.Effect == EAssetWriteEffect::ContentCommittedProjectionPending;
				if (Committed)
				{
					Success = true;
					for (const auto& [Source, Target] : Entry.Assets)
						Changes.Changes.push_back({EContentChangeKind::Renamed, Entry.SourceFile.generic_string(), Entry.TargetFile.generic_string(), Source.ToString(), Target.ToString()});
				}
				if (!Native.Result)
				{
					ReportDiagnostic(Inputs, ELogLevel::Error, Native.Result.Error ? Native.Result.Error->ToString() : "Asset relocation did not complete.");
					for (const auto& Path : Native.BackupLocations) ReportDiagnostic(Inputs, ELogLevel::Warn, "Recovery location: " + Path.generic_string());
					for (const auto& Path : Native.AffectedFiles) ReportDiagnostic(Inputs, ELogLevel::Warn, "Recovery location: " + Path.generic_string());
					Stopped = !Committed && Native.Result.Effect != EAssetWriteEffect::None;
					Changes.bFullRefresh |= Native.Result.Effect != EAssetWriteEffect::None;
				}
			}
			else
			{
				bool ReportedFailure = false;
				FFeedbackContext Feedback({}, [&](ELogLevel Level, std::string_view Message) {
					ReportedFailure |= Level >= ELogLevel::Error;
					ReportDiagnostic(Inputs, Level, Message);
				});
				const auto& [SourceAsset, TargetAsset] = Entry.Assets.front();
				auto* Copy = IAssetTools::Get().DuplicateAsset({.SourcePath = SourceAsset, .DestinationDirectory = Entry.Target.ToString().substr(0, Entry.Target.ToString().find_last_of('/')), .DestinationPath = TargetAsset}, &Feedback);
				if (!Copy) { if (!ReportedFailure) ReportDiagnostic(Inputs, ELogLevel::Error, "Asset copy failed."); continue; }
				Success = true; // The duplicated resident asset exists even if saving is interrupted.
				bool Committed = false;
				const bool MaySave = Admitted(Inputs, &Error);
				ReportedFailure = false;
				const bool Saved = MaySave && IAssetTools::Get().SaveAssets({.AssetPaths = {Entry.Target}, .OnSaved = [&](const FPackagePath&) { Committed = true; }}, &Feedback);
				if (Committed) Changes.Changes.push_back({EContentChangeKind::Added, {}, Entry.TargetFile.generic_string(), {}, TargetAsset.ToString()});
				if (!Saved)
				{
					if (!ReportedFailure) ReportDiagnostic(Inputs, ELogLevel::Error, "The copied package needs save/recovery inspection: " + Entry.TargetFile.generic_string());
					else ReportDiagnostic(Inputs, ELogLevel::Warn, "Inspect retained copy: " + Entry.TargetFile.generic_string());
					Stopped = !Committed;
					Changes.bFullRefresh = true;
				}
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
