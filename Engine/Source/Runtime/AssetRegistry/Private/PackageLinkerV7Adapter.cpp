#include "PackageLinkerV7Adapter.h"

namespace Durin::Asset::Private
{
	namespace
	{
		using namespace PackageObjectStream;

		auto Fail(FV7LinkerAdapterDiagnostic* Diagnostic, EV7LinkerAdapterFailure Failure,
			std::string Message, std::string Path = {}) -> bool
		{
			if (Diagnostic) *Diagnostic = {Failure, std::move(Path), std::move(Message)};
			return false;
		}

		auto ValueKind(ETypeOpcode Opcode) -> std::optional<ObjectPackage::EValueKind>
		{
			using EKind = ObjectPackage::EValueKind;
			switch (Opcode)
			{
			case ETypeOpcode::Bool: return EKind::Bool;
			case ETypeOpcode::I8: return EKind::I8;
			case ETypeOpcode::I16: return EKind::I16;
			case ETypeOpcode::I32: return EKind::I32;
			case ETypeOpcode::I64: return EKind::I64;
			case ETypeOpcode::U8: return EKind::U8;
			case ETypeOpcode::U16: return EKind::U16;
			case ETypeOpcode::U32: return EKind::U32;
			case ETypeOpcode::U64: return EKind::U64;
			case ETypeOpcode::F32: return EKind::F32;
			case ETypeOpcode::F64: return EKind::F64;
			case ETypeOpcode::String: return EKind::String;
			case ETypeOpcode::Name: return EKind::Name;
			case ETypeOpcode::Guid: return EKind::Guid;
			case ETypeOpcode::Enum: return EKind::Enum;
			case ETypeOpcode::Intrinsic: return EKind::Intrinsic;
			case ETypeOpcode::Struct: return EKind::Struct;
			case ETypeOpcode::FixedArray: return EKind::FixedArray;
			case ETypeOpcode::Array: return EKind::Array;
			case ETypeOpcode::Map: return EKind::Map;
			case ETypeOpcode::HardRef: return EKind::HardReference;
			case ETypeOpcode::SoftRef: return EKind::SoftReference;
			case ETypeOpcode::Bytes: return EKind::Bytes;
			case ETypeOpcode::BulkData: return EKind::BulkData;
			}
			return std::nullopt;
		}

		class FTranslator
		{
		public:
			FTranslator(const FDecodedPackage& InPackage, FV7LinkerAdapterDiagnostic* InDiagnostic)
				: Package(InPackage), Diagnostic(InDiagnostic), CachedTypes(InPackage.Types.size()), ActiveTypes(InPackage.Types.size()) {}

