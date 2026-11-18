#include "Data/ContentBrowserNativeTransfer.h"
#include "ContentBrowserTransferHelpers.h"
#include "Misc/StringHelper.h"
#include "AssetTools/AssetDeleteExtensions.h"
#include "AssetRegistry/Catalog.h"
#include "Misc/FileHelper.h"

namespace Durin::ContentBrowser::FileDataSource::Private
{
	using namespace ContentBrowser::Transfer;
	auto ExecuteFileTransfer(std::vector<FNativeTransferInput> Inputs, std::string Destination, bool Move, std::optional<std::string> NewName)
		-> bool
	{
		struct FEntry
		{
			std::filesystem::path Source, Target;
			bool Directory = false;
		};
		std::vector<FEntry> Entries;
		const auto Catalog = CaptureAssetCatalogSnapshot();
		std::string Error;
		std::error_code Ec;
		if (!SafePath(Destination, true, &Error) || !std::filesystem::is_directory(Destination, Ec) || Ec)
			return Rejected(Inputs, Error.empty() ? "The transfer destination is not a directory." : Error);
		for (const auto& Input : Inputs)
		{
			const auto Source = std::filesystem::path(Input.Item.PhysicalPath).lexically_normal();
			if (std::ranges::any_of(Inputs, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder && !Same(Source, Other.Item.PhysicalPath)
					&& FPaths::IsLexicalDescendantPath(Source, Other.Item.PhysicalPath, true);
			})) continue;
			const auto Mount = ContentStorage::FContentMountPaths::ResolveMountPath(Source.generic_string());
			if (!Mount || Same(Source, Mount.Mount->GetContentDir()) || !SafePath(Source, Move, &Error)
				|| (Move && Mount.Mount != ContentStorage::FContentMountPaths::ResolveMountPath(Destination).Mount))
				return Rejected(Inputs, Error.empty() ? "The selected source cannot be transferred." : Error);
			const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
			if (!Folder && Input.Item.Kind != EContentBrowserItemKind::File) return Rejected(Inputs, "File transfer requires file-source items.");
			auto Target = std::filesystem::path(Destination) / Source.filename();
			if (NewName)
			{
				Target = Source.parent_path() / *NewName;
				if (!Folder && Target.extension().empty()) Target += Source.extension();
				if ((std::filesystem::exists(Target, Ec) && !(Folder && std::filesystem::is_directory(Target, Ec))) || Ec) return Rejected(Inputs, "The rename destination is already occupied.");
			}
			for (const auto& Other : Inputs)
				if (Same(Target, Other.Item.PhysicalPath) || FPaths::IsLexicalDescendantPath(Target, Other.Item.PhysicalPath, true))
					return Rejected(Inputs, "A destination cannot be inside a selected source.");
			auto Collect = [&](const std::filesystem::path& Path, bool Independent) -> bool {
				if (!SafePath(Path, Move, &Error)) return false;
				const auto EntryMount = ContentStorage::FContentMountPaths::ResolveMountPath(Path.generic_string());
				if (!EntryMount || EntryMount.Mount != Mount.Mount) { Error = "Transfers cannot cross nested mount boundaries."; return false; }
				const bool Directory = std::filesystem::is_directory(Path, Ec);
				if (Ec || (!Directory && !std::filesystem::is_regular_file(Path, Ec)) || Ec) { Error = "A transfer source is unavailable or changed type."; return false; }
				if (Independent && Directory != Folder) { Error = "The selected source changed type."; return false; }
				if (!Directory)
				{
					const auto Extension = StringUtils::FoldAscii(Path.extension().generic_string());
					if (Extension == ".dasset")
					{
						const bool Registered = std::ranges::any_of(Catalog.Assets, [&](const auto& Pair) { return Same(Path, Pair.second.PhysicalPath); });
						if (!Registered || Independent) { Error = "Packages cannot be transferred by the File source."; return false; }
						return true;
					}
					FAssetCompanionOwnership Ownership;
					std::optional<UnifiedError::FError> OwnershipResultError;
					const bool OwnershipResult = QueryAssetCompanionOwnership(Path, Ownership, &OwnershipResultError);
					if (!OwnershipResult) { Error = (OwnershipResultError ? OwnershipResultError->ToString() : std::string{}); return false; }
					if (Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
					{
						if (!Move && Extension != ".dbulk") { Error = "Folder copy cannot duplicate a custom managed companion."; return false; }
						if (Independent || Ownership.State == EAssetCompanionOwnershipState::Ambiguous) { Error = "Transfer the owning package instead of a managed companion: " + (Ownership.Owners.empty() ? std::string{} : Ownership.Owners.front().ToString()); return false; }
						return true;
					}
					if (Extension == ".dbulk") { Error = "An orphan bulk payload cannot be transferred as an ordinary file."; return false; }
				}
				const auto To = Independent ? Target : Target / Path.lexically_relative(Source);
				if (!SafePath(To, true, &Error)) return false;
				const auto TargetMount = ContentStorage::FContentMountPaths::ResolveMountPath(To.generic_string());
				if (Move && (!TargetMount || TargetMount.Mount != Mount.Mount)) { Error = "Moves cannot cross nested destination mount boundaries."; return false; }
				const auto TargetStatus = std::filesystem::symlink_status(To, Ec);
				if (Ec == std::errc::no_such_file_or_directory) Ec.clear();
				if (Ec || (std::filesystem::exists(TargetStatus) && Directory != std::filesystem::is_directory(TargetStatus)))
				{ Error = "A file/directory ownership conflict occupies a transfer destination."; return false; }
				if (!Directory)
				{
					FAssetCompanionOwnership TargetOwnership;
					std::optional<UnifiedError::FError> InspectionError;
					if (!QueryAssetCompanionOwnership(To, TargetOwnership, &InspectionError))
					{ Error = InspectionError->ToString(); return false; }
					if (TargetOwnership.State != EAssetCompanionOwnershipState::Unclaimed)
					{ Error = "A package companion owns the destination path."; return false; }
				}
				if (std::ranges::any_of(Entries, [&](const auto& Entry) { return Same(Entry.Source, Path); })) return true;
				Entries.push_back({Path, To, Directory});
				return true;
			};
			if (!Collect(Source, true)) return Rejected(Inputs, Error);
			if (Folder)
			{
				for (std::filesystem::recursive_directory_iterator It(Source, Ec), End; !Ec && It != End; It.increment(Ec))
					if (!Collect(It->path(), false)) return Rejected(Inputs, Error);
				if (Ec) return Rejected(Inputs, "Could not enumerate the complete source directory: " + Ec.message());
			}
		}
		std::ranges::sort(Entries, [](const auto& A, const auto& B) {
			if (A.Directory != B.Directory) return A.Directory;
			if (A.Directory && A.Source.native().size() != B.Source.native().size()) return A.Source.native().size() < B.Source.native().size();
			return A.Source.generic_string() < B.Source.generic_string();
		});
		auto Validate = [&](std::string* Out) {
			if (!Admitted(Inputs, Out)) return false;
			for (const auto& Entry : Entries)
				if (!SafePath(Entry.Source, false, Out) || !SafePath(Entry.Target, true, Out)) return false;
			return Admitted(Inputs, Out);
		};
		if (!Validate(&Error)) return Rejected(Inputs, Error);
		bool Success = false;
		bool Stopped = false;
		FContentChangeBatch Changes;
		for (const auto& Entry : Entries)
		{
			std::string Error;
			if (!Admitted(Inputs, &Error) || !SafePath(Entry.Source, Move, &Error) || !SafePath(Entry.Target, true, &Error))
			{ ReportDiagnostic(Inputs, ELogLevel::Error, Error); Stopped = true; break; }
			std::error_code Ec;
			if (Entry.Directory)
			{
				const bool Created = std::filesystem::create_directory(Entry.Target, Ec);
				if (Ec || (!Created && !std::filesystem::is_directory(Entry.Target, Ec)))
				{ ReportDiagnostic(Inputs, ELogLevel::Error, "Could not create destination directory: " + Ec.message()); continue; }
				Success = true;
				if (Created) Changes.Changes.push_back({EContentChangeKind::Added, {}, Entry.Target.generic_string(), {}, {}, true});
				continue;
			}
			FAssetCompanionOwnership Ownership;
			if (!QueryAssetCompanionOwnership(Entry.Source, Ownership) || Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
			{ ReportDiagnostic(Inputs, ELogLevel::Error, "File ownership changed before transfer."); Stopped = true; break; }
			if (!QueryAssetCompanionOwnership(Entry.Target, Ownership) || Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
			{ ReportDiagnostic(Inputs, ELogLevel::Error, "Destination ownership changed before transfer."); Stopped = true; break; }
			if (!Admitted(Inputs, &Error)) { ReportDiagnostic(Inputs, ELogLevel::Error, Error); Stopped = true; break; }
			const auto Identity = FFileHelper::HashFileXx128(Entry.Source);
			if (!Identity) { ReportDiagnostic(Inputs, ELogLevel::Error, Identity.error().GetMessage()); continue; }
			if (!std::filesystem::copy_file(Entry.Source, Entry.Target, std::filesystem::copy_options::none, Ec))
			{
				ReportDiagnostic(Inputs, ELogLevel::Error, "Copy failed: " + Ec.message());
				if (Ec != std::errc::file_exists)
				{
					Changes.bFullRefresh = true;
					ReportDiagnostic(Inputs, ELogLevel::Warn, "Inspect retained destination: " + Entry.Target.generic_string());
					Stopped = true;
					break;
				}
				continue;
			}
			Success = true;
			bool Removed = false;
			if (Move)
			{
				const bool MayRemove = Admitted(Inputs, &Error) && SafePath(Entry.Source, true, &Error);
				const auto SourceIdentity = FFileHelper::HashFileXx128(Entry.Source), TargetIdentity = FFileHelper::HashFileXx128(Entry.Target);
				Removed = MayRemove && SourceIdentity && TargetIdentity && *Identity == *SourceIdentity && *Identity == *TargetIdentity
					&& std::filesystem::remove(Entry.Source, Ec);
				if (!Removed)
				{
					ReportDiagnostic(Inputs, ELogLevel::Error, "Destination retained; source removal failed or source changed. " + Error + Ec.message());
					ReportDiagnostic(Inputs, ELogLevel::Warn, "Inspect retained source and destination: " + Entry.Source.generic_string() + " -> " + Entry.Target.generic_string());
					Stopped = true;
				}
			}
			Changes.Changes.push_back({Removed ? EContentChangeKind::Renamed : EContentChangeKind::Added, Removed ? Entry.Source.generic_string() : std::string{}, Entry.Target.generic_string()});
			if (Stopped) break;
		}
		if (Move && !Stopped)
			for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
				if (It->Directory)
				{
					std::error_code Ec;
					if (Admitted(Inputs, nullptr) && SafePath(It->Source, true, nullptr) && std::filesystem::remove(It->Source, Ec))
					{
						Changes.Changes.push_back({EContentChangeKind::Removed, It->Source.generic_string(), {}, {}, {}, true});
						Success = true;
					} else if (Ec)
						ReportDiagnostic(Inputs, ELogLevel::Warn, "Source directory could not be removed; retained contents require separate handling: " + It->Source.generic_string());
				}
		Publish(Inputs, std::move(Changes));
		return Success;
	}
}
