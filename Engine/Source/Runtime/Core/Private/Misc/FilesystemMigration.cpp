#include "Misc/FilesystemMigration.h"

namespace Durin
{
	auto MigrateLegacyFileIfMissing(
		const std::filesystem::path& LegacyPath,
		const std::filesystem::path& DestinationPath,
		std::string* OutWarning) -> bool
	{
		if (OutWarning) OutWarning->clear();
		std::error_code Error;
		const bool bLegacyExists = std::filesystem::exists(LegacyPath, Error);
		if (Error)
		{
			if (OutWarning) *OutWarning = std::format(
				"Could not inspect legacy file '{}': {}", LegacyPath.string(), Error.message());
			return false;
		}
		if (!bLegacyExists) return true;
		if (std::filesystem::exists(DestinationPath, Error)) return !Error;
		if (Error)
		{
			if (OutWarning) *OutWarning = std::format(
				"Could not inspect destination file '{}': {}",
				DestinationPath.string(), Error.message());
			return false;
		}

		std::filesystem::rename(LegacyPath, DestinationPath, Error);
		if (!Error) return true;
		Error.clear();
		std::filesystem::copy_file(
			LegacyPath, DestinationPath, std::filesystem::copy_options::none, Error);
		if (!Error)
		{
			std::error_code RemoveError;
			std::filesystem::remove(LegacyPath, RemoveError);
			if (!RemoveError) return true;
			Error = RemoveError;
		}
		if (OutWarning) *OutWarning = std::format(
			"Could not migrate legacy file '{}' to '{}': {}",
			LegacyPath.string(), DestinationPath.string(), Error.message());
		return false;
	}
}
