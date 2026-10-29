#include "DerivedDataCache/DerivedDataCacheMaintenance.h"
#include "CacheRecordStorage.h"
#include "CacheStorageOwnership.h"
#include "CompressedCacheValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::DerivedData
{
	namespace
	{
		auto Error(ECacheError Code, std::string Message) -> std::unexpected<FCacheError>
		{ return std::unexpected(FCacheError{Code, std::move(Message)}); }
		struct FEntry { std::filesystem::path Path, Relative; uint64 Bytes; bool Directory; };
		struct FInspector
		{
			const FCacheMaintenanceOptions& Options;
			std::filesystem::path Root;
			FCacheMaintenanceReport Report;
			uint32 Entries = 0;
			auto Check() const -> std::expected<void, FCacheError>
			{
				if (Options.ShouldCancel && Options.ShouldCancel()) return Error(ECacheError::Canceled, "Cache maintenance canceled; it may be rerun.");
				return {};
			}
			auto AddDiagnostic(const std::filesystem::path& Path, std::string_view Message) -> void
			{ if (Report.Diagnostics.size() < 32) Report.Diagnostics.push_back(Path.lexically_relative(Root).generic_string() + ": " + std::string(Message)); }
			auto Load(const std::filesystem::path& Path, uint64 RetainedBytes = 0) -> std::expected<FSharedByteBuffer, FCacheError>
			{
				if (auto Checked = Check(); !Checked) return std::unexpected(Checked.error());
				std::error_code Ec; std::filesystem::path Resolved;
				if (!FPaths::TryResolveContainedPath(Path, Root, Resolved, Ec)) return Error(ECacheError::StorageFailure, "Maintenance read escaped its root.");
				const auto Status = std::filesystem::symlink_status(Path, Ec);
				if (Ec || !std::filesystem::is_regular_file(Status)) return Error(ECacheError::StorageFailure, "Maintenance input is not a regular file.");
				if (RetainedBytes > Options.MaximumWorkingSetBytes) return Error(ECacheError::ValueTooLarge, "Verification retained-data limit reached.");
				const auto Budget = std::min({Options.MaximumFileBytes, Options.MaximumReadBytes - Report.ReadBytes, Options.MaximumWorkingSetBytes - RetainedBytes});
				const auto Size = std::filesystem::file_size(Path, Ec);
				if (Ec) return Error(ECacheError::StorageFailure, "Cannot stat maintenance input.");
				if (Size > Budget) return Error(ECacheError::ValueTooLarge, "Verification read/workspace limit reached; increase explicit limits to continue.");
				auto Bytes = FFileHelper::LoadFileToArray(Resolved, {.MaximumBytes = Budget});
				if (!Bytes) return Error(ECacheError::StorageFailure, Bytes.error().ToString());
				Report.ReadBytes += Bytes->size(); return FSharedByteBuffer::Take(std::move(*Bytes));
			}
			auto VerifyBlob(const std::filesystem::path& Path, const FValue* Expected = nullptr, uint64 RetainedBytes = 0) -> std::expected<void, FCacheError>
			{
				auto Bytes = Load(Path, RetainedBytes); if (!Bytes) return std::unexpected(Bytes.error());
				const auto EncodedBytes = Bytes->GetSize();
				if (RetainedBytes > Options.MaximumWorkingSetBytes - EncodedBytes)
					return Error(ECacheError::ValueTooLarge, "Verification retains too much manifest/blob data.");
				auto Blob = FCompressedCacheValue::Load(std::move(*Bytes), Options.MaximumWorkingSetBytes, Options.MaximumFileBytes);
				if (!Blob) return std::unexpected(Blob.error());
				const auto Key = FCacheKey::FromString(FCacheBucket::FromString("Content"), Path.stem().generic_string());
				if (!Key.IsValid() || Path.parent_path().filename() != Key.ToString().substr(0, 2)
					|| Blob->GetRawHash() != Key.GetHash() || (Expected && (Blob->GetRawHash() != Expected->GetRawHash() || Blob->GetRawSize() != Expected->GetRawSize())))
					return Error(ECacheError::Corrupt, "Blob address or manifest identity mismatch.");
				auto Value = Blob->Decompress(Options.MaximumWorkingSetBytes - EncodedBytes - RetainedBytes);
				if (!Value) return std::unexpected(Value.error());
				return Check();
			}
			auto Verify(const FEntry& Entry) -> std::expected<void, FCacheError>
			{
				const auto Relative = Entry.Relative.generic_string();
				const bool Record = Relative.starts_with(std::string(Private::CacheRecordNamespace) + "/");
				const bool Value = Relative.starts_with("values/v1/Content/");
				if ((!Record && !Value) || Entry.Path.extension() != ".bin")
				{ ++Report.UnsupportedFiles; AddDiagnostic(Entry.Path, "Unsupported namespace or file; preserved."); return {}; }
				const uint64 MissingBefore = Report.MissingValues;
				std::expected<void, FCacheError> Valid;
				if (Value) Valid = VerifyBlob(Entry.Path);
				else
				{
					auto Bytes = Load(Entry.Path); if (!Bytes) return std::unexpected(Bytes.error());
					const uint64 ManifestBytes = Bytes->GetSize();
					const auto BucketPath = Entry.Path.parent_path().parent_path().lexically_relative(Root / Private::CacheRecordNamespace).generic_string();
					const auto Key = FCacheKey::FromString(FCacheBucket::FromString(BucketPath), Entry.Path.stem().generic_string());
					if (!Key.IsValid() || Entry.Path.parent_path().filename() != Key.ToString().substr(0, 2))
						Valid = Error(ECacheError::Corrupt, "Manifest address is invalid.");
					else
					{
						auto Manifest = Private::DecodeCacheManifest(Key, std::move(*Bytes), {}, Options.MaximumFileBytes);
						if (!Manifest) Valid = std::unexpected(Manifest.error());
						else for (const auto& Item : Manifest->GetValues())
						{
							if (auto Checked = Check(); !Checked) return std::unexpected(Checked.error());
							const auto Hash = Item.Value.GetRawHash().ToString();
							const auto Path = Root / "values/v1/Content" / Hash.substr(0, 2) / (Hash + ".bin");
							std::error_code Ec;
							if (!std::filesystem::exists(Path, Ec) && !Ec)
							{ ++Report.MissingValues; AddDiagnostic(Entry.Path, "Referenced value is missing."); continue; }
							Valid = VerifyBlob(Path, &Item.Value, ManifestBytes);
							if (!Valid) break;
						}
					}
				}
				if (!Valid)
				{
					if (Valid.error().Code != ECacheError::Corrupt) return std::unexpected(Valid.error());
					++Report.InvalidFiles; AddDiagnostic(Entry.Path, Valid.error().Diagnostic);
				}
				else if (Report.MissingValues == MissingBefore) ++Report.VerifiedFiles;
				return {};
			}
		};
	}
	auto MaintainCacheStorage(const FCacheMaintenanceOptions& Options) -> std::expected<FCacheMaintenanceReport, FCacheError>
	try
	{
		if (Options.Root.empty() || !Options.Root.is_absolute() || Options.Operation > ECacheMaintenanceOperation::Clear
			|| !Options.MaximumEntries || Options.MaximumEntries > 1000000 || !Options.MaximumFileBytes
			|| !Options.MaximumReadBytes || !Options.MaximumWorkingSetBytes)
			return Error(ECacheError::InvalidRequest, "Maintenance requires an absolute root and positive bounded limits.");
		std::error_code Ec;
		const auto Status = std::filesystem::symlink_status(Options.Root, Ec);
		if (Ec || !std::filesystem::is_directory(Status)) return Error(ECacheError::InvalidRequest, "Maintenance root must be a directory, not a link.");
		if (Options.Operation == ECacheMaintenanceOperation::Clear && !std::filesystem::is_regular_file(Options.Root / Private::CacheLockName, Ec))
			return Error(ECacheError::InvalidRequest, "Clear requires an ownership marker from a cache user or prior inspection.");
		auto Lease = Private::OpenCacheStorageLease(Options.Root, true);
		if (!Lease) return std::unexpected(Lease.error());
		FInspector Inspector{Options, (*Lease)->Root};
		std::vector<FEntry> Entries;
		for (std::filesystem::directory_iterator It(Inspector.Root, Ec), End; It != End && !Ec; It.increment(Ec))
		{
			if (++Inspector.Entries > Options.MaximumEntries) return Error(ECacheError::ValueTooLarge, "Maintenance entry limit exceeded before mutation.");
			if (auto Checked = Inspector.Check(); !Checked) return std::unexpected(Checked.error());
			const auto Name = It->path().filename();
			if (Name == Private::CacheLockName) continue;
			if (Name != "records" && Name != "values") { ++Inspector.Report.OtherRootEntries; continue; }
			const auto BranchStatus = It->symlink_status(Ec);
			if (Ec || !std::filesystem::is_directory(BranchStatus)) return Error(ECacheError::StorageFailure, "Cache namespace must be a directory, not a link.");
			Entries.push_back({It->path(), Name, 0, true});
			for (std::filesystem::recursive_directory_iterator Child(It->path(), Ec), ChildEnd; Child != ChildEnd && !Ec; Child.increment(Ec))
			{
				if (++Inspector.Entries > Options.MaximumEntries) return Error(ECacheError::ValueTooLarge, "Maintenance entry limit exceeded before mutation.");
				if (auto Checked = Inspector.Check(); !Checked) return std::unexpected(Checked.error());
				const auto ChildStatus = Child->symlink_status(Ec);
				const bool Directory = std::filesystem::is_directory(ChildStatus);
				if (Ec || (!Directory && !std::filesystem::is_regular_file(ChildStatus))) return Error(ECacheError::StorageFailure, "Cache maintenance refuses links and special files.");
				const auto Size = Directory ? 0 : Child->file_size(Ec);
				if (Ec) break;
				Entries.push_back({Child->path(), Child->path().lexically_relative(Inspector.Root), Size, Directory});
				if (!Directory)
				{
					if (Name == "records") { ++Inspector.Report.RecordFiles; Inspector.Report.RecordBytes += Size; }
					else { ++Inspector.Report.ValueFiles; Inspector.Report.ValueBytes += Size; }
				}
			}
			if (Ec) break;
		}
		if (Ec) return Error(ECacheError::StorageFailure, "Cannot scan cache: " + Ec.message());
		if (Options.Operation == ECacheMaintenanceOperation::Verify)
			for (const auto& Entry : Entries) if (!Entry.Directory)
			{ if (auto Valid = Inspector.Verify(Entry); !Valid) return std::unexpected(Valid.error()); }
		if (Options.Operation == ECacheMaintenanceOperation::Clear)
		{
			// Remove every manifest before any value. Cancellation/crash leaves safe
			// cold misses or retained orphan blobs; the same clear can be rerun.
			std::ranges::sort(Entries, [](const auto& A, const auto& B) {
				const bool ARecord = *A.Relative.begin() == "records", BRecord = *B.Relative.begin() == "records";
				if (ARecord != BRecord) return ARecord;
				if (A.Directory != B.Directory) return !A.Directory;
				return A.Path.native().size() > B.Path.native().size();
			});
			for (const auto& Entry : Entries)
			{
				if (auto Checked = Inspector.Check(); !Checked) return std::unexpected(Checked.error());
				std::filesystem::path Resolved;
				if (!FPaths::TryResolveContainedPath(Entry.Path, Inspector.Root, Resolved, Ec)) return Error(ECacheError::StorageFailure, "Clear path escaped its root.");
				if (!std::filesystem::remove(Entry.Path, Ec) || Ec) return Error(ECacheError::StorageFailure, "Cache clear failed; rerun after fixing filesystem access.");
				if (!Entry.Directory) { ++Inspector.Report.RemovedFiles; Inspector.Report.RemovedBytes += Entry.Bytes; }
			}
		}
		return std::move(Inspector.Report);
	}
	catch (const std::exception& Exception) { return Error(ECacheError::StorageFailure, std::string("Cache maintenance failed: ") + Exception.what()); }
}
