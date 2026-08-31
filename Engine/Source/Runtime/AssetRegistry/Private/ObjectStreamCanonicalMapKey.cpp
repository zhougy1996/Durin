#include "AssetRegistry/ObjectStream.h"

#include "DObject/CanonicalMapKey.h"

namespace Durin::Asset::PackageObjectStream
{
	namespace
	{
		auto Fail(FReaderDiagnostic* Diagnostic, EReaderFailure Failure,
			std::string Message, std::string Path = {}) -> bool
		{
			if (Diagnostic)
				*Diagnostic = {Failure, std::move(Path), std::move(Message)};
			return false;
		}

		auto ValueKind(ETypeOpcode Opcode)
			-> std::optional<ObjectPackage::EValueKind>
		{
			using K = ObjectPackage::EValueKind;
			switch (Opcode)
			{
			case ETypeOpcode::Bool: return K::Bool;
			case ETypeOpcode::I8: return K::I8;
			case ETypeOpcode::I16: return K::I16;
			case ETypeOpcode::I32: return K::I32;
			case ETypeOpcode::I64: return K::I64;
			case ETypeOpcode::U8: return K::U8;
			case ETypeOpcode::U16: return K::U16;
			case ETypeOpcode::U32: return K::U32;
			case ETypeOpcode::U64: return K::U64;
			case ETypeOpcode::F32: return K::F32;
			case ETypeOpcode::F64: return K::F64;
			case ETypeOpcode::String: return K::String;
			case ETypeOpcode::Name: return K::Name;
			case ETypeOpcode::Guid: return K::Guid;
			case ETypeOpcode::Enum: return K::Enum;
			case ETypeOpcode::Intrinsic: return K::Intrinsic;
			case ETypeOpcode::Struct: return K::Struct;
			case ETypeOpcode::FixedArray: return K::FixedArray;
			case ETypeOpcode::Array: return K::Array;
			case ETypeOpcode::Map: return K::Map;
			case ETypeOpcode::HardRef: return K::HardReference;
			case ETypeOpcode::SoftRef: return K::SoftReference;
			case ETypeOpcode::Bytes: return K::Bytes;
			case ETypeOpcode::BulkData: return K::BulkData;
			}
			return std::nullopt;
		}

		class FTranslator
		{
		public:
			FTranslator(const FDecodedPackage& InPackage,
				FReaderDiagnostic* InDiagnostic)
				: Package(InPackage), Diagnostic(InDiagnostic),
				  CachedTypes(InPackage.Types.size()),
				  ActiveTypes(InPackage.Types.size())
			{
			}

			auto Type(uint64 Id, ObjectPackage::FSerializedType& Out,
				std::string Path) -> bool
			{
				if (Id == 0 || Id > Package.Types.size())
					return Fail(Diagnostic, EReaderFailure::InvalidTable,
						"Object-stream type id is out of range.", std::move(Path));
				const size_t Index = static_cast<size_t>(Id - 1);
				if (CachedTypes[Index])
				{
					Out = *CachedTypes[Index];
					return true;
				}
				if (ActiveTypes[Index])
					return Fail(Diagnostic, EReaderFailure::DescriptorCycle,
						"Object-stream type graph contains a cycle.",
						std::move(Path));
				ActiveTypes[Index] = true;
				const FDecodedType& Source = Package.Types[Index];
				const auto Kind = ValueKind(Source.Opcode);
				if (!Kind)
					return Fail(Diagnostic, EReaderFailure::InvalidTable,
						"Object-stream type opcode is unsupported.",
						std::move(Path));
				ObjectPackage::FSerializedType Result{
					.Kind = *Kind,
					.QualifiedName = Source.QualifiedName,
					.Parameter = Source.Parameter};
				if (Source.Opcode == ETypeOpcode::Enum)
				{
					const auto Storage = ValueKind(
						static_cast<ETypeOpcode>(Source.Parameter));
					if (!Storage || !(*Storage >= ObjectPackage::EValueKind::I8
						&& *Storage <= ObjectPackage::EValueKind::U64))
						return Fail(Diagnostic, EReaderFailure::InvalidTable,
							"Object-stream enum storage type is invalid.",
							std::move(Path));
					Result.Parameter = static_cast<uint64>(*Storage);
				}
				for (uint64 ChildId : Source.ChildTypeIds)
				{
					ObjectPackage::FSerializedType Child;
					if (!Type(ChildId, Child, Path)) return false;
					Result.Children.push_back(std::move(Child));
				}
				if (Source.Opcode == ETypeOpcode::Struct)
					if (const FDecodedSchema* Schema = FindSchema(Source.QualifiedName))
						for (const FDecodedField& Field : Schema->Fields)
						{
							ObjectPackage::FSerializedType Child;
							if (!Type(Field.TypeId, Child,
								Path + "::" + Field.Name)) return false;
							Result.Children.push_back(std::move(Child));
						}
				ActiveTypes[Index] = false;
				CachedTypes[Index] = Result;
				Out = std::move(Result);
				return true;
			}

