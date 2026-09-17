#include "Misc/FilePublication.h"

namespace Durin
{
	auto FFilePublicationStamp::Inspect(const std::filesystem::path& Path, FFilePublicationStamp& Out) -> bool
	{
		Out = {};
		std::error_code Ec;
		Out.Exists = std::filesystem::exists(Path, Ec);
		if (Ec) return false;
		if (!Out.Exists) return true;
		if (!std::filesystem::is_regular_file(Path, Ec) || Ec) return false;
		Out.Size = std::filesystem::file_size(Path, Ec);
		if (Ec) return false;
		Out.Time = std::filesystem::last_write_time(Path, Ec);
		return !Ec;
	}
	auto FFileReplacement::Publish(std::string& Error) -> bool
	{
		if (bPublished || bBackedUp) { Error = "Replacement is already active."; return false; }
		std::error_code Ec;
		if (std::filesystem::exists(Backup, Ec) || Ec)
		{ Error = "Replacement backup is occupied or inaccessible."; return false; }
		FFilePublicationStamp Stamp;
		if (!FFilePublicationStamp::Inspect(Destination, Stamp))
		{ Error = "Replacement destination is unavailable."; return false; }
		if (Stamp.Exists)
		{
			std::filesystem::rename(Destination, Backup, Ec);
			if (Ec) { Error = Ec.message(); return false; }
			bBackedUp = true;
		}
		// An empty stage denotes a transactional deletion (e.g. an obsolete companion).
		if (!Staged.empty()) std::filesystem::rename(Staged, Destination, Ec);
		if (Ec)
		{
			Error = Ec.message();
			std::string RestoreError;
			if (!Rollback(RestoreError)) Error += "; rollback: " + RestoreError;
			return false;
		}
		bPublished = true;
		Error.clear(); return true;
	}
	auto FFileReplacement::Rollback(std::string& Error) -> bool
	{
		std::error_code Ec;
		if (bPublished && !Staged.empty()) std::filesystem::remove(Destination, Ec);
		if (Ec) { Error = Ec.message(); return false; }
		if (bBackedUp) std::filesystem::rename(Backup, Destination, Ec);
		if (Ec) { Error = Ec.message(); return false; }
		bPublished = bBackedUp = false;
		Error.clear(); return true;
	}
	auto FFileReplacement::Finalize(std::string& Error) -> bool
	{
		std::error_code Ec;
		if (bBackedUp) std::filesystem::remove(Backup, Ec);
		if (Ec) { Error = Ec.message(); return false; }
		bBackedUp = bPublished = false;
		Error.clear(); return true;
	}
}
