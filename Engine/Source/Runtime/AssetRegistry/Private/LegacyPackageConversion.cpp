#include "AssetRegistry/LegacyPackageConversion.h"

#include "AssetRegistry/ObjectStream.h"
#include "AssetRegistry/PackageHeader.h"
#include "DObject/PackageFormat.h"
#include "PackageLinkerV7Adapter.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 V7BulkDescriptorBytes = 72;
		constexpr uint32 V7BulkContentIdVersion = 1;

		auto Fail(FLegacyPackageConversionDiagnostic* Diagnostic,
			ELegacyPackageConversionFailure Failure, std::string Message,
			std::string Path = {}) -> bool
		{
			if (Diagnostic)
				*Diagnostic = {Failure, std::move(Path), std::move(Message)};
			return false;
		}

		struct FBulkRange
		{
			uint64 Begin = 0;
			uint64 End = 0;
			std::string Path;
		};

		class FBulkResolver
		{
		public:
			FBulkResolver(ObjectPackage::FLinkerTables& InLinker,
				std::span<const std::byte> InExternal,
				FLegacyPackageConversionDiagnostic* InDiagnostic)
				: Linker(InLinker), External(InExternal), Diagnostic(InDiagnostic) {}

			auto ResolveAll() -> bool
			{
				for (ObjectPackage::FPackageExport& Export : Linker.Exports)
					for (ObjectPackage::FPropertyTag& Property : Export.Properties)
						if (!Resolve(Property.Type, Property.Value,
							std::format("{}::{}::{}", Export.ObjectName,
								Property.DeclaringType, Property.FieldName)))
							return false;
				std::ranges::sort(ExternalRanges,
					[](const FBulkRange& Left, const FBulkRange& Right) {
						return std::tie(Left.Begin, Left.End)
							< std::tie(Right.Begin, Right.End);
					});
				uint64 Cursor = 0;
				for (const FBulkRange& Range : ExternalRanges)
				{
					if (Range.Begin < Cursor)
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidBulkData,
							"DAST v7 external BulkData ranges overlap.", Range.Path);
					if (!AllZero(Cursor, Range.Begin))
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidBulkData,
							"DAST v7 external BulkData alignment padding is nonzero.",
							Range.Path);
					Cursor = Range.End;
				}
				if (!AllZero(Cursor, External.size()))
					return Fail(Diagnostic,
						ELegacyPackageConversionFailure::InvalidBulkData,
						"DAST v7 external BulkData segment has unreferenced trailing bytes.");
				return true;
			}

		private:
			auto AllZero(uint64 Begin, uint64 End) const -> bool
			{
				if (Begin > End || End > External.size()) return false;
				return std::ranges::all_of(External.subspan(
					static_cast<size_t>(Begin), static_cast<size_t>(End - Begin)),
					[](std::byte Value) { return Value == std::byte{0}; });
			}

			auto FindSchema(std::string_view Name) const
				-> const ObjectPackage::FSerializedSchema*
			{
				const auto It = std::ranges::find(
					Linker.Schemas, Name,
					&ObjectPackage::FSerializedSchema::QualifiedName);
				return It == Linker.Schemas.end() ? nullptr : &*It;
			}

			auto Resolve(const ObjectPackage::FSerializedType& Type,
				ObjectPackage::FSerializedValue& Value, const std::string& Path) -> bool
			{
				using ObjectPackage::EValueKind;
				switch (Type.Kind)
				{
				case EValueKind::BulkData:
					return ResolveBulk(Value, Path);
				case EValueKind::Struct:
				{
					const ObjectPackage::FSerializedSchema* Schema =
						FindSchema(Type.QualifiedName);
					if (!Schema || Value.FieldNames.size() != Value.Elements.size())
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidV7Package,
							"DAST v7 Struct BulkData traversal shape is invalid.", Path);
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					{
						const auto Field = std::ranges::find(
							Schema->Fields, Value.FieldNames[Index],
							&ObjectPackage::FSerializedField::Name);
						if (Field == Schema->Fields.end()
							|| !Resolve(Field->Type, Value.Elements[Index],
								Path + "::" + Value.FieldNames[Index])) return false;
					}
					return true;
				}
				case EValueKind::FixedArray:
				case EValueKind::Array:
					if (Type.Children.size() != 1)
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidV7Package,
							"DAST v7 Array BulkData traversal type is invalid.", Path);
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
						if (!Resolve(Type.Children[0], Value.Elements[Index],
							std::format("{}[{}]", Path, Index))) return false;
					return true;
				case EValueKind::Map:
					if (Type.Children.size() != 2 || Value.Elements.size() % 2 != 0)
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidV7Package,
							"DAST v7 Map BulkData traversal shape is invalid.", Path);
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
						if (!Resolve(Type.Children[Index % 2], Value.Elements[Index],
							std::format("{}[{}]", Path, Index))) return false;
					return true;
				default:
					return true;
				}
			}

			auto ResolveBulk(ObjectPackage::FSerializedValue& Value,
				const std::string& Path) -> bool
			{
				const auto Encoded = std::span<const std::byte>(Value.Bytes);
				uint64 FieldIndex = 0;
				uint8 Placement = 0, Flags = 0;
				uint16 Alignment = 0;
				uint32 ContentIdVersion = 0;
				FGuid PayloadId;
				FXxHash128 ContentHash;
				uint64 LogicalSize = 0, StoredSize = 0, Offset = 0;
				if (Encoded.size() < V7BulkDescriptorBytes
					|| !ReadLittleEndianAt(Encoded, 0, FieldIndex) || FieldIndex == 0
					|| !ReadLittleEndianAt(Encoded, 8, Placement) || Placement > 1
					|| !ReadLittleEndianAt(Encoded, 9, Flags) || Flags != 0
					|| !ReadLittleEndianAt(Encoded, 10, Alignment)
					|| !ReadLittleEndianAt(Encoded, 12, ContentIdVersion)
					|| ContentIdVersion != V7BulkContentIdVersion
					|| !ReadLittleEndianAt(Encoded, 16, PayloadId.A)
					|| !ReadLittleEndianAt(Encoded, 20, PayloadId.B)
					|| !ReadLittleEndianAt(Encoded, 24, PayloadId.C)
					|| !ReadLittleEndianAt(Encoded, 28, PayloadId.D)
					|| !PayloadId.IsValid()
					|| !ReadLittleEndianAt(Encoded, 32, ContentHash.HashLow)
					|| !ReadLittleEndianAt(Encoded, 40, ContentHash.HashHigh)
					|| ContentHash.IsZero()
					|| !ReadLittleEndianAt(Encoded, 48, LogicalSize)
					|| !ReadLittleEndianAt(Encoded, 56, StoredSize)
					|| LogicalSize != StoredSize
					|| StoredSize > ObjectPackage::DastV8MaximumBulkBytes
					|| !ReadLittleEndianAt(Encoded, 64, Offset)
					|| !FieldIndices.insert(FieldIndex).second)
					return Fail(Diagnostic,
						ELegacyPackageConversionFailure::InvalidBulkData,
						"DAST v7 BulkData descriptor is invalid or duplicated.", Path);

				std::span<const std::byte> Payload;
				if (Placement == 0)
				{
					if (Alignment != 1 || Offset != 0
						|| StoredSize != Encoded.size() - V7BulkDescriptorBytes)
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidBulkData,
							"DAST v7 inline BulkData descriptor is inconsistent.", Path);
					Payload = Encoded.subspan(static_cast<size_t>(V7BulkDescriptorBytes));
					Value.BulkStorage = ObjectPackage::EBulkStorageKind::Inline;
				}
				else
				{
					if (Encoded.size() != V7BulkDescriptorBytes || Alignment == 0
						|| Alignment > 4096 || (Alignment & (Alignment - 1)) != 0
						|| Offset % Alignment != 0 || Offset > External.size()
						|| StoredSize > External.size() - Offset)
						return Fail(Diagnostic,
							ELegacyPackageConversionFailure::InvalidBulkData,
							"DAST v7 external BulkData range is invalid.", Path);
					Payload = External.subspan(static_cast<size_t>(Offset),
						static_cast<size_t>(StoredSize));
					ExternalRanges.push_back({Offset, Offset + StoredSize, Path});
					Value.BulkStorage = ObjectPackage::EBulkStorageKind::External;
				}
				if (FXxHash128::HashBuffer(Payload) != ContentHash)
					return Fail(Diagnostic,
						ELegacyPackageConversionFailure::InvalidBulkData,
						"DAST v7 BulkData payload hash does not match its descriptor.", Path);
				Value.Bytes.assign(Payload.begin(), Payload.end());
				Value.BulkElementSize = 1;
				Value.BulkAlignment = Alignment;
				return true;
			}

			ObjectPackage::FLinkerTables& Linker;
			std::span<const std::byte> External;
			FLegacyPackageConversionDiagnostic* Diagnostic = nullptr;
			std::unordered_set<uint64> FieldIndices;
			std::vector<FBulkRange> ExternalRanges;
		};
	}

	auto ResolveDastV7LinkerBulkDataForV8(
		ObjectPackage::FLinkerTables& Linker,
		std::span<const std::byte> V7BulkBytes,
		FLegacyPackageConversionDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FBulkResolver Resolver(Linker, V7BulkBytes, OutDiagnostic);
		return Resolver.ResolveAll();
	}

	auto AdaptDecodedDastV7ToLinker(
		const PackageObjectStream::FDecodedPackage& Package,
		std::string_view PackageName,
		ObjectPackage::FLinkerTables& OutLinker,
		FLegacyPackageConversionDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		Private::FV7LinkerAdapterDiagnostic Diagnostic;
		if (Private::AdaptDecodedPackageV7(
			Package, PackageName, OutLinker, &Diagnostic)) return true;
		return Fail(OutDiagnostic,
			Diagnostic.Failure == Private::EV7LinkerAdapterFailure::UnsupportedRetainedValue
				? ELegacyPackageConversionFailure::UnsupportedV7Value
				: ELegacyPackageConversionFailure::InvalidV7Package,
			std::move(Diagnostic.Message), std::move(Diagnostic.LogicalPath));
	}

	auto ConvertDastV7PackageToV8(std::span<const std::byte> V7PackageBytes,
		std::span<const std::byte> V7BulkBytes, std::string_view PackageName,
		std::vector<std::byte>& OutV8PackageBytes,
		std::vector<std::byte>& OutV8BulkBytes,
		FLegacyPackageConversionDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FAssetPath ParsedPackageName;
		if (!FAssetPath::TryCreate(PackageName, ParsedPackageName))
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidIdentity,
				"DAST v7 conversion requires a registered canonical package identity.");
		uint64 HeaderBytes = 0;
		if (!ReadLittleEndianAt(V7PackageBytes, 32, HeaderBytes)
			|| HeaderBytes > V7PackageBytes.size())
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidV7Package,
				"DAST v7 package declares invalid front matter.");
		FAssetPackageHeader Header;
		if (FAssetResult HeaderResult = ReadAssetPackageHeaderBytes(
			V7PackageBytes.first(static_cast<size_t>(HeaderBytes)),
			V7PackageBytes.size(), V7BulkBytes.size(), ParsedPackageName, Header);
			!HeaderResult)
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidV7Package,
				std::move(HeaderResult.Message));
		if (Header.FormatVersion != AssetPackageV7FormatVersion)
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidV7Package,
				"Offline conversion accepts only DAST v7 input.");
		if (Header.BulkSegmentExtent != V7BulkBytes.size()
			|| (Header.BulkSegmentExtent != 0
				&& FXxHash128::HashBuffer(V7BulkBytes) != Header.BulkSegmentDigest))
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidBulkData,
				"DAST v7 external bulk segment binding is invalid.");

		std::vector<std::byte> ObjectStream;
		if (FAssetResult Extracted = PackageObjectStream::ExtractDastObjectStream(
			V7PackageBytes, ObjectStream); !Extracted)
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidV7Package,
				std::move(Extracted.Message));
		PackageObjectStream::FDecodedPackage Decoded;
		PackageObjectStream::FReaderDiagnostic ReaderDiagnostic;
		if (!PackageObjectStream::DecodePackage(
			ObjectStream, Decoded, {}, &ReaderDiagnostic))
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::InvalidV7Package,
				std::move(ReaderDiagnostic.Message),
				std::move(ReaderDiagnostic.LogicalPath));
		ObjectPackage::FLinkerTables Linker;
		if (!AdaptDecodedDastV7ToLinker(
			Decoded, PackageName, Linker, OutDiagnostic)) return false;
		if (!ResolveDastV7LinkerBulkDataForV8(
			Linker, V7BulkBytes, OutDiagnostic)) return false;

		std::vector<std::byte> Main;
		std::vector<std::byte> Bulk;
		ObjectPackage::FPackageWriterDiagnostic WriterDiagnostic;
		if (!ObjectPackage::WritePackageV8(
			Linker, Main, Bulk, &WriterDiagnostic))
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::V8WriteFailure,
				std::move(WriterDiagnostic.Message),
				std::move(WriterDiagnostic.LogicalPath));
		ObjectPackage::FLinkerTables Verified;
		ObjectPackage::FPackageReaderDiagnostic V8Diagnostic;
		if (!ObjectPackage::ReadPackageV8(
			Main, Bulk, PackageName, Verified, &V8Diagnostic))
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::V8VerificationFailure,
				std::move(V8Diagnostic.Message),
				std::move(V8Diagnostic.LogicalPath));
		std::vector<std::byte> CanonicalMain;
		std::vector<std::byte> CanonicalBulk;
		if (!ObjectPackage::WritePackageV8(
			Verified, CanonicalMain, CanonicalBulk, &WriterDiagnostic)
			|| CanonicalMain != Main || CanonicalBulk != Bulk)
			return Fail(OutDiagnostic,
				ELegacyPackageConversionFailure::V8VerificationFailure,
				"Converted DAST v8 package did not re-emit canonically.");
		OutV8PackageBytes = std::move(Main);
		OutV8BulkBytes = std::move(Bulk);
		return true;
	}
}
