#include "AssetPackageLinkerAdapter.h"

#include "Asset/PackageObjectStreamWriter.h"
#include "DObject/Package.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::Asset::Private
{
	namespace
	{
		auto Error(std::string Message, std::string* OutError = nullptr) -> FAssetResult
		{
			if (OutError) *OutError = Message;
			return {EAssetError::CorruptFile, std::move(Message)};
		}

		auto LinkerKind(PackageObjectStream::ETypeOpcode Opcode)
			-> std::optional<ObjectPackage::EValueKind>
		{
			using K = ObjectPackage::EValueKind;
			using O = PackageObjectStream::ETypeOpcode;
			switch (Opcode)
			{
			case O::Bool: return K::Bool;
			case O::I8: return K::I8;
			case O::I16: return K::I16;
			case O::I32: return K::I32;
			case O::I64: return K::I64;
			case O::U8: return K::U8;
			case O::U16: return K::U16;
			case O::U32: return K::U32;
			case O::U64: return K::U64;
			case O::F32: return K::F32;
			case O::F64: return K::F64;
			case O::String: return K::String;
			case O::Name: return K::Name;
			case O::Guid: return K::Guid;
			case O::Enum: return K::Enum;
			case O::Intrinsic: return K::Intrinsic;
			case O::Struct: return K::Struct;
			case O::FixedArray: return K::FixedArray;
			case O::Array: return K::Array;
			case O::Map: return K::Map;
			case O::HardRef: return K::HardReference;
			case O::SoftRef: return K::SoftReference;
			case O::Bytes: return K::Bytes;
			case O::BulkData: return K::BulkData;
			}
			return std::nullopt;
		}

		auto FindSchema(const PackageObjectStream::FPackageInput& Input,
			std::string_view Name) -> const PackageObjectStream::FSchemaDescriptor*
		{
			const auto It = std::ranges::find(
				Input.Schemas, Name, &PackageObjectStream::FSchemaDescriptor::QualifiedName);
			return It == Input.Schemas.end() ? nullptr : &*It;
		}

		auto ToLinkerType(const PackageObjectStream::FPackageInput& Input,
			const PackageObjectStream::FTypeDescriptor& Source,
			ObjectPackage::FSerializedType& Out, std::string& OutError,
			uint32 Depth = 0) -> bool
		{
			if (Depth > ObjectPackage::DastV8MaximumValueDepth)
			{
				OutError = "Live reflected type exceeds the v8 nesting limit.";
				return false;
			}
			const auto Kind = LinkerKind(Source.Opcode);
			if (!Kind)
			{
				OutError = "Live reflected type has no linker representation.";
				return false;
			}
			ObjectPackage::FSerializedType Result{
				.Kind = *Kind,
				.QualifiedName = Source.QualifiedName,
				.Parameter = Source.Parameter};
			if (Source.Opcode == PackageObjectStream::ETypeOpcode::Enum)
			{
				const auto Storage = LinkerKind(
					static_cast<PackageObjectStream::ETypeOpcode>(Source.Parameter));
				if (!Storage)
				{
					OutError = "Live enum has invalid storage.";
					return false;
				}
				Result.Parameter = static_cast<uint64>(*Storage);
			}
			if (Source.Opcode == PackageObjectStream::ETypeOpcode::Struct)
			{
				const auto* Schema = FindSchema(Input, Source.QualifiedName);
				if (Schema) for (const auto& Field : Schema->Fields)
				{
					if (!Field.Type)
					{
						OutError = "Live struct schema has a null field type.";
						return false;
					}
					ObjectPackage::FSerializedType Child;
					if (!ToLinkerType(Input, *Field.Type, Child, OutError, Depth + 1))
						return false;
					Result.Children.push_back(std::move(Child));
				}
			}
			else for (const auto& ChildSource : Source.Children)
			{
				if (!ChildSource)
				{
					OutError = "Live reflected type has a null child.";
					return false;
				}
				ObjectPackage::FSerializedType Child;
				if (!ToLinkerType(Input, *ChildSource, Child, OutError, Depth + 1))
					return false;
				Result.Children.push_back(std::move(Child));
			}
			Out = std::move(Result);
			return true;
		}

		auto ToLinkerValue(const PackageObjectStream::FPackageInput& Input,
			const PackageObjectStream::FTypeDescriptor& Type,
			const PackageObjectStream::FValue& Source,
			ObjectPackage::FSerializedValue& Out,
			std::vector<std::string>& SoftReferences,
			size_t ExportCount, size_t ImportCount,
			std::string& OutError, uint32 Depth = 0) -> bool
		{
			if (Depth > ObjectPackage::DastV8MaximumValueDepth)
			{
				OutError = "Live reflected value exceeds the v8 nesting limit.";
				return false;
			}
			ObjectPackage::FSerializedValue Result{
				.Bool = Source.Bool,
				.Signed = Source.Signed,
				.Unsigned = Source.Unsigned,
				.FloatingBits = Source.FloatingBits,
				.Text = Source.Text,
				.Guid = Source.Guid,
				.Bytes = Source.Bytes,
				.ComponentBits = Source.ComponentBits,
				.FieldNames = Source.FieldNames};
			if (Type.Opcode == PackageObjectStream::ETypeOpcode::BulkData)
			{
				if (!Source.bDetachedBulk || Source.BulkElementSize == 0
					|| Source.BulkAlignment == 0 || Source.BulkStorage > 1)
				{
					OutError = "Live BulkData is not a detached linker value.";
					return false;
				}
				Result.BulkElementSize = static_cast<uint32>(Source.BulkElementSize);
				Result.BulkAlignment = Source.BulkAlignment;
				Result.BulkStorage = Source.BulkStorage == 0
					? ObjectPackage::EBulkStorageKind::Inline
					: ObjectPackage::EBulkStorageKind::External;
			}
			for (EDefaultDeltaProvenance Provenance : Source.Provenances)
				Result.Provenances.push_back(
					Provenance == EDefaultDeltaProvenance::Forced
						? ObjectPackage::EPropertyProvenance::Forced
						: ObjectPackage::EPropertyProvenance::Explicit);
			using O = PackageObjectStream::ETypeOpcode;
			if (Type.Opcode == O::HardRef)
			{
				if (Source.ReferenceTag == 0) Result.Reference = ObjectPackage::FPackageIndex::Null();
				else if (Source.ReferenceTag == 1 && Source.ReferenceId != 0
					&& Source.ReferenceId <= ExportCount)
					ObjectPackage::FPackageIndex::TryExport(Source.ReferenceId - 1, Result.Reference);
				else if (Source.ReferenceTag == 2 && Source.ReferenceId != 0
					&& Source.ReferenceId <= ImportCount)
					ObjectPackage::FPackageIndex::TryImport(Source.ReferenceId - 1, Result.Reference);
				else
				{
					OutError = "Live hard reference has an invalid package index.";
					return false;
				}
			}
			else if (Type.Opcode == O::SoftRef)
			{
				if (Source.ReferenceTag > 1)
				{
					OutError = "Live soft reference has an invalid tag.";
					return false;
				}
				if (Source.ReferenceTag == 1) SoftReferences.push_back(Source.Text);
			}

			auto ConvertChild = [&](const PackageObjectStream::FTypeDescriptor& ChildType,
				const PackageObjectStream::FValue& ChildValue) -> bool {
				ObjectPackage::FSerializedValue Child;
				if (!ToLinkerValue(Input, ChildType, ChildValue, Child,
					SoftReferences, ExportCount, ImportCount, OutError, Depth + 1))
					return false;
				Result.Elements.push_back(std::move(Child));
				return true;
			};
			if (Type.Opcode == O::Struct)
			{
				const auto* Schema = FindSchema(Input, Type.QualifiedName);
				if (!Schema || Source.FieldNames.size() != Source.Elements.size()
					|| Source.Provenances.size() != Source.Elements.size())
				{
					OutError = "Live struct value does not match its schema.";
					return false;
				}
				for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
				{
					const auto Field = std::ranges::find(
						Schema->Fields, Source.FieldNames[Index],
						&PackageObjectStream::FFieldDescriptor::Name);
					if (Field == Schema->Fields.end() || !Field->Type
						|| !ConvertChild(*Field->Type, Source.Elements[Index])) return false;
				}
			}
			else if (Type.Opcode == O::FixedArray || Type.Opcode == O::Array)
			{
				if (Type.Children.size() != 1 || !Type.Children[0])
				{
					OutError = "Live array has an invalid child type.";
					return false;
				}
				for (const auto& Element : Source.Elements)
					if (!ConvertChild(*Type.Children[0], Element)) return false;
			}
			else if (Type.Opcode == O::Map)
			{
				if (Type.Children.size() != 2 || !Type.Children[0] || !Type.Children[1]
					|| Source.Elements.size() % 2 != 0)
				{
					OutError = "Live map has an invalid shape.";
					return false;
				}
				for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
					if (!ConvertChild(*Type.Children[Index % 2], Source.Elements[Index]))
						return false;
			}
			Out = std::move(Result);
			return true;
		}

		auto BuildLinkerTables(const PackageObjectStream::FPackageInput& Input,
			std::string_view PackageName, ObjectPackage::FLinkerTables& Out,
			std::string& OutError) -> bool
		{
			if (Input.Objects.empty() || Input.Objects.size() != Input.ObjectValues.size())
			{
				OutError = "Live capture produced an invalid object/value topology.";
				return false;
			}
			ObjectPackage::FLinkerTables Result;
			Result.Summary.PackageName = PackageName;
			Result.Summary.AssetClass = Input.AssetClass;
			Result.Summary.bRedirect = Input.EntryKind == EAssetRegistryEntryKind::Redirector;
			Result.Summary.RedirectDestination = Input.RedirectDestination;
			Result.Summary.HardPackageReferences = Input.Dependencies;
			Result.Names = Input.AdditionalNames;
			ObjectPackage::FPackageIndex::TryExport(0, Result.Summary.MainExport);
			for (const auto& Dependency : Input.Dependencies)
				Result.Imports.push_back({.PackageName = Dependency});
			for (const auto& Version : Input.CustomVersions)
				Result.CustomVersions.push_back({Version.Guid, Version.Value,
					Version.EmissionValue, Version.MaximumSupported,
					Version.bCodecKnown, Version.bRequiredForInterpretation});
			for (const auto& SourceType : Input.Types)
			{
				if (!SourceType)
				{
					OutError = "Live capture produced a null type.";
					return false;
				}
				ObjectPackage::FSerializedType Type;
				if (!ToLinkerType(Input, *SourceType, Type, OutError)) return false;
				Result.Types.push_back(std::move(Type));
			}
			for (const auto& SourceSchema : Input.Schemas)
			{
				ObjectPackage::FSerializedSchema Schema{
					.QualifiedName = SourceSchema.QualifiedName};
				for (const auto& SourceField : SourceSchema.Fields)
				{
					if (!SourceField.Type)
					{
						OutError = "Live capture produced a null schema field type.";
						return false;
					}
					ObjectPackage::FSerializedType Type;
					if (!ToLinkerType(Input, *SourceField.Type, Type, OutError)) return false;
					Schema.Fields.push_back({SourceField.Name, std::move(Type),
						SourceField.AuthoredFlags});
				}
				Result.Schemas.push_back(std::move(Schema));
			}

			std::vector<const PackageObjectStream::FObjectDescriptor*> Objects;
			for (const auto& Object : Input.Objects) Objects.push_back(&Object);
			std::ranges::sort(Objects, [](const auto* A, const auto* B) {
				if (A->OuterPath.empty() != B->OuterPath.empty()) return A->OuterPath.empty();
				if (A->OuterPath != B->OuterPath) return A->OuterPath < B->OuterPath;
				if (A->ClassName != B->ClassName) return A->ClassName < B->ClassName;
				return A->ObjectName < B->ObjectName;
			});
			std::unordered_map<std::string_view, size_t> ExportByPath;
			for (size_t Index = 0; Index < Objects.size(); ++Index)
				ExportByPath.emplace(Objects[Index]->Path, Index);
			for (size_t Index = 0; Index < Objects.size(); ++Index)
			{
				const auto& Source = *Objects[Index];
				ObjectPackage::FPackageIndex Outer;
				if (!Source.OuterPath.empty())
				{
					const auto It = ExportByPath.find(Source.OuterPath);
					if (It == ExportByPath.end() || It->second >= Index
						|| !ObjectPackage::FPackageIndex::TryExport(It->second, Outer))
					{
						OutError = "Live capture produced invalid Outer topology.";
						return false;
					}
				}
				Result.Exports.push_back({.ObjectName = Source.ObjectName,
					.ClassName = Source.ClassName, .Outer = Outer});
			}
			for (size_t ExportIndex = 0; ExportIndex < Objects.size(); ++ExportIndex)
			{
				const auto Values = std::ranges::find(
					Input.ObjectValues, Objects[ExportIndex]->Path,
					&PackageObjectStream::FObjectValueInput::ObjectPath);
				if (Values == Input.ObjectValues.end() || !Values->RetainedUnknownOverrides.empty())
				{
					OutError = "Live capture contains missing or retained-unknown values.";
					return false;
				}
				for (const auto& Override : Values->KnownOverrides)
				{
					const auto* Schema = FindSchema(Input, Override.SchemaName);
					const auto Field = Schema ? std::ranges::find(
						Schema->Fields, Override.FieldName,
						&PackageObjectStream::FFieldDescriptor::Name)
						: std::vector<PackageObjectStream::FFieldDescriptor>::const_iterator{};
					if (!Schema || Field == Schema->Fields.end() || !Field->Type)
					{
						OutError = "Live property override is absent from its schema.";
						return false;
					}
					ObjectPackage::FSerializedType Type;
					ObjectPackage::FSerializedValue Value;
					if (!ToLinkerType(Input, *Field->Type, Type, OutError)
						|| !ToLinkerValue(Input, *Field->Type, Override.Value, Value,
							Result.Summary.SoftPackageReferences, Objects.size(),
							Input.Dependencies.size(), OutError)) return false;
					Result.Exports[ExportIndex].Properties.push_back({
						.DeclaringType = Override.SchemaName,
						.FieldName = Override.FieldName,
						.Type = std::move(Type),
						.Provenance = Override.Provenance == EDefaultDeltaProvenance::Forced
							? ObjectPackage::EPropertyProvenance::Forced
							: ObjectPackage::EPropertyProvenance::Explicit,
						.Value = std::move(Value)});
				}
			}
			std::ranges::sort(Result.Summary.SoftPackageReferences);
			Result.Summary.SoftPackageReferences.erase(std::ranges::unique(
				Result.Summary.SoftPackageReferences).begin(),
				Result.Summary.SoftPackageReferences.end());
			Out = std::move(Result);
			return true;
		}

		auto StreamOpcode(ObjectPackage::EValueKind Kind)
			-> std::optional<PackageObjectStream::ETypeOpcode>
		{
			using K = ObjectPackage::EValueKind;
			using O = PackageObjectStream::ETypeOpcode;
			switch (Kind)
			{
			case K::Bool: return O::Bool;
			case K::I8: return O::I8;
			case K::I16: return O::I16;
			case K::I32: return O::I32;
			case K::I64: return O::I64;
			case K::U8: return O::U8;
			case K::U16: return O::U16;
			case K::U32: return O::U32;
			case K::U64: return O::U64;
			case K::F32: return O::F32;
			case K::F64: return O::F64;
			case K::String: return O::String;
			case K::Name: return O::Name;
			case K::Guid: return O::Guid;
			case K::Enum: return O::Enum;
			case K::Intrinsic: return O::Intrinsic;
			case K::Struct: return O::Struct;
			case K::FixedArray: return O::FixedArray;
			case K::Array: return O::Array;
			case K::Map: return O::Map;
			case K::HardReference: return O::HardRef;
			case K::SoftReference: return O::SoftRef;
			case K::Byte: return O::U8;
			case K::Bytes: return O::Bytes;
			case K::BulkData: return O::BulkData;
			}
			return std::nullopt;
		}

		auto ToStreamType(const ObjectPackage::FSerializedType& Source,
			PackageObjectStream::FTypePtr& Out, std::string& OutError) -> bool
		{
			const auto Opcode = StreamOpcode(Source.Kind);
			if (!Opcode)
			{
				OutError = "Linker application encountered an unsupported value kind.";
				return false;
			}
			uint64 Parameter = Source.Parameter;
			if (Source.Kind == ObjectPackage::EValueKind::Enum)
			{
				const auto Storage = StreamOpcode(
					static_cast<ObjectPackage::EValueKind>(Source.Parameter));
				if (!Storage)
				{
					OutError = "Linker application encountered invalid enum storage.";
					return false;
				}
				Parameter = static_cast<uint64>(*Storage);
			}
			std::vector<PackageObjectStream::FTypePtr> Children;
			if (Source.Kind != ObjectPackage::EValueKind::Struct)
				for (const auto& Child : Source.Children)
				{
					PackageObjectStream::FTypePtr Converted;
					if (!ToStreamType(Child, Converted, OutError)) return false;
					Children.push_back(std::move(Converted));
				}
			Out = PackageObjectStream::MakeType(
				*Opcode, Source.QualifiedName, Parameter, std::move(Children));
			return true;
		}

		auto MakeBulkDescriptor(const ObjectPackage::FSerializedValue& Value,
			uint64 FieldIndex) -> std::vector<std::byte>
		{
			const FXxHash128 Hash = FXxHash128::HashBuffer(Value.Bytes);
			FGuid PayloadId{
				static_cast<uint32>(Hash.HashLow),
				static_cast<uint32>(Hash.HashLow >> 32),
				static_cast<uint32>(Hash.HashHigh),
				static_cast<uint32>(Hash.HashHigh >> 32)};
			if (!PayloadId.IsValid()) PayloadId.A = 1;
			FBinaryWriter Writer;
			Writer.WriteU64(FieldIndex);
			const bool bExternal = Value.BulkStorage
				== ObjectPackage::EBulkStorageKind::External;
			Writer.WriteU8(bExternal ? 1 : 0);
			Writer.WriteU8(0);
			Writer.WriteU16(static_cast<uint16>(Value.BulkAlignment));
			Writer.WriteU32(1);
			Writer.WriteGuid(PayloadId);
			Writer.WriteHash128(Hash);
			Writer.WriteU64(Value.Bytes.size());
			Writer.WriteU64(Value.Bytes.size());
			Writer.WriteU64(bExternal ? Value.BulkOffset : 0);
			if (!bExternal) Writer.WriteBytes(Value.Bytes);
			return Writer.TakeBytes();
		}

		auto ToStreamValue(const ObjectPackage::FSerializedType& Type,
			const ObjectPackage::FSerializedValue& Source,
			const ObjectPackage::FLinkerTables& Linker,
			PackageObjectStream::FPackageInput& Input,
			PackageObjectStream::FValue& Out,
			uint64& BulkFieldIndex,
			std::string& OutError) -> bool
		{
			Out.Bool = Source.Bool;
			Out.Signed = Source.Signed;
			Out.Unsigned = Source.Unsigned;
			Out.FloatingBits = Source.FloatingBits;
			Out.Text = Source.Text;
			Out.Guid = Source.Guid;
			Out.Bytes = Type.Kind == ObjectPackage::EValueKind::BulkData
				? MakeBulkDescriptor(Source, ++BulkFieldIndex) : Source.Bytes;
			Out.ComponentBits = Source.ComponentBits;
			Out.FieldNames = Source.FieldNames;
			for (auto Provenance : Source.Provenances)
				Out.Provenances.push_back(Provenance == ObjectPackage::EPropertyProvenance::Forced
					? EDefaultDeltaProvenance::Forced : EDefaultDeltaProvenance::Explicit);
			if (Type.Kind == ObjectPackage::EValueKind::Name && !Source.Text.empty())
				Input.AdditionalNames.push_back(Source.Text);
			if (Type.Kind == ObjectPackage::EValueKind::SoftReference && !Source.Text.empty())
			{
				Out.ReferenceTag = 1;
				Input.AdditionalNames.push_back(Source.Text);
			}
			if (Type.Kind == ObjectPackage::EValueKind::HardReference)
			{
				if (Source.Reference.IsNull()) Out.ReferenceTag = 0;
				else if (Source.Reference.IsExport())
				{
					Out.ReferenceTag = 1;
					Out.ReferenceId = Source.Reference.GetTableIndex() + 1;
				}
				else
				{
					const auto& Import = Linker.Imports[Source.Reference.GetTableIndex()];
					const auto Dependency = std::ranges::find(
						Input.Dependencies, Import.PackageName);
					if (Dependency == Input.Dependencies.end())
					{
						OutError = "Linker import is absent from hard package references.";
						return false;
					}
					Out.ReferenceTag = 2;
					Out.ReferenceId = std::distance(Input.Dependencies.begin(), Dependency) + 1;
				}
			}
			if (Type.Kind == ObjectPackage::EValueKind::Struct)
			{
				if (Type.Children.size() != Source.Elements.size())
				{
					OutError = "Linker struct descriptor/value shape differs.";
					return false;
				}
				for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
				{
					PackageObjectStream::FValue Child;
					if (!ToStreamValue(Type.Children[Index], Source.Elements[Index],
						Linker, Input, Child, BulkFieldIndex, OutError)) return false;
					Out.Elements.push_back(std::move(Child));
				}
			}
			else if (Type.Kind == ObjectPackage::EValueKind::FixedArray
				|| Type.Kind == ObjectPackage::EValueKind::Array)
			{
				if (Type.Children.size() != 1)
				{
					OutError = "Linker array descriptor is invalid.";
					return false;
				}
				for (const auto& Element : Source.Elements)
				{
					PackageObjectStream::FValue Child;
					if (!ToStreamValue(Type.Children[0], Element, Linker, Input,
						Child, BulkFieldIndex, OutError)) return false;
					Out.Elements.push_back(std::move(Child));
				}
			}
			else if (Type.Kind == ObjectPackage::EValueKind::Map)
			{
				if (Type.Children.size() != 2 || Source.Elements.size() % 2 != 0)
				{
					OutError = "Linker map descriptor/value shape is invalid.";
					return false;
				}
				for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
				{
					PackageObjectStream::FValue Child;
					if (!ToStreamValue(Type.Children[Index % 2], Source.Elements[Index],
						Linker, Input, Child, BulkFieldIndex, OutError)) return false;
					Out.Elements.push_back(std::move(Child));
				}
			}
			return true;
		}

		auto BuildStreamInput(const ObjectPackage::FLinkerTables& Linker,
			PackageObjectStream::FPackageInput& Out, std::string& OutError) -> bool
		{
			PackageObjectStream::FPackageInput Input;
			Input.AssetClass = Linker.Summary.AssetClass;
			Input.EntryKind = Linker.Summary.bRedirect
				? EAssetRegistryEntryKind::Redirector : EAssetRegistryEntryKind::Asset;
			Input.RedirectDestination = Linker.Summary.RedirectDestination;
			Input.Dependencies = Linker.Summary.HardPackageReferences;
			Input.AdditionalNames = Linker.Names;
			for (const auto& Type : Linker.Types)
			{
				PackageObjectStream::FTypePtr Converted;
				if (!ToStreamType(Type, Converted, OutError)) return false;
				Input.Types.push_back(std::move(Converted));
			}
			for (const auto& Schema : Linker.Schemas)
			{
				PackageObjectStream::FSchemaDescriptor Converted{
					.QualifiedName = Schema.QualifiedName};
				for (const auto& Field : Schema.Fields)
				{
					PackageObjectStream::FTypePtr Type;
					if (!ToStreamType(Field.Type, Type, OutError)) return false;
					Converted.Fields.push_back({Field.Name, std::move(Type), Field.AuthoredFlags});
				}
				Input.Schemas.push_back(std::move(Converted));
			}
			for (const auto& Version : Linker.CustomVersions)
				Input.CustomVersions.push_back({Version.Guid, Version.Value,
					Version.EmissionValue, Version.MaximumSupported,
					Version.bCodecKnown, Version.bRequiredForInterpretation});
			std::vector<std::string> Paths(Linker.Exports.size());
			// Build paths without relying on wire ids or reinterpretation.
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				ObjectPackage::FPackageIndex ExportIndex;
				if (!ObjectPackage::FPackageIndex::TryExport(Index, ExportIndex)
					|| !Linker.TryResolvePath(ExportIndex, Paths[Index]))
				{
					OutError = "Linker export topology cannot resolve a path.";
					return false;
				}
				const auto& Export = Linker.Exports[Index];
				std::string OuterPath;
				if (Export.Outer.IsExport())
					OuterPath = Paths[Export.Outer.GetTableIndex()];
				Input.Objects.push_back({Paths[Index], OuterPath,
					Export.ClassName, Export.ObjectName});
			}
			uint64 BulkFieldIndex = 0;
			for (size_t Index = 0; Index < Linker.Exports.size(); ++Index)
			{
				PackageObjectStream::FObjectValueInput Values{
					.ObjectPath = Paths[Index]};
				for (const auto& Property : Linker.Exports[Index].Properties)
				{
					PackageObjectStream::FValue Value;
					if (!ToStreamValue(Property.Type, Property.Value, Linker,
						Input, Value, BulkFieldIndex, OutError)) return false;
					Values.KnownOverrides.push_back({Property.DeclaringType,
						Property.FieldName,
						Property.Provenance == ObjectPackage::EPropertyProvenance::Forced
							? EDefaultDeltaProvenance::Forced
							: EDefaultDeltaProvenance::Explicit,
						std::move(Value)});
				}
				Input.ObjectValues.push_back(std::move(Values));
			}
			Out = std::move(Input);
			return true;
		}

		auto BuildDecodedPackage(const PackageObjectStream::FPackageInput& Input,
			PackageObjectStream::FDecodedPackage& Out, std::string& OutError) -> bool
		{
			using namespace PackageObjectStream;
			FDecodedPackage Result;
			Result.Header.AssetClass = Input.AssetClass;
			Result.Header.EntryKind = Input.EntryKind;
			Result.Header.RedirectDestination = Input.RedirectDestination;
			Result.Header.Dependencies = Input.Dependencies;
			Result.Header.ObjectCount = Input.Objects.size();
			Result.Names = Input.AdditionalNames;
			Result.CustomVersions.reserve(Input.CustomVersions.size());
			for (const auto& Version : Input.CustomVersions)
				Result.CustomVersions.push_back({Version.Guid, Version.Value,
					Version.EmissionValue, Version.MaximumSupported,
					Version.bCodecKnown, Version.bRequiredForInterpretation});

			std::unordered_map<std::string, uint64> TypeIds;
			std::function<uint64(const FTypeDescriptor&)> InternType =
				[&](const FTypeDescriptor& Type) -> uint64 {
					std::vector<uint64> Children;
					if (Type.Opcode != ETypeOpcode::Struct)
						for (const auto& Child : Type.Children)
						{
							if (!Child) return 0;
							const uint64 ChildId = InternType(*Child);
							if (ChildId == 0) return 0;
							Children.push_back(ChildId);
						}
					std::string Key = std::format("{}:{}:{}", static_cast<uint32>(Type.Opcode),
						Type.Parameter, Type.QualifiedName);
					for (uint64 Child : Children) Key += std::format(":{}", Child);
					if (const auto Existing = TypeIds.find(Key); Existing != TypeIds.end())
						return Existing->second;
					Result.Types.push_back({Type.Opcode, Type.QualifiedName,
						Type.Parameter, std::move(Children)});
					const uint64 Id = Result.Types.size();
					TypeIds.emplace(std::move(Key), Id);
					return Id;
				};
			for (const auto& Schema : Input.Schemas)
			{
				FDecodedSchema Decoded{.QualifiedName = Schema.QualifiedName};
				for (const auto& Field : Schema.Fields)
				{
					const uint64 TypeId = Field.Type ? InternType(*Field.Type) : 0;
					if (TypeId == 0)
					{
						OutError = "Live linker schema contains an invalid field type.";
						return false;
					}
					Decoded.Fields.push_back({Field.Name, TypeId, Field.AuthoredFlags});
				}
				Result.Schemas.push_back(std::move(Decoded));
			}
			for (size_t Index = 0; Index < Input.Objects.size(); ++Index)
			{
				const auto& Object = Input.Objects[Index];
				uint64 OuterId = 0;
				if (!Object.OuterPath.empty())
				{
					const auto Outer = std::ranges::find(
						std::span(Input.Objects).first(Index), Object.OuterPath,
						&FObjectDescriptor::Path);
					if (Outer == std::span(Input.Objects).first(Index).end())
					{
						OutError = "Live linker object Outer follows its child.";
						return false;
					}
					OuterId = std::distance(Input.Objects.begin(), Outer) + 1;
				}
				Result.Objects.push_back({Index + 1, OuterId, Object.Path,
					Object.ClassName, Object.ObjectName});
				const auto Values = std::ranges::find(
					Input.ObjectValues, Object.Path, &FObjectValueInput::ObjectPath);
				if (Values == Input.ObjectValues.end())
				{
					OutError = "Live linker object has no value record.";
					return false;
				}
				FDecodedObjectValues DecodedValues;
				for (const auto& Override : Values->KnownOverrides)
				{
					const auto Schema = std::ranges::find(
						Input.Schemas, Override.SchemaName,
						&FSchemaDescriptor::QualifiedName);
					if (Schema == Input.Schemas.end())
					{
						OutError = "Live linker override schema is absent.";
						return false;
					}
					const auto Field = std::ranges::find(
						Schema->Fields, Override.FieldName, &FFieldDescriptor::Name);
					if (Field == Schema->Fields.end())
					{
						OutError = "Live linker override field is absent.";
						return false;
					}
					DecodedValues.Overrides.push_back({
						.SchemaId = static_cast<uint64>(
							std::distance(Input.Schemas.begin(), Schema)) + 1,
						.FieldId = static_cast<uint64>(
							std::distance(Schema->Fields.begin(), Field)) + 1,
						.Provenance = Override.Provenance == EDefaultDeltaProvenance::Forced
							? uint8{1} : uint8{0},
						.Value = Override.Value});
				}
				Result.ObjectValues.push_back(std::move(DecodedValues));
			}
			Out = std::move(Result);
			return true;
		}
	}

	auto CaptureLivePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
		const FAssetPackageSerializationOptions& Options,
		ObjectPackage::FLinkerTables& OutLinker,
		std::string* OutError) -> FAssetResult
	{
		std::vector<FEditorBulkDataStoragePayload> Payloads;
		FAssetPackageSerializationOptions EffectiveOptions = Options;
		EffectiveOptions.EditorBulkDataStoragePayloads = &Payloads;
		PackageObjectStream::FPackageInput Input;
		PackageObjectStream::FWriterDiagnostic WriterDiagnostic;
		if (FAssetResult Result = PackageObjectStream::CaptureAssetPackage(Package,
			Input, {.DeltaMode = DeltaMode, .Serialization = EffectiveOptions},
			&WriterDiagnostic); !Result) return Result;
		ObjectPackage::FLinkerTables Linker;
		std::string AdapterError;
		if (!Package || !BuildLinkerTables(
			Input, Package->GetPackagePath(), Linker, AdapterError))
			return Error(AdapterError.empty()
				? "Live package has no linker identity." : std::move(AdapterError), OutError);
		OutLinker = std::move(Linker);
		if (OutError) OutError->clear();
		return {};
	}

	auto ApplyLivePackageLinker(const ObjectPackage::FLinkerTables& Linker,
		const FAssetPath& PackagePath, DPackage*& OutPackage,
		FAssetLoadReport* OutReport,
		const PackageObjectStream::FLiveLoadOptions& Options,
		std::string* OutError) -> FAssetResult
	{
		OutPackage = nullptr;
		PackageObjectStream::FPackageInput Input;
		std::string ErrorMessage;
		if (!BuildStreamInput(Linker, Input, ErrorMessage))
			return Error(std::move(ErrorMessage), OutError);
		PackageObjectStream::FDecodedPackage Decoded;
		if (!BuildDecodedPackage(Input, Decoded, ErrorMessage))
			return Error(std::move(ErrorMessage), OutError);
		PackageObjectStream::FLoadedAssetPackage Loaded;
		PackageObjectStream::FReaderDiagnostic ReaderDiagnostic;
		auto EffectiveOptions = Options;
		EffectiveOptions.SourceFormatVersion = AssetPackageV8FormatVersion;
		FAssetResult Result = PackageObjectStream::LoadDecodedAssetPackage(
			std::move(Decoded), PackagePath, Loaded, OutReport,
			EffectiveOptions, &ReaderDiagnostic);
		if (!Result)
		{
			if (OutError) *OutError = ReaderDiagnostic.Message;
			return Result;
		}
		OutPackage = Loaded.Release();
		if (OutError) OutError->clear();
		return {};
	}
}
