#pragma once

#include "Misc/Guid.h"

namespace Durin
{
	// Records the nonnegative format version used to write data, independent of local support.
	struct FCustomVersion
	{
		FGuid Guid;
		int32 Version = 0;
		auto operator<=>(const FCustomVersion&) const = default;
	};

	// Describes an immutable, process-lifetime local format definition; it is never serialized.
	struct FCustomVersionDefinition
	{
		FGuid Guid;
		int32 CurrentVersion = 0;
		std::string Name;
	};

	// Owns copied definitions for the process lifetime, including after module unload.
	class FCustomVersionRegistry
	{
	public:
		// Thread-safe. Identical registration is idempotent; invalid or conflicting definitions fail.
		CORE_API static auto Register(FCustomVersionDefinition Definition) -> bool;
		CORE_API static auto GetAll() -> std::vector<FCustomVersionDefinition>;
		// Rejects invalid, duplicate, unknown and future records. Old-version migration belongs to serializers.
		CORE_API static auto Validate(std::span<const FCustomVersion> Versions, std::string& Error) -> bool;
	};

	// Registers a module's format definition at startup; conflicts are programming errors.
	class FCustomVersionRegistration
	{
	public:
		CORE_API FCustomVersionRegistration(FGuid Guid, int32 Version, std::string_view Name);
	};
}
