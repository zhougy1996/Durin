#include "DObject/ObjectDiagnostic.h"

namespace Durin
{
	auto FormatObjectError(const FObjectError& Error) -> std::string
	{
		if (!Error.HasError()) return {};
		if (const auto* Code = std::get_if<EObjectPathError>(&Error.Code))
		{
			const char* Part = "Object path";
			switch (Error.Part)
			{
			case EObjectPathPart::Package: Part = "Package path"; break;
			case EObjectPathPart::PackageSegment: Part = "Package path segment"; break;
			case EObjectPathPart::Asset: Part = "Top-level asset path"; break;
			case EObjectPathPart::AssetName: Part = "Top-level asset name"; break;
			case EObjectPathPart::Subobject: Part = "Subobject name"; break;
			case EObjectPathPart::Object: break;
			}
			switch (*Code)
			{
			case EObjectPathError::EmptyComponent: return std::format("{} cannot be empty.", Part);
			case EObjectPathError::ComponentTooLong: return std::format("{} exceeds the {} byte component limit.", Part, Error.MaximumBytes);
			case EObjectPathError::InvalidUtf8: return std::format("{} must be valid UTF-8.", Part);
			case EObjectPathError::ReservedSeparator: return std::format("{} contains a reserved separator.", Part);
			case EObjectPathError::NotAbsolute: return "Package path must be absolute.";
			case EObjectPathError::InternedNameTooLong: return std::format("Package path exceeds the {} byte interned-name limit.", Error.MaximumBytes);
			case EObjectPathError::PathTooLong: return std::format("{} exceeds the {} byte path limit.", Part, Error.MaximumBytes);
			case EObjectPathError::MissingPackageName: return "Package path must name a package.";
			case EObjectPathError::PackageSuffix: return "Package path cannot contain an object or file suffix.";
			case EObjectPathError::WrongDeferredMount: return "Deferred package path must use the /Game mount.";
			case EObjectPathError::MountLookupFailed:
				if (Error.MountError == EMountPathError::UnknownMount)
					return "Virtual path does not use a registered mount.";
				return std::format("Package mount lookup failed for {} (mount error {}).", Error.Subject, static_cast<uint8>(Error.MountError));
			case EObjectPathError::SubobjectSuffix: return "Top-level asset path cannot contain a subobject suffix.";
			case EObjectPathError::AssetSeparator: return "Top-level asset path must contain exactly one asset separator.";
			case EObjectPathError::MissingPackagePath: return "Top-level asset path requires a package path.";
			case EObjectPathError::MultipleSubobjectSeparators: return "Object path can contain at most one subobject separator.";
			case EObjectPathError::EmptySubobject: return "Object path contains an empty subobject name.";
			case EObjectPathError::MissingAssetPath: return "Object path requires a top-level asset path.";
			}
		}
		if (const auto* Code = std::get_if<ESoftObjectError>(&Error.Code))
		{
			switch (*Code)
			{
			case ESoftObjectError::NullLoadedObject: return "A loaded soft-object cache cannot be null.";
			case ESoftObjectError::PackageObject: return "A package cannot be assigned as a soft object.";
			case ESoftObjectError::TransientObject: return "A transient object cannot be assigned as a soft object.";
			case ESoftObjectError::ClassMismatch: return std::format("Object {} is not a {} (actual class {}).", Error.Subject, Error.Expected, Error.Actual);
			case ESoftObjectError::UnpackagedObject: return "An unpackaged object cannot be assigned as a soft object.";
			case ESoftObjectError::LoadedPathMismatch: return "The loaded object does not match the stored soft-object path.";
			case ESoftObjectError::AuthoredPathMismatch: return "The resolved object does not match the authored soft-object path.";
			case ESoftObjectError::ResolvedPathMismatch: return "The resolved object does not match the exact resolved object path.";
			}
		}
		return "Unknown object error.";
	}
}
