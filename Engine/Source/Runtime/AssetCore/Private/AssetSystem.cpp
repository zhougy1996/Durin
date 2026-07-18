#include "AssetSystem.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 AssetMagic = 0x54534144; // DAST
		constexpr uint32 AssetVersion = 2;

		struct FByteWriter
		{
			std::vector<uint8> Bytes;

			template<typename T> auto Write(const T& Value) -> void
			{
				const auto* Data = reinterpret_cast<const uint8*>(&Value);
				Bytes.insert(Bytes.end(), Data, Data + sizeof(T));
			}

			auto WriteBytes(const void* Data, size_t Size) -> void
			{
				const auto* Source = static_cast<const uint8*>(Data);
				Bytes.insert(Bytes.end(), Source, Source + Size);
			}

			auto WriteString(std::string_view Value) -> void
			{
				const uint64 Size = Value.size();
				Write(Size);
				WriteBytes(Value.data(), Value.size());
			}
		};

		struct FByteReader
		{
			std::span<const uint8> Bytes;
			size_t Offset = 0;

			template<typename T> auto Read(T& Value) -> bool
			{
				if (Offset + sizeof(T) > Bytes.size()) return false;
				std::memcpy(&Value, Bytes.data() + Offset, sizeof(T));
				Offset += sizeof(T);
				return true;
			}

			auto ReadBytes(void* Data, size_t Size) -> bool
			{
				if (Offset + Size > Bytes.size()) return false;
				std::memcpy(Data, Bytes.data() + Offset, Size);
				Offset += Size;
				return true;
			}

			auto ReadString(std::string& Value) -> bool
			{
				uint64 Size = 0;
				if (!Read(Size) || Size > Bytes.size() - Offset) return false;
				Value.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), static_cast<size_t>(Size));
				Offset += static_cast<size_t>(Size);
				return true;
			}

			auto ReadSpan(size_t Size, std::span<const uint8>& Out) -> bool
			{
				if (Offset + Size > Bytes.size()) return false;
				Out = Bytes.subspan(Offset, Size);
				Offset += Size;
				return true;
			}
		};

		struct FFieldRecord
		{
			std::string DeclaringClass;
			std::string Name;
			DurinCodeGen::EPropertyGenFlags Kind = DurinCodeGen::EPropertyGenFlags::None;
			std::string TypeSignature;
			std::vector<uint8> Payload;
		};

		struct FObjectRecord
		{
			uint64 Id = 0;
			uint64 OuterId = 0;
			std::string ClassName;
			std::string ObjectName;
			std::vector<FFieldRecord> Fields;
		};

		struct FPackageFile
		{
			uint32 FormatVersion = 0;
			std::string AssetClassName;
			std::vector<FAssetPath> Dependencies;
			std::vector<FObjectRecord> Objects;
		};

		auto Error(EAssetError Code, std::string Message) -> FAssetResult { return {Code, std::move(Message)}; }

		auto GetMoveContributors() -> std::unordered_map<DClass*, FAssetMoveContributor>&
		{
			static std::unordered_map<DClass*, FAssetMoveContributor> Contributors;
			return Contributors;
		}

		auto GetDeleteContributors() -> std::unordered_map<DClass*, FAssetDeleteContributor>&
		{
			static std::unordered_map<DClass*, FAssetDeleteContributor> Contributors;
			return Contributors;
		}

		auto GetPhysicalPath(const FAssetPath& Path) -> std::string
		{
			return FPaths::Resolve(Path.GetView()) + ".dasset";
		}

		auto PhysicalToAssetPath(const std::filesystem::path& File, FAssetPath& OutPath) -> bool
		{
			const std::filesystem::path NormalizedFile = std::filesystem::absolute(File).lexically_normal();
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				const std::filesystem::path Root = std::filesystem::absolute(Mount.PhysicalPath).lexically_normal();
				std::error_code Ec;
				std::filesystem::path Relative = std::filesystem::relative(NormalizedFile, Root, Ec);
				if (Ec || Relative.empty() || Relative.native().starts_with(L"..")) continue;
				Relative.replace_extension();
				return FAssetPath::TryCreate(Mount.VirtualRoot + Relative.generic_string(), OutPath);
			}
			return false;
		}

		auto GetTypeSignature(FProperty* Property) -> std::string
		{
			if (!Property) return "Invalid";
			const auto Kind = Property->GetKind();
			if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
				return std::format("Array<{}>", GetTypeSignature(static_cast<FArrayProperty*>(Property)->GetInner()));
			if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				return std::format("Map<{},{}>", GetTypeSignature(Map->GetKeyProp()), GetTypeSignature(Map->GetValueProp()));
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Object)
				return std::format("Object:{}:{}", Property->GetReferencedClass() ? Property->GetReferencedClass()->GetQualifiedName().ToString() : "DObject", Property->IsObjectPtrWrapper());
			if (Kind == DurinCodeGen::EPropertyGenFlags::Enum)
			{
				auto* EnumProperty = static_cast<FEnumProperty*>(Property);
				return std::format("Enum:{}:{}", EnumProperty->GetEnum() ? EnumProperty->GetEnum()->GetQualifiedName().ToString() : "", Property->GetElementSize());
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				return std::format("Struct<{}>", StructProperty->GetStruct() ? StructProperty->GetStruct()->GetQualifiedName().ToString() : "");
			}
			return std::format("{}:{}", static_cast<uint32>(Kind), Property->GetElementSize());
		}

		auto GatherObjects(DObject* Object, std::vector<DObject*>& OutObjects) -> void
		{
			if (!Object) return;
			OutObjects.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object)) GatherObjects(Inner, OutObjects);
		}

		auto SerializeValue(
			FProperty* Property,
			const void* Container,
			uint32 ArrayIndex,
			FByteWriter& Writer,
			const std::unordered_map<DObject*, uint64>& ObjectIds,
			std::unordered_set<FAssetPath>& Dependencies
		) -> FAssetResult
		{
			if (!Property) return Error(EAssetError::UnsupportedProperty, "Missing property metadata.");
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
			case DurinCodeGen::EPropertyGenFlags::Enum:
				Writer.WriteBytes(Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize());
				return {};
			case DurinCodeGen::EPropertyGenFlags::String:
				Writer.WriteString(*static_cast<FStringProperty*>(Property)->GetStringValuePtr(Container, ArrayIndex));
				return {};
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				if (!ObjectProperty->IsObjectPtrWrapper()) return Error(EAssetError::UnsupportedProperty, "Raw object pointer properties are not serializable.");
				DObject* Referenced = ObjectProperty->GetObjectPropertyValue(Container, ArrayIndex);
				if (!Referenced) { Writer.Write(uint8(0)); return {}; }
				if (auto It = ObjectIds.find(Referenced); It != ObjectIds.end())
				{
					Writer.Write(uint8(1));
					Writer.Write(It->second);
					return {};
				}
				DPackage* ExternalPackage = Referenced->GetPackage();
				if (!ExternalPackage || ExternalPackage->GetAsset() != Referenced) return Error(EAssetError::InvalidObjectGraph, "Cross-package references may only target a package main asset.");
				FAssetPath ExternalPath;
				if (!FAssetPath::TryCreate(ExternalPackage->GetPackagePath(), ExternalPath)) return Error(EAssetError::InvalidPath, "Referenced package has an invalid path.");
				Dependencies.insert(ExternalPath);
				Writer.Write(uint8(2));
				Writer.WriteString(ExternalPath.GetView());
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct) return Error(EAssetError::UnsupportedProperty, "Struct property has no reflected type.");
				Writer.WriteString(Struct->GetQualifiedName().ToString());
				std::vector<FProperty*> Fields;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient)) Fields.push_back(Field);
				}, false);
				Writer.Write(uint64(Fields.size()));
				const void* StructValue = Property->GetValuePtr(Container, ArrayIndex);
				for (FProperty* Field : Fields)
				{
					FByteWriter Payload;
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim(); ++FieldIndex)
					{
						FAssetResult Result = SerializeValue(Field, StructValue, FieldIndex, Payload, ObjectIds, Dependencies);
						if (!Result) return Result;
					}
					Writer.WriteString(Struct->GetQualifiedName().ToString());
					Writer.WriteString(Field->NamePrivate.ToString());
					Writer.Write(uint8(Field->GetKind()));
					Writer.WriteString(GetTypeSignature(Field));
					Writer.Write(uint64(Payload.Bytes.size()));
					Writer.WriteBytes(Payload.Bytes.data(), Payload.Bytes.size());
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* Array = static_cast<FArrayProperty*>(Property);
				if (!Array->HasArrayHelper() || !Array->GetInner()) return Error(EAssetError::UnsupportedProperty, "Array property lacks runtime helpers.");
				const uint64 Num = Array->Num(Container, ArrayIndex);
				Writer.Write(Num);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					FAssetResult Result = SerializeValue(Array->GetInner(), Array->GetElementPtr(Container, Index, ArrayIndex), 0, Writer, ObjectIds, Dependencies);
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				if (!Map->HasMapHelper() || !Map->GetKeyProp() || !Map->GetValueProp()) return Error(EAssetError::UnsupportedProperty, "Map property lacks runtime helpers.");
				const uint64 Num = Map->Num(Container, ArrayIndex);
				Writer.Write(Num);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					FAssetResult Result = SerializeValue(Map->GetKeyProp(), Map->GetKeyPtr(Container, Index, ArrayIndex), 0, Writer, ObjectIds, Dependencies);
					if (!Result) return Result;
					Result = SerializeValue(Map->GetValueProp(), Map->GetMappedValuePtr(Container, Index, ArrayIndex), 0, Writer, ObjectIds, Dependencies);
					if (!Result) return Result;
				}
				return {};
			}
			default:
				return Error(EAssetError::UnsupportedProperty, std::format("Unsupported property kind {}.", static_cast<uint32>(Property->GetKind())));
			}
		}

		auto DeserializeValue(FProperty* Property, void* Container, uint32 ArrayIndex, FByteReader& Reader, const std::vector<DObject*>& Objects) -> FAssetResult
		{
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Bool:
			case DurinCodeGen::EPropertyGenFlags::Int8:
			case DurinCodeGen::EPropertyGenFlags::Int16:
			case DurinCodeGen::EPropertyGenFlags::Int32:
			case DurinCodeGen::EPropertyGenFlags::Int64:
			case DurinCodeGen::EPropertyGenFlags::UInt8:
			case DurinCodeGen::EPropertyGenFlags::UInt16:
			case DurinCodeGen::EPropertyGenFlags::UInt32:
			case DurinCodeGen::EPropertyGenFlags::UInt64:
			case DurinCodeGen::EPropertyGenFlags::Float:
			case DurinCodeGen::EPropertyGenFlags::Double:
			case DurinCodeGen::EPropertyGenFlags::Enum:
				return Reader.ReadBytes(Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize()) ? FAssetResult{} : Error(EAssetError::CorruptFile, "Truncated property payload.");
			case DurinCodeGen::EPropertyGenFlags::String:
			{
				std::string Value;
				if (!Reader.ReadString(Value)) return Error(EAssetError::CorruptFile, "Truncated string property.");
				*static_cast<FStringProperty*>(Property)->GetStringValuePtr(Container, ArrayIndex) = std::move(Value);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind)) return Error(EAssetError::CorruptFile, "Truncated object reference.");
				DObject* Value = nullptr;
				if (ReferenceKind == 1)
				{
					uint64 Id = 0;
					if (!Reader.Read(Id) || Id == 0 || Id > Objects.size()) return Error(EAssetError::InvalidObjectGraph, "Invalid internal object reference.");
					Value = Objects[static_cast<size_t>(Id - 1)];
				}
				else if (ReferenceKind == 2)
				{
					std::string PathString;
					FAssetPath Path;
					if (!Reader.ReadString(PathString) || !FAssetPath::TryCreate(PathString, Path)) return Error(EAssetError::InvalidPath, "Invalid external object reference.");
					FAssetResult Result = FAssetManager::Get().LoadAsset(Path, Value);
					if (!Result) return Error(EAssetError::MissingDependency, Result.Message);
				}
				else if (ReferenceKind != 0) return Error(EAssetError::CorruptFile, "Unknown object reference kind.");
				if (Value && ObjectProperty->GetReferencedClass() && !Value->IsA(ObjectProperty->GetReferencedClass())) return Error(EAssetError::TypeMismatch, "Object reference class mismatch.");
				ObjectProperty->SetObjectPropertyValue(Container, Value, ArrayIndex);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				std::string StructName;
				uint64 FieldCount = 0;
				if (!Struct || !Reader.ReadString(StructName) || StructName != Struct->GetQualifiedName().ToString() || !Reader.Read(FieldCount) || FieldCount > 100000)
					return Error(EAssetError::CorruptFile, "Invalid struct payload header.");
				void* StructValue = Property->GetValuePtr(Container, ArrayIndex);
				for (uint64 Index = 0; Index < FieldCount; ++Index)
				{
					std::string DeclaringStruct, FieldName, Signature;
					uint8 Kind = 0;
					uint64 PayloadSize = 0;
					std::span<const uint8> Payload;
					if (!Reader.ReadString(DeclaringStruct) || !Reader.ReadString(FieldName) || !Reader.Read(Kind) || !Reader.ReadString(Signature) || !Reader.Read(PayloadSize) || PayloadSize > Reader.Bytes.size() || !Reader.ReadSpan(static_cast<size_t>(PayloadSize), Payload))
						return Error(EAssetError::CorruptFile, "Invalid struct field record.");
					if (DeclaringStruct != StructName) continue;
					FProperty* Field = Struct->FindPropertyByName(FName(FieldName), false);
					if (!Field || static_cast<uint8>(Field->GetKind()) != Kind || GetTypeSignature(Field) != Signature)
					{
						DURIN_WARN("Skipping incompatible struct field {}::{}", StructName, FieldName);
						continue;
					}
					FByteReader PayloadReader{Payload};
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim(); ++FieldIndex)
					{
						FAssetResult Result = DeserializeValue(Field, StructValue, FieldIndex, PayloadReader, Objects);
						if (!Result) return Result;
					}
					if (PayloadReader.Offset != Payload.size()) return Error(EAssetError::CorruptFile, "Struct field payload has trailing bytes.");
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* Array = static_cast<FArrayProperty*>(Property);
				uint64 Num = 0;
				if (!Array->HasArrayHelper() || !Reader.Read(Num) || Num > 10000000) return Error(EAssetError::CorruptFile, "Invalid array payload.");
				Array->Resize(Container, Num, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					FAssetResult Result = DeserializeValue(Array->GetInner(), Array->GetMutableElementPtr(Container, Index, ArrayIndex), 0, Reader, Objects);
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Num = 0;
				if (!Map->HasMapHelper() || !Reader.Read(Num) || Num > 10000000) return Error(EAssetError::CorruptFile, "Invalid map payload.");
				Map->Clear(Container, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					void* Key = Map->CreateKey();
					void* Value = Map->CreateValue();
					if (!Key || !Value) return Error(EAssetError::UnsupportedProperty, "Failed to create map entry storage.");
					FAssetResult Result = DeserializeValue(Map->GetKeyProp(), Key, 0, Reader, Objects);
					if (Result) Result = DeserializeValue(Map->GetValueProp(), Value, 0, Reader, Objects);
					if (Result) Map->Insert(Container, Key, Value, ArrayIndex);
					Map->DestroyKey(Key);
					Map->DestroyValue(Value);
					if (!Result) return Result;
				}
				return {};
			}
			default:
				return Error(EAssetError::UnsupportedProperty, "Unsupported property kind.");
			}
		}

		auto WritePackageFile(const FPackageFile& File, FByteWriter& Writer) -> void
		{
			Writer.Write(AssetMagic);
			Writer.Write(AssetVersion);
			Writer.WriteString(File.AssetClassName);
			Writer.Write(uint64(File.Dependencies.size()));
			for (const FAssetPath& Dependency : File.Dependencies) Writer.WriteString(Dependency.GetView());
			Writer.Write(uint64(File.Objects.size()));
			for (const FObjectRecord& Object : File.Objects)
			{
				Writer.Write(Object.Id);
				Writer.Write(Object.OuterId);
				Writer.WriteString(Object.ClassName);
				Writer.WriteString(Object.ObjectName);
				Writer.Write(uint64(Object.Fields.size()));
				for (const FFieldRecord& Field : Object.Fields)
				{
					Writer.WriteString(Field.DeclaringClass);
					Writer.WriteString(Field.Name);
					Writer.Write(uint8(Field.Kind));
					Writer.WriteString(Field.TypeSignature);
					Writer.Write(uint64(Field.Payload.size()));
					Writer.WriteBytes(Field.Payload.data(), Field.Payload.size());
				}
			}
		}

		auto ReadPackageFile(std::span<const uint8> Bytes, FPackageFile& OutFile, bool bHeaderOnly) -> FAssetResult
		{
			FByteReader Reader{Bytes};
			uint32 Magic = 0, Version = 0;
			if (!Reader.Read(Magic) || !Reader.Read(Version)) return Error(EAssetError::CorruptFile, "Truncated asset header.");
			if (Magic != AssetMagic) return Error(EAssetError::CorruptFile, "Invalid asset magic.");
			if (Version != AssetVersion) return Error(EAssetError::UnsupportedVersion, std::format("Unsupported asset version {}.", Version));
			OutFile.FormatVersion = Version;
			if (!Reader.ReadString(OutFile.AssetClassName)) return Error(EAssetError::CorruptFile, "Invalid asset header strings.");
			uint64 DependencyCount = 0;
			if (!Reader.Read(DependencyCount) || DependencyCount > 100000) return Error(EAssetError::CorruptFile, "Invalid dependency count.");
			for (uint64 Index = 0; Index < DependencyCount; ++Index)
			{
				std::string DependencyString;
				FAssetPath Dependency;
				if (!Reader.ReadString(DependencyString) || !FAssetPath::TryCreate(DependencyString, Dependency)) return Error(EAssetError::CorruptFile, "Invalid dependency path.");
				OutFile.Dependencies.push_back(std::move(Dependency));
			}
			uint64 ObjectCount = 0;
			if (!Reader.Read(ObjectCount) || ObjectCount == 0 || ObjectCount > 1000000) return Error(EAssetError::CorruptFile, "Invalid object count.");
			if (bHeaderOnly) return {};
			OutFile.Objects.resize(static_cast<size_t>(ObjectCount));
			for (FObjectRecord& Object : OutFile.Objects)
			{
				uint64 FieldCount = 0;
				if (!Reader.Read(Object.Id) || !Reader.Read(Object.OuterId) || !Reader.ReadString(Object.ClassName) || !Reader.ReadString(Object.ObjectName) || !Reader.Read(FieldCount) || FieldCount > 100000) return Error(EAssetError::CorruptFile, "Invalid object record.");
				Object.Fields.resize(static_cast<size_t>(FieldCount));
				for (FFieldRecord& Field : Object.Fields)
				{
					uint8 Kind = 0;
					uint64 PayloadSize = 0;
					std::span<const uint8> Payload;
					if (!Reader.ReadString(Field.DeclaringClass) || !Reader.ReadString(Field.Name) || !Reader.Read(Kind) || !Reader.ReadString(Field.TypeSignature) || !Reader.Read(PayloadSize) || PayloadSize > Bytes.size() || !Reader.ReadSpan(static_cast<size_t>(PayloadSize), Payload)) return Error(EAssetError::CorruptFile, "Invalid field record.");
					Field.Kind = static_cast<DurinCodeGen::EPropertyGenFlags>(Kind);
					Field.Payload.assign(Payload.begin(), Payload.end());
				}
			}
			if (Reader.Offset != Bytes.size()) return Error(EAssetError::CorruptFile, "Trailing bytes after package data.");
			return {};
		}

		auto FindExistingInner(DObject* Outer, std::string_view Name, DClass* Class, bool& bTypeMismatch) -> DObject*
		{
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Outer))
			{
				if (Inner->GetName() != Name) continue;
				if (Inner->GetClass() == Class) return Inner;
				bTypeMismatch = true;
				return nullptr;
			}
			return nullptr;
		}
	}

	auto RegisterAssetMoveContributor(DClass* Class, FAssetMoveContributor Contributor) -> void
	{
		if (Class && Contributor) GetMoveContributors().insert_or_assign(Class, std::move(Contributor));
	}

	auto RegisterAssetDeleteContributor(DClass* Class, FAssetDeleteContributor Contributor) -> void
	{
		if (Class && Contributor) GetDeleteContributors().insert_or_assign(Class, std::move(Contributor));
	}

	auto FAssetRegistry::ScanMountedContent() -> FAssetResult
	{
		std::unordered_map<FAssetPath, FAssetData> NewAssets;
		ScanErrors.clear();
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			std::error_code Ec;
			if (!std::filesystem::exists(Mount.PhysicalPath, Ec)) continue;
			for (std::filesystem::recursive_directory_iterator It(Mount.PhysicalPath, Ec), End; !Ec && It != End; It.increment(Ec))
			{
				if (!It->is_regular_file() || It->path().extension() != ".dasset") continue;
				std::vector<uint8> Bytes;
				FPackageFile PackageFile;
				FAssetPath DiskPath;
				if (!FFileHelper::LoadFileToArray(Bytes, It->path().generic_string()) || !PhysicalToAssetPath(It->path(), DiskPath))
				{
					ScanErrors.push_back(Error(EAssetError::IoError, std::format("Failed to read asset header {}.", It->path().generic_string())));
					continue;
				}
				FAssetResult Result = ReadPackageFile(Bytes, PackageFile, true);
				if (!Result) { ScanErrors.push_back(std::move(Result)); continue; }
				if (NewAssets.contains(DiskPath)) { ScanErrors.push_back(Error(EAssetError::AlreadyExists, std::format("Duplicate asset path {}.", DiskPath.ToString()))); continue; }
				NewAssets.emplace(DiskPath, FAssetData{DiskPath, It->path().generic_string(), PackageFile.AssetClassName, PackageFile.FormatVersion, PackageFile.Dependencies, It->last_write_time(Ec)});
			}
		}
		Assets = std::move(NewAssets);
		return {};
	}

	auto FAssetRegistry::FindAsset(const FAssetPath& Path) const -> const FAssetData*
	{
		auto It = Assets.find(Path);
		return It == Assets.end() ? nullptr : &It->second;
	}

	auto FAssetRegistry::AddOrUpdate(FAssetData Data) -> void { Assets.insert_or_assign(Data.PackagePath, std::move(Data)); }

	auto FAssetManager::Get() -> FAssetManager&
	{
		static FAssetManager Instance;
		return Instance;
	}

	auto FAssetManager::CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!Path.IsValid() || !Class || !Class->ClassConstructor) return Error(EAssetError::InvalidPath, "Invalid asset path or class.");
		if (LoadedPackages.contains(Path) || Registry.FindAsset(Path)) return Error(EAssetError::AlreadyExists, std::format("Asset {} already exists.", Path.ToString()));

		DPackage* Package = NewObject<DPackage>(nullptr, FName(Path.GetAssetName()));
		Package->InitializeAssetPackage(Path);
		AddToRoot(Package);
		FStaticConstructObjectParameters Params{Class, Package, FName(Path.GetAssetName()), Size};
		OutAsset = StaticConstructObject(Params);
		DObjectForceRegistration(OutAsset);
		if (!Package->SetAsset(OutAsset))
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutAsset = nullptr;
			return Error(EAssetError::InvalidObjectGraph, "Failed to assign package asset.");
		}
		LoadedPackages.emplace(Path, Package);
		return {};
	}

	auto FAssetManager::SavePackage(DPackage* Package) -> FAssetResult
	{
		if (Package && !Package->IsAssetPackage()) return Error(EAssetError::InvalidPackageType, "Only asset packages can be saved.");
		if (!Package || !Package->GetAsset()) return Error(EAssetError::InvalidObjectGraph, "Package has no main asset.");
		FAssetPath Path;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), Path)) return Error(EAssetError::InvalidPath, "Package path is invalid.");

		std::vector<DObject*> Objects;
		GatherObjects(Package->GetAsset(), Objects);
		std::unordered_map<DObject*, uint64> ObjectIds;
		for (size_t Index = 0; Index < Objects.size(); ++Index) ObjectIds.emplace(Objects[Index], Index + 1);

		FPackageFile File;
		File.AssetClassName = Package->GetAsset()->GetClass()->GetQualifiedName().ToString();
		std::unordered_set<FAssetPath> Dependencies;
		for (size_t Index = 0; Index < Objects.size(); ++Index)
		{
			DObject* Object = Objects[Index];
			FObjectRecord Record;
			Record.Id = Index + 1;
			if (Object == Package->GetAsset()) Record.OuterId = 0;
			else
			{
				auto OuterIt = ObjectIds.find(Object->GetOuter());
				if (OuterIt == ObjectIds.end()) return Error(EAssetError::InvalidObjectGraph, "Package inner object has an outer outside the package graph.");
				Record.OuterId = OuterIt->second;
			}
			Record.ClassName = Object->GetClass()->GetQualifiedName().ToString();
			Record.ObjectName = Object->GetName();
			FAssetResult SerializationResult;
			Object->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (!SerializationResult) return;
				if (!Property || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
				FFieldRecord Field;
				DClass* DeclaringClass = Cast<DClass>(Property->Owner.ToDObject());
				Field.DeclaringClass = DeclaringClass ? DeclaringClass->GetQualifiedName().ToString() : Object->GetClass()->GetQualifiedName().ToString();
				Field.Name = Property->NamePrivate.ToString();
				Field.Kind = Property->GetKind();
				Field.TypeSignature = GetTypeSignature(Property);
				FByteWriter PayloadWriter;
				for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
				{
					FAssetResult Result = SerializeValue(Property, Object, ArrayIndex, PayloadWriter, ObjectIds, Dependencies);
					if (!Result) { SerializationResult = std::move(Result); return; }
				}
				Field.Payload = std::move(PayloadWriter.Bytes);
				Record.Fields.push_back(std::move(Field));
			}, true);
			if (!SerializationResult) return SerializationResult;
			File.Objects.push_back(std::move(Record));
		}
		File.Dependencies.assign(Dependencies.begin(), Dependencies.end());
		std::ranges::sort(File.Dependencies, [](const FAssetPath& A, const FAssetPath& B) { return A.GetView() < B.GetView(); });

		FByteWriter Writer;
		WritePackageFile(File, Writer);
		const std::filesystem::path Destination(GetPhysicalPath(Path));
		const std::filesystem::path Temporary = Destination.string() + ".tmp";
		if (!FFileHelper::SaveArrayToFile(std::span{reinterpret_cast<const std::byte*>(Writer.Bytes.data()), Writer.Bytes.size()}, Temporary)) return Error(EAssetError::IoError, "Failed to write temporary asset file.");
		std::error_code Ec;
		std::filesystem::rename(Temporary, Destination, Ec);
		if (Ec)
		{
			const std::filesystem::path Backup = Destination.string() + ".bak";
			std::filesystem::remove(Backup, Ec);
			Ec.clear();
			if (std::filesystem::exists(Destination)) std::filesystem::rename(Destination, Backup, Ec);
			if (!Ec) std::filesystem::rename(Temporary, Destination, Ec);
			if (Ec)
			{
				if (std::filesystem::exists(Backup)) std::filesystem::rename(Backup, Destination);
				return Error(EAssetError::IoError, "Failed to replace asset file.");
			}
			std::filesystem::remove(Backup, Ec);
		}
		Package->ClearDirty();
		Registry.AddOrUpdate(FAssetData{Path, Destination.generic_string(), File.AssetClassName, AssetVersion, File.Dependencies, std::filesystem::last_write_time(Destination)});
		return {};
	}

	auto FAssetManager::MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult
	{
		if (!OldPath.IsValid() || !NewPath.IsValid() || OldPath == NewPath) return Error(EAssetError::InvalidPath, "Asset move paths are invalid or identical.");
		const FAssetData* SourceData = Registry.FindAsset(OldPath);
		if (!SourceData) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", OldPath.ToString()));
		const std::filesystem::path OldFile(GetPhysicalPath(OldPath));
		const std::filesystem::path NewFile(GetPhysicalPath(NewPath));
		if (Registry.FindAsset(NewPath) || LoadedPackages.contains(NewPath) || std::filesystem::exists(NewFile)) return Error(EAssetError::AlreadyExists, std::format("Asset {} already exists.", NewPath.ToString()));

		std::vector<FAssetPath> ReferrerPaths;
		for (const auto& [Path, Data] : Registry.GetAssets())
			if (Path != OldPath && std::ranges::find(Data.Dependencies, OldPath) != Data.Dependencies.end()) ReferrerPaths.push_back(Path);

		DPackage* MovingPackage = nullptr;
		FAssetResult Result = LoadPackageInternal(OldPath, MovingPackage);
		if (!Result) return Result;
		std::vector<DPackage*> Referrers;
		for (const FAssetPath& Path : ReferrerPaths)
		{
			DPackage* Package = nullptr;
			Result = LoadPackageInternal(Path, Package);
			if (!Result) return Result;
			Referrers.push_back(Package);
		}

		FAssetMoveContribution Contribution;
		for (DClass* Class = MovingPackage->GetAsset()->GetClass(); Class; Class = Class->GetSuperClass())
		{
			auto It = GetMoveContributors().find(Class);
			if (It == GetMoveContributors().end()) continue;
			Result = It->second(MovingPackage->GetAsset(), OldPath, NewPath, Contribution);
			if (!Result) return Result;
			break;
		}
		for (const auto& [From, To] : Contribution.Files)
		{
			if (!std::filesystem::is_regular_file(From)) return Error(EAssetError::NotFound, std::format("Companion file {} was not found.", From.generic_string()));
			if (std::filesystem::exists(To)) return Error(EAssetError::AlreadyExists, std::format("Companion destination {} already exists.", To.generic_string()));
		}

		const auto RegistryBackup = Registry.Assets;
		const std::string OldName = MovingPackage->GetAsset()->GetName();
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Backups;
		auto Backup = [&](const std::filesystem::path& File) -> bool {
			if (!std::filesystem::exists(File)) return false;
			const std::filesystem::path Copy = File.string() + ".movebak";
			std::error_code Ec; std::filesystem::remove(Copy, Ec); Ec.clear();
			std::filesystem::copy_file(File, Copy, std::filesystem::copy_options::overwrite_existing, Ec);
			if (Ec) return false;
			Backups.emplace_back(File, Copy); return true;
		};
		if (!Backup(OldFile)) return Error(EAssetError::IoError, "Failed to back up source asset.");
		for (const FAssetPath& Path : ReferrerPaths) if (!Backup(GetPhysicalPath(Path))) return Error(EAssetError::IoError, "Failed to back up an asset referrer.");
		for (const auto& [From, To] : Contribution.Files) if (!Backup(From)) return Error(EAssetError::IoError, "Failed to back up a companion file.");

		auto Rollback = [&]() {
			if (Contribution.Rollback) Contribution.Rollback();
			MovingPackage->RelocateAssetPackage(OldPath);
			MovingPackage->Rename(FName(OldPath.GetAssetName()));
			MovingPackage->GetAsset()->Rename(FName(OldName));
			std::error_code Ec;
			std::filesystem::remove(NewFile, Ec);
			for (const auto& [From, To] : Contribution.Files) std::filesystem::remove(To, Ec);
			for (const auto& [Original, Copy] : Backups) { Ec.clear(); std::filesystem::copy_file(Copy, Original, std::filesystem::copy_options::overwrite_existing, Ec); std::filesystem::remove(Copy, Ec); }
			Registry.Assets = RegistryBackup;
		};

		if (!MovingPackage->RelocateAssetPackage(NewPath)) { Rollback(); return Error(EAssetError::AlreadyExists, "The destination package path is registered."); }
		MovingPackage->Rename(FName(NewPath.GetAssetName()));
		if (OldPath.GetAssetName() != NewPath.GetAssetName()) MovingPackage->GetAsset()->Rename(FName(NewPath.GetAssetName()));
		if (Contribution.Apply) Contribution.Apply();
		for (const auto& [From, To] : Contribution.Files)
		{
			std::error_code Ec; std::filesystem::create_directories(To.parent_path(), Ec); Ec.clear(); std::filesystem::rename(From, To, Ec);
			if (Ec) { Rollback(); return Error(EAssetError::IoError, "Failed to move a companion file."); }
		}
		LoadedPackages.erase(OldPath);
		LoadedPackages.emplace(NewPath, MovingPackage);
		Registry.Assets.erase(OldPath);
		std::error_code DirectoryEc;
		std::filesystem::create_directories(NewFile.parent_path(), DirectoryEc);
		if (DirectoryEc) { LoadedPackages.erase(NewPath); LoadedPackages.emplace(OldPath, MovingPackage); Rollback(); return Error(EAssetError::IoError, "Failed to create the destination directory."); }
		Result = SavePackage(MovingPackage);
		if (Result) for (DPackage* Referrer : Referrers) { Result = SavePackage(Referrer); if (!Result) break; }
		if (!Result) { LoadedPackages.erase(NewPath); LoadedPackages.emplace(OldPath, MovingPackage); Rollback(); return Result; }
		std::error_code Ec; std::filesystem::remove(OldFile, Ec);
		if (Ec) { LoadedPackages.erase(NewPath); LoadedPackages.emplace(OldPath, MovingPackage); Rollback(); return Error(EAssetError::IoError, "Failed to remove the old asset file."); }
		for (const auto& [Original, Copy] : Backups) std::filesystem::remove(Copy, Ec);
		return {};
	}

	auto FAssetManager::AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult
	{
		OutAnalysis = {};
		OutAnalysis.AssetPath = Path;
		const FAssetData* Data = Registry.FindAsset(Path);
		if (!Path.IsValid() || !Data) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));

		for (const auto& [OtherPath, OtherData] : Registry.GetAssets())
		{
			if (OtherPath != Path && std::ranges::find(OtherData.Dependencies, Path) != OtherData.Dependencies.end())
				OutAnalysis.DirectReferencers.push_back(OtherPath);
		}
		std::ranges::sort(OutAnalysis.DirectReferencers, [](const FAssetPath& A, const FAssetPath& B) { return A.GetView() < B.GetView(); });
		OutAnalysis.bLoaded = LoadedPackages.contains(Path);
		OutAnalysis.bLoading = LoadingPackages.contains(Path);

		DPackage* Package = nullptr;
		const bool bTemporarilyLoaded = !OutAnalysis.bLoaded;
		std::unordered_set<FAssetPath> PreviouslyLoaded;
		if (bTemporarilyLoaded)
			for (const auto& [LoadedPath, LoadedPackage] : LoadedPackages) PreviouslyLoaded.insert(LoadedPath);
		DObject* LoadedAsset = nullptr;
		FAssetResult Result = LoadAsset(Path, LoadedAsset);
		Package = LoadedAsset ? LoadedAsset->GetPackage() : nullptr;
		if (!Result) return Result;
		if (Package && Package->GetAsset())
		{
			for (DClass* Class = Package->GetAsset()->GetClass(); Class; Class = Class->GetSuperClass())
			{
				auto It = GetDeleteContributors().find(Class);
				if (It == GetDeleteContributors().end()) continue;
				FAssetDeleteContribution Contribution;
				Result = It->second(Package->GetAsset(), Contribution);
				if (Result) OutAnalysis.CompanionFiles = std::move(Contribution.Files);
				break;
			}
		}
		if (bTemporarilyLoaded)
		{
			FAssetResult UnloadResult = UnloadPackage(Path);
			if (Result && !UnloadResult) Result = std::move(UnloadResult);
			bool bMadeProgress = true;
			while (bMadeProgress)
			{
				bMadeProgress = false;
				std::vector<FAssetPath> TemporaryDependencies;
				for (const auto& [LoadedPath, LoadedPackage] : LoadedPackages)
					if (!PreviouslyLoaded.contains(LoadedPath)) TemporaryDependencies.push_back(LoadedPath);
				for (const FAssetPath& TemporaryPath : TemporaryDependencies)
				{
					if (UnloadPackage(TemporaryPath)) bMadeProgress = true;
				}
			}
		}
		return Result;
	}

	auto FAssetManager::DeleteAsset(const FAssetPath& Path) -> FAssetResult
	{
		FAssetDeleteAnalysis Analysis;
		FAssetResult Result = AnalyzeAssetDeletion(Path, Analysis);
		if (!Result) return Result;
		if (!Analysis.DirectReferencers.empty())
			return Error(EAssetError::InUse, std::format("Asset {} is referenced by {} asset(s).", Path.ToString(), Analysis.DirectReferencers.size()));
		if (Analysis.bLoaded) return Error(EAssetError::InUse, "Asset is loaded and may still be used by the editor or runtime.");
		if (Analysis.bLoading) return Error(EAssetError::InUse, "Asset is currently loading.");

		const FAssetData* Data = Registry.FindAsset(Path);
		if (!Data) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));
		std::vector<std::filesystem::path> Files;
		Files.emplace_back(Data->PhysicalPath);
		for (const std::filesystem::path& Companion : Analysis.CompanionFiles)
		{
			const std::filesystem::path Normalized = std::filesystem::absolute(Companion).lexically_normal();
			if (std::ranges::find(Files, Normalized) == Files.end()) Files.push_back(Normalized);
		}

		const auto RegistryBackup = Registry.Assets;
		struct FStagedDeleteFile
		{
			std::filesystem::path Original;
			std::filesystem::path Staged;
			std::filesystem::path RecoveryCopy;
		};
		std::vector<FStagedDeleteFile> StagedFiles;
		auto Rollback = [&]() {
			std::error_code Ec;
			for (auto It = StagedFiles.rbegin(); It != StagedFiles.rend(); ++It)
			{
				Ec.clear();
				if (std::filesystem::exists(It->Staged)) std::filesystem::rename(It->Staged, It->Original, Ec);
				else if (std::filesystem::exists(It->RecoveryCopy)) std::filesystem::copy_file(It->RecoveryCopy, It->Original, std::filesystem::copy_options::overwrite_existing, Ec);
				std::filesystem::remove(It->RecoveryCopy, Ec);
			}
			Registry.Assets = RegistryBackup;
		};

		for (const std::filesystem::path& File : Files)
		{
			if (!std::filesystem::exists(File)) continue;
			std::filesystem::path Staged = File.string() + ".deletebak";
			std::error_code Ec;
			std::filesystem::remove(Staged, Ec);
			Ec.clear();
			std::filesystem::rename(File, Staged, Ec);
			if (Ec) { Rollback(); return Error(EAssetError::IoError, std::format("Failed to stage {} for deletion.", File.generic_string())); }
			const std::filesystem::path RecoveryCopy = Staged.string() + ".copy";
			Ec.clear();
			std::filesystem::remove(RecoveryCopy, Ec);
			Ec.clear();
			std::filesystem::copy_file(Staged, RecoveryCopy, std::filesystem::copy_options::overwrite_existing, Ec);
			if (Ec)
			{
				std::filesystem::rename(Staged, File, Ec);
				Rollback();
				return Error(EAssetError::IoError, std::format("Failed to prepare rollback data for {}.", File.generic_string()));
			}
			StagedFiles.push_back({File, Staged, RecoveryCopy});
		}

		Registry.Assets.erase(Path);
		for (const FStagedDeleteFile& File : StagedFiles)
		{
			std::error_code Ec;
			if (!std::filesystem::remove(File.Staged, Ec) || Ec)
			{
				Rollback();
				return Error(EAssetError::IoError, std::format("Failed to delete {}.", File.Original.generic_string()));
			}
		}
		for (const FStagedDeleteFile& File : StagedFiles)
		{
			std::error_code Ec;
			std::filesystem::remove(File.RecoveryCopy, Ec);
		}
		return {};
	}

	auto FAssetManager::LoadAsset(const FAssetPath& Path, DObject*& OutAsset) -> FAssetResult
	{
		const bool bRootLoad = LoadDepth++ == 0;
		if (bRootLoad) TransactionPackages.clear();
		DPackage* Package = nullptr;
		FAssetResult Result = LoadPackageInternal(Path, Package);
		--LoadDepth;
		if (bRootLoad)
		{
			if (!Result)
			{
				bool bDiscardedPackage = false;
				for (auto It = TransactionPackages.rbegin(); It != TransactionPackages.rend(); ++It)
				{
					auto LoadedIt = LoadedPackages.find(*It);
					if (LoadedIt == LoadedPackages.end()) continue;
					DPackage* TransactionPackage = LoadedIt->second;
					LoadedPackages.erase(LoadedIt);
					LoadingPackages.erase(*It);
					RemoveFromRoot(TransactionPackage);
					MarkObjectHierarchyAsGarbage(TransactionPackage);
					bDiscardedPackage = true;
				}
				if (bDiscardedPackage) CollectGarbage();
			}
			TransactionPackages.clear();
		}
		OutAsset = Result && Package ? Package->GetAsset() : nullptr;
		return Result;
	}

	auto FAssetManager::LoadPackageInternal(const FAssetPath& Path, DPackage*& OutPackage) -> FAssetResult
	{
		if (auto It = LoadedPackages.find(Path); It != LoadedPackages.end())
		{
			OutPackage = It->second;
			return {};
		}
		std::vector<uint8> Bytes;
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath)) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));
		FPackageFile File;
		FAssetResult Result = ReadPackageFile(Bytes, File, false);
		if (!Result) return Result;

		DPackage* Package = NewObject<DPackage>(nullptr, FName(Path.GetAssetName()));
		Package->InitializeAssetPackage(Path);
		AddToRoot(Package);
		LoadedPackages.emplace(Path, Package);
		LoadingPackages.insert(Path);
		if (LoadDepth > 0) TransactionPackages.push_back(Path);
		std::vector<DObject*> Objects(File.Objects.size(), nullptr);

		auto Rollback = [&]() {
			LoadingPackages.erase(Path);
			LoadedPackages.erase(Path);
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
			CollectGarbage();
			OutPackage = nullptr;
		};

		for (const FObjectRecord& Record : File.Objects)
		{
			if (Record.Id == 0 || Record.Id > Objects.size() || Objects[Record.Id - 1] || (Record.Id != 1 && (Record.OuterId == 0 || Record.OuterId >= Record.Id))) { Rollback(); return Error(EAssetError::InvalidObjectGraph, "Invalid object identifiers or ordering."); }
			DClass* Class = FindClassByQualifiedName(Record.ClassName);
			if (!Class || !Class->ClassConstructor) { Rollback(); return Error(EAssetError::UnknownClass, std::format("Unknown asset class {}.", Record.ClassName)); }
			DObject* Outer = Record.OuterId == 0 ? static_cast<DObject*>(Package) : Objects[Record.OuterId - 1];
			bool bTypeMismatch = false;
			DObject* Object = FindExistingInner(Outer, Record.ObjectName, Class, bTypeMismatch);
			if (bTypeMismatch) { Rollback(); return Error(EAssetError::TypeMismatch, "Existing default inner object has a different class."); }
			if (!Object)
			{
				FStaticConstructObjectParameters Params{Class, Outer, FName(Record.ObjectName), Class->PropertiesSize};
				Object = StaticConstructObject(Params);
				DObjectForceRegistration(Object);
			}
			Objects[Record.Id - 1] = Object;
			if (Record.Id == 1 && !Package->SetAsset(Object)) { Rollback(); return Error(EAssetError::InvalidObjectGraph, "Failed to set package main asset."); }
		}

		for (const FAssetPath& Dependency : File.Dependencies)
		{
			DPackage* DependencyPackage = nullptr;
			Result = LoadPackageInternal(Dependency, DependencyPackage);
			if (!Result) { Rollback(); return Error(EAssetError::MissingDependency, Result.Message); }
		}

		for (const FObjectRecord& Record : File.Objects)
		{
			DObject* Object = Objects[Record.Id - 1];
			for (const FFieldRecord& Field : Record.Fields)
			{
				DClass* DeclaringClass = FindClassByQualifiedName(Field.DeclaringClass);
				if (!DeclaringClass || !Object->IsA(DeclaringClass)) continue;
				FProperty* Property = DeclaringClass->FindPropertyByName(FName(Field.Name), false);
				if (!Property || Property->GetKind() != Field.Kind || GetTypeSignature(Property) != Field.TypeSignature)
				{
					DURIN_WARN("Skipping incompatible asset field {}::{} in {}", Field.DeclaringClass, Field.Name, Path.ToString());
					continue;
				}
				FByteReader FieldReader{Field.Payload};
				for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
				{
					Result = DeserializeValue(Property, Object, ArrayIndex, FieldReader, Objects);
					if (!Result) { Rollback(); return Result; }
				}
				if (FieldReader.Offset != Field.Payload.size()) { Rollback(); return Error(EAssetError::CorruptFile, "Property payload has trailing bytes."); }
			}
		}

		for (auto It = Objects.rbegin(); It != Objects.rend(); ++It)
		{
			std::string PostLoadError;
			if (*It && !(*It)->PostLoad(PostLoadError))
			{
				Rollback();
				return Error(EAssetError::InvalidObjectGraph, PostLoadError.empty() ? "Object PostLoad failed." : std::move(PostLoadError));
			}
		}

		Package->ClearDirty();
		LoadingPackages.erase(Path);
		OutPackage = Package;
		Registry.AddOrUpdate(FAssetData{Path, PhysicalPath, File.AssetClassName, File.FormatVersion, File.Dependencies, std::filesystem::last_write_time(PhysicalPath)});
		return {};
	}

	auto FAssetManager::FindLoadedPackage(const FAssetPath& Path) const -> DPackage*
	{
		auto It = LoadedPackages.find(Path);
		return It == LoadedPackages.end() ? nullptr : It->second;
	}

	auto FAssetManager::IsPackageReferenced(const DPackage* Package) const -> bool
	{
		if (!Package) return false;
		FAssetPath Path;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), Path)) return false;
		for (const auto& [OtherPath, OtherPackage] : LoadedPackages)
		{
			if (OtherPackage == Package) continue;
			const FAssetData* Data = Registry.FindAsset(OtherPath);
			if (Data && std::ranges::find(Data->Dependencies, Path) != Data->Dependencies.end()) return true;
		}
		return false;
	}

	auto FAssetManager::UnloadPackage(const FAssetPath& Path) -> FAssetResult
	{
		auto It = LoadedPackages.find(Path);
		if (It == LoadedPackages.end()) return Error(EAssetError::NotFound, "Package is not loaded.");
		if (LoadingPackages.contains(Path) || IsPackageReferenced(It->second)) return Error(EAssetError::InUse, "Package is still referenced.");
		DPackage* Package = It->second;
		LoadedPackages.erase(It);
		RemoveFromRoot(Package);
		MarkObjectHierarchyAsGarbage(Package);
		CollectGarbage();
		return {};
	}

	auto FAssetManager::Shutdown() -> void
	{
		std::vector<DPackage*> Packages;
		Packages.reserve(LoadedPackages.size());
		for (const auto& [Path, Package] : LoadedPackages)
		{
			if (Package) Packages.push_back(Package);
		}
		LoadedPackages.clear();
		LoadingPackages.clear();
		TransactionPackages.clear();
		LoadDepth = 0;
		for (DPackage* Package : Packages)
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
		CollectGarbage();
	}

	auto LoadAsset(const FAssetPath& Path, DObject*& OutAsset) -> FAssetResult { return FAssetManager::Get().LoadAsset(Path, OutAsset); }
	auto SavePackage(DPackage* Package) -> FAssetResult { return FAssetManager::Get().SavePackage(Package); }
	auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult { return FAssetManager::Get().MoveAsset(OldPath, NewPath); }
	auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult { return FAssetManager::Get().AnalyzeAssetDeletion(Path, OutAnalysis); }
	auto DeleteAsset(const FAssetPath& Path) -> FAssetResult { return FAssetManager::Get().DeleteAsset(Path); }
	auto FindLoadedPackage(const FAssetPath& Path) -> DPackage* { return FAssetManager::Get().FindLoadedPackage(Path); }
	auto UnloadPackage(const FAssetPath& Path) -> FAssetResult { return FAssetManager::Get().UnloadPackage(Path); }
	auto ShutdownAssetManager() -> void { FAssetManager::Get().Shutdown(); }
	auto GetAssetRegistry() -> FAssetRegistry& { return FAssetManager::Get().GetRegistry(); }
}
