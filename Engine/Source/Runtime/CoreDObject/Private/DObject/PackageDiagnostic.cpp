#include "DObject/PackageFormat.h"
#include "PackageDiagnosticInternal.h"

namespace Durin::ObjectPackage
{
	auto FormatEnvelopeError(EBinaryEnvelopeError Error) -> std::string
	{
		switch (Error)
		{
		case EBinaryEnvelopeError::None: return {};
		case EBinaryEnvelopeError::InvalidLimits: return "BinaryEnvelopeInvalidLimits: limits must bound a complete preamble and file.";
		case EBinaryEnvelopeError::InvalidFormatIdentity: return "BinaryEnvelopeInvalidFormatIdentity: FormatId must be nonzero.";
		case EBinaryEnvelopeError::UnsupportedFormatVersion: return "BinaryEnvelopeUnsupportedFormatVersion: format version is not supported.";
		case EBinaryEnvelopeError::InvalidExtent: return "BinaryEnvelopeInvalidExtent: declared extents are inconsistent.";
		case EBinaryEnvelopeError::InvalidDescriptor: return "BinaryEnvelopeInvalidDescriptor: descriptor fields or limits are invalid.";
		case EBinaryEnvelopeError::DuplicateFormatIdentity: return "BinaryEnvelopeDuplicateFormatIdentity: FormatId values must be unique.";
		case EBinaryEnvelopeError::DuplicateFormatName: return "BinaryEnvelopeDuplicateFormatName: debug names must be unique.";
		case EBinaryEnvelopeError::Truncated: return "BinaryEnvelopeTruncated: the 64-byte preamble is incomplete.";
		case EBinaryEnvelopeError::InvalidMagic: return "BinaryEnvelopeInvalidMagic: expected DURF.";
		case EBinaryEnvelopeError::UnsupportedHeaderVersion: return "BinaryEnvelopeUnsupportedHeaderVersion: HeaderVersion is not supported.";
		case EBinaryEnvelopeError::InvalidPreambleSize: return "BinaryEnvelopeInvalidPreambleSize: PreambleBytes must equal 64.";
		case EBinaryEnvelopeError::FileSizeMismatch: return "BinaryEnvelopeFileSizeMismatch: FileBytes must equal the physical file size.";
		case EBinaryEnvelopeError::DestinationTooSmall: return "BinaryEnvelopeDestinationTooSmall: destination cannot hold the preamble.";
		case EBinaryEnvelopeError::UnknownFormat: return "BinaryEnvelopeUnknownFormat: FormatId is not registered.";
		case EBinaryEnvelopeError::UnsupportedRequiredFeatures: return "BinaryEnvelopeUnsupportedRequiredFeatures: required feature bits are not supported.";
		case EBinaryEnvelopeError::HeaderHashMismatch: return "BinaryEnvelopeHeaderHashMismatch: front matter integrity check failed.";
		}
		return {};
	}