			auto Value(const FDecodedType& TypeSource, const FValue& Source,
				ObjectPackage::FSerializedValue& Out, std::string Path) -> bool
			{
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
				for (EDefaultDeltaProvenance Provenance : Source.Provenances)
					Result.Provenances.push_back(
						Provenance == EDefaultDeltaProvenance::Forced
							? ObjectPackage::EPropertyProvenance::Forced
							: ObjectPackage::EPropertyProvenance::Explicit);

				if (TypeSource.Opcode == ETypeOpcode::HardRef)
				{
					if (Source.ReferenceTag == 0)
						Result.Reference = ObjectPackage::FPackageIndex::Null();
					else if (Source.ReferenceTag == 1)
					{
						if (Source.ReferenceId == 0
							|| Source.ReferenceId > Package.Objects.size()
							|| !ObjectPackage::FPackageIndex::TryExport(
								Source.ReferenceId - 1, Result.Reference))
							return Fail(Diagnostic, EReaderFailure::InvalidValue,
								"Object-stream internal reference is invalid.",
								std::move(Path));
					}
					else if (Source.ReferenceTag == 2)
					{
						if (Source.ReferenceId == 0
							|| Source.ReferenceId > Package.Header.Dependencies.size()
							|| !ObjectPackage::FPackageIndex::TryImport(
								Source.ReferenceId - 1, Result.Reference))
							return Fail(Diagnostic, EReaderFailure::InvalidValue,
								"Object-stream external reference is invalid.",
								std::move(Path));
					}
					else return Fail(Diagnostic, EReaderFailure::InvalidValue,
						"Object-stream reference tag is invalid.", std::move(Path));
				}

				auto ConvertChild = [&](uint64 ChildId, const FValue& ChildSource,
					std::string ChildPath) -> bool
				{
					if (ChildId == 0 || ChildId > Package.Types.size())
						return Fail(Diagnostic, EReaderFailure::InvalidTable,
							"Object-stream child type is invalid.",
							std::move(ChildPath));
					ObjectPackage::FSerializedValue Child;
					if (!Value(Package.Types[static_cast<size_t>(ChildId - 1)],
						ChildSource, Child, std::move(ChildPath))) return false;
					Result.Elements.push_back(std::move(Child));
					return true;
				};
				if (TypeSource.Opcode == ETypeOpcode::Struct)
				{
					const FDecodedSchema* Schema = FindSchema(TypeSource.QualifiedName);
					if (!Schema || Source.FieldNames.size() != Source.Elements.size()
						|| Source.Provenances.size() != Source.Elements.size())
						return Fail(Diagnostic, EReaderFailure::InvalidValue,
							"Object-stream Struct value shape is invalid.",
							std::move(Path));
					for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
					{
						const auto It = std::ranges::find(
							Schema->Fields, Source.FieldNames[Index],
							&FDecodedField::Name);
						if (It == Schema->Fields.end()
							|| !ConvertChild(It->TypeId, Source.Elements[Index],
								Path + "::" + Source.FieldNames[Index])) return false;
					}
				}
				else if (TypeSource.Opcode == ETypeOpcode::FixedArray
					|| TypeSource.Opcode == ETypeOpcode::Array)
				{
					if (TypeSource.ChildTypeIds.size() != 1
						|| (TypeSource.Opcode == ETypeOpcode::FixedArray
							&& Source.Elements.size() != TypeSource.Parameter))
						return Fail(Diagnostic, EReaderFailure::InvalidValue,
							"Object-stream array shape is invalid.", std::move(Path));
					for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
						if (!ConvertChild(TypeSource.ChildTypeIds[0],
							Source.Elements[Index], std::format("{}[{}]", Path, Index)))
							return false;
				}
				else if (TypeSource.Opcode == ETypeOpcode::Map)
				{
					if (TypeSource.ChildTypeIds.size() != 2
						|| Source.Elements.size() % 2 != 0)
						return Fail(Diagnostic, EReaderFailure::InvalidValue,
							"Object-stream Map shape is invalid.", std::move(Path));
					for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
						if (!ConvertChild(TypeSource.ChildTypeIds[Index % 2],
							Source.Elements[Index], std::format("{}[{}]", Path, Index)))
							return false;
				}
				Out = std::move(Result);
				return true;
			}

		private:
			auto FindSchema(std::string_view Name) const -> const FDecodedSchema*
			{
				const auto It = std::ranges::find(
					Package.Schemas, Name, &FDecodedSchema::QualifiedName);
				return It == Package.Schemas.end() ? nullptr : &*It;
			}

			const FDecodedPackage& Package;
			FReaderDiagnostic* Diagnostic;
			std::vector<std::optional<ObjectPackage::FSerializedType>> CachedTypes;
			std::vector<bool> ActiveTypes;
		};
	}

	auto BuildCanonicalMapKeyToken(const FDecodedPackage& Package,
		const FDecodedType& Type, const FValue& Value,
		std::vector<std::byte>& OutToken, FReaderDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		const auto It = std::find_if(Package.Types.begin(), Package.Types.end(),
			[&](const FDecodedType& Candidate) { return &Candidate == &Type; });
		if (It == Package.Types.end())
			return Fail(OutDiagnostic, EReaderFailure::InvalidTable,
				"Map key type is not owned by the decoded package.");
		FTranslator Translator(Package, OutDiagnostic);
		ObjectPackage::FSerializedType TranslatedType;
		if (!Translator.Type(
			static_cast<uint64>(std::distance(Package.Types.begin(), It)) + 1,
			TranslatedType, "MapKey")) return false;
		ObjectPackage::FSerializedValue TranslatedValue;
		if (!Translator.Value(Type, Value, TranslatedValue, "MapKey")) return false;
		std::vector<std::byte> Token;
		std::string Error;
		if (!ObjectPackage::BuildCanonicalMapKeyToken(
			TranslatedType, TranslatedValue, Token, &Error))
			return Fail(OutDiagnostic, EReaderFailure::InvalidValue,
				std::move(Error), "MapKey");
		OutToken = std::move(Token);
		return true;
	}
}
