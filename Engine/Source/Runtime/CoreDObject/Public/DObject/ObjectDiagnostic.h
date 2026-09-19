#pragma once

#include "CoreDObjectAPI.h"
#include "Misc/MountPaths.h"
#include <variant>

namespace Durin
{
	enum class EObjectPathError : uint8
	{
		EmptyComponent, ComponentTooLong, InvalidUtf8, ReservedSeparator,
		NotAbsolute, InternedNameTooLong, PathTooLong, MissingPackageName,
		PackageSuffix, MountLookupFailed, SubobjectSuffix,
		AssetSeparator, MissingPackagePath, MultipleSubobjectSeparators,
		EmptySubobject,
	};

	enum class ESoftObjectError : uint8
	{
		NullLoadedObject, PackageObject, TransientObject, ClassMismatch,
		UnpackagedObject, LoadedPathMismatch, AuthoredPathMismatch, ResolvedPathMismatch,
	};

	enum class EObjectPathPart : uint8 { Package, PackageSegment, Asset, AssetName, Object, Subobject };

	// Owns diagnostic identities so a failure may outlive input views and objects.
	// Prose is generated only by FormatObjectError at presentation/legacy boundaries.
	struct FObjectError
	{
		std::variant<std::monostate, EObjectPathError, ESoftObjectError> Code;
		EObjectPathPart Part = EObjectPathPart::Object;
		std::string Subject;
		std::string Expected;
		std::string Actual;
		size_t ActualBytes = 0;
		size_t MaximumBytes = 0;
		size_t ComponentIndex = 0;
		EMountPathError MountError = EMountPathError::None;

		auto HasError() const -> bool { return !std::holds_alternative<std::monostate>(Code); }
	};

	struct FObjectOperationResult
	{
		FObjectError Error;
		auto Succeeded() const -> bool { return !Error.HasError(); }
		explicit operator bool() const { return Succeeded(); }
	};

	COREDOBJECT_API auto FormatObjectError(const FObjectError& Error) -> std::string;
}
