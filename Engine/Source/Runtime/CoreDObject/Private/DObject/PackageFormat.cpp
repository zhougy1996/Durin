#include "DObject/PackageFormat.h"

#include "DObject/CanonicalMapKey.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

namespace Durin::ObjectPackage
{
	namespace
	{
		constexpr uint64 PreambleBytes = BinaryEnvelopePreambleBytes;
		constexpr uint64 FormatHeaderOffset = DastV8FormatHeaderOffset;
		constexpr uint64 DirectoryOffset = DastV8DirectoryOffset;
		constexpr uint64 FirstSectionOffset = DirectoryOffset
			+ uint64(DastV8SectionCount) * DastV8SectionEntryBytes;

		static_assert(PreambleBytes == 64);
		static_assert(FormatHeaderOffset == 64);
		static_assert(DirectoryOffset == 96);
		static_assert(FirstSectionOffset == 528);
		static_assert(FirstSectionOffset == DastV8FirstSectionOffset);

		struct FBulkOccurrence
		{
			const FSerializedValue* Value = nullptr;
			uint32 ExportId = 0;
			uint32 SchemaId = 0;
			uint32 FieldId = 0;
			std::string LogicalPath;
		};

		struct FFrozenPackage
		{
			const FLinkerTables* Source = nullptr;
			bool bV9 = false;
			std::vector<std::string> Names;
			std::vector<FSerializedType> Types;
			std::vector<FSerializedSchema> Schemas;
			std::vector<FCustomVersion> CustomVersions;
			std::vector<uint32> ImportOrder;
			std::vector<uint32> ExportOrder;
			std::vector<uint32> ImportRemap;
			std::vector<uint32> ExportRemap;
			std::vector<std::string> ImportPaths;
			std::vector<std::string> ExportPaths;
			std::vector<FBulkOccurrence> BulkValues;
			FPackageWriterManifest Manifest;
		};

		struct FSection
		{
			EDastV8Section Kind{};
			std::vector<std::byte> Bytes;
			uint64 Offset = 0;
			FXxHash128 Hash;
		};

		auto Fail(FPackageWriterDiagnostic* Diagnostic, EPackageWriterFailure Failure,
			std::string Message, std::string Path = {}) -> bool
		{
			if (Diagnostic) *Diagnostic = {Failure, std::move(Path), std::move(Message)};
			return false;
		}

		auto BytewiseLess(std::string_view Left, std::string_view Right) -> bool
		{
			return std::lexicographical_compare(Left.begin(), Left.end(), Right.begin(), Right.end(),
				[](char A, char B) { return static_cast<uint8>(A) < static_cast<uint8>(B); });
		}

		auto CompareIdentity(std::initializer_list<std::pair<std::string_view, std::string_view>> Parts) -> int
		{
			for (const auto& [Left, Right] : Parts)
			{
				if (BytewiseLess(Left, Right)) return -1;
				if (BytewiseLess(Right, Left)) return 1;
			}
			return 0;
		}

		auto SignedFits(EValueKind Kind, int64 Value) -> bool
		{
			switch (Kind)
			{
			case EValueKind::I8: return Value >= std::numeric_limits<int8>::min() && Value <= std::numeric_limits<int8>::max();
			case EValueKind::I16: return Value >= std::numeric_limits<int16>::min() && Value <= std::numeric_limits<int16>::max();
			case EValueKind::I32: return Value >= std::numeric_limits<int32>::min() && Value <= std::numeric_limits<int32>::max();
			case EValueKind::I64: return true;
			default: return false;
			}
		}

		auto UnsignedFits(EValueKind Kind, uint64 Value) -> bool
		{
			switch (Kind)
			{
			case EValueKind::U8: return Value <= std::numeric_limits<uint8>::max();
			case EValueKind::U16: return Value <= std::numeric_limits<uint16>::max();
			case EValueKind::U32: return Value <= std::numeric_limits<uint32>::max();
			case EValueKind::U64: return true;
			default: return false;
			}
		}

		auto IsValidUtf8(std::string_view Value) -> bool
		{
			for (size_t Index = 0; Index < Value.size();)
			{
				const uint8 Lead = static_cast<uint8>(Value[Index++]);
				if (Lead < 0x80) continue;
				uint32 Code = 0;
				size_t Continuations = 0;
				if ((Lead & 0xe0) == 0xc0) { Code = Lead & 0x1f; Continuations = 1; }
				else if ((Lead & 0xf0) == 0xe0) { Code = Lead & 0x0f; Continuations = 2; }
				else if ((Lead & 0xf8) == 0xf0) { Code = Lead & 0x07; Continuations = 3; }
				else return false;
				if (Index + Continuations > Value.size()) return false;
				for (size_t Part = 0; Part < Continuations; ++Part)
				{
					const uint8 Next = static_cast<uint8>(Value[Index++]);
					if ((Next & 0xc0) != 0x80) return false;
					Code = (Code << 6) | (Next & 0x3f);
				}
				if ((Continuations == 1 && Code < 0x80) || (Continuations == 2 && Code < 0x800)
					|| (Continuations == 3 && Code < 0x10000) || Code > 0x10ffff
					|| (Code >= 0xd800 && Code <= 0xdfff)) return false;
			}
			return true;
		}

		auto AddName(std::vector<std::string>& Names, std::string_view Name,
			FPackageWriterDiagnostic* Diagnostic, std::string_view Path, bool bNullable = false) -> bool
		{
			if (Name.empty())
				return bNullable || Fail(Diagnostic, EPackageWriterFailure::InvalidInput,
					"A required package name is empty.", std::string(Path));
			if (Name.size() > DastV8MaximumStringBytes)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A package name exceeds the v8 string limit.", std::string(Path));
			if (!IsValidUtf8(Name))
				return Fail(Diagnostic, EPackageWriterFailure::InvalidUtf8,
					"A package name is not valid UTF-8.", std::string(Path));
			Names.emplace_back(Name);
			return true;
		}

		auto FindNameId(const FFrozenPackage& Frozen, std::string_view Name) -> uint32
		{
			if (Name.empty()) return 0;
			const auto It = std::lower_bound(Frozen.Names.begin(), Frozen.Names.end(), Name,
				[](const std::string& Left, std::string_view Right) { return BytewiseLess(Left, Right); });
			return It == Frozen.Names.end() || *It != Name ? 0
				: static_cast<uint32>(std::distance(Frozen.Names.begin(), It) + 1);
		}

		auto FindTypeId(const FFrozenPackage& Frozen, const FSerializedType& Type) -> uint32
		{
			const auto It = std::lower_bound(Frozen.Types.begin(), Frozen.Types.end(), Type);
			return It == Frozen.Types.end() || *It != Type ? 0
				: static_cast<uint32>(std::distance(Frozen.Types.begin(), It) + 1);
		}

		auto FindSchemaId(const FFrozenPackage& Frozen, std::string_view Name) -> uint32
		{
			const auto It = std::lower_bound(Frozen.Schemas.begin(), Frozen.Schemas.end(), Name,
				[](const FSerializedSchema& Left, std::string_view Right)
				{ return BytewiseLess(Left.QualifiedName, Right); });
			return It == Frozen.Schemas.end() || It->QualifiedName != Name ? 0
				: static_cast<uint32>(std::distance(Frozen.Schemas.begin(), It) + 1);
		}

