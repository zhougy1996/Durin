#include "Data/ContentBrowserNativeTransfer.h"
#include "Operations/ContentBrowserTransferPaths.h"
#include "AssetTools/AssetDeletion.h"
#include "AssetRegistry/Catalog.h"
#include "Misc/FileHelper.h"

namespace Durin::ContentBrowser::Private
{
	using namespace TransferPaths;
	auto PrepareFileTransfer(std::vector<FNativeTransferInput> Inputs, std::string Destination, bool Move, std::optional<std::string> NewName)
		-> std::unique_ptr<FContentBrowserItemDataTransferContext>
	{
		struct FEntry
		{
			std::filesystem::path Source, Target;
			FContentBrowserItemContributionKey OldKey, NewKey;
			bool Directory = false;
		};
		std::vector<FEntry> Entries;
		std::vector<FContentBrowserTransferDestination> Claims;
		std::vector<FContentBrowserItemContributionKey> Keys;
		const auto Catalog = CaptureAssetCatalogSnapshot();
		std::string Error;
		std::error_code Ec;
		if (!SafePath(Destination, true, &Error) || !std::filesystem::is_directory(Destination, Ec) || Ec)
			return Rejected(std::move(Inputs), Error.empty() ? "The transfer destination is not a directory." : Error);
		for (const auto& Input : Inputs)
		{
			Keys.push_back(Input.Identity);
			const auto Source = std::filesystem::path(Input.Item.PhysicalPath).lexically_normal();
			if (std::ranges::any_of(Inputs, [&](const auto& Other) {
				return Other.Item.Kind == EContentBrowserItemKind::Folder && !Same(Source, Other.Item.PhysicalPath)
					&& FPaths::IsLexicalDescendantPath(Source, Other.Item.PhysicalPath, true);
			})) continue;
			const auto Mount = FContentBrowserPaths::ResolveMountPath(Source.generic_string());
			if (!Mount || Same(Source, Mount.Mount->GetContentDir()) || !SafePath(Source, Move, &Error)
				|| (Move && Mount.Mount != FContentBrowserPaths::ResolveMountPath(Destination).Mount))
				return Rejected(std::move(Inputs), Error.empty() ? "The selected source cannot be transferred." : Error);
			const bool Folder = Input.Item.Kind == EContentBrowserItemKind::Folder;
			if (!Folder && Input.Item.Kind != EContentBrowserItemKind::File) return Rejected(std::move(Inputs), "File transfer requires file-source items.");
			auto Target = std::filesystem::path(Destination) / Source.filename();
			if (NewName)
			{
				Target = Source.parent_path() / *NewName;
				if (!Folder && Target.extension().empty()) Target += Source.extension();
				if (std::filesystem::exists(Target, Ec) || Ec) return Rejected(std::move(Inputs), "The rename destination is already occupied.");
			}
			for (const auto& Other : Inputs)
				if (Same(Target, Other.Item.PhysicalPath) || FPaths::IsLexicalDescendantPath(Target, Other.Item.PhysicalPath, true))
					return Rejected(std::move(Inputs), "A destination cannot be inside a selected source.");
			auto Collect = [&](const std::filesystem::path& Path, bool Independent) -> bool {
				if (!SafePath(Path, Move, &Error)) return false;
				const auto EntryMount = FContentBrowserPaths::ResolveMountPath(Path.generic_string());
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
					const auto OwnershipResult = QueryAssetCompanionOwnership(Path, Ownership);
					if (!OwnershipResult) { Error = OwnershipResult.Message; return false; }
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
				const auto TargetStatus = std::filesystem::symlink_status(To, Ec);
				if (Ec == std::errc::no_such_file_or_directory) Ec.clear();
				if (Ec || (std::filesystem::exists(TargetStatus) && Directory != std::filesystem::is_directory(TargetStatus)))
				{ Error = "A file/directory ownership conflict occupies a transfer destination."; return false; }
				if (!Directory)
				{
					FAssetCompanionOwnership TargetOwnership;
					const auto Inspected = QueryAssetCompanionOwnership(To, TargetOwnership);
					if (!Inspected || TargetOwnership.State != EAssetCompanionOwnershipState::Unclaimed)
					{ Error = "A package companion owns the destination path."; return false; }
				}
				if (std::ranges::any_of(Entries, [&](const auto& Entry) { return Same(Entry.Source, Path); })) return true;
				const auto OldVirtual = FContentBrowserPaths::ResolveMountPath(Path.generic_string()).VirtualPath;
				const auto NewVirtual = FContentBrowserPaths::ResolveMountPath(To.generic_string()).VirtualPath;
				Entries.push_back({Path, To, KeyAt(Input.Identity, OldVirtual, Directory), KeyAt(Input.Identity, NewVirtual, Directory), Directory});
				Claims.push_back({Claim(To), Directory});
				return true;
			};
			if (!Collect(Source, true)) return Rejected(std::move(Inputs), Error);
			if (Folder)
			{
				for (std::filesystem::recursive_directory_iterator It(Source, Ec), End; !Ec && It != End; It.increment(Ec))
					if (!Collect(It->path(), false)) return Rejected(std::move(Inputs), Error);
				if (Ec) return Rejected(std::move(Inputs), "Could not enumerate the complete source directory: " + Ec.message());
			}
		}
		std::ranges::sort(Entries, [](const auto& A, const auto& B) {
			if (A.Directory != B.Directory) return A.Directory;
			if (A.Directory && A.Source.native().size() != B.Source.native().size()) return A.Source.native().size() < B.Source.native().size();
			return A.Source.generic_string() < B.Source.generic_string();
		});
		auto Validate = [Inputs, Entries](std::string* Out) {
			if (!Admitted(Inputs, Out)) return false;
			for (const auto& Entry : Entries)
				if (!SafePath(Entry.Source, false, Out) || !SafePath(Entry.Target, true, Out)) return false;
			return Admitted(Inputs, Out);
		};
		return std::make_unique<FContentBrowserItemDataTransferContext>(std::move(Keys), std::move(Claims), Validate,
			[Inputs = std::move(Inputs), Entries = std::move(Entries), Move](const auto& Cancelled) {
				FContentBrowserOperationResult Result;
				FContentChangeBatch Changes;
				for (const auto& Entry : Entries)
				{
					std::string Error;
					if (Cancelled()) Result.bCancelled = true;
					if (!Admitted(Inputs, &Error)) Result.bStopped = true;
					if (Cancelled()) Result.bCancelled = true;
					if (Result.bStopped || Result.bCancelled)
					{ Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Unattempted, Error.empty() ? "Transfer stopped before this effect." : Error); continue; }
					if (!SafePath(Entry.Source, Move, &Error) || !SafePath(Entry.Target, true, &Error))
					{ Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, Error); Result.bStopped = true; continue; }
					std::error_code Ec;
					if (Entry.Directory)
					{
						const bool Created = std::filesystem::create_directory(Entry.Target, Ec);
						if (Ec || (!Created && !std::filesystem::is_directory(Entry.Target, Ec)))
						{ Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, "Could not create destination directory: " + Ec.message()); continue; }
						if (Created) Result.Effects.push_back({Entry.OldKey, Entry.NewKey, EContentBrowserEffectStatus::Committed});
						else Result.bNoChanges = true;
						if (Created) Changes.Changes.push_back({EContentChangeKind::Added, {}, Entry.Target.generic_string(), {}, {}, true});
						continue;
					}
					FAssetCompanionOwnership Ownership;
					const auto Owned = QueryAssetCompanionOwnership(Entry.Source, Ownership);
					if (!Owned || Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
					{ Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, "File ownership changed before transfer."); Result.bStopped = true; continue; }
					const auto TargetOwned = QueryAssetCompanionOwnership(Entry.Target, Ownership);
					if (!TargetOwned || Ownership.State != EAssetCompanionOwnershipState::Unclaimed)
					{ Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, "Destination ownership changed before transfer."); Result.bStopped = true; continue; }
					if (!Admitted(Inputs, &Error) || Cancelled()) { Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Unattempted, Error); Result.bStopped = !Cancelled(); Result.bCancelled = Cancelled(); continue; }
					const auto Identity = FFileHelper::HashFileXx128(Entry.Source);
					if (!Identity) { Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, Identity.error().ToString()); continue; }
					if (!std::filesystem::copy_file(Entry.Source, Entry.Target, std::filesystem::copy_options::none, Ec))
					{
						Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, "Copy failed: " + Ec.message());
						if (Ec != std::errc::file_exists)
						{ Result.bStopped = true; Result.Effects.back().RecoveryLocations = {Entry.Target.generic_string()}; Changes.bFullRefresh = true; }
						continue;
					}
					Result.Effects.push_back({Entry.OldKey, Entry.NewKey, EContentBrowserEffectStatus::Committed});
					bool Removed = false;
					if (Move)
					{
						const bool MayRemove = Admitted(Inputs, &Error) && !Cancelled() && SafePath(Entry.Source, true, &Error);
						Result.bCancelled |= Cancelled();
						const auto SourceIdentity = FFileHelper::HashFileXx128(Entry.Source), TargetIdentity = FFileHelper::HashFileXx128(Entry.Target);
						Removed = MayRemove && SourceIdentity && TargetIdentity && *Identity == *SourceIdentity && *Identity == *TargetIdentity
							&& std::filesystem::remove(Entry.Source, Ec);
						if (!Removed)
						{
							Result.Add(Entry.OldKey, EContentBrowserEffectStatus::Failed, "Destination retained; source removal failed or source changed. " + Error + Ec.message());
							Result.Effects.back().RecoveryLocations = {Entry.Source.generic_string(), Entry.Target.generic_string()};
							Result.bStopped = true;
						}
					}
					Changes.Changes.push_back({Removed ? EContentChangeKind::Renamed : EContentChangeKind::Added, Removed ? Entry.Source.generic_string() : std::string{}, Entry.Target.generic_string()});
				}
				if (Move && !Result.bCancelled && !Result.bStopped)
					for (auto It = Entries.rbegin(); It != Entries.rend(); ++It)
						if (It->Directory)
						{
							std::error_code Ec;
							if (Admitted(Inputs, nullptr) && SafePath(It->Source, true, nullptr) && std::filesystem::remove(It->Source, Ec))
							{
								Changes.Changes.push_back({EContentChangeKind::Removed, It->Source.generic_string(), {}, {}, {}, true});
								Result.Effects.push_back({It->OldKey, It->NewKey, EContentBrowserEffectStatus::Committed});
							} else if (Ec && Inputs.front().Policy->ReportDiagnostic)
								Inputs.front().Policy->ReportDiagnostic(ELogLevel::Warn, "Source directory could not be removed; retained contents require separate handling: " + It->Source.generic_string());
						}
				Publish(Inputs, std::move(Changes));
				return Result;
			});
	}
}
