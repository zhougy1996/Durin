#include "AssetMutationJournalInternal.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetPackageCodec.h"
#include "AssetPublicationCoordinatorInternal.h"
#include "Asset/PackageVersionPolicy.h"

#include "Misc/FileTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"

#include <charconv>

namespace Durin::AssetPrivate
{
	namespace
	{
		std::filesystem::path GMutationRecoveryDirectoryOverride;

		auto GetMutationRecoveryDirectory() -> std::filesystem::path
		{
			if (!GMutationRecoveryDirectoryOverride.empty())
				return NormalizePhysicalPath(
					GMutationRecoveryDirectoryOverride
				);
			const std::string RecoveryBase = FPaths::ProjectDir().empty() ? FPaths::LaunchDir() : FPaths::ProjectDir();
			return NormalizePhysicalPath(RecoveryBase)
				   / "Saved" / "AssetMutationRecovery";
		}

		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto MakeMutationOperationId() -> std::string
		{
			static std::atomic<uint64> Counter = 1;
			const uint64 Time = static_cast<uint64>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			return std::format("{:016x}{:016x}", Time, Counter++);
		}

		auto GetMutationOperationType(
			EAssetMutationJournalKind OperationKind) -> std::string_view
		{
			switch (OperationKind)
			{
			case EAssetMutationJournalKind::Relocation:
				return "relocation";
			case EAssetMutationJournalKind::RedirectorFixup:
				return "fixup";
			}
			return "unknown";
		}

		auto MakeMutationJournalOwnerMarker(std::string_view OperationId)
			-> std::string
		{
			return std::format("durin-asset-mutation\n{}\n", OperationId);
		}

		auto IsReadOnlyMutationInput(const std::filesystem::path& Path) -> bool
		{
			std::error_code ErrorCode;
			const std::filesystem::perms Permissions =
				std::filesystem::status(Path, ErrorCode).permissions();
			constexpr auto WritePermissions =
				std::filesystem::perms::owner_write
				| std::filesystem::perms::group_write
				| std::filesystem::perms::others_write;
			return ErrorCode || (Permissions & WritePermissions)
				== std::filesystem::perms::none;
		}
	}