		auto ValidateType(const FSerializedType& Type, uint32 Depth,
			FPackageWriterDiagnostic* Diagnostic, std::string_view Path) -> bool
		{
			if (Depth > DastV8MaximumValueDepth)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A serialized type exceeds the v8 nesting limit.", std::string(Path));
			const size_t Children = Type.Children.size();
			switch (Type.Kind)
			{
			case EValueKind::Enum:
				if (Type.QualifiedName.empty() || Children != 0
					|| Type.Parameter < uint64(EValueKind::I8) || Type.Parameter > uint64(EValueKind::U64))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"An enum type has an invalid name, storage kind, or child list.", std::string(Path));
				break;
			case EValueKind::Intrinsic:
				if (Children != 0 || Type.Parameter < 1 || Type.Parameter > 6)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"An intrinsic type has an invalid layout.", std::string(Path));
				break;
			case EValueKind::Struct:
				if (Type.QualifiedName.empty())
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"A struct type has no qualified name.", std::string(Path));
				break;
			case EValueKind::FixedArray:
				if (Children != 1 || Type.Parameter > DastV8MaximumContainerElements)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"A fixed-array type has an invalid element descriptor or count.", std::string(Path));
				break;
			case EValueKind::Array:
				if (Children != 1)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"An array type must have one child type.", std::string(Path));
				break;
			case EValueKind::Map:
				if (Children != 2)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"A Map type must have key and value child types.", std::string(Path));
				break;
			default:
				if (Children != 0)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidType,
						"A scalar type cannot have child descriptors.", std::string(Path));
				break;
			}
			for (const FSerializedType& Child : Type.Children)
				if (!ValidateType(Child, Depth + 1, Diagnostic, Path)) return false;
			return true;
		}

		auto CollectType(const FSerializedType& Type, std::vector<FSerializedType>& Types,
			std::vector<std::string>& Names, FPackageWriterDiagnostic* Diagnostic,
			std::string_view Path) -> bool
		{
			if (!ValidateType(Type, 0, Diagnostic, Path)) return false;
			if (!Type.QualifiedName.empty() && !AddName(Names, Type.QualifiedName, Diagnostic, Path)) return false;
			for (const FSerializedType& Child : Type.Children)
				if (!CollectType(Child, Types, Names, Diagnostic, Path)) return false;
			Types.push_back(Type);
			return true;
		}

		auto ResolvePaths(const FLinkerTables& Linker, bool bImports,
			std::vector<std::string>& Out, FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			const size_t Count = bImports ? Linker.Imports.size() : Linker.Exports.size();
			Out.resize(Count);
			for (size_t Index = 0; Index < Count; ++Index)
			{
				FPackageIndex PackageIndex;
				const bool bIndexOk = bImports ? FPackageIndex::TryImport(Index, PackageIndex)
					: FPackageIndex::TryExport(Index, PackageIndex);
				FLinkerDiagnostic LinkerDiagnostic;
				if (!bIndexOk || !Linker.TryResolvePath(PackageIndex, Out[Index], &LinkerDiagnostic)
					|| Out[Index].empty())
					return Fail(Diagnostic,
						LinkerDiagnostic.Failure == ELinkerFailure::InvalidTopology
							? EPackageWriterFailure::InvalidTopology : EPackageWriterFailure::InvalidIndex,
						"A package table path cannot be resolved.",
						std::string(bImports ? "Imports[" : "Exports[") + std::to_string(Index) + "]");
			}
			return true;
		}

		auto RemapIndex(const FFrozenPackage& Frozen, FPackageIndex Index, int64& Out,
			FPackageWriterDiagnostic* Diagnostic, std::string_view Path) -> bool
		{
			if (Index.IsNull()) { Out = 0; return true; }
			if (Index.IsImport())
			{
				if (Index.GetTableIndex() >= Frozen.ImportRemap.size())
					return Fail(Diagnostic, EPackageWriterFailure::InvalidIndex,
						"An import index is out of range.", std::string(Path));
				Out = -int64(Frozen.ImportRemap[Index.GetTableIndex()] + 1);
				return true;
			}
			if (Index.GetTableIndex() >= Frozen.ExportRemap.size())
				return Fail(Diagnostic, EPackageWriterFailure::InvalidIndex,
					"An export index is out of range.", std::string(Path));
			Out = int64(Frozen.ExportRemap[Index.GetTableIndex()] + 1);
			return true;
		}

		auto CollectValue(FFrozenPackage& Frozen, const FSerializedType& Type,
			const FSerializedValue& Value, uint32 ExportId, uint32 SchemaId, uint32 FieldId,
			std::string Path, uint32 Depth, FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			if (Depth > DastV8MaximumValueDepth)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A serialized value exceeds the v8 nesting limit.", std::move(Path));
			switch (Type.Kind)
			{
			case EValueKind::I8: case EValueKind::I16: case EValueKind::I32: case EValueKind::I64:
				if (!SignedFits(Type.Kind, Value.Signed))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"A signed value is out of range for its type.", Path);
				break;
			case EValueKind::U8: case EValueKind::U16: case EValueKind::U32: case EValueKind::U64:
				if (!UnsignedFits(Type.Kind, Value.Unsigned))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"An unsigned value is out of range for its type.", Path);
				break;
			case EValueKind::F32:
				if (Value.FloatingBits > std::numeric_limits<uint32>::max())
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"An F32 value contains bits outside its storage width.", Path);
				break;
			case EValueKind::Enum:
			{
				const EValueKind Storage = static_cast<EValueKind>(Type.Parameter);
				if ((Storage >= EValueKind::I8 && Storage <= EValueKind::I64 && !SignedFits(Storage, Value.Signed))
					|| (Storage >= EValueKind::U8 && Storage <= EValueKind::U64 && !UnsignedFits(Storage, Value.Unsigned)))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"An enum value is out of range for its storage type.", Path);
				break;
			}
			case EValueKind::String:
				if (Value.Text.size() > DastV8MaximumStringBytes || !IsValidUtf8(Value.Text))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"A serialized string is invalid or exceeds the v8 limit.", std::move(Path));
				break;
			case EValueKind::Name:
			case EValueKind::SoftReference:
				if (!AddName(Frozen.Names, Value.Text, Diagnostic, Path,
					Type.Kind == EValueKind::SoftReference)) return false;
				break;
			case EValueKind::HardReference:
			{
				int64 Ignored = 0;
				if (!RemapIndex(Frozen, Value.Reference, Ignored, Diagnostic, Path)) return false;
				break;
			}
			case EValueKind::Intrinsic:
			{
				const uint64 Count = Type.Parameter == 1 ? 2 : Type.Parameter == 2 ? 3
					: (Type.Parameter == 3 || Type.Parameter == 4 || Type.Parameter == 6) ? 4
					: Type.Parameter == 5 ? 10 : 0;
				if (Value.ComponentBits.size() != Count)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"An intrinsic value has the wrong component count.", std::move(Path));
				break;
			}
			case EValueKind::Struct:
				if (Value.Elements.size() != Type.Children.size()
					|| Value.FieldNames.size() != Value.Elements.size()
					|| (!Value.Provenances.empty() && Value.Provenances.size() != Value.Elements.size()))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"A struct value does not match its descriptor.", std::move(Path));
				{
					std::vector<size_t> Order(Value.Elements.size());
					std::iota(Order.begin(), Order.end(), 0);
					std::ranges::sort(Order, [&](size_t A, size_t B)
					{ return BytewiseLess(Value.FieldNames[A], Value.FieldNames[B]); });
					for (size_t Position = 0; Position < Order.size(); ++Position)
					{
						const size_t Index = Order[Position];
						if (Position != 0 && Value.FieldNames[Order[Position - 1]] == Value.FieldNames[Index])
							return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
								"A struct contains duplicate field names.", Path);
						if (!AddName(Frozen.Names, Value.FieldNames[Index], Diagnostic, Path)) return false;
						if (!CollectValue(Frozen, Type.Children[Index], Value.Elements[Index], ExportId,
							SchemaId, FieldId, Path + "." + Value.FieldNames[Index], Depth + 1, Diagnostic)) return false;
					}
				}
				break;
			case EValueKind::FixedArray:
				if (Value.Elements.size() != Type.Parameter)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"A fixed-array value has the wrong element count.", std::move(Path));
				[[fallthrough]];
			case EValueKind::Array:
				if (Value.Elements.size() > DastV8MaximumContainerElements)
					return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
						"An array exceeds the v8 element limit.", std::move(Path));
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!CollectValue(Frozen, Type.Children.front(), Value.Elements[Index], ExportId,
						SchemaId, FieldId, Path + "[" + std::to_string(Index) + "]", Depth + 1, Diagnostic)) return false;
				break;
			case EValueKind::Map:
				if ((Value.Elements.size() % 2) != 0 || Value.Elements.size() / 2 > DastV8MaximumContainerElements)
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"A Map has an invalid entry count.", std::move(Path));
				{
					struct FEntry { size_t Index; std::vector<std::byte> Token; };
					std::vector<FEntry> Entries;
					for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
					{
						std::vector<std::byte> Token;
						std::string Error;
						if (!BuildCanonicalMapKeyToken(Type.Children[0], Value.Elements[Index], Token, &Error))
							return Fail(Diagnostic, EPackageWriterFailure::InvalidValue, std::move(Error), Path);
						Entries.push_back({Index, std::move(Token)});
					}
					std::ranges::sort(Entries, [](const FEntry& A, const FEntry& B) { return A.Token < B.Token; });
					for (size_t Position = 0; Position < Entries.size(); ++Position)
					{
						if (Position != 0 && Entries[Position - 1].Token == Entries[Position].Token)
							return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
								"A Map contains colliding canonical keys.", Path);
						const size_t Index = Entries[Position].Index;
						const std::string EntryPath = Path + "[" + std::to_string(Position) + "]";
						if (!CollectValue(Frozen, Type.Children[0], Value.Elements[Index], ExportId,
							SchemaId, FieldId, EntryPath + ".Key", Depth + 1, Diagnostic)
							|| !CollectValue(Frozen, Type.Children[1], Value.Elements[Index + 1], ExportId,
							SchemaId, FieldId, EntryPath + ".Value", Depth + 1, Diagnostic)) return false;
					}
				}
				break;
			case EValueKind::Bytes:
				if (Value.Bytes.size() > DastV8MaximumPackageBytes)
					return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
						"A byte blob exceeds the v8 package limit.", std::move(Path));
				break;
			case EValueKind::BulkData:
			{
				const uint64 BulkSize = Value.bBulkPayloadAvailable
					? Value.Bytes.size() : Value.BulkStoredSize;
				if (Value.BulkStorage == EBulkStorageKind::Unset || Value.BulkElementSize == 0
					|| Value.BulkAlignment == 0 || Value.BulkAlignment > 4096
					|| (Value.BulkAlignment & (Value.BulkAlignment - 1)) != 0
					|| BulkSize > DastV8MaximumBulkBytes
					|| (BulkSize % Value.BulkElementSize) != 0
					|| (!Value.bBulkPayloadAvailable
						&& (Value.BulkStorage != EBulkStorageKind::External
							|| !Value.Bytes.empty() || Value.BulkContentHash.IsZero())))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidBulkData,
						"BulkData requires explicit valid storage, element size, alignment, and extent.", std::move(Path));
				Frozen.BulkValues.push_back({&Value, ExportId, SchemaId, FieldId, std::move(Path)});
				break;
			}
			default: break;
			}
			return true;
		}

		auto ValidateV9ObjectPaths(const FSerializedType& Type,
			const FSerializedValue& Value, std::string_view Path, uint32 Depth,
			FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			if (Depth > DastV8MaximumValueDepth)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A v9 value exceeds the nesting limit.", std::string(Path));
			if (Type.Kind == EValueKind::SoftReference)
			{
				FObjectPath ObjectPath;
				if (!Value.Text.empty() && !FObjectPath::TryCreate(Value.Text, ObjectPath))
					return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
						"A v9 soft reference is not a canonical complete object path.", std::string(Path));
				return true;
			}
			if (Type.Kind == EValueKind::Struct)
			{
				if (Type.Children.size() != Value.Elements.size()) return true;
				for (size_t Index = 0; Index < Type.Children.size(); ++Index)
					if (!ValidateV9ObjectPaths(Type.Children[Index], Value.Elements[Index],
						Path, Depth + 1, Diagnostic)) return false;
			}
			else if (Type.Kind == EValueKind::FixedArray || Type.Kind == EValueKind::Array)
			{
				if (Type.Children.empty()) return true;
				for (const FSerializedValue& Element : Value.Elements)
					if (!ValidateV9ObjectPaths(Type.Children.front(), Element,
						Path, Depth + 1, Diagnostic)) return false;
			}
			else if (Type.Kind == EValueKind::Map)
			{
				if (Type.Children.size() != 2) return true;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!ValidateV9ObjectPaths(Type.Children[Index % 2], Value.Elements[Index],
						Path, Depth + 1, Diagnostic)) return false;
			}
			return true;
		}

		auto Freeze(const FLinkerTables& Linker, bool bV9, FFrozenPackage& Out,
			FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			FFrozenPackage Frozen;
			Frozen.Source = &Linker;
			Frozen.bV9 = bV9;
			if (Linker.Imports.size() > DastV8MaximumTableEntries
				|| Linker.Exports.size() > DastV8MaximumTableEntries
				|| Linker.Schemas.size() > DastV8MaximumTableEntries)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A package table exceeds the v8 entry limit.");

			if (bV9)
			{
				if (!Linker.Summary.PackagePath.IsValid()
					|| !AddName(Frozen.Names, Linker.Summary.PackagePath.ToString(), Diagnostic,
						"Summary.PackagePath")) return false;
				for (const FPackageSummary::FTopLevelAsset& Asset : Linker.Summary.TopLevelAssets)
				{
					if (!Asset.Export.IsExport() || Asset.Export.GetTableIndex() >= Linker.Exports.size()
						|| !Asset.AssetPath.IsValid()
						|| Asset.AssetPath.GetPackagePath() != Linker.Summary.PackagePath
						|| Asset.ClassName.empty())
					{
						return Fail(Diagnostic, EPackageWriterFailure::InvalidTopology,
							"A v9 top-level asset record is invalid.", "Summary.TopLevelAssets");
					}
					const FPackageExport& Export = Linker.Exports[Asset.Export.GetTableIndex()];
					if (!Export.Outer.IsNull() || Export.ObjectName != Asset.AssetPath.GetAssetName()
						|| Export.ClassName != Asset.ClassName)
					{
						return Fail(Diagnostic, EPackageWriterFailure::InvalidTopology,
							"A v9 top-level asset record does not match its package-outer export.",
							Asset.AssetPath.ToString());
					}
					if (!AddName(Frozen.Names, Asset.AssetPath.ToString(), Diagnostic, "Summary.TopLevelAssets.AssetPath")
						|| !AddName(Frozen.Names, Asset.ClassName, Diagnostic, "Summary.TopLevelAssets.ClassName")
						|| !AddName(Frozen.Names, Asset.RedirectDestination.ToString(), Diagnostic,
							"Summary.TopLevelAssets.RedirectDestination", !Asset.RedirectDestination.IsValid())) return false;
				}
				for (const FPackagePath& Path : Linker.Summary.HardPackageDependencies)
					if (!Path.IsValid() || !AddName(Frozen.Names, Path.ToString(), Diagnostic,
						"Summary.HardPackageDependencies")) return false;
				for (const FPackagePath& Path : Linker.Summary.SoftPackageDependencies)
					if (!Path.IsValid() || !AddName(Frozen.Names, Path.ToString(), Diagnostic,
						"Summary.SoftPackageDependencies")) return false;
			}
			else
			{
				if (!AddName(Frozen.Names, Linker.Summary.PackageName, Diagnostic, "Summary.PackageName")
					|| !AddName(Frozen.Names, Linker.Summary.AssetClass, Diagnostic, "Summary.AssetClass")
					|| !AddName(Frozen.Names, Linker.Summary.RedirectDestination, Diagnostic,
						"Summary.RedirectDestination", !Linker.Summary.bRedirect)) return false;
				if (Linker.Summary.bRedirect && Linker.Summary.RedirectDestination.empty())
					return Fail(Diagnostic, EPackageWriterFailure::InvalidInput,
						"A redirect package requires a destination.", "Summary.RedirectDestination");
				for (const auto& [List, Path] : {
					std::pair{&Linker.Summary.HardPackageReferences, "Summary.HardPackageReferences"},
					std::pair{&Linker.Summary.SoftPackageReferences, "Summary.SoftPackageReferences"}})
					for (const std::string& Name : *List)
						if (!AddName(Frozen.Names, Name, Diagnostic, Path)) return false;
			}
			for (const std::string& Name : Linker.Summary.SearchableNames)
				if (!AddName(Frozen.Names, Name, Diagnostic, "Summary.SearchableNames")) return false;
			for (const std::string& Name : Linker.Names)
				if (!AddName(Frozen.Names, Name, Diagnostic, "Names")) return false;

			if (!ResolvePaths(Linker, true, Frozen.ImportPaths, Diagnostic)
				|| !ResolvePaths(Linker, false, Frozen.ExportPaths, Diagnostic)) return false;
			for (const FPackageImport& Import : Linker.Imports)
			{
				if (bV9)
				{
					if (!Import.ObjectPath.IsValid() || !Import.Outer.IsNull()
						|| !AddName(Frozen.Names, Import.ObjectPath.ToString(), Diagnostic, "Imports.ObjectPath")
						|| !AddName(Frozen.Names, Import.ClassName, Diagnostic, "Imports.ClassName", true)) return false;
				}
				else if (!AddName(Frozen.Names, Import.PackageName, Diagnostic, "Imports.PackageName")
					|| !AddName(Frozen.Names, Import.ObjectName, Diagnostic, "Imports.ObjectName", true)
					|| !AddName(Frozen.Names, Import.ClassName, Diagnostic, "Imports.ClassName", true)) return false;
			}
			for (const FPackageExport& Export : Linker.Exports)
				if (!AddName(Frozen.Names, Export.ObjectName, Diagnostic, "Exports.ObjectName")
					|| !AddName(Frozen.Names, Export.ClassName, Diagnostic, "Exports.ClassName")) return false;

			Frozen.ImportOrder.resize(Linker.Imports.size());
			std::iota(Frozen.ImportOrder.begin(), Frozen.ImportOrder.end(), 0u);
			std::ranges::sort(Frozen.ImportOrder, [&](uint32 Left, uint32 Right)
			{
				const FPackageImport& A = Linker.Imports[Left];
				const FPackageImport& B = Linker.Imports[Right];
				return CompareIdentity({{A.PackageName, B.PackageName}, {A.ObjectName, B.ObjectName},
					{A.ClassName, B.ClassName}, {Frozen.ImportPaths[Left], Frozen.ImportPaths[Right]}}) < 0;
			});
			Frozen.ImportRemap.resize(Linker.Imports.size());
			for (uint32 NewIndex = 0; NewIndex < Frozen.ImportOrder.size(); ++NewIndex)
				Frozen.ImportRemap[Frozen.ImportOrder[NewIndex]] = NewIndex;
			for (size_t Index = 1; Index < Frozen.ImportOrder.size(); ++Index)
				if (Frozen.ImportPaths[Frozen.ImportOrder[Index - 1]] == Frozen.ImportPaths[Frozen.ImportOrder[Index]])
					return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
						"Two imports have the same logical identity.", Frozen.ImportPaths[Frozen.ImportOrder[Index]]);

			Frozen.ExportOrder.resize(Linker.Exports.size());
			std::iota(Frozen.ExportOrder.begin(), Frozen.ExportOrder.end(), 0u);
			std::ranges::sort(Frozen.ExportOrder, [&](uint32 Left, uint32 Right)
			{ return BytewiseLess(Frozen.ExportPaths[Left], Frozen.ExportPaths[Right]); });
			Frozen.ExportRemap.resize(Linker.Exports.size());
			for (uint32 NewIndex = 0; NewIndex < Frozen.ExportOrder.size(); ++NewIndex)
				Frozen.ExportRemap[Frozen.ExportOrder[NewIndex]] = NewIndex;
			for (size_t Index = 1; Index < Frozen.ExportOrder.size(); ++Index)
				if (Frozen.ExportPaths[Frozen.ExportOrder[Index - 1]] == Frozen.ExportPaths[Frozen.ExportOrder[Index]])
					return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
						"Two exports have the same logical identity.", Frozen.ExportPaths[Frozen.ExportOrder[Index]]);
			if (bV9)
			{
				std::unordered_set<uint32> RecordedExports;
				std::unordered_set<FTopLevelAssetPath> RecordedPaths;
				for (const FPackageSummary::FTopLevelAsset& Asset : Linker.Summary.TopLevelAssets)
				{
					if (!RecordedExports.insert(Asset.Export.GetTableIndex()).second
						|| !RecordedPaths.insert(Asset.AssetPath).second)
					{
						return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
							"A v9 top-level asset record is duplicated.", Asset.AssetPath.ToString());
					}
				}
				for (uint32 ExportIndex = 0; ExportIndex < Linker.Exports.size(); ++ExportIndex)
					if (Linker.Exports[ExportIndex].Outer.IsNull()
						&& !RecordedExports.contains(ExportIndex))
					{
						return Fail(Diagnostic, EPackageWriterFailure::InvalidTopology,
							"A package-outer export has no v9 top-level asset record.",
							Linker.Exports[ExportIndex].ObjectName);
					}
			}

			Frozen.Schemas = Linker.Schemas;
			for (FSerializedSchema& Schema : Frozen.Schemas)
			{
				if (!AddName(Frozen.Names, Schema.QualifiedName, Diagnostic, "Schemas.QualifiedName")) return false;
				std::ranges::sort(Schema.Fields, [](const FSerializedField& A, const FSerializedField& B)
				{ return BytewiseLess(A.Name, B.Name); });
				for (size_t Index = 0; Index < Schema.Fields.size(); ++Index)
				{
					FSerializedField& Field = Schema.Fields[Index];
					if (!AddName(Frozen.Names, Field.Name, Diagnostic, Schema.QualifiedName)
						|| !CollectType(Field.Type, Frozen.Types, Frozen.Names, Diagnostic, Schema.QualifiedName)) return false;
					if (Index != 0 && Schema.Fields[Index - 1].Name == Field.Name)
						return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
							"A schema contains duplicate field names.", Schema.QualifiedName + "." + Field.Name);
				}
			}
			std::ranges::sort(Frozen.Schemas, [](const FSerializedSchema& A, const FSerializedSchema& B)
			{ return BytewiseLess(A.QualifiedName, B.QualifiedName); });
			for (size_t Index = 1; Index < Frozen.Schemas.size(); ++Index)
				if (Frozen.Schemas[Index - 1].QualifiedName == Frozen.Schemas[Index].QualifiedName)
					return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
						"Two schemas have the same qualified name.", Frozen.Schemas[Index].QualifiedName);

			for (const FSerializedType& Type : Linker.Types)
				if (!CollectType(Type, Frozen.Types, Frozen.Names, Diagnostic, "Types")) return false;
			std::ranges::sort(Frozen.Types);
			Frozen.Types.erase(std::unique(Frozen.Types.begin(), Frozen.Types.end()), Frozen.Types.end());
			if (Frozen.Types.size() > DastV8MaximumTableEntries)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"The canonical type table exceeds the v8 entry limit.");

			Frozen.CustomVersions = Linker.CustomVersions;
			std::ranges::sort(Frozen.CustomVersions, [](const FCustomVersion& A, const FCustomVersion& B)
			{ return std::tie(A.Guid.A, A.Guid.B, A.Guid.C, A.Guid.D)
				< std::tie(B.Guid.A, B.Guid.B, B.Guid.C, B.Guid.D); });
			for (size_t Index = 1; Index < Frozen.CustomVersions.size(); ++Index)
				if (Frozen.CustomVersions[Index - 1].Guid == Frozen.CustomVersions[Index].Guid)
					return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
						"Two custom versions have the same GUID.", "CustomVersions");

			for (uint32 NewExport = 0; NewExport < Frozen.ExportOrder.size(); ++NewExport)
			{
				const uint32 OldExport = Frozen.ExportOrder[NewExport];
				const FPackageExport& Export = Linker.Exports[OldExport];
				std::vector<const FPropertyTag*> Properties;
				for (const FPropertyTag& Property : Export.Properties) Properties.push_back(&Property);
				std::ranges::sort(Properties, [](const FPropertyTag* A, const FPropertyTag* B)
				{ return CompareIdentity({{A->DeclaringType, B->DeclaringType}, {A->FieldName, B->FieldName}}) < 0; });
				for (size_t PropertyIndex = 0; PropertyIndex < Properties.size(); ++PropertyIndex)
				{
					const FPropertyTag& Property = *Properties[PropertyIndex];
					const std::string Path = Frozen.ExportPaths[OldExport] + "."
						+ Property.DeclaringType + "." + Property.FieldName;
					if (!Property.Payload.empty())
						return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
							"An opaque retained property payload cannot be emitted as DAST v8.", Path);
					if (PropertyIndex != 0 && Properties[PropertyIndex - 1]->DeclaringType == Property.DeclaringType
						&& Properties[PropertyIndex - 1]->FieldName == Property.FieldName)
						return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
							"An export contains duplicate property identities.", Path);
					const uint32 SchemaId = FindSchemaId(Frozen, Property.DeclaringType);
					if (SchemaId == 0)
						return Fail(Diagnostic, EPackageWriterFailure::ManifestMismatch,
							"A property declaring schema is missing.", Path);
					const FSerializedSchema& Schema = Frozen.Schemas[SchemaId - 1];
					const auto FieldIt = std::lower_bound(Schema.Fields.begin(), Schema.Fields.end(), Property.FieldName,
						[](const FSerializedField& Field, std::string_view Name) { return BytewiseLess(Field.Name, Name); });
					if (FieldIt == Schema.Fields.end() || FieldIt->Name != Property.FieldName || FieldIt->Type != Property.Type)
						return Fail(Diagnostic, EPackageWriterFailure::ManifestMismatch,
							"A property does not match its frozen schema field.", Path);
					if (!CollectType(Property.Type, Frozen.Types, Frozen.Names, Diagnostic, Path)) return false;
					if (bV9 && !ValidateV9ObjectPaths(Property.Type, Property.Value,
						Path, 0, Diagnostic)) return false;
					const uint32 FieldId = static_cast<uint32>(std::distance(Schema.Fields.begin(), FieldIt) + 1);
					if (!CollectValue(Frozen, Property.Type, Property.Value, NewExport + 1,
						SchemaId, FieldId, Path, 0, Diagnostic)) return false;
				}
			}

			std::ranges::sort(Frozen.Types);
			Frozen.Types.erase(std::unique(Frozen.Types.begin(), Frozen.Types.end()), Frozen.Types.end());
			for (const FBulkOccurrence& Bulk : Frozen.BulkValues)
				if (!AddName(Frozen.Names, Bulk.LogicalPath, Diagnostic, "BulkData.LogicalPath")) return false;
			std::ranges::sort(Frozen.Names, BytewiseLess);
			Frozen.Names.erase(std::unique(Frozen.Names.begin(), Frozen.Names.end()), Frozen.Names.end());
			if (Frozen.Names.size() > DastV8MaximumTableEntries)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"The canonical name table exceeds the v8 entry limit.");

			Frozen.Manifest.Names = Frozen.Names;
			Frozen.Manifest.Types = Frozen.Types;
			for (const FSerializedSchema& Schema : Frozen.Schemas)
				Frozen.Manifest.Schemas.push_back(Schema.QualifiedName);
			for (uint32 Index : Frozen.ImportOrder) Frozen.Manifest.Imports.push_back(Frozen.ImportPaths[Index]);
			for (uint32 Index : Frozen.ExportOrder) Frozen.Manifest.Exports.push_back(Frozen.ExportPaths[Index]);
			for (const FBulkOccurrence& Bulk : Frozen.BulkValues) Frozen.Manifest.BulkValues.push_back(Bulk.LogicalPath);
			Out = std::move(Frozen);
			return true;
		}

		auto CanonicalFloat32(uint32 Bits) -> uint32
		{
			return (Bits & 0x7f800000u) == 0x7f800000u && (Bits & 0x007fffffu) != 0
				? 0x7fc00000u : Bits;
		}

		auto CanonicalFloat64(uint64 Bits) -> uint64
		{
			return (Bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull
				&& (Bits & 0x000fffffffffffffull) != 0 ? 0x7ff8000000000000ull : Bits;
		}

		auto WriteValue(FBinaryWriter& Writer, const FFrozenPackage& Frozen,
			const FSerializedType& Type, const FSerializedValue& Value, size_t& BulkCursor,
			std::string_view Path, uint32 Depth, FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			if (Depth > DastV8MaximumValueDepth)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A value exceeds the v8 nesting limit.", std::string(Path));
			Writer.WriteU8(static_cast<uint8>(Type.Kind) + 1);
			switch (Type.Kind)
			{
			case EValueKind::Bool: Writer.WriteU8(Value.Bool ? 1 : 0); break;
			case EValueKind::I8: case EValueKind::I16: case EValueKind::I32: case EValueKind::I64:
				Writer.WriteVarInt(Value.Signed); break;
			case EValueKind::U8: case EValueKind::U16: case EValueKind::U32: case EValueKind::U64:
				Writer.WriteVarUInt(Value.Unsigned); break;
			case EValueKind::F32: Writer.WriteU32(CanonicalFloat32(static_cast<uint32>(Value.FloatingBits))); break;
			case EValueKind::F64: Writer.WriteU64(CanonicalFloat64(Value.FloatingBits)); break;
			case EValueKind::String: Writer.WriteString(Value.Text); break;
			case EValueKind::Name:
				Writer.WriteVarUInt(FindNameId(Frozen, Value.Text)); Writer.WriteVarUInt(Value.NameNumber); break;
			case EValueKind::Guid: Writer.WriteGuid(Value.Guid); break;
			case EValueKind::Enum:
				if (Type.Parameter >= uint64(EValueKind::I8) && Type.Parameter <= uint64(EValueKind::I64))
					Writer.WriteVarInt(Value.Signed);
				else Writer.WriteVarUInt(Value.Unsigned);
				break;
			case EValueKind::Intrinsic:
				Writer.WriteVarUInt(Value.ComponentBits.size());
				for (uint64 Bits : Value.ComponentBits)
					if (Type.Parameter == 6) Writer.WriteU32(CanonicalFloat32(static_cast<uint32>(Bits)));
					else Writer.WriteU64(CanonicalFloat64(Bits));
				break;
			case EValueKind::Struct:
			{
				std::vector<size_t> Order(Value.Elements.size());
				std::iota(Order.begin(), Order.end(), 0);
				std::ranges::sort(Order, [&](size_t A, size_t B)
				{ return BytewiseLess(Value.FieldNames[A], Value.FieldNames[B]); });
				for (size_t Index = 1; Index < Order.size(); ++Index)
					if (Value.FieldNames[Order[Index - 1]] == Value.FieldNames[Order[Index]])
						return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
							"A struct contains duplicate field names.", std::string(Path));
				Writer.WriteVarUInt(Order.size());
				for (size_t Index : Order)
				{
					Writer.WriteVarUInt(FindNameId(Frozen, Value.FieldNames[Index]));
					Writer.WriteVarUInt(FindTypeId(Frozen, Type.Children[Index]));
					Writer.WriteU8(static_cast<uint8>(Value.Provenances.empty()
						? EPropertyProvenance::Implicit : Value.Provenances[Index]));
					if (!WriteValue(Writer, Frozen, Type.Children[Index], Value.Elements[Index], BulkCursor,
						std::string(Path) + "." + Value.FieldNames[Index], Depth + 1, Diagnostic)) return false;
				}
				break;
			}
			case EValueKind::FixedArray:
			case EValueKind::Array:
				Writer.WriteVarUInt(Value.Elements.size());
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!WriteValue(Writer, Frozen, Type.Children.front(), Value.Elements[Index], BulkCursor,
						std::string(Path) + "[" + std::to_string(Index) + "]", Depth + 1, Diagnostic)) return false;
				break;
			case EValueKind::Map:
			{
				struct FEntry { size_t Index; std::vector<std::byte> Token; };
				std::vector<FEntry> Entries;
				for (size_t Index = 0; Index < Value.Elements.size(); Index += 2)
				{
					std::vector<std::byte> Token;
					std::string Error;
					if (!BuildCanonicalMapKeyToken(Type.Children[0], Value.Elements[Index], Token, &Error))
						return Fail(Diagnostic, EPackageWriterFailure::InvalidValue, std::move(Error), std::string(Path));
					Entries.push_back({Index, std::move(Token)});
				}
				std::ranges::sort(Entries, [](const FEntry& A, const FEntry& B) { return A.Token < B.Token; });
				for (size_t Index = 1; Index < Entries.size(); ++Index)
					if (Entries[Index - 1].Token == Entries[Index].Token)
						return Fail(Diagnostic, EPackageWriterFailure::DuplicateIdentity,
							"A Map contains colliding canonical keys.", std::string(Path));
				Writer.WriteVarUInt(Entries.size());
				for (size_t Position = 0; Position < Entries.size(); ++Position)
				{
					const FEntry& Entry = Entries[Position];
					const std::string EntryPath = std::string(Path) + "[" + std::to_string(Position) + "]";
					if (!WriteValue(Writer, Frozen, Type.Children[0], Value.Elements[Entry.Index], BulkCursor,
						EntryPath + ".Key", Depth + 1, Diagnostic)
						|| !WriteValue(Writer, Frozen, Type.Children[1], Value.Elements[Entry.Index + 1], BulkCursor,
						EntryPath + ".Value", Depth + 1, Diagnostic)) return false;
				}
				break;
			}
			case EValueKind::HardReference:
			{
				int64 Index = 0;
				if (!RemapIndex(Frozen, Value.Reference, Index, Diagnostic, Path)) return false;
				Writer.WriteVarInt(Index);
				break;
			}
			case EValueKind::SoftReference: Writer.WriteVarUInt(FindNameId(Frozen, Value.Text)); break;
			case EValueKind::Byte:
				if (Value.Unsigned > 255) return Fail(Diagnostic, EPackageWriterFailure::InvalidValue,
					"A byte value is out of range.", std::string(Path));
				Writer.WriteU8(static_cast<uint8>(Value.Unsigned)); break;
			case EValueKind::Bytes:
				Writer.WriteVarUInt(Value.Bytes.size()); Writer.WriteBytes(Value.Bytes); break;
			case EValueKind::BulkData:
				if (BulkCursor >= Frozen.BulkValues.size()
					|| Frozen.BulkValues[BulkCursor].Value != &Value
					|| Frozen.BulkValues[BulkCursor].LogicalPath != Path)
					return Fail(Diagnostic, EPackageWriterFailure::ManifestMismatch,
						"BulkData discovery and emission order differ.", std::string(Path));
				Writer.WriteVarUInt(++BulkCursor);
				break;
			}
			return !Writer.HasError() || Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
				"A value section exceeded its writer limit.", std::string(Path));
		}

		auto AlignPayload(std::vector<std::byte>& Bytes, uint32 Alignment,
			FPackageWriterDiagnostic* Diagnostic, std::string_view Path) -> bool
		{
			const uint64 Current = Bytes.size();
			const uint64 Mask = Alignment - 1;
			if (Current > std::numeric_limits<uint64>::max() - Mask)
				return Fail(Diagnostic, EPackageWriterFailure::ArithmeticOverflow,
					"BulkData alignment overflowed.", std::string(Path));
			const uint64 Aligned = (Current + Mask) & ~Mask;
			if (Aligned > DastV8MaximumBulkBytes)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"BulkData padding exceeds the v8 limit.", std::string(Path));
			Bytes.resize(static_cast<size_t>(Aligned), std::byte{0});
			return true;
		}

		auto AlignPayloadExtent(uint64& Extent, uint32 Alignment,
			FPackageWriterDiagnostic* Diagnostic, std::string_view Path) -> bool
		{
			const uint64 Mask = Alignment - 1;
			if (Extent > std::numeric_limits<uint64>::max() - Mask)
				return Fail(Diagnostic, EPackageWriterFailure::ArithmeticOverflow,
					"BulkData alignment overflowed.", std::string(Path));
			Extent = (Extent + Mask) & ~Mask;
			return Extent <= DastV8MaximumBulkBytes || Fail(Diagnostic,
				EPackageWriterFailure::LimitExceeded,
				"BulkData padding exceeds the v8 limit.", std::string(Path));
		}

		struct FPlacedBulk
		{
			const FBulkOccurrence* Occurrence = nullptr;
			uint64 Offset = 0;
			FXxHash128 Hash;
		};

		auto PlaceBulk(const FFrozenPackage& Frozen, std::vector<std::byte>& Inline,
			std::vector<std::byte>* External, uint64& ExternalExtent,
			std::vector<FPlacedBulk>& Placed,
			FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			for (const FBulkOccurrence& Bulk : Frozen.BulkValues)
			{
				const FSerializedValue& Value = *Bulk.Value;
				const uint64 BulkSize = Value.bBulkPayloadAvailable
					? Value.Bytes.size() : Value.BulkStoredSize;
				const FXxHash128 BulkHash = Value.bBulkPayloadAvailable
					? FXxHash128::HashBuffer(Value.Bytes) : Value.BulkContentHash;
				if (Value.BulkStorage == EBulkStorageKind::Inline)
				{
					if (!Value.bBulkPayloadAvailable
						|| !AlignPayload(Inline, Value.BulkAlignment, Diagnostic, Bulk.LogicalPath)) return false;
					const uint64 Offset = Inline.size();
					if (BulkSize > DastV8MaximumBulkBytes - Inline.size())
						return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
							"BulkData payloads exceed the v8 segment limit.", Bulk.LogicalPath);
					Inline.insert(Inline.end(), Value.Bytes.begin(), Value.Bytes.end());
					Placed.push_back({&Bulk, Offset, BulkHash});
					continue;
				}

				if (!AlignPayloadExtent(ExternalExtent, Value.BulkAlignment,
						Diagnostic, Bulk.LogicalPath)
					|| BulkSize > DastV8MaximumBulkBytes - ExternalExtent)
					return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
						"BulkData payloads exceed the v8 segment limit.", Bulk.LogicalPath);
				const uint64 Offset = ExternalExtent;
				ExternalExtent += BulkSize;
				if (External)
				{
					if (!Value.bBulkPayloadAvailable) return Fail(Diagnostic,
						EPackageWriterFailure::InvalidBulkData,
						"External BulkData payload bytes are unavailable for full emission.",
						Bulk.LogicalPath);
					External->resize(static_cast<size_t>(Offset), std::byte{0});
					External->insert(External->end(), Value.Bytes.begin(), Value.Bytes.end());
				}
				Placed.push_back({&Bulk, Offset, BulkHash});
			}
			return true;
		}

		auto WriteRecord(FBinaryWriter& Destination, const std::function<void(FBinaryWriter&)>& Body,
			FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			FBinaryWriter Record({DastV8MaximumPackageBytes, DastV8MaximumStringBytes});
			Body(Record);
			if (Record.HasError())
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A package record exceeds the v8 limit.");
			Destination.WriteVarUInt(Record.Tell());
			Destination.WriteBytes(Record.GetBytes());
			return !Destination.HasError();
		}

		auto EncodeSections(const FFrozenPackage& Frozen, std::vector<FSection>& Sections,
			std::vector<std::byte>* External, uint64 BoundExternalExtent,
			FXxHash128 BoundExternalHash, FPackageWriterDiagnostic* Diagnostic) -> bool
		{
			std::vector<std::byte> Inline;
			std::vector<FPlacedBulk> Placed;
			uint64 ExternalExtent = 0;
			if (!PlaceBulk(Frozen, Inline, External, ExternalExtent, Placed, Diagnostic)) return false;
			const FXxHash128 ExternalHash = External
				? (External->empty() ? FXxHash128{} : FXxHash128::HashBuffer(*External))
				: BoundExternalHash;
			if (!External && (ExternalExtent != BoundExternalExtent
					|| ((ExternalExtent == 0) != ExternalHash.IsZero())))
				return Fail(Diagnostic, EPackageWriterFailure::InvalidBulkData,
					"External BulkData descriptors do not match their package binding.");

			auto MakeWriter = [] { return std::make_unique<FBinaryWriter>(
				FBinaryCursorLimits{DastV8MaximumPackageBytes, DastV8MaximumStringBytes}); };
			auto Publish = [&](EDastV8Section Kind, std::unique_ptr<FBinaryWriter> Writer) -> bool
			{
				if (Writer->HasError()) return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"A package section exceeds the v8 limit.");
				Sections.push_back({Kind, Writer->TakeBytes()});
				return true;
			};

			auto Writer = MakeWriter();
			Writer->WriteU32(Frozen.bV9 ? DastV9RegistryVersion : DastV8RegistryVersion);
			if (Frozen.bV9)
			{
				Writer->WriteVarUInt(Frozen.ExportOrder.size());
				std::vector<const FPackageSummary::FTopLevelAsset*> Assets;
				for (const FPackageSummary::FTopLevelAsset& Asset : Frozen.Source->Summary.TopLevelAssets)
					Assets.push_back(&Asset);
				std::ranges::sort(Assets, [](const auto* Left, const auto* Right) {
					return Left->AssetPath.GetView() < Right->AssetPath.GetView();
				});
				Writer->WriteVarUInt(Assets.size());
				for (const FPackageSummary::FTopLevelAsset* Asset : Assets)
				{
					int64 Export = 0;
					if (!RemapIndex(Frozen, Asset->Export, Export, Diagnostic,
						"Summary.TopLevelAssets.Export") || Export <= 0) return false;
					Writer->WriteVarUInt(static_cast<uint64>(Export));
					Writer->WriteVarUInt(FindNameId(Frozen, Asset->AssetPath.ToString()));
					Writer->WriteVarUInt(FindNameId(Frozen, Asset->ClassName));
					Writer->WriteVarUInt(FindNameId(Frozen, Asset->RedirectDestination.ToString()));
				}
			}
			else
			{
				Writer->WriteVarUInt(FindNameId(Frozen, Frozen.Source->Summary.AssetClass));
				Writer->WriteVarUInt(FindNameId(Frozen, Frozen.Source->Summary.RedirectDestination));
				int64 MainExport = 0;
				if (!RemapIndex(Frozen, Frozen.Source->Summary.MainExport, MainExport, Diagnostic,
					"Summary.MainExport") || MainExport < 0) return false;
				Writer->WriteVarUInt(static_cast<uint64>(MainExport));
				Writer->WriteVarUInt(Frozen.ExportOrder.size());
			}
			std::array<std::vector<std::string>, 3> RegistryLists;
			if (Frozen.bV9)
			{
				for (const FPackagePath& Path : Frozen.Source->Summary.HardPackageDependencies)
					RegistryLists[0].push_back(Path.ToString());
				for (const FPackagePath& Path : Frozen.Source->Summary.SoftPackageDependencies)
					RegistryLists[1].push_back(Path.ToString());
			}
			else
			{
				RegistryLists[0] = Frozen.Source->Summary.HardPackageReferences;
				RegistryLists[1] = Frozen.Source->Summary.SoftPackageReferences;
			}
			RegistryLists[2] = Frozen.Source->Summary.SearchableNames;
			for (const std::vector<std::string>& List : RegistryLists)
			{
				std::vector<uint32> Ids;
				for (const std::string& Name : List) Ids.push_back(FindNameId(Frozen, Name));
				std::ranges::sort(Ids); Ids.erase(std::unique(Ids.begin(), Ids.end()), Ids.end());
				Writer->WriteVarUInt(Ids.size());
				for (uint32 Id : Ids) Writer->WriteVarUInt(Id);
			}
			Writer->WriteU64(ExternalExtent);
			Writer->WriteHash128(ExternalHash);
			if (!Publish(EDastV8Section::Registry, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion); Writer->WriteVarUInt(Frozen.Names.size());
			for (const std::string& Name : Frozen.Names) Writer->WriteString(Name);
			if (!Publish(EDastV8Section::Names, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion); Writer->WriteVarUInt(Frozen.ImportOrder.size());
			for (uint32 OldIndex : Frozen.ImportOrder)
			{
				const FPackageImport& Import = Frozen.Source->Imports[OldIndex];
				Writer->WriteVarUInt(FindNameId(Frozen,
					Frozen.bV9 ? Import.ObjectPath.ToString() : Import.PackageName));
				Writer->WriteVarUInt(Frozen.bV9 ? 0 : FindNameId(Frozen, Import.ObjectName));
				Writer->WriteVarUInt(FindNameId(Frozen, Import.ClassName));
				int64 Outer = 0; if (!RemapIndex(Frozen, Import.Outer, Outer, Diagnostic, "Imports.Outer")) return false;
				Writer->WriteVarInt(Outer);
			}
			if (!Publish(EDastV8Section::Imports, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion); Writer->WriteVarUInt(Frozen.ExportOrder.size());
			for (uint32 OldIndex : Frozen.ExportOrder)
			{
				const FPackageExport& Export = Frozen.Source->Exports[OldIndex];
				Writer->WriteVarUInt(FindNameId(Frozen, Export.ObjectName));
				Writer->WriteVarUInt(FindNameId(Frozen, Export.ClassName));
				int64 Outer = 0; if (!RemapIndex(Frozen, Export.Outer, Outer, Diagnostic, "Exports.Outer")) return false;
				Writer->WriteVarInt(Outer);
			}
			if (!Publish(EDastV8Section::Exports, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion); Writer->WriteVarUInt(Frozen.Types.size());
			for (const FSerializedType& Type : Frozen.Types)
				if (!WriteRecord(*Writer, [&](FBinaryWriter& Record)
				{
					Record.WriteU8(static_cast<uint8>(Type.Kind) + 1);
					Record.WriteVarUInt(FindNameId(Frozen, Type.QualifiedName));
					Record.WriteVarUInt(Type.Parameter);
					Record.WriteVarUInt(Type.Children.size());
					for (const FSerializedType& Child : Type.Children) Record.WriteVarUInt(FindTypeId(Frozen, Child));
				}, Diagnostic)) return false;
			if (!Publish(EDastV8Section::Types, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion);
			Writer->WriteVarUInt(Frozen.CustomVersions.size());
			for (const FCustomVersion& Version : Frozen.CustomVersions)
			{
				Writer->WriteGuid(Version.Guid); Writer->WriteU32(Version.Value);
				uint8 Flags = (Version.EmissionValue ? 1 : 0) | (Version.MaximumSupported ? 2 : 0)
					| (Version.bCodecKnown ? 4 : 0) | (Version.bRequiredForInterpretation ? 8 : 0);
				Writer->WriteU8(Flags);
				if (Version.EmissionValue) Writer->WriteU32(*Version.EmissionValue);
				if (Version.MaximumSupported) Writer->WriteU32(*Version.MaximumSupported);
			}
			Writer->WriteVarUInt(Frozen.Schemas.size());
			for (const FSerializedSchema& Schema : Frozen.Schemas)
			{
				Writer->WriteVarUInt(FindNameId(Frozen, Schema.QualifiedName));
				Writer->WriteVarUInt(Schema.Fields.size());
				for (const FSerializedField& Field : Schema.Fields)
				{
					Writer->WriteVarUInt(FindNameId(Frozen, Field.Name));
					Writer->WriteVarUInt(FindTypeId(Frozen, Field.Type));
					Writer->WriteVarUInt(Field.AuthoredFlags);
				}
			}
			if (!Publish(EDastV8Section::Schemas, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion); Writer->WriteVarUInt(Frozen.ExportOrder.size());
			size_t BulkCursor = 0;
			for (uint32 NewIndex = 0; NewIndex < Frozen.ExportOrder.size(); ++NewIndex)
			{
				const uint32 OldIndex = Frozen.ExportOrder[NewIndex];
				const FPackageExport& Export = Frozen.Source->Exports[OldIndex];
				std::vector<const FPropertyTag*> Properties;
				for (const FPropertyTag& Property : Export.Properties) Properties.push_back(&Property);
				std::ranges::sort(Properties, [](const FPropertyTag* A, const FPropertyTag* B)
				{ return CompareIdentity({{A->DeclaringType, B->DeclaringType}, {A->FieldName, B->FieldName}}) < 0; });
				Writer->WriteVarUInt(NewIndex + 1); Writer->WriteVarUInt(Properties.size());
				for (const FPropertyTag* Property : Properties)
				{
					const uint32 SchemaId = FindSchemaId(Frozen, Property->DeclaringType);
					const FSerializedSchema& Schema = Frozen.Schemas[SchemaId - 1];
					const auto FieldIt = std::lower_bound(Schema.Fields.begin(), Schema.Fields.end(), Property->FieldName,
						[](const FSerializedField& Field, std::string_view Name) { return BytewiseLess(Field.Name, Name); });
					Writer->WriteVarUInt(SchemaId);
					Writer->WriteVarUInt(std::distance(Schema.Fields.begin(), FieldIt) + 1);
					Writer->WriteVarUInt(FindTypeId(Frozen, Property->Type));
					Writer->WriteU8(static_cast<uint8>(Property->Provenance));
					const std::string Path = Frozen.ExportPaths[OldIndex] + "."
						+ Property->DeclaringType + "." + Property->FieldName;
					if (!WriteValue(*Writer, Frozen, Property->Type, Property->Value, BulkCursor,
						Path, 0, Diagnostic)) return false;
				}
			}
			if (BulkCursor != Frozen.BulkValues.size())
				return Fail(Diagnostic, EPackageWriterFailure::ManifestMismatch,
					"The emitted BulkData count differs from the frozen manifest.");
			if (!Publish(EDastV8Section::Values, std::move(Writer))) return false;

			Writer = MakeWriter(); Writer->WriteU32(DastV8TableVersion); Writer->WriteVarUInt(Placed.size());
			for (const FPlacedBulk& Bulk : Placed)
			{
				const FSerializedValue& Value = *Bulk.Occurrence->Value;
				Writer->WriteVarUInt(Bulk.Occurrence->ExportId);
				Writer->WriteVarUInt(Bulk.Occurrence->SchemaId);
				Writer->WriteVarUInt(Bulk.Occurrence->FieldId);
				Writer->WriteVarUInt(FindNameId(Frozen, Bulk.Occurrence->LogicalPath));
				const uint64 BulkSize = Value.bBulkPayloadAvailable
					? Value.Bytes.size() : Value.BulkStoredSize;
				Writer->WriteU64(BulkSize); Writer->WriteHash128(Bulk.Hash);
				Writer->WriteU32(Value.BulkElementSize); Writer->WriteU32(Value.BulkAlignment);
				Writer->WriteU8(static_cast<uint8>(Value.BulkStorage));
				Writer->WriteU64(Bulk.Offset); Writer->WriteU64(BulkSize);
			}
			if (!Publish(EDastV8Section::BulkDirectory, std::move(Writer))) return false;

			Sections.push_back({EDastV8Section::InlineBulk, std::move(Inline)});
			return Sections.size() == DastV8SectionCount;
		}

		template<std::unsigned_integral T>
		auto WriteLittleEndianAt(std::vector<std::byte>& Bytes, uint64 Offset, T Value) -> void
		{
			for (size_t Index = 0; Index < sizeof(T); ++Index)
				Bytes[static_cast<size_t>(Offset + Index)] = static_cast<std::byte>(Value >> (Index * 8));
		}

		auto Assemble(std::vector<FSection>& Sections, std::vector<std::byte>& Out,
			FPackageWriterDiagnostic* Diagnostic, uint32 FormatVersion,
			bool bRedirect) -> bool
		{
			uint64 Cursor = FirstSectionOffset;
			for (FSection& Section : Sections)
			{
				if (Section.Bytes.size() > DastV8MaximumPackageBytes - Cursor)
					return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
						"The assembled package exceeds the v8 file limit.");
				Section.Offset = Cursor;
				Section.Hash = FXxHash128::HashBuffer(Section.Bytes);
				Cursor += Section.Bytes.size();
			}
			const uint64 HeaderBytes = Sections[2].Offset + Sections[2].Bytes.size();
			if (HeaderBytes > DastV8MaximumHeaderBytes)
				return Fail(Diagnostic, EPackageWriterFailure::LimitExceeded,
					"The v8 discovery header exceeds its limit.");
			std::vector<std::byte> Bytes(static_cast<size_t>(Cursor), std::byte{0});
			FBinaryEnvelopePreamble Preamble{
				.FormatId = DastFormatId, .FormatVersion = FormatVersion,
				.RequiredFeatures = 0, .HeaderBytes = HeaderBytes, .FileBytes = Cursor};
			FBinaryEnvelopeDiagnostic EnvelopeDiagnostic;
			if (!EncodeBinaryEnvelopePreamble(Preamble,
				std::span(Bytes).first(BinaryEnvelopePreambleBytes), &EnvelopeDiagnostic))
				return Fail(Diagnostic, EPackageWriterFailure::EnvelopeFailure,
					std::string(EnvelopeDiagnostic.Message));
			WriteLittleEndianAt(Bytes, FormatHeaderOffset, uint32(bRedirect ? 1 : 0));
			WriteLittleEndianAt(Bytes, FormatHeaderOffset + 4, uint32(0));
			WriteLittleEndianAt(Bytes, FormatHeaderOffset + 8, uint64(DirectoryOffset));
			WriteLittleEndianAt(Bytes, FormatHeaderOffset + 16, uint32(DastV8SectionCount));
			WriteLittleEndianAt(Bytes, FormatHeaderOffset + 20, uint32(DastV8SectionEntryBytes));
			WriteLittleEndianAt(Bytes, FormatHeaderOffset + 24, uint64(0));
			for (size_t Index = 0; Index < Sections.size(); ++Index)
			{
				const FSection& Section = Sections[Index];
				const uint64 Base = DirectoryOffset + Index * DastV8SectionEntryBytes;
				WriteLittleEndianAt(Bytes, Base, static_cast<uint32>(Section.Kind));
				WriteLittleEndianAt(Bytes, Base + 4, uint32(1));
				WriteLittleEndianAt(Bytes, Base + 8, Section.Offset);
				WriteLittleEndianAt(Bytes, Base + 16, uint64(Section.Bytes.size()));
				WriteLittleEndianAt(Bytes, Base + 24, Section.Hash.HashLow);
				WriteLittleEndianAt(Bytes, Base + 32, Section.Hash.HashHigh);
				WriteLittleEndianAt(Bytes, Base + 40, uint64(0));
				std::copy(Section.Bytes.begin(), Section.Bytes.end(), Bytes.begin() + static_cast<ptrdiff_t>(Section.Offset));
			}
			if (!FinalizeBinaryEnvelopeHeader(std::span(Bytes).first(static_cast<size_t>(HeaderBytes)), Cursor,
				{DastV8MaximumHeaderBytes, DastV8MaximumPackageBytes}, &EnvelopeDiagnostic))
				return Fail(Diagnostic, EPackageWriterFailure::EnvelopeFailure,
					std::string(EnvelopeDiagnostic.Message));
			Out = std::move(Bytes);
			return true;
		}
	}

	auto FreezePackageV8(const FLinkerTables& Linker, FPackageWriterManifest& OutManifest,
		FPackageWriterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FFrozenPackage Frozen;
		if (!Freeze(Linker, false, Frozen, OutDiagnostic)) return false;
		OutManifest = std::move(Frozen.Manifest);
		return true;
	}

	auto WritePackageV8(const FLinkerTables& Linker, std::vector<std::byte>& OutPackageBytes,
		std::vector<std::byte>& OutBulkBytes, FPackageWriterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (&OutPackageBytes == &OutBulkBytes)
			return Fail(OutDiagnostic, EPackageWriterFailure::AliasedOutput,
				"The main and bulk output buffers must not alias.");
		FFrozenPackage Frozen;
		if (!Freeze(Linker, false, Frozen, OutDiagnostic)) return false;
		std::vector<FSection> Sections;
		std::vector<std::byte> BulkBytes;
		if (!EncodeSections(Frozen, Sections, &BulkBytes, 0, {}, OutDiagnostic)) return false;
		std::vector<std::byte> PackageBytes;
		if (!Assemble(Sections, PackageBytes, OutDiagnostic, DastV8FormatVersion,
			Linker.Summary.bRedirect)) return false;
		OutPackageBytes = std::move(PackageBytes);
		OutBulkBytes = std::move(BulkBytes);
		return true;
	}

	auto WritePackageV8Main(const FLinkerTables& Linker, uint64 ExternalBulkBytes,
		FXxHash128 ExternalBulkHash, std::vector<std::byte>& OutPackageBytes,
		FPackageWriterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (ExternalBulkBytes > DastV8MaximumBulkBytes
			|| ((ExternalBulkBytes == 0) != ExternalBulkHash.IsZero()))
			return Fail(OutDiagnostic, EPackageWriterFailure::InvalidBulkData,
				"External BulkData binding is invalid.");
		FFrozenPackage Frozen;
		if (!Freeze(Linker, false, Frozen, OutDiagnostic)) return false;
		std::vector<FSection> Sections;
		if (!EncodeSections(Frozen, Sections, nullptr, ExternalBulkBytes,
				ExternalBulkHash, OutDiagnostic)) return false;
		std::vector<std::byte> PackageBytes;
		if (!Assemble(Sections, PackageBytes, OutDiagnostic, DastV8FormatVersion,
				Linker.Summary.bRedirect)) return false;
		OutPackageBytes = std::move(PackageBytes);
		return true;
	}

	auto FreezePackageV9(const FLinkerTables& Linker, FPackageWriterManifest& OutManifest,
		FPackageWriterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FFrozenPackage Frozen;
		if (!Freeze(Linker, true, Frozen, OutDiagnostic)) return false;
		OutManifest = std::move(Frozen.Manifest);
		return true;
	}

	auto WritePackageV9(const FLinkerTables& Linker,
		std::vector<std::byte>& OutPackageBytes,
		std::vector<std::byte>& OutBulkBytes,
		FPackageWriterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (&OutPackageBytes == &OutBulkBytes)
			return Fail(OutDiagnostic, EPackageWriterFailure::AliasedOutput,
				"The main and bulk output buffers must not alias.");
		FFrozenPackage Frozen;
		if (!Freeze(Linker, true, Frozen, OutDiagnostic)) return false;
		std::vector<FSection> Sections;
		std::vector<std::byte> BulkBytes;
		if (!EncodeSections(Frozen, Sections, &BulkBytes, 0, {}, OutDiagnostic)) return false;
		std::vector<std::byte> PackageBytes;
		if (!Assemble(Sections, PackageBytes, OutDiagnostic,
			DastV9FormatVersion, false)) return false;
		OutPackageBytes = std::move(PackageBytes);
		OutBulkBytes = std::move(BulkBytes);
		return true;
	}

	auto WritePackageV9Main(const FLinkerTables& Linker, uint64 ExternalBulkBytes,
		FXxHash128 ExternalBulkHash, std::vector<std::byte>& OutPackageBytes,
		FPackageWriterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (ExternalBulkBytes > DastV8MaximumBulkBytes
			|| ((ExternalBulkBytes == 0) != ExternalBulkHash.IsZero()))
			return Fail(OutDiagnostic, EPackageWriterFailure::InvalidBulkData,
				"External BulkData binding is invalid.");
		FFrozenPackage Frozen;
		if (!Freeze(Linker, true, Frozen, OutDiagnostic)) return false;
		std::vector<FSection> Sections;
		if (!EncodeSections(Frozen, Sections, nullptr, ExternalBulkBytes,
			ExternalBulkHash, OutDiagnostic)) return false;
		std::vector<std::byte> PackageBytes;
		if (!Assemble(Sections, PackageBytes, OutDiagnostic,
			DastV9FormatVersion, false)) return false;
		OutPackageBytes = std::move(PackageBytes);
		return true;
	}
}