	auto FormatPackageError(const FPackageWriterResult& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.LogicalPath.empty() ? Error.Message
			: std::format("{} at '{}'", Error.Message, Error.LogicalPath);
		switch (Error.Reason)
		{
		case EPackageWriterReason::IncompleteEncoding: return "Package encoding did not complete.";
		case EPackageWriterReason::None: return {};
		case EPackageWriterReason::RequiredNameEmpty: return "A required package name is empty.";
		case EPackageWriterReason::NameLimit: return "A package name exceeds the format string limit.";
		case EPackageWriterReason::InvalidNameUtf8: return "A package name is not valid UTF-8.";
		case EPackageWriterReason::TypeDepth: return "A serialized type exceeds the format nesting limit.";
		case EPackageWriterReason::InvalidEnumType: return "An enum type has an invalid name, storage kind, or child list.";
		case EPackageWriterReason::InvalidIntrinsicType: return "An intrinsic type has an invalid layout.";
		case EPackageWriterReason::MissingStructName: return "A struct type has no qualified name.";
		case EPackageWriterReason::InvalidFixedArrayType: return "A fixed-array type has an invalid element descriptor or count.";
		case EPackageWriterReason::InvalidArrayType: return "An array type must have one child type.";
		case EPackageWriterReason::InvalidMapType: return "A Map type must have key and value child types.";
		case EPackageWriterReason::ScalarChildren: return "A scalar type cannot have child descriptors.";
		case EPackageWriterReason::UnresolvedTablePath: return "A package table path cannot be resolved.";
		case EPackageWriterReason::ImportIndex: return "An import index is out of range.";
		case EPackageWriterReason::ExportIndex: return "An export index is out of range.";
		case EPackageWriterReason::InvalidStructBaseline: return "A Struct value requests an invalid or unavailable baseline.";
		case EPackageWriterReason::ValueDepth: return "A serialized value exceeds the format nesting limit.";
		case EPackageWriterReason::SignedRange: return "A signed value is out of range for its type.";
		case EPackageWriterReason::UnsignedRange: return "An unsigned value is out of range for its type.";
		case EPackageWriterReason::Float32Width: return "An F32 value contains bits outside its storage width.";
		case EPackageWriterReason::EnumRange: return "An enum value is out of range for its storage type.";
		case EPackageWriterReason::InvalidString: return "A serialized string is invalid or exceeds the format limit.";
		case EPackageWriterReason::IntrinsicComponentCount: return "An intrinsic value has the wrong component count.";
		case EPackageWriterReason::MissingStructSchema: return "Missing Struct schema.";
		case EPackageWriterReason::MissingHardReferenceFields: return "Type-relative Struct must carry hard-reference fields.";
		case EPackageWriterReason::MissingCompleteFields: return "Complete Struct value is missing fields.";
		case EPackageWriterReason::StructDescriptor: return "A struct value does not match its descriptor.";
		case EPackageWriterReason::DuplicateStructField: return "A struct contains duplicate field names.";
		case EPackageWriterReason::StructFieldType: return "Struct field type does not match schema.";
		case EPackageWriterReason::FixedArrayCount: return "A fixed-array value has the wrong element count.";
		case EPackageWriterReason::ArrayLimit: return "An array exceeds the format element limit.";
		case EPackageWriterReason::MapCount: return "A Map has an invalid entry count.";
		case EPackageWriterReason::DuplicateMapKey: return "A Map contains colliding canonical keys.";
		case EPackageWriterReason::BlobLimit: return "A byte blob exceeds the format package limit.";
		case EPackageWriterReason::InvalidBulkDescriptor: return "BulkData requires explicit valid storage, element size, alignment, and extent.";
		case EPackageWriterReason::ReferenceDepth: return "A value exceeds the nesting limit.";
		case EPackageWriterReason::InvalidSoftPath: return "A soft reference is not a canonical complete object path.";
		case EPackageWriterReason::UnsupportedVersion: return "Unsupported writer version.";
		case EPackageWriterReason::TableLimit: return "A package table exceeds the format entry limit.";
		case EPackageWriterReason::InvalidTopLevelAsset: return "A top-level asset record is invalid.";
		case EPackageWriterReason::TopLevelExportMismatch: return "A top-level asset record does not match its package-outer export.";
		case EPackageWriterReason::DuplicateImport: return "Two imports have the same logical identity.";
		case EPackageWriterReason::DuplicateExport: return "Two exports have the same logical identity.";
		case EPackageWriterReason::DuplicateTopLevelAsset: return "A top-level asset record is duplicated.";
		case EPackageWriterReason::MissingTopLevelAsset: return "A package-outer export has no top-level asset record.";
		case EPackageWriterReason::DuplicateSchemaField: return "A schema contains duplicate field names.";
		case EPackageWriterReason::DuplicateSchema: return "Two schemas have the same qualified name.";
		case EPackageWriterReason::TypeTableLimit: return "The canonical type table exceeds the format entry limit.";
		case EPackageWriterReason::CustomVersionLimit: return "The custom version table exceeds the format entry limit.";
		case EPackageWriterReason::InvalidCustomVersion: return "A custom version requires a valid GUID and nonnegative version.";
		case EPackageWriterReason::DuplicateCustomVersion: return "Two custom versions have the same GUID.";
		case EPackageWriterReason::OpaqueProperty: return "An opaque retained property payload cannot be emitted as DAST.";
		case EPackageWriterReason::DuplicateProperty: return "An export contains duplicate property identities.";
		case EPackageWriterReason::MissingPropertySchema: return "A property declaring schema is missing.";
		case EPackageWriterReason::PropertySchemaMismatch: return "A property does not match its frozen schema field.";
		case EPackageWriterReason::NameTableLimit: return "The canonical name table exceeds the format entry limit.";
		case EPackageWriterReason::EmissionDepth: return "A value exceeds the format nesting limit.";
		case EPackageWriterReason::ByteRange: return "A byte value is out of range.";
		case EPackageWriterReason::BulkOrder: return "BulkData discovery and emission order differ.";
		case EPackageWriterReason::ValueSectionLimit: return "A value section exceeded its writer limit.";
		case EPackageWriterReason::BulkAlignmentOverflow: return "BulkData alignment overflowed.";
		case EPackageWriterReason::BulkPaddingLimit: return "BulkData padding exceeds the format limit.";
		case EPackageWriterReason::BulkSegmentLimit: return "BulkData payloads exceed the format segment limit.";
		case EPackageWriterReason::MissingBulkPayload: return "External BulkData payload bytes are unavailable for full emission.";
		case EPackageWriterReason::RecordLimit: return "A package record exceeds the format limit.";
		case EPackageWriterReason::BulkBindingMismatch: return "External BulkData descriptors do not match their package binding.";
		case EPackageWriterReason::SectionLimit: return "A package section exceeds the format limit.";
		case EPackageWriterReason::BulkCount: return "The emitted BulkData count differs from the frozen manifest.";
		case EPackageWriterReason::PackageLimit: return "The assembled package exceeds the format file limit.";
		case EPackageWriterReason::HeaderLimit: return "The discovery header exceeds its limit.";
		case EPackageWriterReason::AliasedOutput: return "The main and bulk output buffers must not alias.";
		case EPackageWriterReason::InvalidBulkBinding: return "External BulkData binding is invalid.";
		case EPackageWriterReason::EnvelopeRejected:
			return "Binary envelope validation failed.";
		case EPackageWriterReason::CanonicalKeyRejected:
			return "Canonical Map key validation failed.";
		}
		return {};
	}

