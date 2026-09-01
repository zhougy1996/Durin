#include "AssetPackageV9Codec.h"
#include "AssetPackageByteSource.h"
#include "AssetPackageLinker.h"
#include "AssetRegistryResultAdapter.h"

#include "Asset/PackageInspection.h"
#include "Asset/RedirectorFixup.h"
#include "Asset/References.h"
#include "AssetRegistry/PackageHeader.h"
#include "DObject/CanonicalMapKey.h"
#include "DObject/PackageFormat.h"
#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Misc/MountPaths.h"

namespace Durin::AssetPrivate::DastV9
{
	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		auto ReaderError(const ObjectPackage::FPackageReaderDiagnostic& Diagnostic)
			-> FAssetResult
		{
			return Error(EAssetError::CorruptFile,
				std::format("DAST v9 package validation failed: {}", Diagnostic.Message));
		}

		auto ReadLinker(const FAssetPackageReadContext& Context,
			ObjectPackage::FLinkerTables& Out) -> FAssetResult
		{
			if (!Context.PackagePath.IsValid())
				return Error(EAssetError::InvalidPath,
					"DAST v9 requires the mounted package identity.");
			ObjectPackage::FPackageReaderDiagnostic Diagnostic;
			const bool bRead = Context.bResourceBackedBulk
				? ObjectPackage::ReadPackageV9Metadata(Context.PackageBytes,
					Context.PhysicalBulkBytes, Context.PackagePath, Out, &Diagnostic)
				: ObjectPackage::ReadPackageV9(Context.PackageBytes, Context.BulkBytes,
					Context.PackagePath, Out, &Diagnostic);
			if (!bRead)
				return ReaderError(Diagnostic);
			return {};
		}

		auto ReadHeader(const FAssetPackageReadContext& Context,
			FAssetPackageHeader& OutHeader) -> FAssetResult
		{
			const uint64 PhysicalBytes = Context.PhysicalPackageBytes == 0
				? Context.PackageBytes.size() : Context.PhysicalPackageBytes;
			const uint64 PhysicalBulkBytes = Context.bResourceBackedBulk
				? Context.PhysicalBulkBytes : Context.BulkBytes.size();
			return AssetPrivate::ToAssetResult(ReadAssetPackageHeaderBytes(
				Context.PackageBytes, PhysicalBytes, PhysicalBulkBytes,
				Context.PackagePath, OutHeader));
		}

