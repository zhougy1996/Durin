#include "DerivedDataObjectStore.h"

#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset::Build
{
	namespace
	{
		auto IsRelativeChild(const std::filesystem::path& Root, const std::filesystem::path& Candidate) -> bool
		{
			const std::filesystem::path Relative = Candidate.lexically_relative(Root);
			if (Relative.empty() || Relative.is_absolute()) return false;
			return *Relative.begin() != "..";
		}

		auto IsResolvedChild(
			const std::filesystem::path& Root,
			const std::filesystem::path& Candidate,
			std::error_code& OutError) -> bool
		{
			const std::filesystem::path ResolvedRoot = std::filesystem::weakly_canonical(Root, OutError);
			if (OutError) return false;
			const std::filesystem::path ResolvedCandidate = std::filesystem::weakly_canonical(Candidate, OutError);
			return !OutError && IsRelativeChild(ResolvedRoot, ResolvedCandidate);
		}

		auto IsValidRelativeRoot(const std::filesystem::path& Root) -> bool
		{
			if (Root.empty() || Root.is_absolute() || Root.has_root_path()) return false;
			const std::filesystem::path Normalized = Root.lexically_normal();
			if (Normalized != Root) return false;
			return std::ranges::none_of(Root, [](const std::filesystem::path& Part) {
				return Part.empty() || Part == "." || Part == "..";
			});
		}

		struct FCleanupCandidate
		{
			std::filesystem::path Path;
			std::filesystem::file_time_type LastWriteTime;
			uint64 Size = 0;
		};
	}

	FDerivedDataObjectStore::FDerivedDataObjectStore(
		std::filesystem::path InRelativeRoot,
		uint64 InMaximumObjectBytes,
		uint32 InKeyLength)
		: RelativeRoot(std::move(InRelativeRoot))
		, MaximumObjectBytes(InMaximumObjectBytes)
		, KeyLength(InKeyLength)
		, bValidRoot(IsValidRelativeRoot(RelativeRoot) && MaximumObjectBytes > 0 && KeyLength >= 2)
	{
	}

	auto FDerivedDataObjectStore::GetRoot() const -> std::filesystem::path
	{
		return (std::filesystem::path(FPaths::DerivedDataCacheDir()) / RelativeRoot).lexically_normal();
	}

	auto FDerivedDataObjectStore::IsValidKey(std::string_view Key) const -> bool
	{
		return bValidRoot && Key.size() == KeyLength
			&& std::ranges::all_of(Key, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
	}

	auto FDerivedDataObjectStore::GetObjectPath(
		std::string_view Key,
		std::filesystem::path& OutPath,
		std::string* OutError) const -> bool
	{
		if (!IsValidKey(Key))
		{
			if (OutError) *OutError = "Derived-data object key is not canonical lowercase hexadecimal.";
			return false;
		}
		const std::filesystem::path Root = GetRoot();
		const std::filesystem::path Candidate =
			(Root / std::string(Key.substr(0, 2)) / (std::string(Key) + ".bin")).lexically_normal();
		if (!IsRelativeChild(Root, Candidate))
		{
			if (OutError) *OutError = "Derived-data object path escapes its configured root.";
			return false;
		}
		OutPath = Candidate;
		if (OutError) OutError->clear();
		return true;
	}

	auto FDerivedDataObjectStore::Read(
		std::string_view Key,
		std::vector<uint8>& OutBytes) const -> FDerivedDataObjectReadResult
	{
		std::filesystem::path Path;
		std::string Error;
		if (!GetObjectPath(Key, Path, &Error))
			return {EDerivedDataObjectReadStatus::InvalidKey, std::move(Error)};

		std::error_code ErrorCode;
		const std::filesystem::file_status Status = std::filesystem::symlink_status(Path, ErrorCode);
		if (ErrorCode || !std::filesystem::exists(Status))
			return {EDerivedDataObjectReadStatus::Missing, "Derived-data object is missing."};
		if (!std::filesystem::is_regular_file(Status))
			return {EDerivedDataObjectReadStatus::ReadFailure, "Derived-data object is not a regular file."};
		if (!IsResolvedChild(GetRoot(), Path, ErrorCode))
			return {EDerivedDataObjectReadStatus::ReadFailure, "Derived-data object resolves outside its configured root."};
		const uint64 FileSize = std::filesystem::file_size(Path, ErrorCode);
		if (ErrorCode)
			return {EDerivedDataObjectReadStatus::ReadFailure, "Failed to inspect derived-data object size."};
		if (FileSize > MaximumObjectBytes)
			return {EDerivedDataObjectReadStatus::TooLarge, "Derived-data object exceeds its configured size limit."};

		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			return {EDerivedDataObjectReadStatus::ReadFailure, "Failed to read derived-data object."};
		OutBytes = std::move(Bytes);
		return {EDerivedDataObjectReadStatus::Hit, {}};
	}

	auto FDerivedDataObjectStore::Write(
		std::string_view Key,
		std::span<const uint8> Bytes,
		std::string* OutError) const -> bool
	{
		std::filesystem::path Path;
		if (!GetObjectPath(Key, Path, OutError)) return false;
		if (Bytes.size() > MaximumObjectBytes)
		{
			if (OutError) *OutError = "Derived-data object exceeds its configured size limit.";
			return false;
		}
		std::error_code ErrorCode;
		std::filesystem::create_directories(Path.parent_path(), ErrorCode);
		if (ErrorCode)
		{
			if (OutError) *OutError = std::format(
				"Failed to create derived-data object directory: {}", ErrorCode.message());
			return false;
		}
		if (!IsResolvedChild(GetRoot(), Path, ErrorCode))
		{
			if (OutError) *OutError = ErrorCode
				? std::format("Failed to resolve derived-data object path: {}", ErrorCode.message())
				: "Derived-data object resolves outside its configured root.";
			return false;
		}
		return DerivedDataCache::WriteFileAtomically(Path, Bytes, OutError);
	}

	auto FDerivedDataObjectStore::CleanupToBudget(
		uint64 BudgetBytes,
		uint32 MaximumDeletes) const -> FDerivedDataObjectCleanupResult
	{
		FDerivedDataObjectCleanupResult Result;
		if (!bValidRoot)
		{
			Result.bBudgetSatisfied = false;
			Result.Message = "Derived-data object store root is invalid.";
			return Result;
		}

		const std::filesystem::path Root = GetRoot();
		std::error_code ErrorCode;
		if (!std::filesystem::exists(Root, ErrorCode))
		{
			Result.bBudgetSatisfied = !ErrorCode;
			if (ErrorCode) Result.Message = std::format("Failed to inspect derived-data root: {}", ErrorCode.message());
			return Result;
		}

		std::vector<FCleanupCandidate> Candidates;
		for (std::filesystem::recursive_directory_iterator It(
			Root, std::filesystem::directory_options::skip_permission_denied, ErrorCode), End;
			!ErrorCode && It != End; It.increment(ErrorCode))
		{
			const std::filesystem::file_status Status = It->symlink_status(ErrorCode);
			if (ErrorCode) break;
			if (std::filesystem::is_symlink(Status))
			{
				if (std::filesystem::is_directory(Status)) It.disable_recursion_pending();
				continue;
			}
			if (!std::filesystem::is_regular_file(Status)) continue;

			const std::filesystem::path Path = It->path().lexically_normal();
			const std::filesystem::path Relative = Path.lexically_relative(Root);
			const std::string FileName = Path.filename().generic_string();
			const std::string Key = Path.stem().generic_string();
			const bool bExpectedShape = std::distance(Relative.begin(), Relative.end()) == 2
				&& Relative.begin()->generic_string() == Key.substr(0, std::min<size_t>(2, Key.size()))
				&& Path.extension() == ".bin"
				&& FileName == Key + ".bin"
				&& IsValidKey(Key);
			if (!bExpectedShape || !IsRelativeChild(Root, Path)
				|| !IsResolvedChild(Root, Path, ErrorCode)) continue;

			const uint64 Size = std::filesystem::file_size(Path, ErrorCode);
			if (ErrorCode) break;
			const std::filesystem::file_time_type LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
			if (ErrorCode) break;
			if (Size > std::numeric_limits<uint64>::max() - Result.BytesBefore)
			{
				Result.bBudgetSatisfied = false;
				Result.Message = "Derived-data object byte accounting overflowed.";
				return Result;
			}
			Result.BytesBefore += Size;
			Candidates.push_back({Path, LastWriteTime, Size});
		}
		if (ErrorCode)
		{
			Result.bBudgetSatisfied = false;
			Result.Message = std::format("Failed to enumerate derived-data objects: {}", ErrorCode.message());
			return Result;
		}

		Result.BytesAfter = Result.BytesBefore;
		std::ranges::sort(Candidates, [](const FCleanupCandidate& Left, const FCleanupCandidate& Right) {
			return Left.LastWriteTime < Right.LastWriteTime
				|| (Left.LastWriteTime == Right.LastWriteTime && Left.Path.generic_string() < Right.Path.generic_string());
		});
		for (const FCleanupCandidate& Candidate : Candidates)
		{
			if (Result.BytesAfter <= BudgetBytes || Result.DeletedObjects >= MaximumDeletes) break;
			if (!IsRelativeChild(Root, Candidate.Path))
			{
				Result.bBudgetSatisfied = false;
				Result.Message = "Refused to delete a derived-data object outside its configured root.";
				return Result;
			}
			if (!std::filesystem::remove(Candidate.Path, ErrorCode) || ErrorCode)
			{
				Result.bBudgetSatisfied = false;
				Result.Message = std::format("Failed to delete derived-data object: {}", ErrorCode.message());
				return Result;
			}
			Result.BytesAfter -= Candidate.Size;
			Result.DeletedBytes += Candidate.Size;
			++Result.DeletedObjects;
		}
		Result.bBudgetSatisfied = Result.BytesAfter <= BudgetBytes;
		if (!Result.bBudgetSatisfied)
			Result.Message = "Derived-data cleanup reached its deletion bound before satisfying the disk budget.";
		return Result;
	}
}