	auto FormatPackageError(const FPackageReaderResult& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.LogicalPath.empty() ? Error.Message
			: std::format("{} at '{}'", Error.Message, Error.LogicalPath);
		switch (Error.Reason)
		{
		case EPackageReaderReason::None: return {};
		case EPackageReaderReason::InvalidLimits: return "DAST reader limits are internally inconsistent or exceed format limits.";
		case EPackageReaderReason::InputExtent: return "DAST input is truncated or exceeds the package limit.";
		case EPackageReaderReason::DeclaredExtent: return "DAST input does not match its declared header or file extent.";
		case EPackageReaderReason::UnsupportedVersion: return "Unsupported package version.";
		case EPackageReaderReason::FormatHeader: return "DAST format header is invalid.";
		case EPackageReaderReason::Directory: return "DAST section directory is invalid.";
		case EPackageReaderReason::SectionOverflow: return "DAST section extent overflows the file.";
		case EPackageReaderReason::SectionHash: return "A DAST section digest does not match.";
		case EPackageReaderReason::SectionCoverage: return "DAST sections or header boundary do not cover their exact declared extents.";
		case EPackageReaderReason::NameTableHeader: return "DAST name table header is invalid.";
		case EPackageReaderReason::NonCanonicalNames: return "DAST names are invalid, duplicate, or out of canonical order.";
		case EPackageReaderReason::NameTableTrailing: return "DAST name table has trailing or malformed bytes.";
		case EPackageReaderReason::ImportTableHeader: return "DAST import table header is invalid.";
		case EPackageReaderReason::ImportRecord: return "A DAST import record is invalid.";
		case EPackageReaderReason::ImportPath: return "A DAST import target is not an exact object path.";
		case EPackageReaderReason::ImportTableTrailing: return "DAST import table has trailing or malformed bytes.";
		case EPackageReaderReason::MissingPackageIdentity: return "The caller-supplied package identity is absent from the canonical name table.";
		case EPackageReaderReason::RegistryIdentity: return "DAST Registry identity fields are invalid.";
		case EPackageReaderReason::TopLevelRecord: return "A DAST top-level asset record is invalid.";
		case EPackageReaderReason::TopLevelPath: return std::format("DAST top-level asset path '{}' is invalid.", Error.Subject);
		case EPackageReaderReason::TopLevelOrder: return "DAST top-level asset paths are duplicate or out of order.";
		case EPackageReaderReason::RedirectPath: return "A DAST redirect destination is invalid.";
		case EPackageReaderReason::RegistryListCount: return "A DAST Registry list count is invalid.";
		case EPackageReaderReason::RegistryListOrder: return "A DAST Registry id list is duplicate or out of order.";
		case EPackageReaderReason::DependencyPath: return "A DAST dependency is not a canonical mounted package path.";
		case EPackageReaderReason::RegistryBulk: return "DAST Registry bulk binding or extent is invalid.";
		case EPackageReaderReason::ExportTableHeader: return "DAST export table header is invalid.";
		case EPackageReaderReason::ExportRecord: return "A DAST export record is invalid.";
		case EPackageReaderReason::ExportTableTrailing: return "DAST export table has trailing or malformed bytes.";
		case EPackageReaderReason::TypeTableHeader: return "DAST type table header is invalid.";
		case EPackageReaderReason::TypeRecordExtent: return "A DAST type record extent is invalid.";
		case EPackageReaderReason::TypeRecordHeader: return "A DAST type record header is invalid.";
		case EPackageReaderReason::ChildTypeIndex: return "A DAST child type id is invalid.";
		case EPackageReaderReason::TypeRecordTrailing: return "A DAST type record has trailing bytes.";
		case EPackageReaderReason::TypeTableTrailing: return "DAST type table has trailing bytes.";
		case EPackageReaderReason::TypeCycle: return "DAST structural type graph contains a cycle.";
		case EPackageReaderReason::SchemaSectionHeader: return "DAST schema section header is invalid.";
		case EPackageReaderReason::CustomVersionRecord: return "A DAST custom-version record is invalid.";
		case EPackageReaderReason::SchemaCount: return "DAST schema count is invalid.";
		case EPackageReaderReason::SchemaHeader: return "A DAST schema header is invalid.";
		case EPackageReaderReason::SchemaField: return "A DAST schema field is invalid.";
		case EPackageReaderReason::SchemaTrailing: return "DAST schema section has trailing bytes.";
		case EPackageReaderReason::ValueDepth: return "A DAST value exceeds the nesting limit.";
		case EPackageReaderReason::ValueTag: return "A DAST value tag does not match its declared type.";
		case EPackageReaderReason::StructBaseline: return "Invalid Struct baseline mode.";
		case EPackageReaderReason::ExportBaseline: return "Invalid export baseline mode.";
		case EPackageReaderReason::PropertyValue: return "A DAST property value is malformed.";
		case EPackageReaderReason::ValueTrailing: return "DAST value section has trailing bytes.";
		case EPackageReaderReason::BulkHandle: return "A BulkData handle is missing, repeated, or out of order.";
		case EPackageReaderReason::BulkOwner: return "A BulkData directory owner or shape is invalid.";
		case EPackageReaderReason::BulkRange: return "A BulkData range, alignment, or padding is invalid.";
		case EPackageReaderReason::BulkHash: return "A BulkData payload digest does not match.";
		case EPackageReaderReason::BulkLimit: return "The supplied DAST bulk segment exceeds its limit.";
		case EPackageReaderReason::MalformedTable: return "A DAST package table is malformed.";
		case EPackageReaderReason::TopLevelExportIndex: return "A DAST top-level asset export id is invalid.";
		case EPackageReaderReason::TopLevelTopology: return "A DAST top-level asset record does not match export topology.";
		case EPackageReaderReason::TopLevelIndexOverflow: return "A DAST top-level asset export id cannot be represented.";
		case EPackageReaderReason::MissingTopLevelAsset: return "A DAST package-outer export is missing its top-level asset record.";
		case EPackageReaderReason::ExternalBulkHash: return "The external DAST bulk segment binding does not match.";
		case EPackageReaderReason::ExportTopology: return "The DAST export topology is invalid.";
		case EPackageReaderReason::BulkCoverage: return "DAST bulk entries do not consume their exact inline/external segments.";
		case EPackageReaderReason::ValueShape: return "A DAST value violates its declared type, schema or baseline context.";
		case EPackageReaderReason::NonCanonicalBytes: return "DAST bytes violate canonical ordering or value representation.";
		case EPackageReaderReason::EnvelopeRejected:
			return "Binary envelope validation failed.";
		}
		return {};
	}

}
