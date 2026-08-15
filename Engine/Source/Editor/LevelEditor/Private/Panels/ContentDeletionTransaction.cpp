#include "Panels/ContentBrowserOperations.h"

#include "Misc/LexicalPath.h"
#include "Misc/FileHelper.h"

#include <atomic>
#include <fstream>

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr std::string_view MarkerName = ".durin-content-deletion";
		constexpr std::string_view MarkerContents = "Durin Content Browser Undo v1";
		std::atomic<uint64> NextOperationId = 1;

		auto Normalize(const std::filesystem::path& Path) -> std::filesystem::path
		{
			return std::filesystem::absolute(Path).lexically_normal();
		}

		auto SamePath(
			const std::filesystem::path& A,
			const std::filesystem::path& B) -> bool
		{
			std::filesystem::path Relative;
			return PathUtilities::TryMakeLexicalRelativePath(
				Normalize(A), Normalize(B), Relative) && Relative.empty();
		}

		auto DefaultRename(
			const std::filesystem::path& Source,
			const std::filesystem::path& Destination,
			EContentDeletionMovePhase) -> std::error_code
		{
			std::error_code Ec;
			std::filesystem::rename(Source, Destination, Ec);
			return Ec;
		}
	} // namespace

	FContentDeletionTransaction::FContentDeletionTransaction(
		FContentDeletionPlanPtr InPlan,
		FContentDeletionTransactionHooks InHooks)
		: Plan(std::move(InPlan))
		, Hooks(std::move(InHooks))
	{
		if (!Hooks.Rename) Hooks.Rename = DefaultRename;
		Description = Plan
			? std::format("Delete {}", Plan->DisplayName)
			: "Delete Content";
	}

	FContentDeletionTransaction::~FContentDeletionTransaction()
	{
		CleanupOwnedStagingRoot();
	}

	auto FContentDeletionTransaction::GetDescription() const -> std::string_view
	{
		return Description;
	}

	auto FContentDeletionTransaction::GetDetails(
		::Durin::Editor::ETransactionOperation) const -> std::string
	{
		return Details;
	}

	auto FContentDeletionTransaction::Fail(std::string Message) -> bool
	{
		Details = std::move(Message);
		return false;
	}

	auto FContentDeletionTransaction::EnsureStagingRoot() -> bool
	{
		if (!StagingRoot.empty()) return true;
		if (!Plan || Plan->StagingVolumeRoot.empty())
			return Fail("The deletion plan has no staging volume.");
		const std::filesystem::path Base = Normalize(Plan->StagingVolumeRoot);
		std::error_code Ec;
		std::filesystem::create_directories(Base, Ec);
		if (Ec) return Fail(std::format(
			"Could not create the deletion staging directory: {}", Ec.message()));

		for (uint32 Attempt = 0; Attempt < 64; ++Attempt)
		{
			const uint64 Id = NextOperationId.fetch_add(1);
			const uint64 Time = static_cast<uint64>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			const std::filesystem::path Candidate =
				Base / std::format("operation-{:016x}-{:016x}", Time, Id);
			Ec.clear();
			if (!std::filesystem::create_directory(Candidate, Ec))
			{
				if (Ec) continue;
				continue;
			}
			StagingRoot = Candidate;
			MarkerPath = StagingRoot / MarkerName;
			std::ofstream Marker(MarkerPath, std::ios::binary | std::ios::trunc);
			Marker << MarkerContents;
			Marker.close();
			if (Marker)
			{
				Moves.reserve(Plan->MaximalRoots.size());
				for (size_t Index = 0; Index < Plan->MaximalRoots.size(); ++Index)
					Moves.push_back({
						Normalize(Plan->MaximalRoots[Index].OriginalPath),
						StagingRoot / std::format("entry-{:04}", Index)});
				return true;
			}
			std::filesystem::remove_all(StagingRoot, Ec);
			StagingRoot.clear();
			MarkerPath.clear();
			return Fail("Could not write the deletion staging ownership marker.");
		}
		return Fail("Could not allocate a unique deletion staging directory.");
	}

	auto FContentDeletionTransaction::ValidatePhysicalState(bool bApplied) -> bool
	{
		if (!Plan) return Fail("The deletion plan is unavailable.");
		std::unordered_set<std::string> ExpectedPaths;
		for (const FContentDeletionFingerprint& Entry : Plan->Entries)
		{
			const std::filesystem::path Original = Normalize(Entry.PhysicalPath);
			std::filesystem::path Current;
			for (const FMove& Move : Moves)
			{
				if (SamePath(Original, Move.Original))
				{
					Current = bApplied ? Move.Staged : Move.Original;
					break;
				}
				if (PathUtilities::IsLexicalDescendantPath(
						Original.generic_string(), Move.Original.generic_string(), true))
				{
					Current = (bApplied ? Move.Staged : Move.Original)
						/ Original.lexically_relative(Move.Original);
					break;
				}
			}
			if (Current.empty())
				return Fail(std::format(
					"The deletion manifest does not own {}.", Entry.PhysicalPath));
			ExpectedPaths.insert(Normalize(Current).generic_string());
			std::error_code Ec;
			const auto Status = std::filesystem::symlink_status(Current, Ec);
			if (Ec || std::filesystem::is_symlink(Status))
				return Fail(std::format("Deletion source changed: {}.", Current.generic_string()));
			const bool bDirectory = Entry.Kind == EContentDeletionEntryKind::Directory;
			if (bDirectory != std::filesystem::is_directory(Status))
				return Fail(std::format("Deletion source changed: {}.", Current.generic_string()));
			if (!bDirectory)
			{
				const uintmax_t Size = std::filesystem::file_size(Current, Ec);
				if (Ec || Size != Entry.FileSize)
					return Fail(std::format("Deletion source changed: {}.", Current.generic_string()));
				FXxHash128 ByteIdentity;
				if (!FFileHelper::HashFileXx128(Current, ByteIdentity, Ec))
					return Fail(std::format(
						"Could not fingerprint deletion source {}: {}",
						Current.generic_string(), Ec.message()));
				if (ByteIdentity != Entry.ByteIdentity)
					return Fail(std::format(
						"Deletion source bytes changed: {}.",
						Current.generic_string()));
			}
			const auto WriteTime = std::filesystem::last_write_time(Current, Ec);
			if (Ec || static_cast<int64>(WriteTime.time_since_epoch().count())
				!= Entry.LastWriteTimeTicks)
				return Fail(std::format("Deletion source changed: {}.", Current.generic_string()));
		}

		for (const FMove& Move : Moves)
		{
			const std::filesystem::path Root = bApplied ? Move.Staged : Move.Original;
			std::error_code Ec;
			if (!std::filesystem::is_directory(Root, Ec)) continue;
			for (std::filesystem::recursive_directory_iterator It(
					 Root, std::filesystem::directory_options::none, Ec), End;
				 !Ec && It != End; It.increment(Ec))
				if (!ExpectedPaths.contains(Normalize(It->path()).generic_string()))
					return Fail(std::format(
						"Deletion source gained an unconfirmed entry: {}.",
						It->path().generic_string()));
			if (Ec) return Fail(std::format(
				"Could not inspect deletion source {}: {}",
				Root.generic_string(), Ec.message()));
		}
		return true;
	}

	auto FContentDeletionTransaction::ValidateOriginalDestinations() -> bool
	{
		for (const FMove& Move : Moves)
		{
			std::error_code Ec;
			if (std::filesystem::exists(Move.Original, Ec) || Ec)
				return Fail(std::format(
					"Undo destination is no longer available: {}.",
					Move.Original.generic_string()));
		}
		return true;
	}

	auto FContentDeletionTransaction::MovePath(
		const std::filesystem::path& Source,
		const std::filesystem::path& Destination,
		EContentDeletionMovePhase Phase) -> bool
	{
		const std::error_code Ec = Hooks.Rename(Source, Destination, Phase);
		if (!Ec) return true;
		return Fail(std::format(
			"Could not move {} to {}: {}",
			Source.generic_string(), Destination.generic_string(), Ec.message()));
	}

	auto FContentDeletionTransaction::CompensateMoves(
		size_t Count,
		bool bBackToOriginal,
		EContentDeletionMovePhase Phase) -> bool
	{
		while (Count > 0)
		{
			--Count;
			const FMove& Move = Moves[Count];
			const std::filesystem::path& Source =
				bBackToOriginal ? Move.Staged : Move.Original;
			const std::filesystem::path& Destination =
				bBackToOriginal ? Move.Original : Move.Staged;
			if (!MovePath(Source, Destination, Phase)) return false;
			const auto Entry = std::ranges::find(
				Journal, Move.Original.generic_string(),
				&FContentDeletionJournalEntry::OriginalPath);
			if (Entry != Journal.end()) Entry->bCompensated = true;
		}
		return true;
	}

	auto FContentDeletionTransaction::MakePhysicalTransition()
		-> Asset::FAssetDeletionPhysicalTransition
	{
		return {
			.Stage = [this] { return StagePhysicalDeletion(); },
			.Restore = [this] { return RestorePhysicalDeletion(); },
			.IsRecoveryRequired = [this] {
				return State == EContentDeletionTransactionState::RecoveryRequired;
			},
		};
	}

	auto FContentDeletionTransaction::StagePhysicalDeletion()
		-> Asset::FAssetResult
	{
		if (!ValidatePhysicalState(false))
			return {Asset::EAssetError::IoError, Details};
		size_t Moved = 0;
		for (const FMove& Move : Moves)
		{
			Journal.push_back({
				.Operation = EContentDeletionJournalOperation::MoveToStaging,
				.OriginalPath = Move.Original.generic_string(),
				.StagedPath = Move.Staged.generic_string()});
			if (!MovePath(Move.Original, Move.Staged, EContentDeletionMovePhase::Apply))
			{
				const std::string Cause = Details;
				if (!CompensateMoves(
						Moved, true, EContentDeletionMovePhase::CompensateApply))
				{
					State = EContentDeletionTransactionState::RecoveryRequired;
					return {Asset::EAssetError::IoError, std::format(
						"{} Recovery requires retained staging at {}: {}",
						Cause, StagingRoot.generic_string(), Details)};
				}
				return {Asset::EAssetError::IoError, Cause};
			}
			Journal.back().bCompleted = true;
			++Moved;
		}
		return {};
	}

	auto FContentDeletionTransaction::RestorePhysicalDeletion()
		-> Asset::FAssetResult
	{
		if (!ValidatePhysicalState(true) || !ValidateOriginalDestinations())
			return {Asset::EAssetError::IoError, Details};
		size_t Moved = 0;
		for (const FMove& Move : Moves)
		{
			Journal.push_back({
				.Operation = EContentDeletionJournalOperation::MoveToOriginal,
				.OriginalPath = Move.Original.generic_string(),
				.StagedPath = Move.Staged.generic_string()});
			if (!MovePath(Move.Staged, Move.Original, EContentDeletionMovePhase::Undo))
			{
				const std::string Cause = Details;
				if (!CompensateMoves(
						Moved, false, EContentDeletionMovePhase::CompensateUndo))
				{
					State = EContentDeletionTransactionState::RecoveryRequired;
					return {Asset::EAssetError::IoError, std::format(
						"{} Recovery requires retained staging at {}: {}",
						Cause, StagingRoot.generic_string(), Details)};
				}
				return {Asset::EAssetError::IoError, Cause};
			}
			Journal.back().bCompleted = true;
			++Moved;
		}
		return {};
	}

	auto FContentDeletionTransaction::Redo() -> bool
	{
		Details.clear();
		if (State != EContentDeletionTransactionState::Restored)
			return Fail("The deletion transaction is not in a restorable state.");
		if (!Plan || !Plan->CanExecute())
			return Fail("The deletion plan is blocked or unavailable.");
		if (!EnsureStagingRoot()) return false;
		State = EContentDeletionTransactionState::Applying;
		Journal.clear();
		const Asset::EAssetMutationTransactionState AssetState =
			Plan->AssetTransaction.GetState();
		const Asset::FAssetResult AssetResult =
			AssetState == Asset::EAssetMutationTransactionState::Prepared
			? Plan->AssetTransaction.Commit(MakePhysicalTransition())
			: Plan->AssetTransaction.Redo(MakePhysicalTransition());
		if (!AssetResult)
		{
			if (Plan->AssetTransaction.GetState()
				== Asset::EAssetMutationTransactionState::RecoveryRequired)
				State = EContentDeletionTransactionState::RecoveryRequired;
			else
				State = EContentDeletionTransactionState::Restored;
			return Fail(AssetResult.Message);
		}
		State = EContentDeletionTransactionState::Applied;
		Details = std::format("Staged {} deletion root(s).", Moves.size());
		return true;
	}

	auto FContentDeletionTransaction::Undo() -> bool
	{
		Details.clear();
		if (State != EContentDeletionTransactionState::Applied)
			return Fail("The deletion transaction is not applied.");
		State = EContentDeletionTransactionState::Restoring;
		Journal.clear();
		const Asset::FAssetResult Restore =
			Plan->AssetTransaction.Undo(MakePhysicalTransition());
		if (!Restore)
		{
			if (Plan->AssetTransaction.GetState()
				== Asset::EAssetMutationTransactionState::RecoveryRequired)
				State = EContentDeletionTransactionState::RecoveryRequired;
			else
				State = EContentDeletionTransactionState::Applied;
			return Fail(Restore.Message);
		}
		State = EContentDeletionTransactionState::Restored;
		Details = std::format("Restored {} deletion root(s).", Moves.size());
		return true;
	}

	auto FContentDeletionTransaction::CleanupOwnedStagingRoot() -> void
	{
		if (StagingRoot.empty()
			|| State == EContentDeletionTransactionState::RecoveryRequired
			|| !Plan)
			return;
		const std::filesystem::path Base = Normalize(Plan->StagingVolumeRoot);
		if (!SamePath(StagingRoot.parent_path(), Base)
			|| !StagingRoot.filename().generic_string().starts_with("operation-"))
			return;
		std::error_code Ec;
		const auto RootStatus = std::filesystem::symlink_status(StagingRoot, Ec);
		if (Ec || !std::filesystem::is_directory(RootStatus)
			|| std::filesystem::is_symlink(RootStatus))
			return;
		std::ifstream Marker(MarkerPath, std::ios::binary);
		const std::string Contents(
			(std::istreambuf_iterator<char>(Marker)),
			std::istreambuf_iterator<char>());
		if (Marker.bad() || Contents != MarkerContents) return;
		Marker.close();
		std::filesystem::remove_all(StagingRoot, Ec);
	}
} // namespace Durin::Editor::Level