	FAssetMutationJournal::~FAssetMutationJournal()
	{
		if (State == EAssetMutationState::Publishing
			|| State == EAssetMutationState::RecoveryRequired) return;
		const std::string ExpectedOwner =
			MakeMutationJournalOwnerMarker(OperationId);
		for (const std::filesystem::path& Root : Roots)
		{
			if (Root.filename() != std::format("operation-{}", OperationId)
				|| Root.parent_path().filename() != ".durin-asset-mutation")
				continue;
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(Root / "owner", ErrorCode))
				continue;
			FByteArray OwnerBytes;
			if (!FFileHelper::LoadFileToArray(
					OwnerBytes, (Root / "owner"))
				|| std::string_view(
					reinterpret_cast<const char*>(OwnerBytes.data()),
					OwnerBytes.size()) != ExpectedOwner)
				continue;
			std::filesystem::remove_all(Root, ErrorCode);
		}
		if (LocatorPath.filename() == std::format("operation-{}", OperationId)
			&& LocatorPath.parent_path().filename() == "AssetMutationRecovery")
		{
			std::error_code ErrorCode;
			std::filesystem::remove(LocatorPath, ErrorCode);
			std::filesystem::remove(LocatorPath.parent_path(), ErrorCode);
		}
	}

	auto MakePackageFingerprint(
		std::string_view PhysicalPath,
		std::span<const std::byte> Bytes,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
	{
		std::error_code ErrorCode;
		const std::filesystem::path Path(PhysicalPath);
		const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
		if (ErrorCode)
			return Error(EAssetError::IoError, std::format(
				"Failed to read the last-write time for asset package {}.", PhysicalPath));
		uint32 ReaderVersion = 0;
		if (Path.extension() == ".dasset")
		{
			const FAssetPackageCodec* Codec = nullptr;
			const FAssetResult ResolveResult = ResolveAssetPackageReader(
				Bytes, Codec, &ReaderVersion);
			if (!ResolveResult) return ResolveResult;
		}
		OutFingerprint = {
			.FileSize = Bytes.size(),
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime),
			.ContentHash = FXxHash128::HashBuffer(Bytes),
			.ReaderVersion = ReaderVersion};
		return {};
	}

	auto InitializeMutationJournal(
		FAssetMutationJournal& Journal,
		EAssetMutationJournalKind OperationKind) -> void
	{
		Journal.OperationId = MakeMutationOperationId();
		Journal.OperationType = GetMutationOperationType(OperationKind);
		Journal.LocatorPath = GetMutationRecoveryDirectory()
							  / std::format("operation-{}", Journal.OperationId);
	}

	auto NormalizePhysicalPath(const std::filesystem::path& Path)
		-> std::filesystem::path
	{
		return std::filesystem::absolute(Path).lexically_normal();
	}

	auto LoadRelocationBytes(
		const std::filesystem::path& Path,
		FByteArray& OutBytes) -> FAssetResult
	{
		OutBytes.clear();
		if (!FFileHelper::LoadFileToArray(OutBytes, Path))
			return Error(EAssetError::IoError, std::format(
				"Could not read relocation input {}.", Path.generic_string()));
		return {};
	}

	auto SaveRelocationBytes(
		const std::filesystem::path& Path,
		std::span<const std::byte> Bytes) -> FAssetResult
	{
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Bytes.data()),
					Bytes.size()},
				Path,
				&PublicationError))
			return Error(EAssetError::IoError, PublicationError.ToString());
		return {};
	}

	auto FingerprintRelocationFile(
		const std::filesystem::path& Path,
		FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
	{
		FByteArray Bytes;
		FAssetResult Result = LoadRelocationBytes(Path, Bytes);
		if (!Result) return Result;
		return MakePackageFingerprint(
			Path.generic_string(), Bytes, OutFingerprint);
	}

	auto IsWritableRelocationPath(
		const std::filesystem::path& Path,
		const FMountPoint*& OutMount,
		std::string& OutError) -> bool
	{
		OutMount = nullptr;
		const std::filesystem::path Normalized = NormalizePhysicalPath(Path);
		for (const FMountPoint& Mount :
			FMountPaths::GetRegisteredMountPoints())
		{
			const std::filesystem::path Content =
				NormalizePhysicalPath(Mount.GetContentDir());
			if (!FPaths::IsLexicalDescendantPath(
					Normalized.generic_string(), Content.generic_string(), true))
				continue;
			if (!Mount.bContentWritable)
			{
				OutError = std::format(
					"Content mount {} is read-only.", Mount.VirtualRoot);
				return false;
			}
			for (std::filesystem::path Current = Normalized.parent_path();
				!Current.empty(); Current = Current.parent_path())
			{
				std::error_code StatusError;
				const auto Status = std::filesystem::symlink_status(
					Current, StatusError);
				if (!StatusError && std::filesystem::is_symlink(Status))
				{
					OutError = std::format(
						"Relocation path traverses a reparse point: {}.",
						Current.generic_string());
					return false;
				}
				if (Current == Content) break;
				if (Current == Current.root_path()) break;
			}
			OutMount = &Mount;
			return true;
		}
		OutError = std::format(
			"Relocation path is outside writable mounted content: {}.",
			Path.generic_string());
		return false;
	}

	auto StageMutationJournalEntry(
		FAssetMutationJournal& Journal,
		const FMutationJournalStageRequest& Request,
		size_t& OutEntryIndex) -> FAssetResult
	{
		OutEntryIndex = std::numeric_limits<size_t>::max();
		const std::filesystem::path Normalized =
			NormalizePhysicalPath(Request.PhysicalPath);
		const std::string Key = Normalized.generic_string();
		FAssetMutationJournalEntry Entry{
			.PhysicalPath = Normalized,
			.RegistryPath = Request.RegistryPath,
			.Role = Request.Role,
			.bPreExists = Request.bPreExists,
			.bPostExists = Request.bPostExists};
		if (Request.bPreExists)
			Entry.StagedPreHash = FXxHash128::HashBuffer(Request.PreBytes);
		if (Request.bPostExists)
		{
			Entry.StagedPostHash = FXxHash128::HashBuffer(Request.PostBytes);
			Entry.ExpectedPostFingerprint.FileSize = Request.PostBytes.size();
			Entry.ExpectedPostFingerprint.ContentHash = Entry.StagedPostHash;
		}

		if (const auto Existing = Journal.EntryIndices.find(Key);
			Existing != Journal.EntryIndices.end())
		{
			const FAssetMutationJournalEntry& ExistingEntry =
				Journal.Entries[Existing->second];
			const bool bEquivalent =
				ExistingEntry.RegistryPath == Entry.RegistryPath
				&& ExistingEntry.Role == Entry.Role
				&& ExistingEntry.bPreExists == Entry.bPreExists
				&& ExistingEntry.bPostExists == Entry.bPostExists
				&& ExistingEntry.StagedPreHash == Entry.StagedPreHash
				&& ExistingEntry.StagedPostHash == Entry.StagedPostHash;
			if (Request.DuplicatePolicy
					== EMutationJournalDuplicatePolicy::ReuseEquivalent
				&& bEquivalent)
			{
				OutEntryIndex = Existing->second;
				return {};
			}
			return Error(EAssetError::AlreadyExists, std::format(
				"Asset mutation participants claim the same file {}.", Key));
		}

		std::string PathError;
		const FMountPoint* Mount = nullptr;
		if (!IsWritableRelocationPath(Normalized, Mount, PathError))
			return Error(EAssetError::ReadOnlyMode, std::move(PathError));
		if (Request.bPreExists && IsReadOnlyMutationInput(Normalized))
			return Error(EAssetError::ReadOnlyMode, std::format(
				"Asset mutation input is read-only: {}.", Key));
		if (Request.bPreExists)
		{
			FAssetResult Result = MakePackageFingerprint(
				Key, Request.PreBytes, Entry.ExpectedPreFingerprint);
			if (!Result) return Result;
		}

		const size_t Index = Journal.Entries.size();
		const std::filesystem::path Root =
			NormalizePhysicalPath(Mount->GetContentDir())
			/ ".durin-asset-mutation"
			/ std::format("operation-{}", Journal.OperationId);
		bool bCreatedRoot = false;
		if (std::ranges::find(Journal.Roots, Root) == Journal.Roots.end())
		{
			std::error_code DirectoryError;
			bCreatedRoot = std::filesystem::create_directories(
				Root, DirectoryError);
			if (DirectoryError)
				return Error(EAssetError::IoError, std::format(
					"Could not create asset mutation staging root: {}",
					DirectoryError.message()));
			if (!bCreatedRoot)
				return Error(EAssetError::AlreadyExists, std::format(
					"Asset mutation staging root already exists: {}.",
					Root.generic_string()));
			const std::string Marker =
				MakeMutationJournalOwnerMarker(Journal.OperationId);
			FAssetResult MarkerResult = SaveRelocationBytes(
				Root / "owner", std::as_bytes(std::span(Marker)));
			if (!MarkerResult)
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Root, CleanupError);
				return MarkerResult;
			}
			Journal.Roots.push_back(Root);
		}

		Entry.StagedPrePath = Root / std::format("pre-{:08}", Index);
		Entry.StagedPostPath = Root / std::format("post-{:08}", Index);
		auto CleanupStagedEntry = [&] {
			std::error_code CleanupError;
			std::filesystem::remove(Entry.StagedPrePath, CleanupError);
			CleanupError.clear();
			std::filesystem::remove(Entry.StagedPostPath, CleanupError);
			if (!bCreatedRoot) return;
			CleanupError.clear();
			std::filesystem::remove_all(Root, CleanupError);
			if (!CleanupError) std::erase(Journal.Roots, Root);
		};
		if (Request.bPreExists)
		{
			FAssetResult Result = SaveRelocationBytes(
				Entry.StagedPrePath, Request.PreBytes);
			if (!Result)
			{
				CleanupStagedEntry();
				return Result;
			}
		}
		if (Request.bPostExists)
		{
			FAssetResult Result = SaveRelocationBytes(
				Entry.StagedPostPath, Request.PostBytes);
			if (!Result)
			{
				CleanupStagedEntry();
				return Result;
			}
		}
		Journal.Entries.push_back(std::move(Entry));
		Journal.EntryIndices.emplace(Key, Index);
		OutEntryIndex = Index;
		return {};
	}

	auto WriteMutationJournalState(FAssetMutationJournal& Journal) -> FAssetResult
	{
		std::string Text = std::format(
			"version=2\noperation={}\ntype={}\nstate={}\nentries={}\n",
			Journal.OperationId,
			Journal.OperationType,
			static_cast<uint32>(Journal.State),
			Journal.Entries.size()
		);
		for (size_t Index = 0; Index < Journal.Entries.size(); ++Index)
		{
			const FAssetMutationJournalEntry& Entry = Journal.Entries[Index];
			Text += std::format(
				"entry.{}.role={}\n"
				"entry.{}.order={}\n"
				"entry.{}.registry={}\n"
				"entry.{}.original={}\n"
				"entry.{}.staged_pre={}\n"
				"entry.{}.staged_post={}\n"
				"entry.{}.published={}\n"
				"entry.{}.pre_exists={}\n"
				"entry.{}.post_exists={}\n"
				"entry.{}.pre_fingerprint={}:{}:{}\n"
				"entry.{}.post_fingerprint={}:{}:{}\n"
				"entry.{}.staged_pre_hash={}\n"
				"entry.{}.staged_post_hash={}\n"
				"entry.{}.completed={}\n",
				Index, static_cast<uint32>(Entry.Role),
				Index, Entry.PublicationOrder,
				Index, Entry.RegistryPath.ToString(),
				Index, Entry.PhysicalPath.generic_string(),
				Index, Entry.StagedPrePath.generic_string(),
				Index, Entry.StagedPostPath.generic_string(),
				Index, Entry.PhysicalPath.generic_string(),
				Index, Entry.bPreExists,
				Index, Entry.bPostExists,
				Index, Entry.ExpectedPreFingerprint.FileSize,
				Entry.ExpectedPreFingerprint.LastWriteTimeTicks,
				Entry.ExpectedPreFingerprint.ContentHash.ToString(),
				Index, Entry.ExpectedPostFingerprint.FileSize,
				Entry.ExpectedPostFingerprint.LastWriteTimeTicks,
				Entry.ExpectedPostFingerprint.ContentHash.ToString(),
				Index, Entry.StagedPreHash.ToString(),
				Index, Entry.StagedPostHash.ToString(),
				Index, Entry.bCompleted);
		}
		Text += std::format(
			"external_participants={}\n",
			Journal.ExternalParticipants.size()
		);
		for (size_t Index = 0;
			 Index < Journal.ExternalParticipants.size(); ++Index)
		{
			const FAssetMutationExternalParticipant& Participant =
				Journal.ExternalParticipants[Index];
			Text += std::format(
				"external.{}.provider={}\n"
				"external.{}.fingerprint={}\n"
				"external.{}.completed={}\n"
				"external.{}.rewrites={}\n",
				Index, Participant.ProviderId,
				Index, Participant.ExpectedFingerprint,
				Index, Participant.bCompleted,
				Index, Participant.Rewrites.size()
			);
			for (size_t RewriteIndex = 0;
				 RewriteIndex < Participant.Rewrites.size(); ++RewriteIndex)
			{
				const FAssetReferenceRewrite& Rewrite =
					Participant.Rewrites[RewriteIndex];
				Text += std::format(
					"external.{}.rewrite.{}.stable={}\n"
					"external.{}.rewrite.{}.source={}\n"
					"external.{}.rewrite.{}.destination={}\n",
					Index, RewriteIndex, Rewrite.StableId,
					Index, RewriteIndex, Rewrite.SourcePath.ToString(),
					Index, RewriteIndex, Rewrite.DestinationPath.ToString()
				);
			}
		}
		const auto Bytes = std::as_bytes(std::span(Text));
		for (const std::filesystem::path& Root : Journal.Roots)
		{
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
					Bytes, Root / "journal", &PublicationError))
				return Error(EAssetError::IoError, std::format(
					"Could not persist asset mutation journal: {}",
					PublicationError.ToString()));
		}

		std::string Locator = std::format(
			"version=1\noperation={}\nroots={}\n",
			Journal.OperationId, Journal.Roots.size());
		for (const std::filesystem::path& Root : Journal.Roots)
			Locator += std::format("root={}\n", Root.generic_string());
		std::error_code DirectoryError;
		std::filesystem::create_directories(
			Journal.LocatorPath.parent_path(), DirectoryError);
		if (DirectoryError)
			return Error(EAssetError::IoError, std::format(
				"Could not create asset mutation recovery locator directory {}: {}",
				Journal.LocatorPath.parent_path().generic_string(),
				DirectoryError.message()));
		const auto LocatorBytes = std::as_bytes(std::span(Locator));
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
				LocatorBytes, Journal.LocatorPath, &PublicationError))
			return Error(EAssetError::IoError, std::format(
				"Could not persist asset mutation recovery locator: {}",
				PublicationError.ToString()));
		return {};
	}

	auto TransitionMutationJournalState(
		FAssetMutationJournal& Journal,
		EAssetMutationState State) -> FAssetResult
	{
		const EAssetMutationState PreviousState = Journal.State;
		Journal.State = State;
		FAssetResult Result = WriteMutationJournalState(Journal);
		if (!Result) Journal.State = PreviousState;
		return Result;
	}

	auto IsMutationJournalRecoveryRequired(
		const FAssetMutationJournal& Journal) -> bool
	{
		return Journal.State == EAssetMutationState::RecoveryRequired;
	}

	namespace
	{
		struct FMutationRecoveryFailureInjection
		{
			std::map<EAssetMutationRecoveryFailurePoint, uint32>
				RemainingOccurrences;
		};

		auto GetMutationRecoveryFailureInjection()
			-> FMutationRecoveryFailureInjection&
		{
			static FMutationRecoveryFailureInjection Injection;
			return Injection;
		}

		auto ConsumeMutationRecoveryFailure(
			EAssetMutationRecoveryFailurePoint Point
		) -> bool
		{
			auto& Remaining =
				GetMutationRecoveryFailureInjection().RemainingOccurrences;
			auto Injected = Remaining.find(Point);
			if (Injected == Remaining.end() || Injected->second == 0)
				return false;
			if (--Injected->second != 0) return false;
			Remaining.erase(Injected);
			return true;
		}

		using FJournalFields =
			std::unordered_map<std::string, std::vector<std::string>>;

		auto LoadTextFile(
			const std::filesystem::path& Path,
			std::string& OutText
		) -> FAssetResult
		{
			FByteArray Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path))
				return Error(EAssetError::IoError, std::format("Could not read asset mutation recovery record {}.", Path.generic_string()));
			OutText.assign(
				reinterpret_cast<const char*>(Bytes.data()), Bytes.size()
			);
			return {};
		}

		auto ParseJournalFields(
			std::string_view Text,
			FJournalFields& OutFields
		) -> FAssetResult
		{
			OutFields.clear();
			while (!Text.empty())
			{
				const size_t End = Text.find('\n');
				std::string_view Line = Text.substr(0, End);
				Text = End == std::string_view::npos ? std::string_view{} : Text.substr(End + 1);
				if (Line.empty()) continue;
				const size_t Separator = Line.find('=');
				if (Separator == std::string_view::npos
					|| Separator == 0)
					return Error(EAssetError::CorruptFile, "An asset mutation recovery record contains a malformed field.");
				OutFields[std::string(Line.substr(0, Separator))]
					.emplace_back(Line.substr(Separator + 1));
			}
			return {};
		}

		auto ReadSingleField(
			const FJournalFields& Fields,
			std::string_view Name,
			std::string_view& OutValue
		) -> bool
		{
			const auto Found = Fields.find(std::string(Name));
			if (Found == Fields.end() || Found->second.size() != 1)
				return false;
			OutValue = Found->second.front();
			return true;
		}

		template<typename TValue>
		auto ParseUnsigned(std::string_view Text, TValue& OutValue) -> bool
		{
			static_assert(std::is_unsigned_v<TValue>);
			TValue Parsed = 0;
			const auto [End, ErrorCode] = std::from_chars(
				Text.data(), Text.data() + Text.size(), Parsed
			);
			if (ErrorCode != std::errc{} || End != Text.data() + Text.size())
				return false;
			OutValue = Parsed;
			return true;
		}

		template<typename TValue>
		auto ParseSigned(std::string_view Text, TValue& OutValue) -> bool
		{
			static_assert(std::is_signed_v<TValue>);
			TValue Parsed = 0;
			const auto [End, ErrorCode] = std::from_chars(
				Text.data(), Text.data() + Text.size(), Parsed
			);
			if (ErrorCode != std::errc{} || End != Text.data() + Text.size())
				return false;
			OutValue = Parsed;
			return true;
		}

		auto ReadUnsignedField(
			const FJournalFields& Fields,
			std::string_view Name,
			uint64& OutValue
		) -> bool
		{
			std::string_view Text;
			return ReadSingleField(Fields, Name, Text)
				   && ParseUnsigned(Text, OutValue);
		}

		auto ReadBoolField(
			const FJournalFields& Fields,
			std::string_view Name,
			bool& OutValue
		) -> bool
		{
			std::string_view Text;
			if (!ReadSingleField(Fields, Name, Text)) return false;
			if (Text == "true" || Text == "1")
			{
				OutValue = true;
				return true;
			}
			if (Text == "false" || Text == "0")
			{
				OutValue = false;
				return true;
			}
			return false;
		}

		auto ParseFingerprint(
			std::string_view Text,
			FAssetPackageFingerprint& OutFingerprint
		) -> bool
		{
			const size_t First = Text.find(':');
			const size_t Second = First == std::string_view::npos ? std::string_view::npos : Text.find(':', First + 1);
			if (First == std::string_view::npos
				|| Second == std::string_view::npos
				|| Text.find(':', Second + 1) != std::string_view::npos)
				return false;
			uint64 Size = 0;
			int64 Ticks = 0;
			const std::string_view Hash = Text.substr(Second + 1);
			if (!ParseUnsigned(Text.substr(0, First), Size)
				|| !ParseSigned(Text.substr(First + 1, Second - First - 1), Ticks)
				|| Hash.size() != 32)
				return false;
			OutFingerprint.FileSize = Size;
			OutFingerprint.LastWriteTimeTicks = Ticks;
			OutFingerprint.ContentHash = FXxHash128::FromString(Hash);
			return true;
		}

		auto ReadPackagePathField(
			const FJournalFields& Fields,
			std::string_view Name,
			FPackagePath& OutPath,
			bool bAllowEmpty = false
		) -> bool
		{
			std::string_view Text;
			if (!ReadSingleField(Fields, Name, Text)) return false;
			if (Text.empty() && bAllowEmpty)
			{
				OutPath = {};
				return true;
			}
			return FPackagePath::TryCreate(Text, OutPath);
		}

		auto ParseMutationJournal(
			std::string_view Text,
			const std::filesystem::path& LocatorPath,
			std::vector<std::filesystem::path> Roots,
			std::unique_ptr<FAssetMutationJournal>& OutJournal
		) -> FAssetResult
		{
			OutJournal.reset();
			FJournalFields Fields;
			FAssetResult Result = ParseJournalFields(Text, Fields);
			if (!Result) return Result;
			std::string_view Version;
			std::string_view Operation;
			std::string_view Type;
			uint64 StateValue = 0;
			uint64 EntryCount = 0;
			if (!ReadSingleField(Fields, "version", Version)
				|| (Version != "1" && Version != "2")
				|| !ReadSingleField(Fields, "operation", Operation)
				|| Operation.empty()
				|| !ReadSingleField(Fields, "type", Type)
				|| (Type != "relocation" && Type != "fixup")
				|| !ReadUnsignedField(Fields, "state", StateValue)
				|| StateValue > static_cast<uint64>(EAssetMutationState::RecoveryRequired)
				|| !ReadUnsignedField(Fields, "entries", EntryCount)
				|| EntryCount > 65536)
				return Error(EAssetError::CorruptFile, "An asset mutation journal header is invalid.");

			auto Journal = std::make_unique<FAssetMutationJournal>();
			Journal->OperationId = Operation;
			Journal->OperationType = Type;
			Journal->LocatorPath = LocatorPath;
			Journal->Roots = std::move(Roots);
			Journal->State = static_cast<EAssetMutationState>(StateValue);
			Journal->Entries.reserve(static_cast<size_t>(EntryCount));
			for (size_t Index = 0; Index < EntryCount; ++Index)
			{
				const auto Field = [Index](std::string_view Suffix) {
					return std::format("entry.{}.{}", Index, Suffix);
				};
				FAssetMutationJournalEntry Entry;
				uint64 Role = 0;
				uint64 Order = 0;
				std::string_view PhysicalPath;
				std::string_view StagedPrePath;
				std::string_view StagedPostPath;
				std::string_view PreFingerprint;
				std::string_view PostFingerprint;
				std::string_view PreHash;
				std::string_view PostHash;
				if (!ReadUnsignedField(Fields, Field("role"), Role)
					|| Role > static_cast<uint64>(EAssetMutationPublicationRole::Redirector)
					|| !ReadUnsignedField(Fields, Field("order"), Order)
					|| !ReadPackagePathField(
						Fields, Field("registry"), Entry.RegistryPath, true
					)
					|| !ReadSingleField(Fields, Field("original"), PhysicalPath)
					|| !ReadSingleField(Fields, Field("staged_pre"), StagedPrePath)
					|| !ReadSingleField(Fields, Field("staged_post"), StagedPostPath)
					|| !ReadBoolField(Fields, Field("pre_exists"), Entry.bPreExists)
					|| !ReadBoolField(Fields, Field("post_exists"), Entry.bPostExists)
					|| !ReadSingleField(Fields, Field("pre_fingerprint"), PreFingerprint)
					|| !ParseFingerprint(PreFingerprint, Entry.ExpectedPreFingerprint)
					|| !ReadSingleField(Fields, Field("post_fingerprint"), PostFingerprint)
					|| !ParseFingerprint(PostFingerprint, Entry.ExpectedPostFingerprint)
					|| !ReadSingleField(Fields, Field("staged_pre_hash"), PreHash)
					|| PreHash.size() != 32
					|| !ReadSingleField(Fields, Field("staged_post_hash"), PostHash)
					|| PostHash.size() != 32
					|| !ReadBoolField(Fields, Field("completed"), Entry.bCompleted))
					return Error(EAssetError::CorruptFile, "An asset mutation journal entry is invalid.");
				Entry.Role = static_cast<EAssetMutationPublicationRole>(Role);
				Entry.PublicationOrder = Order;
				Entry.PhysicalPath = NormalizePhysicalPath(PhysicalPath);
				Entry.StagedPrePath = NormalizePhysicalPath(StagedPrePath);
				Entry.StagedPostPath = NormalizePhysicalPath(StagedPostPath);
				Entry.StagedPreHash = FXxHash128::FromString(PreHash);
				Entry.StagedPostHash = FXxHash128::FromString(PostHash);
				const std::string Key = Entry.PhysicalPath.generic_string();
				if (!Journal->EntryIndices.emplace(Key, Index).second)
					return Error(EAssetError::CorruptFile, "An asset mutation journal repeats a physical participant.");
				bool bOwnedStaging = false;
				for (const std::filesystem::path& Root : Journal->Roots)
				{
					if (FPaths::IsLexicalDescendantPath(
							Entry.StagedPrePath.generic_string(),
							Root.generic_string(), true
						)
						&& FPaths::IsLexicalDescendantPath(
							Entry.StagedPostPath.generic_string(),
							Root.generic_string(), true
						))
					{
						bOwnedStaging = true;
						break;
					}
				}
				const FMountPoint* Mount = nullptr;
				std::string PathError;
				if (!bOwnedStaging
					|| !IsWritableRelocationPath(
						Entry.PhysicalPath, Mount, PathError
					))
					return Error(EAssetError::CorruptFile, "An asset mutation journal contains an unsafe participant path.");
				Journal->Entries.push_back(std::move(Entry));
			}

			uint64 ExternalCount = 0;
			if ((Version == "2" && !ReadUnsignedField(Fields, "external_participants", ExternalCount))
				|| ExternalCount > 4096)
				return Error(EAssetError::CorruptFile, "An asset mutation journal has an invalid external participant count.");
			if (Version == "1" && Type == "fixup")
				return Error(EAssetError::StaleData, "A legacy Fix Up recovery record lacks durable external-participant descriptors and requires manual review.");
			Journal->ExternalParticipants.reserve(
				static_cast<size_t>(ExternalCount)
			);
			for (size_t Index = 0; Index < ExternalCount; ++Index)
			{
				const auto Field = [Index](std::string_view Suffix) {
					return std::format("external.{}.{}", Index, Suffix);
				};
				FAssetMutationExternalParticipant Participant;
				std::string_view Provider;
				std::string_view Fingerprint;
				uint64 RewriteCount = 0;
				if (!ReadSingleField(Fields, Field("provider"), Provider)
					|| Provider.empty()
					|| !ReadSingleField(Fields, Field("fingerprint"), Fingerprint)
					|| Fingerprint.empty()
					|| !ReadBoolField(Fields, Field("completed"), Participant.bCompleted)
					|| !ReadUnsignedField(Fields, Field("rewrites"), RewriteCount)
					|| RewriteCount == 0 || RewriteCount > 65536)
					return Error(EAssetError::CorruptFile, "An external mutation participant is invalid.");
				Participant.ProviderId = Provider;
				Participant.ExpectedFingerprint = Fingerprint;
				Participant.Rewrites.reserve(static_cast<size_t>(RewriteCount));
				for (size_t RewriteIndex = 0;
					 RewriteIndex < RewriteCount; ++RewriteIndex)
				{
					const auto RewriteField =
						[Index, RewriteIndex](std::string_view Suffix) {
							return std::format(
								"external.{}.rewrite.{}.{}",
								Index, RewriteIndex, Suffix
							);
						};
					FAssetReferenceRewrite Rewrite;
					std::string_view StableId;
					if (!ReadSingleField(
							Fields, RewriteField("stable"), StableId
						)
						|| StableId.empty()
						|| !ReadPackagePathField(
							Fields, RewriteField("source"), Rewrite.SourcePath
						)
						|| !ReadPackagePathField(
							Fields, RewriteField("destination"), Rewrite.DestinationPath
						))
						return Error(EAssetError::CorruptFile, "An external mutation rewrite is invalid.");
					Rewrite.StableId = StableId;
					Participant.Rewrites.push_back(std::move(Rewrite));
				}
				Journal->ExternalParticipants.push_back(std::move(Participant));
			}
			OutJournal = std::move(Journal);
			return {};
		}

		auto LoadMutationJournalFromLocator(
			const std::filesystem::path& LocatorPath,
			std::unique_ptr<FAssetMutationJournal>& OutJournal
		) -> FAssetResult
		{
			std::string LocatorText;
			FAssetResult Result = LoadTextFile(LocatorPath, LocatorText);
			if (!Result) return Result;
			FJournalFields Fields;
			Result = ParseJournalFields(LocatorText, Fields);
			if (!Result) return Result;
			std::string_view Version;
			std::string_view Operation;
			uint64 RootCount = 0;
			const auto RootsField = Fields.find("root");
			if (!ReadSingleField(Fields, "version", Version) || Version != "1"
				|| !ReadSingleField(Fields, "operation", Operation)
				|| !ReadUnsignedField(Fields, "roots", RootCount)
				|| RootCount == 0 || RootCount > 128
				|| RootsField == Fields.end()
				|| RootsField->second.size() != RootCount
				|| LocatorPath.filename()
					   != std::format("operation-{}", Operation))
				return Error(EAssetError::CorruptFile, "An asset mutation recovery locator is invalid.");
			const auto IsNativeTestSandboxPath = [](
													 const std::filesystem::path& Path
												 ) {
				const std::string Text =
					NormalizePhysicalPath(Path).generic_string();
				return Text.find("/Tests/") != std::string::npos
					   && Text.find("/Work/Runs/run-p") != std::string::npos;
			};
			if (!IsNativeTestSandboxPath(LocatorPath)
				&& std::ranges::all_of(
					RootsField->second,
					[&](const std::string& Root) {
						return IsNativeTestSandboxPath(Root);
					}
				))
			{
				// Older native tests wrote locators into the shared project Saved
				// directory even though every authoritative participant lived in
				// an isolated, disposable process sandbox. They are never project
				// recovery authority and must not block authored runtime startup.
				return {};
			}

			std::vector<std::filesystem::path> Roots;
			std::string JournalText;
			for (const std::string& RootText : RootsField->second)
			{
				const std::filesystem::path Root =
					NormalizePhysicalPath(RootText);
				if (Root.filename() != std::format("operation-{}", Operation)
					|| Root.parent_path().filename() != ".durin-asset-mutation")
					return Error(EAssetError::CorruptFile, "An asset mutation recovery root is not owned by the operation.");
				std::string Owner;
				Result = LoadTextFile(Root / "owner", Owner);
				if (!Result) return Result;
				if (Owner != MakeMutationJournalOwnerMarker(Operation))
					return Error(EAssetError::CorruptFile, "An asset mutation recovery owner marker is invalid.");
				std::string Candidate;
				Result = LoadTextFile(Root / "journal", Candidate);
				if (!Result) return Result;
				if (JournalText.empty())
					JournalText = std::move(Candidate);
				else if (JournalText != Candidate)
					return Error(EAssetError::CorruptFile, "Asset mutation journal replicas disagree.");
				Roots.push_back(Root);
			}
			return ParseMutationJournal(
				JournalText, LocatorPath, std::move(Roots), OutJournal
			);
		}

		auto CurrentFileMatches(
			const FAssetMutationJournalEntry& Entry,
			bool bPost
		) -> bool
		{
			const bool bExpectedExists =
				bPost ? Entry.bPostExists : Entry.bPreExists;
			std::error_code ExistsError;
			const bool bExists = std::filesystem::exists(
				Entry.PhysicalPath, ExistsError
			);
			if (ExistsError || bExists != bExpectedExists) return false;
			if (!bExpectedExists) return true;
			FByteArray Bytes;
			if (!LoadRelocationBytes(Entry.PhysicalPath, Bytes)) return false;
			const FXxHash128 Expected =
				bPost ? Entry.StagedPostHash : Entry.StagedPreHash;
			return FXxHash128::HashBuffer(Bytes) == Expected;
		}

		auto MakeRecoveryPending(
			const FAssetMutationJournal& Journal,
			std::string Message
		) -> FAssetResult
		{
			return {
				.Error = EAssetError::IoError,
				.Message = std::move(Message),
				.Disposition = EAssetResultDisposition::ForwardPending,
				.OperationId = Journal.OperationId,
				.DesiredDirection = "Forward",
				.RecoveryLocation = Journal.LocatorPath
			};
		}

		auto MakeRecoveryRequired(
			FAssetMutationJournal& Journal,
			std::string FailedParticipant,
			std::string Message
		) -> FAssetResult
		{
			const FAssetResult Persist = TransitionMutationJournalState(
				Journal, EAssetMutationState::RecoveryRequired
			);
			if (!Persist)
				Message += std::format(
					" Additionally failed to persist recovery state: {}",
					Persist.Message
				);
			return {
				.Error = EAssetError::IoError,
				.Message = std::move(Message),
				.Disposition = EAssetResultDisposition::RecoveryRequired,
				.OperationId = Journal.OperationId,
				.DesiredDirection = "Forward",
				.FailedParticipant = std::move(FailedParticipant),
				.RecoveryLocation = Journal.LocatorPath
			};
		}

		auto PersistRecoveredProgress(FAssetMutationJournal& Journal)
			-> FAssetResult
		{
			FAssetResult Result = WriteMutationJournalState(Journal);
			if (!Result) return MakeRecoveryRequired(
				Journal, "MutationJournal", Result.Message
			);
			if (ConsumeMutationRecoveryFailure(
					EAssetMutationRecoveryFailurePoint::AfterProgressPersistence
				))
				return MakeRecoveryPending(
					Journal,
					"Injected interruption after durable recovery progress persistence."
				);
			return {};
		}

		auto RecoverFileParticipant(
			FAssetMutationJournal& Journal,
			FAssetMutationJournalEntry& Entry
		) -> FAssetResult
		{
			if (Entry.bCompleted)
			{
				if (CurrentFileMatches(Entry, true)) return {};
				return MakeRecoveryRequired(
					Journal, Entry.PhysicalPath.generic_string(),
					"A completed asset mutation participant no longer matches its committed output."
				);
			}
			if (CurrentFileMatches(Entry, true))
			{
				if (Entry.bPostExists)
				{
					FAssetResult Result = FingerprintRelocationFile(
						Entry.PhysicalPath, Entry.ExpectedPostFingerprint
					);
					if (!Result) return MakeRecoveryRequired(
						Journal, Entry.PhysicalPath.generic_string(), Result.Message
					);
				}
				Entry.bCompleted = true;
				return PersistRecoveredProgress(Journal);
			}
			if (!CurrentFileMatches(Entry, false))
				return MakeRecoveryRequired(
					Journal, Entry.PhysicalPath.generic_string(),
					"An asset mutation participant matches neither its recorded input nor output."
				);
			FAssetResult Result = PublishRelocationFile(Entry);
			if (!Result) return MakeRecoveryPending(Journal, Result.Message);
			if (ConsumeMutationRecoveryFailure(
					EAssetMutationRecoveryFailurePoint::AfterParticipantPublication
				))
				return MakeRecoveryPending(
					Journal,
					"Injected interruption after asset mutation participant publication."
				);
			if (Entry.bPostExists)
			{
				Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPostFingerprint
				);
				if (!Result) return MakeRecoveryRequired(
					Journal, Entry.PhysicalPath.generic_string(), Result.Message
				);
			}
			Entry.bCompleted = true;
			return PersistRecoveredProgress(Journal);
		}

		auto RecoverExternalParticipant(
			FAssetMutationJournal& Journal,
			FAssetMutationExternalParticipant& Participant
		) -> FAssetResult
		{
			if (Participant.bCompleted) return {};
			auto& StoreRegistry = GetAssetReferenceStoreRegistry();
			IAssetReferenceStore* Store = nullptr;
			for (const auto& [Handle, Candidate] : StoreRegistry.Stores)
			{
				(void)Handle;
				if (!Candidate) continue;
				FAssetReferenceStoreSnapshot Snapshot;
				if (!Candidate->CaptureSnapshot(Snapshot)) continue;
				if (Snapshot.ProviderId != Participant.ProviderId) continue;
				if (Store)
					return MakeRecoveryRequired(
						Journal, Participant.ProviderId,
						"Multiple external reference stores claim one recovery provider id."
					);
				Store = Candidate;
			}
			if (!Store)
				return MakeRecoveryPending(
					Journal, std::format(
								 "External recovery provider {} is not registered.",
								 Participant.ProviderId
							 )
				);
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetResult Result = Store->CaptureSnapshot(Snapshot);
			if (!Result) return MakeRecoveryPending(Journal, Result.Message);
			std::vector<FAssetReferenceRewrite> Pending;
			for (const FAssetReferenceRewrite& Rewrite : Participant.Rewrites)
			{
				const auto Occurrence = std::ranges::find(
					Snapshot.Occurrences, Rewrite.StableId,
					&FAssetReferenceStoreOccurrence::StableId
				);
				if (Occurrence == Snapshot.Occurrences.end())
					return MakeRecoveryRequired(
						Journal, Participant.ProviderId,
						"An external recovery occurrence is missing."
					);
				if (Occurrence->TargetPath == Rewrite.DestinationPath) continue;
				if (Occurrence->TargetPath != Rewrite.SourcePath)
					return MakeRecoveryRequired(
						Journal, Participant.ProviderId,
						"An external recovery occurrence has an unexpected target."
					);
				Pending.push_back(Rewrite);
			}
			if (!Pending.empty())
			{
				FAssetReferenceStoreRewriteContribution Contribution;
				Result = Store->PrepareRewrite(
					Pending, Snapshot.Fingerprint, Contribution
				);
				if (!Result) return MakeRecoveryPending(Journal, Result.Message);
				if (Contribution.Fingerprint != Snapshot.Fingerprint
					|| Contribution.Rewrites != Pending
					|| !Contribution.Revalidate || !Contribution.Apply
					|| !Contribution.Verify)
					return MakeRecoveryRequired(
						Journal, Participant.ProviderId,
						"An external recovery provider returned an incomplete replay contribution."
					);
				Result = Contribution.Revalidate();
				if (!Result) return MakeRecoveryPending(Journal, Result.Message);
				Result = Contribution.Apply();
				if (!Result) return MakeRecoveryPending(Journal, Result.Message);
				if (ConsumeMutationRecoveryFailure(
						EAssetMutationRecoveryFailurePoint::AfterParticipantPublication
					))
					return MakeRecoveryPending(
						Journal,
						"Injected interruption after external mutation participant publication."
					);
				Result = Contribution.Verify();
				if (!Result) return MakeRecoveryPending(Journal, Result.Message);
			}
			Result = Store->CaptureSnapshot(Snapshot);
			if (!Result) return MakeRecoveryPending(Journal, Result.Message);
			for (const FAssetReferenceRewrite& Rewrite : Participant.Rewrites)
			{
				const auto Occurrence = std::ranges::find(
					Snapshot.Occurrences, Rewrite.StableId,
					&FAssetReferenceStoreOccurrence::StableId
				);
				if (Occurrence == Snapshot.Occurrences.end()
					|| Occurrence->TargetPath != Rewrite.DestinationPath)
					return MakeRecoveryPending(
						Journal,
						"External recovery verification has not converged."
					);
			}
			Participant.ExpectedFingerprint = Snapshot.Fingerprint;
			Participant.bCompleted = true;
			return PersistRecoveredProgress(Journal);
		}

		auto RecoverMutationJournal(
			FAssetMutationJournal& Journal,
			FAssetPublicationCoordinator& Registry
		) -> FAssetResult
		{
			if (Journal.State == EAssetMutationState::Committed) return {};
			if (Journal.State == EAssetMutationState::Planned)
				return MakeRecoveryRequired(
					Journal, "MutationJournal",
					"A durable mutation journal was not fully prepared."
				);
			if (Journal.State != EAssetMutationState::Publishing)
			{
				FAssetResult Result = TransitionMutationJournalState(
					Journal, EAssetMutationState::Publishing
				);
				if (!Result) return MakeRecoveryRequired(
					Journal, "MutationJournal", Result.Message
				);
			}

			std::vector<size_t> Order(Journal.Entries.size());
			for (size_t Index = 0; Index < Order.size(); ++Index)
				Order[Index] = Index;
			std::ranges::stable_sort(Order, [&](size_t Left, size_t Right) {
				return Journal.Entries[Left].Role < Journal.Entries[Right].Role;
			});
			for (const size_t Index : Order)
			{
				FAssetMutationJournalEntry& Entry = Journal.Entries[Index];
				if (Entry.Role == EAssetMutationPublicationRole::Redirector) continue;
				FAssetResult Result = RecoverFileParticipant(Journal, Entry);
				if (!Result) return Result;
			}
			for (FAssetMutationExternalParticipant& Participant :
				 Journal.ExternalParticipants)
			{
				FAssetResult Result = RecoverExternalParticipant(
					Journal, Participant
				);
				if (!Result) return Result;
			}
			for (const size_t Index : Order)
			{
				FAssetMutationJournalEntry& Entry = Journal.Entries[Index];
				if (Entry.Role != EAssetMutationPublicationRole::Redirector) continue;
				FAssetResult Result = RecoverFileParticipant(Journal, Entry);
				if (!Result) return Result;
			}

			std::vector<FPackagePath> Paths;
			for (const FAssetMutationJournalEntry& Entry : Journal.Entries)
				if (Entry.RegistryPath.IsValid()) Paths.push_back(Entry.RegistryPath);
			std::ranges::sort(Paths, [](const FPackagePath& Left, const FPackagePath& Right) {
				return Left.GetView() < Right.GetView();
			});
			Paths.erase(std::ranges::unique(Paths).begin(), Paths.end());
			FenceAssetRegistryProjection(Paths);
			if (ConsumeMutationRecoveryFailure(
					EAssetMutationRecoveryFailurePoint::BeforeProjectionReconcile
				))
				return MakeRecoveryPending(
					Journal,
					"Injected interruption before recovered projection reconciliation."
				);
			FAssetResult Result = Registry.ReconcileProjection(Paths);
			if (!Result) return MakeRecoveryPending(Journal, Result.Message);
			Result = TransitionMutationJournalState(
				Journal, EAssetMutationState::Committed
			);
			if (!Result) return MakeRecoveryRequired(
				Journal, "MutationJournal", Result.Message
			);
			return {};
		}
	} // namespace

	auto RecoverPendingMutationJournals(
		FAssetPublicationCoordinator& Registry
	) -> FAssetResult
	{
		const std::filesystem::path Directory =
			GetMutationRecoveryDirectory();
		std::error_code DirectoryError;
		if (!std::filesystem::exists(Directory, DirectoryError))
			return DirectoryError ? Error(EAssetError::IoError, std::format("Could not inspect asset mutation recovery directory: {}", DirectoryError.message())) : FAssetResult{};
		std::vector<std::filesystem::path> Locators;
		for (const std::filesystem::directory_entry& Entry :
			 std::filesystem::directory_iterator(Directory, DirectoryError))
		{
			if (DirectoryError) break;
			if (Entry.is_regular_file()
				&& Entry.path().filename().generic_string().starts_with("operation-"))
				Locators.push_back(Entry.path());
		}
		if (DirectoryError)
			return Error(EAssetError::IoError, std::format("Could not enumerate asset mutation recovery locators: {}", DirectoryError.message()));
		std::ranges::sort(Locators);
		for (const std::filesystem::path& Locator : Locators)
		{
			std::unique_ptr<FAssetMutationJournal> Journal;
			FAssetResult Result = LoadMutationJournalFromLocator(
				Locator, Journal
			);
			if (!Result) return Result;
			if (!Journal) continue;
			Result = RecoverMutationJournal(*Journal, Registry);
			if (!Result) return Result;
		}
		return {};
	}

	auto PublishRelocationFile(
		const FAssetMutationJournalEntry& Entry) -> FAssetResult
	{
		if (!Entry.bPostExists)
		{
			std::error_code RemoveError;
			if (!std::filesystem::remove(Entry.PhysicalPath, RemoveError)
				&& RemoveError)
				return Error(EAssetError::IoError, std::format(
					"Could not remove relocation input {}: {}",
					Entry.PhysicalPath.generic_string(),
					RemoveError.message()));
			return {};
		}
		FByteArray Bytes;
		FAssetResult Result = LoadRelocationBytes(Entry.StagedPostPath, Bytes);
		if (!Result) return Result;
		std::error_code DirectoryError;
		std::filesystem::create_directories(
			Entry.PhysicalPath.parent_path(), DirectoryError);
		if (DirectoryError)
			return Error(EAssetError::IoError, std::format(
				"Could not create relocation destination directory: {}",
				DirectoryError.message()));
		return SaveRelocationBytes(Entry.PhysicalPath, Bytes);
	}

}

namespace Durin
{
	auto SetAssetMutationRecoveryFailurePointForTesting(
		EAssetMutationRecoveryFailurePoint Point,
		uint32 Occurrence
	) -> void
	{
		auto& Injection =
			AssetPrivate::GetMutationRecoveryFailureInjection();
		if (Point == EAssetMutationRecoveryFailurePoint::None)
		{
			Injection.RemainingOccurrences.clear();
			return;
		}
		Injection.RemainingOccurrences.insert_or_assign(
			Point, std::max(Occurrence, 1u)
		);
	}

	auto SetAssetMutationRecoveryDirectoryForTesting(
		std::filesystem::path Directory
	) -> void
	{
		AssetPrivate::GMutationRecoveryDirectoryOverride =
			std::move(Directory);
	}
} // namespace Durin