		auto Validate(const FAssetPackageReadContext& Context) -> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			return ReadLinker(Context, Linker);
		}

		auto PropertyKind(ObjectPackage::EValueKind Kind)
			-> DurinCodeGen::EPropertyGenFlags
		{
			using K = ObjectPackage::EValueKind;
			using P = DurinCodeGen::EPropertyGenFlags;
			switch (Kind)
			{
			case K::Bool: return P::Bool;
			case K::I8: return P::Int8;
			case K::I16: return P::Int16;
			case K::I32: return P::Int32;
			case K::I64: return P::Int64;
			case K::U8: return P::UInt8;
			case K::U16: return P::UInt16;
			case K::U32: return P::UInt32;
			case K::U64: return P::UInt64;
			case K::F32: return P::Float;
			case K::F64: return P::Double;
			case K::String: return P::String;
			case K::Name: return P::Name;
			case K::Guid: return P::Guid;
			case K::Enum: return P::Enum;
			case K::Struct: return P::Struct;
			case K::Array: return P::Array;
			case K::Map: return P::Map;
			case K::HardReference: return P::Object;
			case K::SoftReference: return P::SoftObject;
			case K::Byte: return P::Byte;
			case K::Bytes: return P::Blob;
			case K::BulkData: return P::BulkData;
			case K::FixedArray:
				return P::None;
			case K::Intrinsic:
				return P::None;
			}
			return P::None;
		}

		auto InspectionPropertyKind(const ObjectPackage::FSerializedType& Type)
			-> DurinCodeGen::EPropertyGenFlags
		{
			if (Type.Kind == ObjectPackage::EValueKind::FixedArray)
				return Type.Children.size() == 1
					? InspectionPropertyKind(Type.Children[0])
					: DurinCodeGen::EPropertyGenFlags::None;
			return PropertyKind(Type.Kind);
		}

		auto ScalarBytes(ObjectPackage::EValueKind Kind) -> uint32
		{
			using K = ObjectPackage::EValueKind;
			switch (Kind)
			{
			case K::Bool: case K::I8: case K::U8: case K::Byte: return 1;
			case K::I16: case K::U16: return 2;
			case K::I32: case K::U32: case K::F32: return 4;
			case K::I64: case K::U64: case K::F64: return 8;
			default: return 0;
			}
		}

		auto TypeSignature(const ObjectPackage::FSerializedType& Type) -> std::string
		{
			using K = ObjectPackage::EValueKind;
			using P = DurinCodeGen::EPropertyGenFlags;
			if (Type.Kind == K::FixedArray)
				return Type.Children.size() == 1 ? TypeSignature(Type.Children[0]) : "Invalid";
			if (Type.Kind == K::Array)
				return std::format("Array<{}>", Type.Children.size() == 1
					? TypeSignature(Type.Children[0]) : "Invalid");
			if (Type.Kind == K::Map)
				return std::format("Map<{},{}>", Type.Children.size() == 2
					? TypeSignature(Type.Children[0]) : "Invalid", Type.Children.size() == 2
					? TypeSignature(Type.Children[1]) : "Invalid");
			if (Type.Kind == K::HardReference)
				return std::format("Object:{}:true",
					Type.QualifiedName.empty() ? "DObject" : Type.QualifiedName);
			if (Type.Kind == K::SoftReference)
				return std::format("SoftObject:{}:v1",
					Type.QualifiedName.empty() ? "DObject" : Type.QualifiedName);
			if (Type.Kind == K::Enum)
				return std::format("Enum:{}:{}", Type.QualifiedName,
					ScalarBytes(static_cast<K>(Type.Parameter)));
			if (Type.Kind == K::Struct)
				return std::format("Struct<{}>", Type.QualifiedName);
			const P Kind = PropertyKind(Type.Kind);
			if (Kind == P::String || Kind == P::Name || Kind == P::Guid
				|| Kind == P::Byte || Kind == P::Blob || Kind == P::BulkData)
				return std::format("{}:v1", static_cast<uint32>(Kind));
			return std::format("{}:{}", static_cast<uint32>(Kind),
				std::max<uint32>(1, ScalarBytes(Type.Kind)));
		}

		template<typename T>
		auto AppendNative(FByteArray& Out, const T& Value) -> void
		{
			const auto Bytes = std::as_bytes(std::span(&Value, 1));
			Out.insert(Out.end(), Bytes.begin(), Bytes.end());
		}

		auto AppendString(FByteArray& Out, std::string_view Value) -> void
		{
			AppendNative(Out, static_cast<uint64>(Value.size()));
			const auto Bytes = std::as_bytes(std::span(Value.data(), Value.size()));
			Out.insert(Out.end(), Bytes.begin(), Bytes.end());
		}

		struct FInspectionEncodeState
		{
			uint64 BulkFieldIndex = 0;
			uint64 NextExternalOffset = 0;
		};

		auto FindSchema(const ObjectPackage::FLinkerTables& Linker,
			std::string_view Name) -> const ObjectPackage::FSerializedSchema*
		{
			const auto It = std::ranges::find(
				Linker.Schemas, Name,
				&ObjectPackage::FSerializedSchema::QualifiedName);
			return It == Linker.Schemas.end() ? nullptr : &*It;
		}

		auto EncodeInspectionPayload(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker,
			FByteArray& Out,
			FInspectionEncodeState& State) -> bool
		{
			using K = ObjectPackage::EValueKind;
			switch (Type.Kind)
			{
			case K::Bool: AppendNative(Out, Value.Bool); return true;
			case K::I8: AppendNative(Out, static_cast<int8>(Value.Signed)); return true;
			case K::I16: AppendNative(Out, static_cast<int16>(Value.Signed)); return true;
			case K::I32: AppendNative(Out, static_cast<int32>(Value.Signed)); return true;
			case K::I64: AppendNative(Out, Value.Signed); return true;
			case K::U8: case K::Byte: AppendNative(Out, static_cast<uint8>(Value.Unsigned)); return true;
			case K::U16: AppendNative(Out, static_cast<uint16>(Value.Unsigned)); return true;
			case K::U32: AppendNative(Out, static_cast<uint32>(Value.Unsigned)); return true;
			case K::U64: AppendNative(Out, Value.Unsigned); return true;
			case K::F32: AppendNative(Out, static_cast<uint32>(Value.FloatingBits)); return true;
			case K::F64: AppendNative(Out, Value.FloatingBits); return true;
			case K::String: case K::Name: AppendString(Out, Value.Text); return true;
			case K::Guid: AppendNative(Out, Value.Guid); return true;
			case K::Enum:
			{
				const uint32 Bytes = ScalarBytes(static_cast<K>(Type.Parameter));
				for (uint32 Index = 0; Index < Bytes; ++Index)
					Out.push_back(static_cast<std::byte>((Value.Unsigned >> (Index * 8)) & 0xff));
				return Bytes != 0;
			}
			case K::HardReference:
			{
				if (Value.Reference.IsNull()) { AppendNative(Out, uint8{0}); return true; }
				if (Value.Reference.IsExport())
				{
					AppendNative(Out, uint8{1});
					AppendNative(Out, uint64(Value.Reference.GetTableIndex() + 1));
					return true;
				}
				if (Value.Reference.GetTableIndex() >= Linker.Imports.size()) return false;
				AppendNative(Out, uint8{2});
				AppendString(Out, Linker.Imports[Value.Reference.GetTableIndex()].ObjectPath.ToString());
				return true;
			}
			case K::SoftReference:
				AppendNative(Out, static_cast<uint8>(Value.Text.empty() ? 0 : 1));
				if (!Value.Text.empty()) AppendString(Out, Value.Text);
				return true;
			case K::Bytes:
				AppendNative(Out, static_cast<uint64>(Value.Bytes.size()));
				Out.insert(Out.end(), Value.Bytes.begin(), Value.Bytes.end());
				return true;
			case K::FixedArray:
			{
				if (Type.Children.size() != 1) return false;
				for (const auto& Element : Value.Elements)
					if (!EncodeInspectionPayload(
						Type.Children[0], Element, Linker, Out, State)) return false;
				return true;
			}
			case K::Array:
			{
				if (Type.Children.size() != 1) return false;
				AppendNative(Out, static_cast<uint64>(Value.Elements.size()));
				for (const auto& Element : Value.Elements)
					if (!EncodeInspectionPayload(
						Type.Children[0], Element, Linker, Out, State)) return false;
				return true;
			}
			case K::Map:
			{
				if (Type.Children.size() != 2 || Value.Elements.size() % 2 != 0)
					return false;
				AppendNative(Out, static_cast<uint64>(Value.Elements.size() / 2));
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!EncodeInspectionPayload(Type.Children[Index % 2],
						Value.Elements[Index], Linker, Out, State)) return false;
				return true;
			}
			case K::Struct:
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()) return false;
				AppendString(Out, Type.QualifiedName);
				AppendNative(Out, static_cast<uint64>(Value.Elements.size()));
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = std::ranges::find(
						Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (Field == Schema->Fields.end()) return false;
					FByteArray Payload;
					if (!EncodeInspectionPayload(
						Field->Type, Value.Elements[Index], Linker, Payload, State))
						return false;
					AppendString(Out, Schema->QualifiedName);
					AppendString(Out, Field->Name);
					AppendNative(Out, static_cast<uint8>(InspectionPropertyKind(Field->Type)));
					AppendString(Out, TypeSignature(Field->Type));
					AppendNative(Out, static_cast<uint64>(Payload.size()));
					Out.insert(Out.end(), Payload.begin(), Payload.end());
				}
				return true;
			}
			case K::BulkData:
			{
				if (Value.BulkElementSize == 0 || Value.BulkAlignment == 0
					|| Value.BulkStorage == ObjectPackage::EBulkStorageKind::Unset)
					return false;
				const bool bExternal = Value.BulkStorage
					== ObjectPackage::EBulkStorageKind::External;
				const uint64 StoredSize = Value.bBulkPayloadAvailable
					? Value.Bytes.size() : Value.BulkStoredSize;
				const uint64 Offset = bExternal
					? (State.NextExternalOffset + Value.BulkAlignment - 1)
						& ~uint64(Value.BulkAlignment - 1)
					: 0;
				if (bExternal) State.NextExternalOffset = Offset + StoredSize;
				const FXxHash128 Hash = Value.bBulkPayloadAvailable
					? FXxHash128::HashBuffer(Value.Bytes) : Value.BulkContentHash;
				FGuid PayloadId{
					static_cast<uint32>(Hash.HashLow),
					static_cast<uint32>(Hash.HashLow >> 32),
					static_cast<uint32>(Hash.HashHigh),
					static_cast<uint32>(Hash.HashHigh >> 32)};
				if (!PayloadId.IsValid()) PayloadId.A = 1;
				AppendNative(Out, uint32{1});
				AppendNative(Out, static_cast<uint8>(bExternal ? 1 : 0));
				AppendNative(Out, uint8{0});
				AppendNative(Out, static_cast<uint16>(Value.BulkAlignment));
				AppendNative(Out, Value.BulkElementSize);
				AppendNative(Out, ++State.BulkFieldIndex);
				AppendNative(Out, PayloadId);
				AppendNative(Out, Hash);
				AppendNative(Out, StoredSize);
				AppendNative(Out, StoredSize);
				AppendNative(Out, Offset);
				if (!bExternal)
					Out.insert(Out.end(), Value.Bytes.begin(), Value.Bytes.end());
				return true;
			}
			default:
				return false;
			}
		}

		auto Inspect(const FAssetPackageReadContext& Context,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			if (FAssetResult Result = ReadLinker(Context, Linker); !Result) return Result;
			FAssetPackageInspection Inspection;
			if (FAssetResult Result = ReadHeader(Context, Inspection.Header); !Result) return Result;
			Inspection.Fingerprint = {
				.FileSize = Context.PackageBytes.size(),
				.ContentHash = FXxHash128::HashBuffer(Context.PackageBytes),
				.ReaderVersion = ObjectPackage::DastV9FormatVersion};
			FInspectionEncodeState EncodeState;
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				const auto& Export = Linker.Exports[Index];
				ObjectPackage::FPackageIndex ExportIndex;
				std::string Path;
				if (!ObjectPackage::FPackageIndex::TryExport(Index, ExportIndex)
					|| !Linker.TryResolvePath(ExportIndex, Path))
					return Error(EAssetError::CorruptFile,
						"DAST export topology cannot resolve an object path.");
				FAssetPackageObjectInspection Object{
					.Id = Index + 1,
					.OuterId = Export.Outer.IsExport()
						? uint64(Export.Outer.GetTableIndex() + 1) : 0,
					.ClassName = Export.ClassName,
					.ObjectName = Export.ObjectName,
					.ObjectPath = std::move(Path)};
				for (const auto& Property : Export.Properties)
				{
					FAssetPackageField Field{
						.DeclaringClass = Property.DeclaringType,
						.Name = Property.FieldName,
						.Kind = InspectionPropertyKind(Property.Type),
						.TypeSignature = TypeSignature(Property.Type),
						.SourceFormatVersion = ObjectPackage::DastV9FormatVersion};
					if (!EncodeInspectionPayload(Property.Type, Property.Value, Linker,
						Field.Payload, EncodeState))
						return Error(EAssetError::CorruptFile,
							std::format("DAST inspection cannot project {}::{}.",
								Property.DeclaringType, Property.FieldName));
					Object.Fields.push_back(std::move(Field));
				}
				Inspection.Objects.push_back(std::move(Object));
			}
			OutInspection = std::move(Inspection);
			return {};
		}

		auto CollectReferences(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Value,
			const ObjectPackage::FLinkerTables& Linker,
			const FAssetPackageReadContext& Context,
			const ObjectPackage::FPackageExport& Export,
			const ObjectPackage::FPropertyTag& Property,
			uint64 ObjectId,
			std::vector<FAssetReferenceRouteSegment>& Route,
			std::vector<FAssetReferenceEdge>& Out,
			uint32 Depth = 0) -> bool
		{
			if (Depth > ObjectPackage::DastV8MaximumValueDepth) return false;
			using K = ObjectPackage::EValueKind;
			auto AppendEdge = [&](EAssetReferenceKind Kind,
				const FObjectPath& Target) {
				std::string Display = std::format("{}::{}", Property.DeclaringType,
					Property.FieldName);
				for (const auto& Segment : Route)
				{
					if (Segment.Kind == EAssetReferenceRouteKind::StructField)
						Display += std::format(".{}", Segment.FieldName);
					else if (Segment.Kind == EAssetReferenceRouteKind::MapValue)
						Display += "{value}";
					else Display += std::format("[{}]", Segment.Index);
				}
				Out.push_back({
					.SourcePackage = Context.PackagePath,
					.SourceFingerprint = {
						.FileSize = Context.PackageBytes.size(),
						.ContentHash = FXxHash128::HashBuffer(Context.PackageBytes),
						.ReaderVersion = ObjectPackage::DastV9FormatVersion},
					.SourceObjectId = ObjectId,
					.SourceClass = Export.ClassName,
					.DeclaringType = Property.DeclaringType,
					.FieldName = Property.FieldName,
					.Kind = Kind,
					.ExpectedClass = Type.QualifiedName,
					.TargetPath = Target,
					.Route = Route,
					.DisplayRoute = std::move(Display)});
			};
			if (Type.Kind == K::HardReference)
			{
				if (!Value.Reference.IsImport()) return Value.Reference.IsNull()
					|| Value.Reference.IsExport();
				if (Value.Reference.GetTableIndex() >= Linker.Imports.size()) return false;
				FObjectPath Target;
				if (!FObjectPath::TryCreate(
					Linker.Imports[Value.Reference.GetTableIndex()].ObjectPath.ToString(), Target))
					return false;
				AppendEdge(EAssetReferenceKind::HardObject, Target);
				return true;
			}
			if (Type.Kind == K::SoftReference)
			{
				if (Value.Text.empty()) return true;
				FObjectPath Target;
				if (!FObjectPath::TryCreate(Value.Text, Target)) return false;
				AppendEdge(EAssetReferenceKind::SoftObject, Target);
				return true;
			}
			if (Type.Kind == K::Struct)
			{
				const auto* Schema = FindSchema(Linker, Type.QualifiedName);
				if (!Schema || Value.FieldNames.size() != Value.Elements.size()) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					const auto Field = std::ranges::find(
						Schema->Fields, Value.FieldNames[Index],
						&ObjectPackage::FSerializedField::Name);
					if (Field == Schema->Fields.end()) return false;
					Route.push_back({.Kind = EAssetReferenceRouteKind::StructField,
						.DeclaringType = Schema->QualifiedName, .FieldName = Field->Name});
					if (!CollectReferences(Field->Type, Value.Elements[Index], Linker,
						Context, Export, Property, ObjectId, Route, Out, Depth + 1))
						return false;
					Route.pop_back();
				}
				return true;
			}
			if (Type.Kind == K::Array || Type.Kind == K::FixedArray)
			{
				if (Type.Children.size() != 1) return false;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
				{
					Route.push_back({.Kind = Type.Kind == K::Array
						? EAssetReferenceRouteKind::ArrayElement
						: EAssetReferenceRouteKind::FixedArray,
						.Index = Index});
					if (!CollectReferences(Type.Children[0], Value.Elements[Index], Linker,
						Context, Export, Property, ObjectId, Route, Out, Depth + 1))
						return false;
					Route.pop_back();
				}
				return true;
			}
			if (Type.Kind == K::Map)
			{
				if (Type.Children.size() != 2 || Value.Elements.size() % 2 != 0)
					return false;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					FByteArray Token;
					if (!ObjectPackage::BuildCanonicalMapKeyToken(
						Type.Children[0], Value.Elements[Index], Token)) return false;
					Route.push_back({.Kind = EAssetReferenceRouteKind::MapValue,
						.Index = Index / 2, .MapKeyToken = std::move(Token)});
					if (!CollectReferences(Type.Children[1], Value.Elements[Index + 1], Linker,
						Context, Export, Property, ObjectId, Route, Out, Depth + 1))
						return false;
					Route.pop_back();
				}
			}
			return true;
		}

		auto ExtractReferences(const FAssetPackageReadContext& Context,
			std::vector<FAssetReferenceEdge>& Out) -> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			if (FAssetResult Result = ReadLinker(Context, Linker); !Result) return Result;
			std::vector<FAssetReferenceEdge> References;
			std::vector<FAssetReferenceRouteSegment> Route;
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
				for (const auto& Property : Linker.Exports[Index].Properties)
					if (!CollectReferences(Property.Type, Property.Value, Linker, Context,
						Linker.Exports[Index], Property, Index + 1, Route, References))
						return Error(EAssetError::CorruptFile,
							"DAST v9 reference traversal encountered an invalid linker value.");
			std::ranges::sort(References, [](const auto& A, const auto& B) {
				return std::tuple(A.TargetPath.ToString(), A.SourceObjectId,
					std::string_view(A.DeclaringType), std::string_view(A.FieldName),
					std::string_view(A.DisplayRoute), A.Kind)
					< std::tuple(B.TargetPath.ToString(), B.SourceObjectId,
						std::string_view(B.DeclaringType), std::string_view(B.FieldName),
						std::string_view(B.DisplayRoute), B.Kind);
			});
			Out = std::move(References);
			return {};
		}

		auto InspectSchema(IAssetPackageByteSource& Source,
			const FPackagePath& Path, const FReflectionSchemaCatalog& Catalog,
			FPackageSchemaInspection& OutRecord, FPackageSchemaReadStats* OutStats,
			bool, const FPackageReadCancellationCheck& IsCancelled) -> FAssetResult
		{
			if (IsCancelled && IsCancelled())
				return Error(EAssetError::IoError, "Asset schema inspection was cancelled.");
			if (Source.GetSize() > ObjectPackage::DastV8MaximumPackageBytes
				|| Source.GetSize() > std::numeric_limits<size_t>::max())
				return Error(EAssetError::CorruptFile, "DAST package exceeds the byte bound.");
			FByteArray Main(static_cast<size_t>(Source.GetSize()));
			std::string ReadError;
			if (!Source.ReadAt(0, Main, &ReadError))
				return Error(EAssetError::IoError, std::move(ReadError));
			FByteArray Bulk;
			const FAssetPathResult Resolved = FMountPaths::ResolveAssetPath(
				Path.GetView(), EMountPathExistence::AllowMissing);
			if (Resolved)
			{
				std::filesystem::path BulkPath = Resolved.PhysicalPath;
				BulkPath.replace_extension(".dbulk");
				std::error_code Ec;
				if (std::filesystem::is_regular_file(BulkPath, Ec)
					&& !FFileHelper::LoadFileToArray(Bulk, BulkPath))
					return Error(EAssetError::IoError, "DAST bulk companion is unreadable.");
			}
			ObjectPackage::FLinkerTables Linker;
			if (FAssetResult Result = ReadLinker({Main, Bulk, Path, Main.size()}, Linker); !Result)
				return Result;
			FPackageSchemaInspection Record{.FormatVersion = ObjectPackage::DastV9FormatVersion};
			if (!Linker.Summary.TopLevelAssets.empty())
				Record.EntryKind = Linker.Summary.TopLevelAssets.front()
					.RedirectDestination.IsValid()
					? EAssetRegistryEntryKind::Redirector : EAssetRegistryEntryKind::Asset;
			Record.Dependencies.assign(Linker.Summary.HardPackageDependencies.begin(),
				Linker.Summary.HardPackageDependencies.end());
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				const auto& Export = Linker.Exports[Index];
				const auto* Class = Catalog.FindClass(Export.ClassName);
				ObjectPackage::FPackageIndex ExportIndex;
				std::string ObjectPath;
				ObjectPackage::FPackageIndex::TryExport(Index, ExportIndex);
				Linker.TryResolvePath(ExportIndex, ObjectPath);
				if (!Class)
				{
					Record.Status = EPackageSchemaStatus::Unsupported;
					Record.Issues.push_back({
						.Code = EPackageSchemaIssueCode::UnavailableClass,
						.ObjectPath = ObjectPath,
						.ClassIdentity = Export.ClassName,
						.Diagnostic = "Serialized class is unavailable."});
					continue;
				}
				for (const auto& Property : Export.Properties)
				{
					const auto Kind = PropertyKind(Property.Type.Kind);
					const std::string Signature = TypeSignature(Property.Type);
					const auto* Expected = Catalog.FindField(
						*Class, Property.DeclaringType, Property.FieldName);
					if (!Expected || Expected->Kind != Kind
						|| Expected->TypeSignature != Signature)
					{
						if (Record.Status == EPackageSchemaStatus::Compatible)
							Record.Status = EPackageSchemaStatus::Incompatible;
						Record.Issues.push_back({
							.Code = Expected
								? EPackageSchemaIssueCode::IncompatibleFieldSignature
								: EPackageSchemaIssueCode::UnknownField,
							.ObjectPath = ObjectPath,
							.ClassIdentity = Export.ClassName,
							.DeclaringType = Property.DeclaringType,
							.FieldName = Property.FieldName,
							.StoredKind = Kind,
							.StoredTypeSignature = Signature,
							.ExpectedKind = Expected ? Expected->Kind
								: DurinCodeGen::EPropertyGenFlags::None,
							.ExpectedTypeSignature = Expected ? Expected->TypeSignature : std::string{},
							.Diagnostic = Expected
								? "Serialized field signature differs from the current reflection catalog."
								: "Serialized field is not present in the current reflection catalog."});
					}
				}
			}
			if (OutStats)
			{
				OutStats->MetadataBytesRead = Main.size();
				OutStats->PeakMetadataBytes = Main.size() + Bulk.size();
			}
			OutRecord = std::move(Record);
			return {};
		}

		auto Load(const FAssetPackageReadContext& Context, DPackage*& OutPackage,
			FAssetLoadReport* OutReport,
			const std::function<FAssetResult(DPackage*)>& OnSkeletonReady,
			const std::function<void(DPackage*)>& OnSkeletonRollback) -> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			if (FAssetResult Result = ReadLinker(Context, Linker); !Result) return Result;
			return ApplyLivePackageLinker(std::move(Linker), Context.PackagePath, OutPackage,
				OutReport, {.OnSkeletonReady = OnSkeletonReady,
					.OnSkeletonRollback = OnSkeletonRollback,
					.SourceFormatVersion = ObjectPackage::DastV9FormatVersion,
					.bCooked = Context.bCooked,
					.Target = Context.bCooked
						? FArchiveTarget{.Platform = "Win64", .Profile = "Game"}
						: FArchiveTarget{}});
		}

		auto Write(DPackage* Package, FAssetPackageEncodedClosure& OutClosure,
			EDefaultDeltaMode DeltaMode,
			const FAssetPackageSerializationOptions& Options) -> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			std::string ErrorMessage;
			if (FAssetResult Result = CaptureLivePackageLinker(Package, DeltaMode,
				Options, Linker, &ErrorMessage); !Result) return Result;
			FAssetPackageEncodedClosure Closure;
			ObjectPackage::FPackageWriterDiagnostic Diagnostic;
			if (!ObjectPackage::WritePackageV9(Linker, Closure.PackageBytes,
				Closure.BulkBytes, &Diagnostic))
				return Error(EAssetError::CorruptFile,
					std::format("DAST v9 package write failed: {}", Diagnostic.Message));
			ObjectPackage::FLinkerTables Verified;
			ObjectPackage::FPackageReaderDiagnostic ReaderDiagnostic;
			if (!ObjectPackage::ReadPackageV9(Closure.PackageBytes, Closure.BulkBytes,
				Linker.Summary.PackagePath, Verified, &ReaderDiagnostic))
				return ReaderError(ReaderDiagnostic);
			OutClosure = std::move(Closure);
			return {};
		}

		auto WriteLinker(ObjectPackage::FLinkerTables Linker,
			FAssetPackageEncodedClosure& OutClosure) -> FAssetResult
		{
			Linker.Names.clear();
			FAssetPackageEncodedClosure Closure;
			ObjectPackage::FPackageWriterDiagnostic Diagnostic;
			if (!ObjectPackage::WritePackageV9(Linker, Closure.PackageBytes,
				Closure.BulkBytes, &Diagnostic))
				return Error(EAssetError::CorruptFile,
					std::format("DAST v9 package mutation failed: {}", Diagnostic.Message));
			ObjectPackage::FLinkerTables Verified;
			ObjectPackage::FPackageReaderDiagnostic ReaderDiagnostic;
			if (!ObjectPackage::ReadPackageV9(Closure.PackageBytes, Closure.BulkBytes,
				Linker.Summary.PackagePath, Verified, &ReaderDiagnostic))
				return ReaderError(ReaderDiagnostic);
			OutClosure = std::move(Closure);
			return {};
		}

		auto RewriteReferences(const FAssetPackageReadContext& Context,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedCount, FAssetPackageEncodedClosure& OutClosure) -> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			if (FAssetResult Result = ReadLinker(Context, Linker); !Result) return Result;
			auto FindDestination = [&](std::string_view Source) -> const FPackagePath* {
				const auto It = std::ranges::find_if(Mappings, [&](const auto& Mapping) {
					return Mapping.RedirectorPath.GetView() == Source;
				});
				return It == Mappings.end() ? nullptr : &It->FinalPath;
			};
			auto RemapObjectPath = [&](const FObjectPath& Source,
				std::string_view DestinationAssetName, FObjectPath& Out) -> bool {
				const FPackagePath* Destination = FindDestination(
					Source.GetPackagePath().GetView());
				if (!Destination) return false;
				FTopLevelAssetPath AssetPath;
				if (!FTopLevelAssetPath::TryCreate(*Destination,
					DestinationAssetName, AssetPath)) return false;
				return FObjectPath::TryCreate(
					AssetPath, Source.GetSubobjectNames(), Out);
			};
			uint64 RewriteCount = 0;
			std::function<bool(const ObjectPackage::FSerializedType&,
				ObjectPackage::FSerializedValue&)> RewriteValue;
			RewriteValue = [&](const ObjectPackage::FSerializedType& Type,
				ObjectPackage::FSerializedValue& Value) -> bool {
				using K = ObjectPackage::EValueKind;
				if (Type.Kind == K::HardReference && Value.Reference.IsImport())
				{
					if (Value.Reference.GetTableIndex() >= Linker.Imports.size()) return false;
					const auto& Import = Linker.Imports[Value.Reference.GetTableIndex()];
					if (FindDestination(Import.ObjectPath.GetPackagePath().GetView())) ++RewriteCount;
					return true;
				}
				if (Type.Kind == K::SoftReference && !Value.Text.empty())
				{
					FObjectPath Source;
					FObjectPath Destination;
					if (!FObjectPath::TryCreate(Value.Text, Source)) return false;
					if (const FPackagePath* Remapped = FindDestination(
						Source.GetPackagePath().GetView());
						Remapped && RemapObjectPath(
							Source, Remapped->GetPackageName(), Destination))
					{
						Value.Text = Destination.ToString();
						++RewriteCount;
					}
					return true;
				}
				if (Type.Kind == K::Struct)
				{
					const auto* Schema = FindSchema(Linker, Type.QualifiedName);
					if (!Schema || Value.FieldNames.size() != Value.Elements.size())
						return false;
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					{
						const auto Field = std::ranges::find(
							Schema->Fields, Value.FieldNames[Index],
							&ObjectPackage::FSerializedField::Name);
						if (Field == Schema->Fields.end()
							|| !RewriteValue(Field->Type, Value.Elements[Index]))
							return false;
					}
				}
				else if (Type.Kind == K::Array || Type.Kind == K::FixedArray)
				{
					if (Type.Children.size() != 1) return false;
					for (auto& Element : Value.Elements)
						if (!RewriteValue(Type.Children[0], Element)) return false;
				}
				else if (Type.Kind == K::Map)
				{
					if (Type.Children.size() != 2 || Value.Elements.size() % 2 != 0) return false;
					for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
						if (!RewriteValue(Type.Children[Index % 2], Value.Elements[Index])) return false;
				}
				return true;
			};
			for (auto& Export : Linker.Exports)
				for (auto& Property : Export.Properties)
					if (!RewriteValue(Property.Type, Property.Value))
						return Error(EAssetError::CorruptFile,
							"DAST reference value has an invalid shape.");
			if (ExpectedCount != std::numeric_limits<uint64>::max()
				&& RewriteCount != ExpectedCount)
				return Error(EAssetError::InUse, std::format(
					"AssetReferenceFixupStaleIndex: expected {} occurrence(s), parsed {}.",
					ExpectedCount, RewriteCount));

			std::vector<ObjectPackage::FPackageImport> Imports;
			std::vector<uint32> ImportRemap(Linker.Imports.size());
			for (size_t Index = 0; Index < Linker.Imports.size(); ++Index)
			{
				auto Import = Linker.Imports[Index];
				FObjectPath DestinationObject;
				if (RemapObjectPath(Import.ObjectPath,
					Import.ObjectPath.GetAssetPath().GetAssetName(), DestinationObject))
					Import.ObjectPath = std::move(DestinationObject);
				const auto Existing = std::ranges::find(Imports, Import);
				if (Existing == Imports.end())
				{
					ImportRemap[Index] = Imports.size();
					Imports.push_back(std::move(Import));
				}
				else ImportRemap[Index] = std::distance(Imports.begin(), Existing);
			}
			std::function<void(ObjectPackage::FSerializedValue&)> RemapValue;
			RemapValue = [&](ObjectPackage::FSerializedValue& Value) {
				if (Value.Reference.IsImport())
				{
					ObjectPackage::FPackageIndex Remapped;
					ObjectPackage::FPackageIndex::TryImport(
						ImportRemap[Value.Reference.GetTableIndex()], Remapped);
					Value.Reference = Remapped;
				}
				for (auto& Element : Value.Elements) RemapValue(Element);
			};
			for (auto& Export : Linker.Exports)
				for (auto& Property : Export.Properties) RemapValue(Property.Value);
			Linker.Imports = std::move(Imports);
			for (FPackagePath& Reference : Linker.Summary.HardPackageDependencies)
				if (const FPackagePath* Destination = FindDestination(Reference.GetView()))
					Reference = *Destination;
			for (FPackagePath& Reference : Linker.Summary.SoftPackageDependencies)
				if (const FPackagePath* Destination = FindDestination(Reference.GetView()))
					Reference = *Destination;
			for (auto* References : {&Linker.Summary.HardPackageDependencies,
				&Linker.Summary.SoftPackageDependencies})
			{
				std::ranges::sort(*References);
				References->erase(std::ranges::unique(*References).begin(), References->end());
			}
			for (auto& Asset : Linker.Summary.TopLevelAssets)
				if (Asset.RedirectDestination.IsValid())
				{
					FObjectPath Destination;
					if (const FPackagePath* Remapped = FindDestination(
						Asset.RedirectDestination.GetPackagePath().GetView());
						Remapped && RemapObjectPath(Asset.RedirectDestination,
							Remapped->GetPackageName(), Destination))
						Asset.RedirectDestination = std::move(Destination);
				}
			return WriteLinker(std::move(Linker), OutClosure);
		}

		auto Relocate(const FAssetPackageReadContext& Context,
			const FPackagePath& Destination, FAssetPackageEncodedClosure& OutClosure)
			-> FAssetResult
		{
			ObjectPackage::FLinkerTables Linker;
			if (FAssetResult Result = ReadLinker(Context, Linker); !Result) return Result;
			if (Linker.Summary.TopLevelAssets.empty()
				|| std::ranges::any_of(Linker.Summary.TopLevelAssets, [](const auto& Asset) {
					return Asset.RedirectDestination.IsValid();
				}))
				return Error(EAssetError::InvalidPackageType,
					"Only a real DAST v9 asset package can be relocated.");
			Linker.Summary.PackagePath = Destination;
			for (auto& Asset : Linker.Summary.TopLevelAssets)
			{
				FTopLevelAssetPath Relocated;
				if (!FTopLevelAssetPath::TryCreate(
					Destination, Asset.AssetPath.GetAssetName(), Relocated))
					return Error(EAssetError::InvalidPath,
						"Relocated top-level asset identity is invalid.");
				Asset.AssetPath = std::move(Relocated);
			}
			return WriteLinker(std::move(Linker), OutClosure);
		}

		auto WriteRedirector(const FPackagePath& Source,
			std::span<const FAssetRedirectorWriteMapping> Mappings,
			FAssetPackageEncodedClosure& OutClosure) -> FAssetResult
		{
			constexpr std::string_view RedirectorClass =
				"Durin::DAssetRedirector";
			if (Mappings.empty())
				return Error(EAssetError::InvalidPath,
					"Redirector creation requires at least one exact asset mapping.");
			ObjectPackage::FSerializedType ReferenceType{
				.Kind = ObjectPackage::EValueKind::HardReference,
				.QualifiedName = "Durin::DObject"};
			ObjectPackage::FLinkerTables Linker;
			Linker.Summary.PackagePath = Source;
			Linker.Types.push_back(ReferenceType);
			Linker.Schemas.push_back({std::string(RedirectorClass),
				{{"DestinationObject", ReferenceType, 0}}});
			for (size_t Index = 0; Index < Mappings.size(); ++Index)
			{
				const FAssetRedirectorWriteMapping& Mapping = Mappings[Index];
				if (!Mapping.Source.IsValid() || Mapping.Source.GetPackagePath() != Source
					|| !Mapping.Destination.IsValid())
					return Error(EAssetError::InvalidPath, "Redirector identity is invalid.");
				ObjectPackage::FPackageIndex Export;
				ObjectPackage::FPackageIndex Import;
				ObjectPackage::FPackageIndex::TryExport(static_cast<uint32>(Index), Export);
				ObjectPackage::FPackageIndex::TryImport(static_cast<uint32>(Index), Import);
				Linker.Summary.TopLevelAssets.push_back({.Export = Export,
					.AssetPath = Mapping.Source,
					.ClassName = std::string(RedirectorClass),
					.RedirectDestination = Mapping.Destination});
				Linker.Summary.HardPackageDependencies.push_back(
					Mapping.Destination.GetPackagePath());
				Linker.Imports.push_back({.ObjectPath = Mapping.Destination});
				Linker.Exports.push_back({
					.ObjectName = std::string(Mapping.Source.GetAssetName()),
					.ClassName = std::string(RedirectorClass),
					.Properties = {{
						.DeclaringType = std::string(RedirectorClass),
						.FieldName = "DestinationObject",
						.Type = ReferenceType,
						.Provenance = ObjectPackage::EPropertyProvenance::Explicit,
						.Value = {.Reference = Import}}}});
			}
			std::ranges::sort(Linker.Summary.HardPackageDependencies);
			Linker.Summary.HardPackageDependencies.erase(std::unique(
				Linker.Summary.HardPackageDependencies.begin(),
				Linker.Summary.HardPackageDependencies.end()),
				Linker.Summary.HardPackageDependencies.end());
			return WriteLinker(std::move(Linker), OutClosure);
		}
	}

	auto GetCodec() -> const FAssetPackageCodec&
	{
		static const FAssetPackageCodec Codec{
			.CodecId = "dast-v9",
			.FormatVersion = ObjectPackage::DastV9FormatVersion,
			.bCanRead = true,
			.bCanWrite = true,
			.bCanMutate = true,
			.ReadHeader = &ReadHeader,
			.Validate = &Validate,
			.Inspect = &Inspect,
			.ExtractReferences = &ExtractReferences,
			.InspectSchema = &InspectSchema,
			.Load = &Load,
			.Write = &Write,
			.RewriteReferences = &RewriteReferences,
			.Relocate = &Relocate,
			.WriteRedirector = &WriteRedirector};
		return Codec;
	}
}
