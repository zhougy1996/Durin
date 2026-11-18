#include "Misc/FilePublication.h"
#include "Misc/Assertion.h"

namespace Durin
{
	namespace
	{
		auto Fail(std::optional<FFileSystemError>* OutError, std::string_view Operation,
			const std::error_code& Error, const FFilePath& Path, const FFilePath& Related = {}) -> bool
		{
			if (OutError) *OutError = FFileSystemError{std::format("Failed to {}: {}. Related path: {}.", Operation, FFilePath(Path).generic_string(), FFilePath(Related).generic_string()), Error};
			return false;
		}
	}
	auto FFilePublicationStamp::Inspect(const FFilePath& Path, FFilePublicationStamp& Out,
		std::optional<FFileSystemError>* OutError) -> bool
	{
		Out = {};
		if (OutError) OutError->reset();
		std::error_code Ec;
		FFilePublicationStamp Stamp;
		Stamp.Exists = std::filesystem::exists(Path, Ec);
		if (Ec) return Fail(OutError, "inspect", Ec, Path);
		if (!Stamp.Exists) return true;
		const bool Regular = std::filesystem::is_regular_file(Path, Ec);
		if (Ec || !Regular) return Fail(OutError, "inspect",
			Ec ? Ec : std::make_error_code(std::errc::invalid_argument), Path);
		Stamp.Size = std::filesystem::file_size(Path, Ec);
		if (Ec) return Fail(OutError, "query size", Ec, Path);
		Stamp.Time = std::filesystem::last_write_time(Path, Ec);
		if (Ec) return Fail(OutError, "query modification time", Ec, Path);
		Out = Stamp;
		return true;
	}
	auto FFileReplacement::Publish(std::optional<FFileSystemError>* OutError) -> bool
	{
		require(!bPublished && !bBackedUp);
		if (OutError) OutError->reset();
		std::error_code Ec;
		const bool Occupied = std::filesystem::exists(Backup, Ec);
		if (Ec || Occupied) return Fail(OutError, "inspect",
			Ec ? Ec : std::make_error_code(std::errc::file_exists), Backup);
		FFilePublicationStamp Stamp;
		if (!FFilePublicationStamp::Inspect(Destination, Stamp, OutError)) return false;
		if (Stamp.Exists)
		{
			std::filesystem::rename(Destination, Backup, Ec);
			if (Ec) return Fail(OutError, "rename", Ec, Destination, Backup);
			bBackedUp = true;
		}
		// An empty stage denotes transactional deletion. The owner restores the whole closure on failure.
		if (!Staged.empty()) std::filesystem::rename(Staged, Destination, Ec);
		if (Ec) return Fail(OutError, "rename", Ec, Staged, Destination);
		bPublished = true;
		return true;
	}
	auto FFileReplacement::Rollback(std::optional<FFileSystemError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		std::error_code Ec;
		if (bPublished && !Staged.empty()) std::filesystem::remove(Destination, Ec);
		if (Ec) return Fail(OutError, "remove", Ec, Destination);
		if (bBackedUp) std::filesystem::rename(Backup, Destination, Ec);
		if (Ec) return Fail(OutError, "rename", Ec, Backup, Destination);
		bPublished = bBackedUp = false;
		return true;
	}
	auto FFileReplacement::Finalize(std::optional<FFileSystemError>* OutError) -> bool
	{
		if (OutError) OutError->reset();
		std::error_code Ec;
		if (bBackedUp) std::filesystem::remove(Backup, Ec);
		if (Ec) return Fail(OutError, "remove", Ec, Backup);
		bBackedUp = bPublished = false;
		return true;
	}
}