			auto Type(uint64 Id, ObjectPackage::FSerializedType& Out, std::string Path) -> bool
			{
				if (Id == 0 || Id > Package.Types.size())
					return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidTable,
						"DAST v7 type id is out of range.", std::move(Path));
				const size_t Index = static_cast<size_t>(Id - 1);
				if (CachedTypes[Index]) { Out = *CachedTypes[Index]; return true; }
				if (ActiveTypes[Index])
					return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidTable,
						"DAST v7 structural type graph contains a cycle.", std::move(Path));
				ActiveTypes[Index] = true;
				const FDecodedType& Source = Package.Types[Index];
				const auto Kind = ValueKind(Source.Opcode);
				if (!Kind)
					return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidTable,
						"DAST v7 type opcode is unsupported.", std::move(Path));
				ObjectPackage::FSerializedType Result{
					.Kind = *Kind,
					.QualifiedName = Source.QualifiedName,
					.Parameter = Source.Parameter,
				};
				if (Source.Opcode == ETypeOpcode::Enum)
				{
					const auto Storage = ValueKind(static_cast<ETypeOpcode>(Source.Parameter));
					if (!Storage || !(*Storage >= ObjectPackage::EValueKind::I8
						&& *Storage <= ObjectPackage::EValueKind::U64))
						return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidTable,
							"DAST v7 enum storage type is invalid.", std::move(Path));
					Result.Parameter = static_cast<uint64>(*Storage);
				}
				for (uint64 ChildId : Source.ChildTypeIds)
				{
					ObjectPackage::FSerializedType Child;
					if (!Type(ChildId, Child, Path)) return false;
					Result.Children.push_back(std::move(Child));
				}
				if (Source.Opcode == ETypeOpcode::Struct)
				{
					const FDecodedSchema* Schema = FindSchema(Source.QualifiedName);
					// v7 may retain a struct descriptor for an empty container without
					// serializing that struct's schema. v8 can represent that qualified
					// leaf descriptor exactly; a materialized value still requires the
					// schema in Value(), where its field layout is needed.
					if (Schema) for (const FDecodedField& Field : Schema->Fields)
					{
						ObjectPackage::FSerializedType Child;
						if (!Type(Field.TypeId, Child, Path + "::" + Field.Name)) return false;
						Result.Children.push_back(std::move(Child));
					}
				}
				ActiveTypes[Index] = false;
				CachedTypes[Index] = Result;
				Out = std::move(Result);
				return true;
			}

			auto Value(const FDecodedType& TypeSource, const FValue& Source,
				ObjectPackage::FSerializedValue& Out, std::string Path,
				std::vector<std::string>* SoftPackages = nullptr) -> bool
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
					.FieldNames = Source.FieldNames,
				};
				for (EDefaultDeltaProvenance Provenance : Source.Provenances)
					Result.Provenances.push_back(Provenance == EDefaultDeltaProvenance::Forced
						? ObjectPackage::EPropertyProvenance::Forced
						: ObjectPackage::EPropertyProvenance::Explicit);

				if (TypeSource.Opcode == ETypeOpcode::HardRef)
				{
					if (Source.ReferenceTag == 0) Result.Reference = ObjectPackage::FPackageIndex::Null();
					else if (Source.ReferenceTag == 1)
					{
						if (Source.ReferenceId == 0 || Source.ReferenceId > Package.Objects.size()
							|| !ObjectPackage::FPackageIndex::TryExport(Source.ReferenceId - 1, Result.Reference))
							return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
								"DAST v7 internal hard reference is invalid.", std::move(Path));
					}
					else if (Source.ReferenceTag == 2)
					{
						if (Source.ReferenceId == 0 || Source.ReferenceId > Package.Header.Dependencies.size()
							|| !ObjectPackage::FPackageIndex::TryImport(Source.ReferenceId - 1, Result.Reference))
							return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
								"DAST v7 external hard reference is invalid.", std::move(Path));
					}
					else return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
						"DAST v7 hard-reference tag is invalid.", std::move(Path));
				}
				else if (TypeSource.Opcode == ETypeOpcode::SoftRef)
				{
					if (Source.ReferenceTag > 1)
						return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
							"DAST v7 soft-reference tag is invalid.", std::move(Path));
					if (Source.ReferenceTag == 1 && SoftPackages) SoftPackages->push_back(Source.Text);
				}

				if (TypeSource.Opcode == ETypeOpcode::Struct)
				{
					const FDecodedSchema* Schema = FindSchema(TypeSource.QualifiedName);
					if (!Schema || Source.FieldNames.size() != Source.Elements.size()
						|| Source.Provenances.size() != Source.Elements.size())
						return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
							"DAST v7 Struct value shape is invalid.", std::move(Path));
					for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
					{
						const auto It = std::ranges::find(Schema->Fields, Source.FieldNames[Index], &FDecodedField::Name);
						if (It == Schema->Fields.end() || It->TypeId == 0 || It->TypeId > Package.Types.size())
							return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
								"DAST v7 Struct field is absent from its schema.", Path);
						ObjectPackage::FSerializedValue Child;
						if (!Value(Package.Types[static_cast<size_t>(It->TypeId - 1)], Source.Elements[Index],
							Child, Path + "::" + It->Name, SoftPackages)) return false;
						Result.Elements.push_back(std::move(Child));
					}
				}
				else if (TypeSource.Opcode == ETypeOpcode::FixedArray || TypeSource.Opcode == ETypeOpcode::Array)
				{
					if (TypeSource.ChildTypeIds.size() != 1 || TypeSource.ChildTypeIds[0] == 0
						|| TypeSource.ChildTypeIds[0] > Package.Types.size()
						|| (TypeSource.Opcode == ETypeOpcode::FixedArray && Source.Elements.size() != TypeSource.Parameter))
						return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
							"DAST v7 array shape is invalid.", std::move(Path));
					const FDecodedType& ChildType = Package.Types[static_cast<size_t>(TypeSource.ChildTypeIds[0] - 1)];
					for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
					{
						ObjectPackage::FSerializedValue Child;
						if (!Value(ChildType, Source.Elements[Index], Child,
							std::format("{}[{}]", Path, Index), SoftPackages)) return false;
						Result.Elements.push_back(std::move(Child));
					}
				}
				else if (TypeSource.Opcode == ETypeOpcode::Map)
				{
					if (TypeSource.ChildTypeIds.size() != 2 || Source.Elements.size() % 2 != 0)
						return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidValue,
							"DAST v7 Map shape is invalid.", std::move(Path));
					for (size_t Index = 0; Index < Source.Elements.size(); ++Index)
					{
						const uint64 ChildId = TypeSource.ChildTypeIds[Index % 2];
						if (ChildId == 0 || ChildId > Package.Types.size())
							return Fail(Diagnostic, EV7LinkerAdapterFailure::InvalidTable,
								"DAST v7 Map child type is invalid.", Path);
						ObjectPackage::FSerializedValue Child;
						if (!Value(Package.Types[static_cast<size_t>(ChildId - 1)], Source.Elements[Index],
							Child, std::format("{}[{}]", Path, Index), SoftPackages)) return false;
						Result.Elements.push_back(std::move(Child));
					}
				}
				Out = std::move(Result);
				return true;
			}

			auto FindSchema(std::string_view Name) const -> const FDecodedSchema*
			{
				const auto It = std::ranges::find(Package.Schemas, Name, &FDecodedSchema::QualifiedName);
				return It == Package.Schemas.end() ? nullptr : &*It;
			}

		private:
			const FDecodedPackage& Package;
			FV7LinkerAdapterDiagnostic* Diagnostic;
			std::vector<std::optional<ObjectPackage::FSerializedType>> CachedTypes;
			std::vector<bool> ActiveTypes;
		};
	}

	auto AdaptDecodedPackageV7(const FDecodedPackage& Package, std::string_view PackageName,
		ObjectPackage::FLinkerTables& OutLinker,
		FV7LinkerAdapterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		if (Package.Objects.size() != Package.ObjectValues.size())
			return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTopology,
				"DAST v7 object and value tables have different sizes.");
		ObjectPackage::FLinkerTables Result;
		Result.Summary.PackageName = PackageName;
		Result.Summary.AssetClass = Package.Header.AssetClass;
		Result.Summary.bRedirect = Package.Header.EntryKind == EAssetRegistryEntryKind::Redirector;
		Result.Summary.RedirectDestination = Package.Header.RedirectDestination;
		Result.Summary.HardPackageReferences = Package.Header.Dependencies;
		Result.Names = Package.Names;
		if (!Package.Objects.empty())
			ObjectPackage::FPackageIndex::TryExport(0, Result.Summary.MainExport);
		for (const std::string& Dependency : Package.Header.Dependencies)
			Result.Imports.push_back({.PackageName = Dependency});
		for (const PackageObjectStream::FCustomVersion& Version : Package.CustomVersions)
			Result.CustomVersions.push_back({Version.Guid, Version.Value,
				Version.EmissionValue, Version.MaximumSupported,
				Version.bCodecKnown, Version.bRequiredForInterpretation});

		FTranslator Translator(Package, OutDiagnostic);
		Result.Types.reserve(Package.Types.size());
		for (size_t Index = 0; Index < Package.Types.size(); ++Index)
		{
			ObjectPackage::FSerializedType Type;
			if (!Translator.Type(Index + 1, Type, std::format("Type[{}]", Index + 1))) return false;
			Result.Types.push_back(std::move(Type));
		}
		for (const FDecodedSchema& SourceSchema : Package.Schemas)
		{
			ObjectPackage::FSerializedSchema Schema{.QualifiedName = SourceSchema.QualifiedName};
			for (const FDecodedField& SourceField : SourceSchema.Fields)
			{
				ObjectPackage::FSerializedType Type;
				if (!Translator.Type(SourceField.TypeId, Type,
					SourceSchema.QualifiedName + "::" + SourceField.Name)) return false;
				Schema.Fields.push_back({SourceField.Name, std::move(Type), SourceField.AuthoredFlags});
			}
			Result.Schemas.push_back(std::move(Schema));
		}

		Result.Exports.reserve(Package.Objects.size());
		for (size_t Index = 0; Index < Package.Objects.size(); ++Index)
		{
			const FDecodedObject& Object = Package.Objects[Index];
			if (Object.Id != Index + 1 || Object.OuterId > Package.Objects.size() || Object.OuterId == Object.Id)
				return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTopology,
					"DAST v7 object identity or Outer id is invalid.", Object.Path);
			ObjectPackage::FPackageIndex Outer;
			if (Object.OuterId != 0 && !ObjectPackage::FPackageIndex::TryExport(Object.OuterId - 1, Outer))
				return Fail(OutDiagnostic, EV7LinkerAdapterFailure::LimitExceeded,
					"DAST v7 Outer id exceeds linker index limits.", Object.Path);
			Result.Exports.push_back({.ObjectName = Object.ObjectName, .ClassName = Object.ClassName, .Outer = Outer});
		}

		for (size_t ObjectIndex = 0; ObjectIndex < Package.Objects.size(); ++ObjectIndex)
		{
			const FDecodedObject& Object = Package.Objects[ObjectIndex];
			for (const FDecodedOverride& Override : Package.ObjectValues[ObjectIndex].Overrides)
			{
				if (Override.Provenance == 2 || !Override.DescriptorClosure.empty() || !Override.RetainedPayload.empty())
					return Fail(OutDiagnostic, EV7LinkerAdapterFailure::UnsupportedRetainedValue,
						"DAST v7 retained unknown values cannot enter the linker model.", Object.Path);
				if (Override.SchemaId == 0 || Override.SchemaId > Package.Schemas.size())
					return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTable,
						"DAST v7 override schema id is invalid.", Object.Path);
				const FDecodedSchema& Schema = Package.Schemas[static_cast<size_t>(Override.SchemaId - 1)];
				if (Override.FieldId == 0 || Override.FieldId > Schema.Fields.size())
					return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTable,
						"DAST v7 override field id is invalid.", Object.Path);
				const FDecodedField& Field = Schema.Fields[static_cast<size_t>(Override.FieldId - 1)];
				if (Field.TypeId == 0 || Field.TypeId > Package.Types.size())
					return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTable,
						"DAST v7 override type id is invalid.", Object.Path);
				ObjectPackage::FSerializedType Type;
				const std::string Path = Object.Path + "::" + Schema.QualifiedName + "::" + Field.Name;
				if (!Translator.Type(Field.TypeId, Type, Path)) return false;
				ObjectPackage::FSerializedValue Value;
				if (!Translator.Value(Package.Types[static_cast<size_t>(Field.TypeId - 1)], Override.Value,
					Value, Path, &Result.Summary.SoftPackageReferences)) return false;
				Result.Exports[ObjectIndex].Properties.push_back({
					.DeclaringType = Schema.QualifiedName,
					.FieldName = Field.Name,
					.Type = std::move(Type),
					.Provenance = Override.Provenance == 1
						? ObjectPackage::EPropertyProvenance::Forced
						: ObjectPackage::EPropertyProvenance::Explicit,
					.Value = std::move(Value),
				});
			}
		}
		std::ranges::sort(Result.Summary.SoftPackageReferences);
		Result.Summary.SoftPackageReferences.erase(std::ranges::unique(
			Result.Summary.SoftPackageReferences).begin(), Result.Summary.SoftPackageReferences.end());

		for (size_t Index = 0; Index < Package.Objects.size(); ++Index)
		{
			ObjectPackage::FPackageIndex Export;
			ObjectPackage::FLinkerDiagnostic Diagnostic;
			std::string Path;
			if (!ObjectPackage::FPackageIndex::TryExport(Index, Export)
				|| !Result.TryResolvePath(Export, Path, &Diagnostic) || Path != Package.Objects[Index].Path)
				return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTopology,
					"DAST v7 object path does not match its Outer topology.", Package.Objects[Index].Path);
		}
		OutLinker = std::move(Result);
		return true;
	}

	auto BuildDecodedCanonicalMapKeyTokenV7(const FDecodedPackage& Package,
		const FDecodedType& Type, const FValue& Value, std::vector<std::byte>& OutToken,
		FV7LinkerAdapterDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		const auto It = std::find_if(Package.Types.begin(), Package.Types.end(),
			[&](const FDecodedType& Candidate) { return &Candidate == &Type; });
		if (It == Package.Types.end())
			return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidTable,
				"DAST v7 key type is not owned by the decoded package.");
		FTranslator Translator(Package, OutDiagnostic);
		ObjectPackage::FSerializedType TranslatedType;
		if (!Translator.Type(static_cast<uint64>(std::distance(Package.Types.begin(), It)) + 1,
			TranslatedType, "MapKey")) return false;
		ObjectPackage::FSerializedValue TranslatedValue;
		if (!Translator.Value(Type, Value, TranslatedValue, "MapKey")) return false;
		std::vector<std::byte> Token;
		std::string Error;
		if (!ObjectPackage::BuildCanonicalMapKeyToken(TranslatedType, TranslatedValue, Token, &Error))
			return Fail(OutDiagnostic, EV7LinkerAdapterFailure::InvalidValue, std::move(Error), "MapKey");
		OutToken = std::move(Token);
		return true;
	}
}

namespace Durin::Asset::PackageObjectStream
{
	auto BuildCanonicalMapKeyToken(const FDecodedPackage& Package, const FDecodedType& Type,
		const FValue& Value, std::vector<std::byte>& OutToken,
		FReaderDiagnostic* OutDiagnostic) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		Private::FV7LinkerAdapterDiagnostic AdapterDiagnostic;
		if (Private::BuildDecodedCanonicalMapKeyTokenV7(
			Package, Type, Value, OutToken, &AdapterDiagnostic)) return true;
		if (OutDiagnostic)
		{
			OutDiagnostic->Failure = EReaderFailure::InvalidValue;
			OutDiagnostic->LogicalPath = std::move(AdapterDiagnostic.LogicalPath);
			OutDiagnostic->Message = std::move(AdapterDiagnostic.Message);
		}
		return false;
	}
}
