#include "AssetSystem.h"
#include "Profiling/Profiling.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/FileHelper.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"

namespace Durin::Asset
{
	namespace
	{
		thread_local FAssetLoadReport* GActiveAssetLoadReport = nullptr;

		constexpr uint32 AssetMagic = 0x54534144; // DAST
		constexpr uint32 AssetVersion = 2;
		constexpr uint64 MaximumPackageStringBytes = 1024 * 1024;

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

			auto ReadString(std::string& Value, uint64 MaximumSize = std::numeric_limits<uint64>::max()) -> bool
			{
				uint64 Size = 0;
				if (!Read(Size) || Size > MaximumSize || Size > Bytes.size() - Offset) return false;
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

		struct FFileByteReader
		{
			std::ifstream Stream;
			uint64 FileSize = 0;
			uint64 Offset = 0;

			explicit FFileByteReader(std::string_view Path)
				: Stream(std::string(Path), std::ios::binary)
			{
				if (!Stream) return;
				Stream.seekg(0, std::ios::end);
				const std::streamoff Size = Stream.tellg();
				if (Size < 0) { Stream.setstate(std::ios::failbit); return; }
				FileSize = static_cast<uint64>(Size);
				Stream.seekg(0, std::ios::beg);
			}

			auto IsOpen() const -> bool { return Stream.is_open() && !Stream.fail(); }

			template<typename T> auto Read(T& Value) -> bool
			{
				if (sizeof(T) > FileSize - std::min(Offset, FileSize)) return false;
				Stream.read(reinterpret_cast<char*>(&Value), sizeof(T));
				if (!Stream) return false;
				Offset += sizeof(T);
				return true;
			}

			auto ReadString(std::string& Value, uint64 MaximumSize = MaximumPackageStringBytes) -> bool
			{
				uint64 Size = 0;
				if (!Read(Size) || Size > MaximumSize || Size > FileSize - std::min(Offset, FileSize)) return false;
				Value.resize(static_cast<size_t>(Size));
				if (Size != 0)
				{
					Stream.read(Value.data(), static_cast<std::streamsize>(Size));
					if (!Stream) return false;
				}
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

		auto IsMissingPathError(const std::error_code& ErrorCode) -> bool
		{
			return ErrorCode == std::errc::no_such_file_or_directory
				|| ErrorCode.value() == 2
				|| ErrorCode.value() == 3;
		}

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

		struct FRegisteredStructureUpgrader
		{
			std::string HandlerId;
			FAssetStructureUpgrader Upgrader;
		};

		auto GetStructureUpgraders() -> std::unordered_map<DClass*, FRegisteredStructureUpgrader>&
		{
			static std::unordered_map<DClass*, FRegisteredStructureUpgrader> Upgraders;
			return Upgraders;
		}

		auto MakePackageFingerprint(
			std::string_view PhysicalPath,
			std::span<const uint8> Bytes,
			FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
		{
			std::error_code ErrorCode;
			const std::filesystem::path Path(PhysicalPath);
			const auto LastWriteTime = std::filesystem::last_write_time(Path, ErrorCode);
			if (ErrorCode)
				return Error(EAssetError::IoError, std::format(
					"Failed to read the last-write time for asset package {}.", PhysicalPath));
			OutFingerprint = {
				.FileSize = Bytes.size(),
				.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime),
				.ContentHash = FXxHash128::HashBuffer(Bytes)};
			return {};
		}

		auto GetPhysicalPath(const FAssetPath& Path) -> std::string
		{
			const FPackageLoadContext& Context = FAssetManager::Get().GetPackageLoadContext();
			if (Context.Mode == EPackageLoadMode::CookedRuntime)
			{
				std::filesystem::path CookedPath;
				if (!ResolveCookedPackagePath(Context.CookRoot, Path.GetView(), CookedPath)) return {};
				return CookedPath.generic_string();
			}
			const PathUtilities::FAssetPathResult Resolved =
				PathUtilities::ResolveAssetPath(Path.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
				DURIN_WARN_CATEGORY(
					"AssetSystem", "Failed to resolve asset path {}: {}", Path.ToString(), Resolved.Message);
			return Resolved ? Resolved.PhysicalPath.generic_string() + ".dasset" : std::string{};
		}

		auto GetSerializedTypeSignature(FProperty* Property) -> std::string
		{
			if (!Property) return "Invalid";
			const auto Kind = Property->GetKind();
			if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
				return std::format(
					"Array<{}>",
					GetSerializedTypeSignature(static_cast<FArrayProperty*>(Property)->GetInner()));
			if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				return std::format(
					"Map<{},{}>",
					GetSerializedTypeSignature(Map->GetKeyProp()),
					GetSerializedTypeSignature(Map->GetValueProp()));
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
			if (Kind == DurinCodeGen::EPropertyGenFlags::String
				|| Kind == DurinCodeGen::EPropertyGenFlags::Name
				|| Kind == DurinCodeGen::EPropertyGenFlags::Guid)
				return std::format("{}:v1", static_cast<uint32>(Kind));
			return std::format("{}:{}", static_cast<uint32>(Kind), Property->GetElementSize());
		}

		auto IsLegacyAbiSizedLogicalSignature(
			DurinCodeGen::EPropertyGenFlags Kind,
			std::string_view Signature) -> bool
		{
			if (Kind != DurinCodeGen::EPropertyGenFlags::String
				&& Kind != DurinCodeGen::EPropertyGenFlags::Name
				&& Kind != DurinCodeGen::EPropertyGenFlags::Guid)
				return false;

			const std::string Prefix = std::format("{}:", static_cast<uint32>(Kind));
			const std::string_view Size = Signature.starts_with(Prefix)
				? Signature.substr(Prefix.size())
				: std::string_view{};
			return !Size.empty() && std::ranges::all_of(Size, [](char Character) {
				return Character >= '0' && Character <= '9';
			});
		}

		auto FindMapSignatureSeparator(std::string_view Signature) -> size_t
		{
			uint32 Depth = 0;
			for (size_t Index = 0; Index < Signature.size(); ++Index)
			{
				if (Signature[Index] == '<') ++Depth;
				else if (Signature[Index] == '>')
				{
					if (Depth == 0) return std::string_view::npos;
					--Depth;
				}
				else if (Signature[Index] == ',' && Depth == 0) return Index;
			}
			return std::string_view::npos;
		}

		auto IsSerializedTypeSignatureCompatible(
			FProperty* Property,
			std::string_view Signature) -> bool
		{
			if (!Property) return false;
			if (GetSerializedTypeSignature(Property) == Signature) return true;

			const auto Kind = Property->GetKind();
			if (IsLegacyAbiSizedLogicalSignature(Kind, Signature)) return true;
			if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
			{
				constexpr std::string_view Prefix = "Array<";
				if (!Signature.starts_with(Prefix) || !Signature.ends_with('>')) return false;
				return IsSerializedTypeSignatureCompatible(
					static_cast<FArrayProperty*>(Property)->GetInner(),
					Signature.substr(Prefix.size(), Signature.size() - Prefix.size() - 1));
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				constexpr std::string_view Prefix = "Map<";
				if (!Signature.starts_with(Prefix) || !Signature.ends_with('>')) return false;
				const std::string_view Entries =
					Signature.substr(Prefix.size(), Signature.size() - Prefix.size() - 1);
				const size_t Separator = FindMapSignatureSeparator(Entries);
				if (Separator == std::string_view::npos) return false;
				auto* Map = static_cast<FMapProperty*>(Property);
				return IsSerializedTypeSignatureCompatible(
						Map->GetKeyProp(), Entries.substr(0, Separator))
					&& IsSerializedTypeSignatureCompatible(
						Map->GetValueProp(), Entries.substr(Separator + 1));
			}
			return false;
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
			case DurinCodeGen::EPropertyGenFlags::Name:
				Writer.WriteString(static_cast<FNameProperty*>(Property)->GetNameValuePtr(Container, ArrayIndex)->ToString());
				return {};
			case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				const FGuid& Value = *static_cast<FGuidProperty*>(Property)->GetGuidValuePtr(Container, ArrayIndex);
				Writer.Write(Value.A);
				Writer.Write(Value.B);
				Writer.Write(Value.C);
				Writer.Write(Value.D);
				return {};
			}
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
				if (!Struct->HasCompleteAuthoredFields())
					return Error(
						EAssetError::UnsupportedProperty,
						std::format(
							"CustomStructCodecRequired: '{}' does not declare a complete authored "
							"field representation.",
							Struct->GetQualifiedName().ToString()));
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
					Writer.WriteString(GetSerializedTypeSignature(Field));
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

		auto DeserializeValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			FByteReader& Reader,
			const std::vector<DObject*>& Objects,
			std::vector<FAssetLegacyField>* OutLegacyFields = nullptr,
			std::string_view PackagePath = {},
			uint32 SourceVersion = AssetVersion) -> FAssetResult
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
			case DurinCodeGen::EPropertyGenFlags::Name:
			{
				std::string Value;
				if (!Reader.ReadString(Value)) return Error(EAssetError::CorruptFile, "Truncated name property.");
				*static_cast<FNameProperty*>(Property)->GetNameValuePtr(Container, ArrayIndex) = FName(Value);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				FGuid Value;
				if (!Reader.Read(Value.A) || !Reader.Read(Value.B) || !Reader.Read(Value.C) || !Reader.Read(Value.D))
					return Error(EAssetError::CorruptFile, "Truncated GUID property.");
				*static_cast<FGuidProperty*>(Property)->GetGuidValuePtr(Container, ArrayIndex) = Value;
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
				if (!Struct)
					return Error(EAssetError::UnsupportedProperty, "Struct property has no reflected type.");
				if (!Struct->HasCompleteAuthoredFields())
					return Error(
						EAssetError::UnsupportedProperty,
						std::format(
							"CustomStructCodecRequired: '{}' does not declare a complete authored "
							"field representation.",
							Struct->GetQualifiedName().ToString()));
				if (!Struct->CanDefaultConstruct() || !Struct->CanDestroy()
					|| !Struct->CanCopyAssign())
					return Error(
						EAssetError::UnsupportedProperty,
						std::format(
							"DStructOperationUnavailable: authored loading requires "
							"DefaultConstruct, Destroy, and CopyAssign for '{}'.",
							Struct->GetQualifiedName().ToString()));
				std::string StorageError;
				FReflectedValueStorage Storage;
				if (!Storage.DefaultConstruct(Property, 0, &StorageError))
					return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
				std::string StructName;
				uint64 FieldCount = 0;
				if (!Reader.ReadString(StructName) || StructName != Struct->GetQualifiedName().ToString() || !Reader.Read(FieldCount) || FieldCount > 100000)
					return Error(EAssetError::CorruptFile, "Invalid struct payload header.");
				void* StructValue = Storage.GetValue();
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
					if (!Field)
					{
						DURIN_WARN("Skipping unknown struct field {}::{}", StructName, FieldName);
						continue;
					}
					if (static_cast<uint8>(Field->GetKind()) != Kind
						|| !IsSerializedTypeSignatureCompatible(Field, Signature))
						return Error(
							EAssetError::TypeMismatch,
							std::format(
								"Serialized struct field {}::{} is incompatible with the current schema.",
								StructName,
								FieldName));
					FByteReader PayloadReader{Payload};
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim(); ++FieldIndex)
					{
						FAssetResult Result = DeserializeValue(
							Field, StructValue, FieldIndex, PayloadReader, Objects,
							OutLegacyFields, PackagePath, SourceVersion);
						if (!Result) return Result;
					}
					if (PayloadReader.Offset != Payload.size()) return Error(EAssetError::CorruptFile, "Struct field payload has trailing bytes.");
				}
				if (Struct->HasPostDeserialize())
				{
					std::string PostDeserializeError;
					FDStructPostDeserializeContext Context{
						.Source = EDStructDeserializeSource::AuthoredAsset,
						.SourceVersion = SourceVersion,
						.Error = &PostDeserializeError};
					if (!Struct->GetOps().PostDeserialize(StructValue, Context))
						return Error(
							EAssetError::CorruptFile,
							PostDeserializeError.empty()
								? std::format(
									"PostDeserializeRejected: '{}' rejected the authored value.",
									Struct->GetQualifiedName().ToString())
								: std::format(
									"PostDeserializeRejected: {}", PostDeserializeError));
				}
				if (!Property->CopyAssignValue(
					Property->GetValuePtr(Container, ArrayIndex), StructValue, &StorageError))
					return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* Array = static_cast<FArrayProperty*>(Property);
				uint64 Num = 0;
				if (!Array->HasArrayHelper() || !Reader.Read(Num) || Num > 10000000) return Error(EAssetError::CorruptFile, "Invalid array payload.");
				std::string ResizeError;
				if (!Array->Resize(Container, Num, ArrayIndex, &ResizeError))
					return Error(EAssetError::UnsupportedProperty, std::move(ResizeError));
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					FAssetResult Result = DeserializeValue(
						Array->GetInner(), Array->GetMutableElementPtr(Container, Index, ArrayIndex),
						0, Reader, Objects, OutLegacyFields, PackagePath, SourceVersion);
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Num = 0;
				if (!Map->HasMapHelper() || !Reader.Read(Num) || Num > 10000000) return Error(EAssetError::CorruptFile, "Invalid map payload.");
				FReflectedValueStorage KeyStorage;
				FReflectedValueStorage ValueStorage;
				std::string StorageError;
				if (Num > 0
					&& (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError)
						|| !ValueStorage.DefaultConstruct(Map->GetValueProp(), 0, &StorageError)))
					return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
				Map->Clear(Container, ArrayIndex);
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					if (Index > 0)
					{
						KeyStorage.Reset();
						ValueStorage.Reset();
						if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError)
							|| !ValueStorage.DefaultConstruct(Map->GetValueProp(), 0, &StorageError))
							return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
					}
					FAssetResult Result = DeserializeValue(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Reader, Objects, OutLegacyFields, PackagePath, SourceVersion);
					if (Result) Result = DeserializeValue(
						Map->GetValueProp(), ValueStorage.GetContainer(), 0, Reader, Objects, OutLegacyFields, PackagePath, SourceVersion);
					if (Result && !Map->Insert(
						Container, KeyStorage.GetValue(), ValueStorage.GetValue(), ArrayIndex, &StorageError))
						Result = Error(EAssetError::UnsupportedProperty, std::move(StorageError));
					if (!Result) return Result;
				}
				return {};
			}
			default:
				return Error(EAssetError::UnsupportedProperty, "Unsupported property kind.");
			}
		}

		auto ReadObjectReferenceValue(
			FByteReader& Reader,
			std::span<DObject* const> Objects,
			DObject*& OutObject) -> FAssetResult
		{
			OutObject = nullptr;
			uint8 ReferenceKind = 0;
			if (!Reader.Read(ReferenceKind)) return Error(EAssetError::CorruptFile, "Truncated object reference.");
			if (ReferenceKind == 0) return {};
			if (ReferenceKind == 1)
			{
				uint64 Id = 0;
				if (!Reader.Read(Id) || Id == 0 || Id > Objects.size())
					return Error(EAssetError::InvalidObjectGraph, "Invalid internal object reference.");
				OutObject = Objects[static_cast<size_t>(Id - 1)];
				return {};
			}
			if (ReferenceKind == 2)
			{
				std::string PathString;
				FAssetPath Path;
				if (!Reader.ReadString(PathString) || !FAssetPath::TryCreate(PathString, Path))
					return Error(EAssetError::InvalidPath, "Invalid external object reference.");
				return FAssetManager::Get().LoadAsset(Path, OutObject);
			}
			return Error(EAssetError::CorruptFile, "Unknown object reference kind.");
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

		template<typename ReaderType>
		auto ReadPackageHeader(ReaderType& Reader, FPackageFile& OutFile, uint64& OutObjectCount) -> FAssetResult
		{
			uint32 Magic = 0, Version = 0;
			if (!Reader.Read(Magic) || !Reader.Read(Version)) return Error(EAssetError::CorruptFile, "Truncated asset header.");
			if (Magic != AssetMagic) return Error(EAssetError::CorruptFile, "Invalid asset magic.");
			if (Version != AssetVersion)
				return Error(EAssetError::UnsupportedVersion, std::format("Unsupported asset version {}.", Version));
			OutFile.FormatVersion = Version;
			if (!Reader.ReadString(OutFile.AssetClassName, MaximumPackageStringBytes)) return Error(EAssetError::CorruptFile, "Invalid asset header strings.");
			uint64 DependencyCount = 0;
			if (!Reader.Read(DependencyCount) || DependencyCount > 100000) return Error(EAssetError::CorruptFile, "Invalid dependency count.");
			for (uint64 Index = 0; Index < DependencyCount; ++Index)
			{
				std::string DependencyString;
				FAssetPath Dependency;
				if (!Reader.ReadString(DependencyString, MaximumPackageStringBytes) || !FAssetPath::TryCreate(DependencyString, Dependency)) return Error(EAssetError::CorruptFile, "Invalid dependency path.");
				OutFile.Dependencies.push_back(std::move(Dependency));
			}
			if (!Reader.Read(OutObjectCount) || OutObjectCount == 0 || OutObjectCount > 1000000) return Error(EAssetError::CorruptFile, "Invalid object count.");
			return {};
		}

		auto ReadPackageFile(std::span<const uint8> Bytes, FPackageFile& OutFile, bool bHeaderOnly) -> FAssetResult
		{
			FByteReader Reader{Bytes};
			uint64 ObjectCount = 0;
			FAssetResult HeaderResult = ReadPackageHeader(Reader, OutFile, ObjectCount);
			if (!HeaderResult) return HeaderResult;
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

		constexpr uint64 MaximumRegistryEntries = 1000000;
		constexpr uint32 MaximumRegistryDependencies = 100000;

		struct FRegistryCacheEntry
		{
			std::string MountRoot;
			std::string RelativePath;
			std::string AssetClassName;
			uint32 FormatVersion = 0;
			std::vector<FAssetPath> Dependencies;
			uint64 FileSize = 0;
			int64 LastWriteTimeTicks = 0;
		};

		auto RegistryCachePath() -> std::filesystem::path
		{
			return std::filesystem::path(FPaths::DerivedDataCacheDir()) / "AssetRegistry" / "Registry.bin";
		}

			auto GetMountManifest() -> std::vector<std::string>
			{
				std::vector<std::string> Roots;
				for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
					if (Mount.bAutoScan) Roots.push_back(Mount.VirtualRoot);
			std::ranges::sort(Roots);
			Roots.erase(std::unique(Roots.begin(), Roots.end()), Roots.end());
			return Roots;
		}

		auto MakeRegistryIdentity(std::string_view MountRoot, std::string_view RelativePath) -> std::string
		{
			return std::format("{}\n{}", MountRoot, RelativePath);
		}

		auto LoadRegistryCache(const std::vector<std::string>& ExpectedMounts,
			std::unordered_map<std::string, FRegistryCacheEntry>& OutEntries, std::string& OutWarning) -> bool
		{
			OutEntries.clear();
			const std::filesystem::path Path = RegistryCachePath();
			std::error_code Ec;
			if (!std::filesystem::exists(Path, Ec)) return false;
			const uintmax_t Size = std::filesystem::file_size(Path, Ec);
			if (Ec || Size > 256ull * 1024ull * 1024ull)
			{
				OutWarning = std::format("Ignoring invalid asset registry cache {}.", Path.generic_string());
				return false;
			}
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutWarning = std::format("Failed to read asset registry cache {}.", Path.generic_string());
				return false;
			}
			DerivedDataCache::FReader Reader(Bytes);
			uint32 MountCount = 0;
			if (!Reader.ReadAndValidateHeader(DerivedDataCache::AssetRegistryMagic, DerivedDataCache::AssetRegistrySchemaVersion, AssetVersion)
				|| !Reader.ReadU32(MountCount) || MountCount > MaximumRegistryEntries)
			{
				OutWarning = "Ignoring incompatible or corrupt asset registry cache header.";
				return false;
			}
			std::vector<std::string> Mounts;
			Mounts.reserve(MountCount);
			for (uint32 Index = 0; Index < MountCount; ++Index)
			{
				std::string Root;
				if (!Reader.ReadString(Root)) { OutWarning = "Ignoring truncated asset registry mount manifest."; return false; }
				Mounts.push_back(std::move(Root));
			}
			if (Mounts != ExpectedMounts)
			{
				OutWarning = "Ignoring asset registry cache because the mount manifest changed.";
				return false;
			}
			uint64 EntryCount = 0;
			if (!Reader.ReadU64(EntryCount) || EntryCount > MaximumRegistryEntries)
			{
				OutWarning = "Ignoring invalid asset registry cache entry count.";
				return false;
			}
			for (uint64 Index = 0; Index < EntryCount; ++Index)
			{
				FRegistryCacheEntry Entry;
				uint32 DependencyCount = 0;
				if (!Reader.ReadString(Entry.MountRoot) || !Reader.ReadString(Entry.RelativePath)
					|| !Reader.ReadString(Entry.AssetClassName) || !Reader.ReadU32(Entry.FormatVersion)
					|| !Reader.ReadU32(DependencyCount) || DependencyCount > MaximumRegistryDependencies)
				{
					OutWarning = "Ignoring corrupt asset registry cache entry.";
					OutEntries.clear();
					return false;
				}
				Entry.Dependencies.reserve(DependencyCount);
				for (uint32 DependencyIndex = 0; DependencyIndex < DependencyCount; ++DependencyIndex)
				{
					std::string DependencyString;
					FAssetPath Dependency;
					if (!Reader.ReadString(DependencyString) || !FAssetPath::TryCreate(DependencyString, Dependency))
					{
						OutWarning = "Ignoring invalid dependency in asset registry cache.";
						OutEntries.clear();
						return false;
					}
					Entry.Dependencies.push_back(std::move(Dependency));
				}
				if (!Reader.ReadU64(Entry.FileSize) || !Reader.ReadI64(Entry.LastWriteTimeTicks)
					|| Entry.FormatVersion != AssetVersion
					|| !std::ranges::binary_search(ExpectedMounts, Entry.MountRoot)
					|| std::filesystem::path(Entry.RelativePath).is_absolute()
					|| std::filesystem::path(Entry.RelativePath).extension() != ".dasset"
					|| Entry.RelativePath.starts_with("../") || Entry.RelativePath.find("/../") != std::string::npos)
				{
					OutWarning = "Ignoring invalid asset registry cache identity.";
					OutEntries.clear();
					return false;
				}
				const std::string Identity = MakeRegistryIdentity(Entry.MountRoot, Entry.RelativePath);
				if (!OutEntries.emplace(Identity, std::move(Entry)).second)
				{
					OutWarning = "Ignoring duplicate asset registry cache identity.";
					OutEntries.clear();
					return false;
				}
			}
			if (!Reader.IsAtEnd())
			{
				OutWarning = "Ignoring asset registry cache with trailing data.";
				OutEntries.clear();
				return false;
			}
			return true;
		}

		auto WriteRegistryCache(const std::vector<std::string>& Mounts, std::vector<FRegistryCacheEntry> Entries,
			std::string& OutWarning) -> bool
		{
			std::ranges::sort(Entries, [](const FRegistryCacheEntry& A, const FRegistryCacheEntry& B) {
				return std::tie(A.MountRoot, A.RelativePath) < std::tie(B.MountRoot, B.RelativePath);
			});
			DerivedDataCache::FWriter Writer;
			Writer.WriteHeader({DerivedDataCache::AssetRegistryMagic, DerivedDataCache::AssetRegistrySchemaVersion, AssetVersion});
			Writer.WriteU32(static_cast<uint32>(Mounts.size()));
			for (const std::string& Mount : Mounts) Writer.WriteString(Mount);
			Writer.WriteU64(Entries.size());
			for (const FRegistryCacheEntry& Entry : Entries)
			{
				Writer.WriteString(Entry.MountRoot);
				Writer.WriteString(Entry.RelativePath);
				Writer.WriteString(Entry.AssetClassName);
				Writer.WriteU32(Entry.FormatVersion);
				Writer.WriteU32(static_cast<uint32>(Entry.Dependencies.size()));
				for (const FAssetPath& Dependency : Entry.Dependencies) Writer.WriteString(Dependency.GetView());
				Writer.WriteU64(Entry.FileSize);
				Writer.WriteI64(Entry.LastWriteTimeTicks);
			}
			std::string ErrorMessage;
			if (!DerivedDataCache::WriteFileAtomically(RegistryCachePath(), Writer.GetBytes(), &ErrorMessage))
			{
				OutWarning = std::move(ErrorMessage);
				return false;
			}
			return true;
		}

		auto BuildRegistryCacheEntries(const std::unordered_map<FAssetPath, FAssetData>& Assets,
			std::vector<FRegistryCacheEntry>& OutEntries, std::string& OutWarning) -> bool
		{
			OutEntries.clear();
			OutEntries.reserve(Assets.size());
			for (const auto& [Path, Data] : Assets)
			{
				const PathUtilities::FMountLookupResult Lookup =
					PathUtilities::FindMountForVirtualPath(Path.GetView());
				if (!Lookup || !Lookup.Mount->bAutoScan)
				{
					OutWarning = std::format("Could not persist asset registry entry {} because its mount is unavailable.", Path.ToString());
					OutEntries.clear();
					return false;
				}
				const std::string RelativeAssetPath = Lookup.RelativePath.generic_string();
				const std::string RelativeString = std::format("{}.dasset", RelativeAssetPath);
				if (RelativeAssetPath.empty() || std::filesystem::path(RelativeString).is_absolute()
					|| RelativeString.starts_with("../") || RelativeString.find("/../") != std::string::npos)
				{
					OutWarning = std::format("Could not persist invalid asset registry path {}.", Path.ToString());
					OutEntries.clear();
					return false;
				}
				OutEntries.push_back(FRegistryCacheEntry{
					.MountRoot = Lookup.Mount->VirtualRoot,
					.RelativePath = RelativeString,
					.AssetClassName = Data.AssetClassName,
					.FormatVersion = Data.FormatVersion,
					.Dependencies = Data.Dependencies,
					.FileSize = Data.FileSize,
					.LastWriteTimeTicks = Data.LastWriteTimeTicks});
			}
			return true;
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

		auto BuildPackageBytes(
			DPackage* Package,
			std::vector<uint8>& OutBytes,
			FPackageFile* OutFile = nullptr,
			const FAssetPackageSerializationOptions& Options = {}) -> FAssetResult
		{
			OutBytes.clear();
			if (Package && !Package->IsAssetPackage())
				return Error(EAssetError::InvalidPackageType, "Only asset packages can be serialized.");
			if (!Package || !Package->GetAsset())
				return Error(EAssetError::InvalidObjectGraph, "Package has no main asset.");

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
					if (OuterIt == ObjectIds.end())
						return Error(EAssetError::InvalidObjectGraph, "Package inner object has an outer outside the package graph.");
					Record.OuterId = OuterIt->second;
				}
				Record.ClassName = Object->GetClass()->GetQualifiedName().ToString();
				Record.ObjectName = Object->GetName();
				FAssetResult SerializationResult;
				Object->GetClass()->ForEachProperty([&](FProperty* Property) {
					if (!SerializationResult || !Property || Property->HasAnyPropertyFlags(EPropertyFlags::Transient)) return;
					if (Options.PropertyFilter && !Options.PropertyFilter(Object, Property)) return;
					FFieldRecord Field;
					DClass* DeclaringClass = Cast<DClass>(Property->Owner.ToDObject());
					Field.DeclaringClass = DeclaringClass
						? DeclaringClass->GetQualifiedName().ToString()
						: Object->GetClass()->GetQualifiedName().ToString();
					Field.Name = Property->NamePrivate.ToString();
					Field.Kind = Property->GetKind();
					Field.TypeSignature = GetSerializedTypeSignature(Property);
					FByteWriter PayloadWriter;
					for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
					{
						FAssetResult Result = SerializeValue(
							Property, Object, ArrayIndex, PayloadWriter, ObjectIds, Dependencies);
						if (!Result) { SerializationResult = std::move(Result); return; }
					}
					Field.Payload = std::move(PayloadWriter.Bytes);
					Record.Fields.push_back(std::move(Field));
				}, true);
				if (!SerializationResult) return SerializationResult;
				File.Objects.push_back(std::move(Record));
			}
			File.Dependencies.assign(Dependencies.begin(), Dependencies.end());
			std::ranges::sort(File.Dependencies, [](const FAssetPath& A, const FAssetPath& B) {
				return A.GetView() < B.GetView();
			});

			FByteWriter Writer;
			WritePackageFile(File, Writer);
			OutBytes = std::move(Writer.Bytes);
			if (OutFile) *OutFile = std::move(File);
			return {};
		}
	}

	auto ReadAssetPackageHeader(std::string_view PhysicalPath, FAssetPackageHeader& OutHeader) -> FAssetResult
	{
		OutHeader = {};
		FFileByteReader Reader(PhysicalPath);
		if (!Reader.IsOpen()) return Error(EAssetError::IoError, std::format("Failed to open asset package {}.", PhysicalPath));
		FPackageFile File;
		FAssetResult Result = ReadPackageHeader(Reader, File, OutHeader.ObjectCount);
		OutHeader.BytesRead = Reader.Offset;
		if (!Result) return Result;
		OutHeader.AssetClassName = std::move(File.AssetClassName);
		OutHeader.FormatVersion = File.FormatVersion;
		OutHeader.Dependencies = std::move(File.Dependencies);
		return {};
	}

	auto ValidateAssetPackageBytes(std::span<const uint8> Bytes) -> FAssetResult
	{
		FPackageFile File;
		return ReadPackageFile(Bytes, File, false);
	}

	auto SerializeAssetPackageBytes(
		DPackage* Package,
		std::vector<uint8>& OutBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetResult
	{
		return BuildPackageBytes(Package, OutBytes, nullptr, Options);
	}

	auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options) -> FAssetResult
	{
		struct FStagedPackage
		{
			DPackage* Package = nullptr;
			FAssetPath Path;
			FPackageFile File;
			std::vector<uint8> Bytes;
			std::filesystem::path Destination;
			std::filesystem::path Staged;
			std::filesystem::path Backup;
			uintmax_t PublishedFileSize = 0;
			std::filesystem::file_time_type PublishedLastWriteTime{};
			bool bHadDestination = false;
			bool bPublished = false;
		};

		if (Packages.empty())
			return Error(EAssetError::InvalidPackageType, "An asset bundle must contain at least one package.");
		FAssetManager& Manager = FAssetManager::Get();
		if (Manager.PackageLoadContext.Mode == EPackageLoadMode::CookedRuntime)
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit bundle saves.");
		if (Options.RootPackage
			&& std::ranges::find(Packages, Options.RootPackage) == Packages.end())
			return Error(EAssetError::InvalidPackageType, "The root package is not part of the asset bundle.");

		std::vector<FStagedPackage> StagedPackages;
		StagedPackages.reserve(Packages.size());
		std::unordered_set<FAssetPath> Paths;
		for (DPackage* Package : Packages)
		{
			if (Manager.CompatibilityRiskPackages.contains(Package)
				&& !Options.bAllowCompatibilityDataLoss)
				return Error(
					EAssetError::UnsupportedProperty,
					"The asset bundle contains compatibility-risk data and cannot be saved.");
			FAssetPath Path;
			if (!Package || !Package->IsAssetPackage()
				|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
				return Error(EAssetError::InvalidPackageType, "The asset bundle contains an invalid package.");
			if (!Paths.insert(Path).second)
				return Error(EAssetError::AlreadyExists, std::format(
					"The asset bundle contains duplicate package {}.", Path.ToString()));
			FStagedPackage& Staged = StagedPackages.emplace_back();
			Staged.Package = Package;
			Staged.Path = Path;
			FAssetResult Result = BuildPackageBytes(Package, Staged.Bytes, &Staged.File);
			if (!Result) return Result;
			Result = ValidateAssetPackageBytes(Staged.Bytes);
			if (!Result) return Result;
			Staged.Destination = GetPhysicalPath(Path);
			if (Staged.Destination.empty())
				return Error(EAssetError::InvalidPath, std::format(
					"Failed to resolve package {}.", Path.ToString()));
			Staged.Staged = Staged.Destination;
			Staged.Staged += ".bundle-stage";
			Staged.Backup = Staged.Destination;
			Staged.Backup += ".bundle-backup";
			std::error_code Ec;
			const bool bDestinationExists = std::filesystem::exists(Staged.Destination, Ec);
			if (Ec && !IsMissingPathError(Ec))
				return Error(EAssetError::IoError, std::format(
					"Failed to inspect package destination {}: {}",
					Staged.Destination.generic_string(), Ec.message()));
			Ec.clear();
			Staged.bHadDestination =
				bDestinationExists && std::filesystem::is_regular_file(Staged.Destination, Ec);
			if (Ec && !IsMissingPathError(Ec))
				return Error(EAssetError::IoError, std::format(
					"Failed to inspect package destination {}: {}",
					Staged.Destination.generic_string(), Ec.message()));
			Ec.clear();
			if (bDestinationExists && !Staged.bHadDestination)
				return Error(EAssetError::AlreadyExists, std::format(
					"Package destination {} is occupied.", Staged.Destination.generic_string()));
			if (std::filesystem::exists(Staged.Staged, Ec)
				|| std::filesystem::exists(Staged.Backup, Ec))
				return Error(EAssetError::AlreadyExists, std::format(
					"Package transaction staging path for {} is occupied.", Path.ToString()));
		}

		auto CleanupStaging = [&] {
			for (FStagedPackage& Staged : StagedPackages)
			{
				std::error_code Ec;
				std::filesystem::remove(Staged.Staged, Ec);
			}
		};
		auto RollbackPublication = [&] {
			for (auto It = StagedPackages.rbegin(); It != StagedPackages.rend(); ++It)
			{
				std::error_code Ec;
				if (It->bPublished) std::filesystem::remove(It->Destination, Ec);
				if (It->bHadDestination && std::filesystem::exists(It->Backup, Ec))
				{
					Ec.clear();
					std::filesystem::rename(It->Backup, It->Destination, Ec);
				}
				std::filesystem::remove(It->Staged, Ec);
			}
		};

		for (size_t Index = 0; Index < StagedPackages.size(); ++Index)
		{
			FStagedPackage& Staged = StagedPackages[Index];
			if (Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::CreateDirectories, Index))
			{
				CleanupStaging();
				return Error(EAssetError::IoError, "Injected asset-bundle directory creation failure.");
			}
			std::error_code Ec;
			std::filesystem::create_directories(Staged.Destination.parent_path(), Ec);
			if (Ec)
			{
				CleanupStaging();
				return Error(EAssetError::IoError, std::format(
					"Failed to create package directory {}: {}",
					Staged.Destination.parent_path().generic_string(), Ec.message()));
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::StagePackage, Index))
			{
				CleanupStaging();
				return Error(EAssetError::IoError, "Injected asset-bundle package staging failure.");
			}
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Staged.Bytes.data()), Staged.Bytes.size()},
				Staged.Staged,
				&PublicationError))
			{
				CleanupStaging();
				return Error(EAssetError::IoError, PublicationError.ToString());
			}
		}

		std::stable_sort(StagedPackages.begin(), StagedPackages.end(), [&](const FStagedPackage& A, const FStagedPackage& B) {
			return A.Package != Options.RootPackage && B.Package == Options.RootPackage;
		});
		for (size_t Index = 0; Index < StagedPackages.size(); ++Index)
		{
			FStagedPackage& Staged = StagedPackages[Index];
			const EAssetBundleSavePhase Phase = Staged.Package == Options.RootPackage
				? EAssetBundleSavePhase::PublishRootPackage
				: EAssetBundleSavePhase::PublishPackage;
			if (Options.ShouldFail && Options.ShouldFail(Phase, Index))
			{
				RollbackPublication();
				return Error(EAssetError::IoError, "Injected asset-bundle package publication failure.");
			}
			std::error_code Ec;
			if (Staged.bHadDestination)
			{
				std::filesystem::rename(Staged.Destination, Staged.Backup, Ec);
				if (Ec)
				{
					RollbackPublication();
					return Error(EAssetError::IoError, std::format(
						"Failed to back up package {}: {}", Staged.Path.ToString(), Ec.message()));
				}
			}
			Ec.clear();
			std::filesystem::rename(Staged.Staged, Staged.Destination, Ec);
			if (Ec)
			{
				if (Staged.bHadDestination)
				{
					std::error_code RestoreError;
					std::filesystem::rename(Staged.Backup, Staged.Destination, RestoreError);
				}
				RollbackPublication();
				return Error(EAssetError::IoError, std::format(
					"Failed to publish package {}: {}", Staged.Path.ToString(), Ec.message()));
			}
			Staged.bPublished = true;
		}
		for (FStagedPackage& Staged : StagedPackages)
		{
			std::error_code Ec;
			Staged.PublishedLastWriteTime =
				std::filesystem::last_write_time(Staged.Destination, Ec);
			if (!Ec) Staged.PublishedFileSize =
				std::filesystem::file_size(Staged.Destination, Ec);
			if (Ec)
			{
				RollbackPublication();
				return Error(EAssetError::IoError, std::format(
					"Failed to inspect published package {}: {}",
					Staged.Path.ToString(), Ec.message()));
			}
		}
		if (Options.ShouldFail
			&& Options.ShouldFail(EAssetBundleSavePhase::PublishRegistry, StagedPackages.size()))
		{
			RollbackPublication();
			return Error(EAssetError::IoError, "Injected asset-bundle registry publication failure.");
		}

		for (FStagedPackage& Staged : StagedPackages)
		{
			FAssetManager::Get().GetRegistry().AddOrUpdate(FAssetData{
				.PackagePath = Staged.Path,
				.PhysicalPath = Staged.Destination.generic_string(),
				.AssetClassName = Staged.File.AssetClassName,
				.FormatVersion = AssetVersion,
				.Dependencies = Staged.File.Dependencies,
				.FileSize = Staged.PublishedFileSize,
				.LastWriteTime = Staged.PublishedLastWriteTime,
				.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(
					Staged.PublishedLastWriteTime)});
			Staged.Package->ClearDirty();
			std::error_code Ec;
			std::filesystem::remove(Staged.Backup, Ec);
		}
		return {};
	}

	auto DiscardUnpublishedPackage(DPackage* Package) -> FAssetResult
	{
		FAssetPath Path;
		if (!Package || !Package->IsAssetPackage()
			|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
			return Error(EAssetError::InvalidPackageType, "The unpublished package is invalid.");
		FAssetManager& Manager = FAssetManager::Get();
		if (Manager.GetRegistry().FindAsset(Path))
			return Error(EAssetError::InUse, std::format(
				"Package {} is registry-visible and cannot be discarded.", Path.ToString()));
		if (Manager.FindLoadedPackage(Path) != Package)
			return Error(EAssetError::NotFound, "The unpublished package is not loaded.");
		return Manager.UnloadPackage(Path);
	}

	auto FAssetPackageField::TryReadString(std::string& OutValue) const -> bool
	{
		FByteReader Reader{Payload};
		return Reader.ReadString(OutValue, MaximumPackageStringBytes) && Reader.Offset == Payload.size();
	}

	namespace
	{
		auto ReadInspectedObjectReference(
			FByteReader& Reader,
			FAssetPackageObjectReference& OutValue) -> bool
		{
			OutValue = {};
			uint8 Kind = 0;
			if (!Reader.Read(Kind) || Kind > 2) return false;
			OutValue.Kind = static_cast<EAssetPackageObjectReferenceKind>(Kind);
			if (OutValue.Kind == EAssetPackageObjectReferenceKind::Null) return true;
			if (OutValue.Kind == EAssetPackageObjectReferenceKind::Internal)
				return Reader.Read(OutValue.ObjectId) && OutValue.ObjectId != 0;
			std::string PathString;
			return Reader.ReadString(PathString, MaximumPackageStringBytes)
				&& FAssetPath::TryCreate(PathString, OutValue.ExternalPath);
		}
	}

	auto FAssetPackageField::TryReadObjectReference(
		FAssetPackageObjectReference& OutValue) const -> bool
	{
		FByteReader Reader{Payload};
		return ReadInspectedObjectReference(Reader, OutValue)
			&& Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryReadObjectReferenceArray(
		std::vector<FAssetPackageObjectReference>& OutValues) const -> bool
	{
		OutValues.clear();
		FByteReader Reader{Payload};
		uint64 Count = 0;
		if (!Reader.Read(Count) || Count > 10000000) return false;
		OutValues.reserve(static_cast<size_t>(Count));
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			FAssetPackageObjectReference Value;
			if (!ReadInspectedObjectReference(Reader, Value)) return false;
			OutValues.push_back(std::move(Value));
		}
		return Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryReadStruct(DStruct* Struct, void* OutValue) const -> bool
	{
		if (!Struct || !OutValue
			|| Struct->PropertiesSize == 0
			|| Struct->PropertiesSize > std::numeric_limits<uint16>::max()
			|| Struct->MinAlignment == 0
			|| (Struct->MinAlignment & (Struct->MinAlignment - 1)) != 0
			|| TypeSignature != std::format("Struct<{}>", Struct->GetQualifiedName().ToString()))
			return false;

		FStructProperty RootProperty(
			FFieldVariant(), FName("InspectedStructValue"), EObjectFlags::NoFlags,
			EPropertyFlags::None, 1, 0, Struct);
		FByteReader Reader{Payload};
		return DeserializeValue(
			&RootProperty, OutValue, 0, Reader, {}, nullptr, {}, AssetVersion)
			&& Reader.Offset == Payload.size();
	}

	auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetResult
	{
		OutInspection = {};
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError, std::format("Failed to open asset package {}.", PhysicalPath));
		FAssetResult Result = MakePackageFingerprint(PhysicalPath, Bytes, OutInspection.Fingerprint);
		if (!Result) return Result;
		FPackageFile File;
		Result = ReadPackageFile(Bytes, File, false);
		if (!Result) return Result;
		if (File.Objects.empty()) return Error(EAssetError::InvalidObjectGraph, "Asset package has no main object.");
		FByteReader HeaderReader{Bytes};
		FPackageFile HeaderFile;
		uint64 HeaderObjectCount = 0;
		Result = ReadPackageHeader(HeaderReader, HeaderFile, HeaderObjectCount);
		if (!Result) return Result;
		OutInspection.Header.AssetClassName = std::move(HeaderFile.AssetClassName);
		OutInspection.Header.FormatVersion = HeaderFile.FormatVersion;
		OutInspection.Header.Dependencies = std::move(HeaderFile.Dependencies);
		OutInspection.Header.ObjectCount = HeaderObjectCount;
		OutInspection.Header.BytesRead = HeaderReader.Offset;
		OutInspection.Objects.reserve(File.Objects.size());
		for (FObjectRecord& Record : File.Objects)
		{
			FAssetPackageObjectInspection Object{
				.Id = Record.Id,
				.OuterId = Record.OuterId,
				.ClassName = std::move(Record.ClassName),
				.ObjectName = std::move(Record.ObjectName)};
			Object.Fields.reserve(Record.Fields.size());
			for (FFieldRecord& Field : Record.Fields)
			{
				Object.Fields.push_back({
					.DeclaringClass = std::move(Field.DeclaringClass),
					.Name = std::move(Field.Name),
					.Kind = Field.Kind,
					.TypeSignature = std::move(Field.TypeSignature),
					.Payload = std::move(Field.Payload)});
			}
			OutInspection.Objects.push_back(std::move(Object));
		}
		return {};
	}

	auto FAssetLoadReport::HasNonUpgradeMutations() const -> bool
	{
		return std::ranges::any_of(Mutations, [](const FAssetLoadMutation& Mutation) {
			return Mutation.Kind == EAssetLoadMutationKind::NonUpgrade;
		});
	}

	auto FAssetLoadReport::HasRiskItems() const -> bool
	{
		return std::ranges::any_of(
			CompatibilityIssues,
			[](const FAssetCompatibilityIssue& Issue) {
				return Issue.Risk != EAssetCompatibilityRisk::None;
			});
	}

	auto ReportAssetLoadMutation(
		DObject* Object,
		std::string HandlerId,
		std::string Summary,
		EAssetLoadMutationKind Kind) -> void
	{
		if (!GActiveAssetLoadReport || !Object) return;
		DPackage* Package = Object->GetPackage();
		FAssetPath PackagePath;
		if (Package) FAssetPath::TryCreate(Package->GetPackagePath(), PackagePath);
		GActiveAssetLoadReport->Mutations.push_back({
			.PackagePath = std::move(PackagePath),
			.ObjectPath = Object->GetObjectPath(),
			.HandlerId = std::move(HandlerId),
			.Summary = std::move(Summary),
			.Kind = Kind});
	}

	auto FAssetLoadReport::GetAffectedObjectCount() const -> uint64
	{
		std::unordered_set<std::string> ObjectPaths;
		for (const FAssetCompatibilityIssue& Issue : CompatibilityIssues) ObjectPaths.insert(Issue.ObjectPath);
		return static_cast<uint64>(ObjectPaths.size());
	}

	auto FAssetLoadReport::GetLegacyFieldCount() const -> uint64
	{
		uint64 Count = 0;
		for (const FAssetCompatibilityIssue& Issue : CompatibilityIssues)
			Count += static_cast<uint64>(Issue.LegacyFields.size());
		return Count;
	}

	auto FAssetLoadReport::GetMigratedDataCount() const -> uint64
	{
		uint64 Count = 0;
		for (const FAssetCompatibilityIssue& Issue : CompatibilityIssues) Count += Issue.MigratedDataCount;
		return Count;
	}

	auto FAssetLoadReport::GetRiskItemCount() const -> uint64
	{
		return static_cast<uint64>(std::ranges::count_if(
			CompatibilityIssues,
			[](const FAssetCompatibilityIssue& Issue) {
				return Issue.Risk != EAssetCompatibilityRisk::None;
			}));
	}

	auto FAssetMigrationContext::ReadObjectReference(
		const FAssetLegacyField& Field,
		DObject*& OutObject) const -> FAssetResult
	{
		FByteReader Reader{Field.Payload};
		FAssetResult Result = ReadObjectReferenceValue(Reader, Objects, OutObject);
		if (Result && Reader.Offset != Field.Payload.size())
			return Error(EAssetError::CorruptFile, "Object-reference payload has trailing bytes.");
		return Result;
	}

	auto FAssetMigrationContext::ReadObjectReferenceArray(
		const FAssetLegacyField& Field,
		std::vector<DObject*>& OutObjects) const -> FAssetResult
	{
		OutObjects.clear();
		FByteReader Reader{Field.Payload};
		uint64 Count = 0;
		if (!Reader.Read(Count) || Count > 10000000)
			return Error(EAssetError::CorruptFile, "Invalid object-reference array payload.");
		OutObjects.reserve(static_cast<size_t>(Count));
		for (uint64 Index = 0; Index < Count; ++Index)
		{
			DObject* Object = nullptr;
			FAssetResult Result = ReadObjectReferenceValue(Reader, Objects, Object);
			if (!Result) return Result;
			OutObjects.push_back(Object);
		}
		if (Reader.Offset != Field.Payload.size())
			return Error(EAssetError::CorruptFile, "Object-reference array payload has trailing bytes.");
		return {};
	}

	auto RegisterAssetStructureUpgrader(
		DClass* Class,
		std::string HandlerId,
		FAssetStructureUpgrader Upgrader) -> void
	{
		if (!Class || HandlerId.empty() || !Upgrader) return;
		GetStructureUpgraders().insert_or_assign(
			Class,
			FRegisteredStructureUpgrader{
				.HandlerId = std::move(HandlerId),
				.Upgrader = std::move(Upgrader)});
	}

	auto RegisterAssetMoveContributor(DClass* Class, FAssetMoveContributor Contributor) -> void
	{
		if (Class && Contributor) GetMoveContributors().insert_or_assign(Class, std::move(Contributor));
	}

	auto RegisterAssetDeleteContributor(DClass* Class, FAssetDeleteContributor Contributor) -> void
	{
		if (Class && Contributor) GetDeleteContributors().insert_or_assign(Class, std::move(Contributor));
	}

	auto FAssetRegistry::ScanMountedContent(EAssetRegistryScanMode Mode) -> FAssetResult
	{
		const auto ScanStartTime = std::chrono::steady_clock::now();
		std::unordered_map<FAssetPath, FAssetData> NewAssets;
		std::vector<FRegistryCacheEntry> NewCacheEntries;
		std::unordered_map<std::string, FRegistryCacheEntry> CachedEntries;
		std::unordered_set<std::string> SeenCachedIdentities;
		ScanErrors.clear();
		LastScanStats = {};
		CacheWarning.clear();
		const std::vector<std::string> MountManifest = GetMountManifest();
		const bool bCacheLoaded = LoadRegistryCache(MountManifest, CachedEntries, CacheWarning);
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			if (!Mount.bAutoScan) continue;
			const std::filesystem::path AssetRoot = Mount.GetContentDir();
			std::error_code Ec;
			if (!std::filesystem::exists(AssetRoot, Ec)) continue;
			for (std::filesystem::recursive_directory_iterator It(AssetRoot, Ec), End; !Ec && It != End; It.increment(Ec))
			{
				std::error_code FileEc;
				if (!It->is_regular_file(FileEc) || It->path().extension() != ".dasset") continue;
				++LastScanStats.Enumerated;
				FAssetPackageHeader PackageHeader;
				FAssetPath DiskPath;
				std::filesystem::path Relative = std::filesystem::relative(It->path(), AssetRoot, FileEc).lexically_normal();
				const std::string RelativeString = Relative.generic_string();
				std::filesystem::path PackageRelative = Relative;
				PackageRelative.replace_extension();
				if (FileEc || Relative.is_absolute() || RelativeString.starts_with("../")
					|| !FAssetPath::TryCreate(Mount.VirtualRoot + PackageRelative.generic_string(), DiskPath))
				{
					ScanErrors.push_back(Error(EAssetError::InvalidPath, std::format("Failed to map asset path {}.", It->path().generic_string())));
					++LastScanStats.Failed;
					continue;
				}
				const std::string Identity = MakeRegistryIdentity(Mount.VirtualRoot, RelativeString);
				if (CachedEntries.contains(Identity)) SeenCachedIdentities.insert(Identity);
				const auto LastWriteTime = It->last_write_time(FileEc);
				const auto FileSize = It->file_size(FileEc);
				if (FileEc)
				{
					ScanErrors.push_back(Error(EAssetError::IoError, std::format("Failed to fingerprint asset {}.", It->path().generic_string())));
					++LastScanStats.Failed;
					continue;
				}
				const int64 LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime);
				std::string AssetClassName;
				uint32 FormatVersion = 0;
				std::vector<FAssetPath> Dependencies;
				const auto CachedIt = CachedEntries.find(Identity);
				if (Mode == EAssetRegistryScanMode::Incremental && CachedIt != CachedEntries.end()
					&& CachedIt->second.FileSize == FileSize && CachedIt->second.LastWriteTimeTicks == LastWriteTimeTicks)
				{
					AssetClassName = CachedIt->second.AssetClassName;
					FormatVersion = CachedIt->second.FormatVersion;
					Dependencies = CachedIt->second.Dependencies;
					++LastScanStats.Reused;
				}
				else
				{
					++LastScanStats.HeaderReadAttempts;
					FAssetResult Result = ReadAssetPackageHeader(It->path().generic_string(), PackageHeader);
					LastScanStats.HeaderBytesRead += PackageHeader.BytesRead;
					if (!Result)
					{
						Result.Message = std::format("{} ({})", Result.Message, It->path().generic_string());
						ScanErrors.push_back(std::move(Result));
						++LastScanStats.Failed;
						continue;
					}
					AssetClassName = std::move(PackageHeader.AssetClassName);
					FormatVersion = PackageHeader.FormatVersion;
					Dependencies = std::move(PackageHeader.Dependencies);
					++LastScanStats.Reparsed;
				}
				if (NewAssets.contains(DiskPath))
				{
					ScanErrors.push_back(Error(EAssetError::AlreadyExists, std::format("Duplicate asset path {}.", DiskPath.ToString())));
					++LastScanStats.Failed;
					continue;
				}
				NewCacheEntries.push_back(FRegistryCacheEntry{
					.MountRoot = Mount.VirtualRoot,
					.RelativePath = RelativeString,
					.AssetClassName = AssetClassName,
					.FormatVersion = FormatVersion,
					.Dependencies = Dependencies,
					.FileSize = FileSize,
					.LastWriteTimeTicks = LastWriteTimeTicks});
				NewAssets.emplace(DiskPath, FAssetData{
					.PackagePath = DiskPath,
					.PhysicalPath = It->path().generic_string(),
					.AssetClassName = std::move(AssetClassName),
					.FormatVersion = FormatVersion,
					.Dependencies = std::move(Dependencies),
					.FileSize = FileSize,
					.LastWriteTime = LastWriteTime,
					.LastWriteTimeTicks = LastWriteTimeTicks});
			}
			if (Ec)
			{
				ScanErrors.push_back(Error(EAssetError::IoError, std::format("Failed to enumerate mount {}.", Mount.VirtualRoot)));
				++LastScanStats.Failed;
			}
		}
		if (bCacheLoaded) LastScanStats.Removed = CachedEntries.size() - SeenCachedIdentities.size();
		if (Assets != NewAssets)
		{
			Assets = std::move(NewAssets);
			++Revision;
		}
		bPersistentSnapshotDirty = !WriteRegistryCache(MountManifest, std::move(NewCacheEntries), CacheWarning);
		LastScanStats.DurationMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - ScanStartTime).count();
		DURIN_INFO_CATEGORY("AssetRegistry",
			"Scanned {} asset package(s) in {:.3f} ms: {} reused, {} reparsed, {} header read(s), {} header byte(s), {} removed, {} failed.",
			LastScanStats.Enumerated, LastScanStats.DurationMilliseconds, LastScanStats.Reused, LastScanStats.Reparsed,
			LastScanStats.HeaderReadAttempts, LastScanStats.HeaderBytesRead, LastScanStats.Removed, LastScanStats.Failed);
		if (!CacheWarning.empty()) DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
		return {};
	}

	auto FAssetRegistry::FlushPersistentSnapshot() -> void
	{
		if (!bPersistentSnapshotDirty) return;
		std::vector<FRegistryCacheEntry> Entries;
		std::string Warning;
		if (BuildRegistryCacheEntries(Assets, Entries, Warning)
			&& WriteRegistryCache(GetMountManifest(), std::move(Entries), Warning))
		{
			bPersistentSnapshotDirty = false;
			CacheWarning.clear();
			return;
		}
		CacheWarning = std::move(Warning);
		if (!CacheWarning.empty()) DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
	}

	auto FAssetRegistry::FindAsset(const FAssetPath& Path) const -> const FAssetData*
	{
		auto It = Assets.find(Path);
		return It == Assets.end() ? nullptr : &It->second;
	}

	auto FAssetRegistry::AddOrUpdate(FAssetData Data) -> void
	{
		const auto Existing = Assets.find(Data.PackagePath);
		if (Existing != Assets.end() && Existing->second == Data)
		{
			bPersistentSnapshotDirty = true;
			return;
		}
		Assets.insert_or_assign(Data.PackagePath, std::move(Data));
		bPersistentSnapshotDirty = true;
		++Revision;
	}

	auto FAssetRegistry::Remove(const FAssetPath& Path) -> void
	{
		if (Assets.erase(Path) == 0) return;
		bPersistentSnapshotDirty = true;
		++Revision;
	}

	auto FAssetManager::Get() -> FAssetManager&
	{
		static FAssetManager Instance;
		return Instance;
	}

	FAssetManager::FAssetManager() = default;

	auto FAssetManager::CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown, "Asset creation is closed while the asset manager is shutting down.");
		if (PackageLoadContext.Mode == EPackageLoadMode::CookedRuntime)
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset creation.");
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
		Registry.bPersistentSnapshotDirty = true;
		return {};
	}

	auto FAssetManager::SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options) -> FAssetResult
	{
		if (PackageLoadContext.Mode == EPackageLoadMode::CookedRuntime)
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit package saves.");
		if (Package && CompatibilityRiskPackages.contains(Package) && !Options.bAllowCompatibilityDataLoss)
			return Error(
				EAssetError::UnsupportedProperty,
				"The package contains unknown incompatible fields and cannot be saved without explicit data-loss consent.");
		FAssetPath Path;
		if (Package && Package->IsAssetPackage()
			&& !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
			return Error(EAssetError::InvalidPath, "Package path is invalid.");
		const std::filesystem::path Destination(GetPhysicalPath(Path));
		FPackageFile File;
		std::vector<uint8> Bytes;
		FAssetResult SerializationResult = BuildPackageBytes(Package, Bytes, &File);
		if (!SerializationResult) return SerializationResult;
		FFileHelper::FAtomicFileError PublicationError;
		if (!FFileHelper::SaveArrayToFileAtomically(
			std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()},
			Destination,
			&PublicationError
		))
		{
			return Error(EAssetError::IoError, PublicationError.ToString());
		}
		Package->ClearDirty();
		CompatibilityRiskPackages.erase(Package);
		const auto LastWriteTime = std::filesystem::last_write_time(Destination);
		Registry.AddOrUpdate(FAssetData{
			.PackagePath = Path,
			.PhysicalPath = Destination.generic_string(),
			.AssetClassName = File.AssetClassName,
			.FormatVersion = AssetVersion,
			.Dependencies = File.Dependencies,
			.FileSize = std::filesystem::file_size(Destination),
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime)});
		return {};
	}

	auto FAssetManager::MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult
	{
		if (PackageLoadContext.Mode == EPackageLoadMode::CookedRuntime)
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset moves.");
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
		std::vector<DPackage*> AdditionalPackages;
		std::unordered_set<DPackage*> SeenAdditional;
		for (DPackage* Package : Contribution.AdditionalPackages)
		{
			if (!Package || Package == MovingPackage || !SeenAdditional.insert(Package).second
				|| std::ranges::find(Referrers, Package) != Referrers.end()) continue;
			AdditionalPackages.push_back(Package);
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
		for (DPackage* Package : AdditionalPackages)
		{
			FAssetPath AdditionalPath;
			if (!FAssetPath::TryCreate(Package->GetPackagePath(), AdditionalPath)
				|| !Backup(GetPhysicalPath(AdditionalPath)))
				return Error(EAssetError::IoError, "Failed to back up an additional move contributor package.");
		}
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
		Registry.Remove(OldPath);
		std::error_code DirectoryEc;
		std::filesystem::create_directories(NewFile.parent_path(), DirectoryEc);
		if (DirectoryEc) { LoadedPackages.erase(NewPath); LoadedPackages.emplace(OldPath, MovingPackage); Rollback(); return Error(EAssetError::IoError, "Failed to create the destination directory."); }
		Result = SavePackage(MovingPackage);
		if (Result) for (DPackage* Referrer : Referrers) { Result = SavePackage(Referrer); if (!Result) break; }
		if (Result) for (DPackage* Additional : AdditionalPackages) { Result = SavePackage(Additional); if (!Result) break; }
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

		DClass* AssetClass = FindClassByQualifiedName(FName(Data->AssetClassName));
		for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
		{
			auto It = GetDeleteContributors().find(Class);
			if (It == GetDeleteContributors().end()) continue;
			FAssetPackageInspection Inspection;
			FAssetResult InspectionResult = InspectAssetPackage(Data->PhysicalPath, Inspection);
			if (!InspectionResult)
			{
				OutAnalysis.Warning = std::format(
					"Could not inspect companion files: {} Only the main asset file will be deleted.",
					InspectionResult.Message);
				break;
			}
			FAssetDeleteContribution Contribution;
			FAssetResult ContributionResult = It->second(*Data, Inspection, Contribution);
			if (ContributionResult) OutAnalysis.CompanionFiles = std::move(Contribution.Files);
			else OutAnalysis.Warning = std::format(
				"Could not determine companion files: {} Only the main asset file will be deleted.",
				ContributionResult.Message);
			break;
		}
		return {};
	}

	auto FAssetManager::AnalyzeAssetDeletionBatch(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionBatchToken& OutToken,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		OutToken = {};
		OutBlockers.clear();
		if (PackageLoadContext.Mode == EPackageLoadMode::CookedRuntime)
			return Error(
				EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset deletion.");

		std::vector<FAssetPath> SortedPaths(Paths.begin(), Paths.end());
		std::ranges::sort(
			SortedPaths,
			[](const FAssetPath& A, const FAssetPath& B) {
				return A.GetView() < B.GetView();
			});
		SortedPaths.erase(
			std::unique(SortedPaths.begin(), SortedPaths.end()),
			SortedPaths.end());
		const std::unordered_set<FAssetPath> DeletionSet(
			SortedPaths.begin(), SortedPaths.end());
		OutToken.RegistryRevision = Registry.GetRevision();
		OutToken.PhysicalRoots.reserve(PhysicalRoots.size());
		for (const std::filesystem::path& Root : PhysicalRoots)
			OutToken.PhysicalRoots.push_back(
				std::filesystem::absolute(Root).lexically_normal());

		auto AddBlocker = [&](EAssetDeletionBatchBlocker Kind,
			const FAssetPath& AssetPath,
			const FAssetPath& RelatedAssetPath,
			std::filesystem::path PhysicalPath,
			std::string Details) {
			OutBlockers.push_back({
				.Kind = Kind,
				.AssetPath = AssetPath,
				.RelatedAssetPath = RelatedAssetPath,
				.PhysicalPath = std::move(PhysicalPath),
				.Details = std::move(Details)});
		};

		auto InspectCompanions = [&](const FAssetData& Data,
			std::vector<std::filesystem::path>& OutFiles) -> FAssetResult {
			DClass* AssetClass = FindClassByQualifiedName(FName(Data.AssetClassName));
			for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
			{
				auto It = GetDeleteContributors().find(Class);
				if (It == GetDeleteContributors().end()) continue;
				FAssetPackageInspection Inspection;
				FAssetResult Result = InspectAssetPackage(Data.PhysicalPath, Inspection);
				if (!Result) return Result;
				FAssetDeleteContribution Contribution;
				Result = It->second(Data, Inspection, Contribution);
				if (!Result) return Result;
				for (const std::filesystem::path& File : Contribution.Files)
				{
					const std::filesystem::path Normalized =
						std::filesystem::absolute(File).lexically_normal();
					if (std::ranges::find(OutFiles, Normalized) == OutFiles.end())
						OutFiles.push_back(Normalized);
				}
				std::ranges::sort(OutFiles);
				break;
			}
			return {};
		};

		for (const FAssetPath& Path : SortedPaths)
		{
			const FAssetData* Data = Registry.FindAsset(Path);
			if (!Path.IsValid() || !Data)
			{
				AddBlocker(
					EAssetDeletionBatchBlocker::MissingAsset,
					Path,
					{},
					{},
					std::format("Asset {} was not found.", Path.ToString()));
				continue;
			}

			FAssetDeletionBatchEntry Entry{
				.RegistryEntry = *Data,
				.bLoaded = LoadedPackages.contains(Path)};
			if (LoadingPackages.contains(Path))
				AddBlocker(
					EAssetDeletionBatchBlocker::LoadingPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset is currently loading.");
			if (const auto Loaded = LoadedPackages.find(Path);
				Loaded != LoadedPackages.end() && Loaded->second
				&& Loaded->second->IsDirty())
				AddBlocker(
					EAssetDeletionBatchBlocker::DirtyPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset has unsaved changes.");

			const FAssetResult CompanionResult =
				InspectCompanions(*Data, Entry.CompanionFiles);
			if (!CompanionResult)
				AddBlocker(
					EAssetDeletionBatchBlocker::CompanionInspectionFailed,
					Path,
					{},
					Data->PhysicalPath,
					CompanionResult.Message);
			OutToken.Entries.push_back(std::move(Entry));
		}

		for (const auto& [OtherPath, OtherData] : Registry.GetAssets())
		{
			if (DeletionSet.contains(OtherPath)) continue;
			for (const FAssetPath& Dependency : OtherData.Dependencies)
			{
				if (!DeletionSet.contains(Dependency)) continue;
				const bool bLoadedReference = LoadedPackages.contains(OtherPath);
				AddBlocker(
					bLoadedReference
						? EAssetDeletionBatchBlocker::ExternalLoadedReference
						: EAssetDeletionBatchBlocker::ExternalPersistentReference,
					Dependency,
					OtherPath,
					OtherData.PhysicalPath,
					std::format(
						"Asset {} is referenced by {}.",
						Dependency.ToString(),
						OtherPath.ToString()));
			}
		}

		std::unordered_map<std::string, std::vector<FAssetPath>> CompanionOwners;
		for (const auto& [OwnerPath, OwnerData] : Registry.GetAssets())
		{
			std::vector<std::filesystem::path> Files;
			if (!InspectCompanions(OwnerData, Files)) continue;
			for (const std::filesystem::path& File : Files)
				CompanionOwners[File.generic_string()].push_back(OwnerPath);
		}
		for (const FAssetDeletionBatchEntry& Entry : OutToken.Entries)
		{
			for (const std::filesystem::path& File : Entry.CompanionFiles)
			{
				auto Owners = CompanionOwners[File.generic_string()];
				std::ranges::sort(
					Owners,
					[](const FAssetPath& A, const FAssetPath& B) {
						return A.GetView() < B.GetView();
					});
				Owners.erase(std::unique(Owners.begin(), Owners.end()), Owners.end());
				if (Owners.size() > 1)
					AddBlocker(
						EAssetDeletionBatchBlocker::CompanionOwnershipConflict,
						Entry.RegistryEntry.PackagePath,
						{},
						File,
						"Companion file is claimed by multiple assets.");
				for (const FAssetPath& Owner : Owners)
					if (!DeletionSet.contains(Owner))
						AddBlocker(
							EAssetDeletionBatchBlocker::ExternalCompanionOwner,
							Entry.RegistryEntry.PackagePath,
							Owner,
							File,
							std::format(
								"Companion file is owned by asset {} outside the deletion set.",
								Owner.ToString()));
			}
		}
		for (const auto& [PhysicalPath, Owners] : CompanionOwners)
		{
			const bool bInsidePhysicalRoot = std::ranges::any_of(
				PhysicalRoots,
				[&](const std::filesystem::path& Root) {
					const std::string NormalizedRoot =
						std::filesystem::absolute(Root).lexically_normal().generic_string();
					return PhysicalPath == NormalizedRoot
						|| PathUtilities::IsLexicalDescendantPath(
							PhysicalPath, NormalizedRoot, true);
				});
			if (!bInsidePhysicalRoot) continue;
			for (const FAssetPath& Owner : Owners)
				if (!DeletionSet.contains(Owner))
					AddBlocker(
						EAssetDeletionBatchBlocker::ExternalCompanionOwner,
						Owner,
						Owner,
						PhysicalPath,
						std::format(
							"Selected content contains a companion owned by asset {} outside the deletion set.",
							Owner.ToString()));
		}

		std::ranges::sort(
			OutBlockers,
			[](const FAssetDeletionBatchBlocker& A,
				const FAssetDeletionBatchBlocker& B) {
				if (A.AssetPath.GetView() != B.AssetPath.GetView())
					return A.AssetPath.GetView() < B.AssetPath.GetView();
				if (A.RelatedAssetPath.GetView() != B.RelatedAssetPath.GetView())
					return A.RelatedAssetPath.GetView()
						< B.RelatedAssetPath.GetView();
				if (A.PhysicalPath != B.PhysicalPath)
					return A.PhysicalPath.generic_string()
						< B.PhysicalPath.generic_string();
				return A.Kind < B.Kind;
			});
		return {};
	}

	auto FAssetManager::RevalidateAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		std::vector<FAssetPath> Paths;
		Paths.reserve(Token.Entries.size());
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			Paths.push_back(Entry.RegistryEntry.PackagePath);

		FAssetDeletionBatchToken Current;
		FAssetResult Result = AnalyzeAssetDeletionBatch(
			Paths, Token.PhysicalRoots, Current, OutBlockers);
		if (!Result || !OutBlockers.empty()) return Result;
		if (Current.Entries.size() != Token.Entries.size())
			return Error(EAssetError::InUse,
				"The asset deletion set changed after confirmation.");
		for (size_t Index = 0; Index < Token.Entries.size(); ++Index)
		{
			const FAssetDeletionBatchEntry& Expected = Token.Entries[Index];
			const FAssetDeletionBatchEntry& Actual = Current.Entries[Index];
			if (!(Expected.RegistryEntry == Actual.RegistryEntry)
				|| Expected.CompanionFiles != Actual.CompanionFiles)
				return Error(EAssetError::InUse, std::format(
					"Asset {} changed after confirmation.",
					Expected.RegistryEntry.PackagePath.ToString()));
		}
		return {};
	}

	auto FAssetManager::UnloadAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		std::vector<DPackage*> Packages;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FAssetPath& Path = Entry.RegistryEntry.PackagePath;
			if (LoadingPackages.contains(Path))
				return Error(EAssetError::InUse, std::format(
					"Asset {} is currently loading.", Path.ToString()));
			const auto Loaded = LoadedPackages.find(Path);
			if (Loaded == LoadedPackages.end()) continue;
			if (Loaded->second && Loaded->second->IsDirty())
				return Error(EAssetError::InUse, std::format(
					"Asset {} has unsaved changes.", Path.ToString()));
			if (Loaded->second) Packages.push_back(Loaded->second);
		}
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			LoadedPackages.erase(Entry.RegistryEntry.PackagePath);
		for (DPackage* Package : Packages)
		{
			CompatibilityRiskPackages.erase(Package);
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!Packages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetManager::ApplyAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		std::vector<FAssetDeletionBatchBlocker> Blockers;
		FAssetResult Result = RevalidateAssetDeletionBatch(Token, Blockers);
		if (!Result) return Result;
		if (!Blockers.empty())
			return Error(EAssetError::InUse, Blockers.front().Details);
		Result = UnloadAssetDeletionBatch(Token);
		if (!Result) return Result;
		return RemoveAssetDeletionBatchRegistryProjection(Token);
	}

	auto FAssetManager::RemoveAssetDeletionBatchRegistryProjection(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		std::unordered_set<FAssetPath> DeletionSet;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			DeletionSet.insert(Entry.RegistryEntry.PackagePath);
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FAssetData* Current = Registry.FindAsset(
				Entry.RegistryEntry.PackagePath);
			if (!Current || !(*Current == Entry.RegistryEntry))
				return Error(EAssetError::InUse, std::format(
					"Asset {} changed before registry removal.",
					Entry.RegistryEntry.PackagePath.ToString()));
		}
		for (const auto& [OtherPath, OtherData] : Registry.GetAssets())
		{
			if (DeletionSet.contains(OtherPath)) continue;
			for (const FAssetPath& Dependency : OtherData.Dependencies)
				if (DeletionSet.contains(Dependency))
					return Error(EAssetError::InUse, std::format(
						"Asset {} gained external referencer {}.",
						Dependency.ToString(), OtherPath.ToString()));
		}
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			Registry.Remove(Entry.RegistryEntry.PackagePath);
		return {};
	}

	auto FAssetManager::RestoreAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FAssetPath& Path = Entry.RegistryEntry.PackagePath;
			if (Registry.FindAsset(Path) || LoadedPackages.contains(Path))
				return Error(EAssetError::AlreadyExists, std::format(
					"Asset {} already exists and cannot be restored.",
					Path.ToString()));
			std::error_code Ec;
			if (!std::filesystem::is_regular_file(
					Entry.RegistryEntry.PhysicalPath, Ec))
				return Error(EAssetError::NotFound, std::format(
					"Restored package file is missing: {}.",
					Entry.RegistryEntry.PhysicalPath));
		}
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			Registry.AddOrUpdate(Entry.RegistryEntry);
		return {};
	}

	auto FAssetManager::DeleteAsset(const FAssetPath& Path) -> FAssetResult
	{
		if (PackageLoadContext.Mode == EPackageLoadMode::CookedRuntime)
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset deletion.");
		FAssetDeleteAnalysis Analysis;
		FAssetResult Result = AnalyzeAssetDeletion(Path, Analysis);
		if (!Result) return Result;
		if (!Analysis.DirectReferencers.empty())
			return Error(EAssetError::InUse, std::format("Asset {} is referenced by {} asset(s).", Path.ToString(), Analysis.DirectReferencers.size()));
		if (Analysis.bLoading) return Error(EAssetError::InUse, "Asset is currently loading.");
		if (Analysis.bLoaded)
		{
			// LoadedPackages is a residency cache. Once persistent package references are gone,
			// keeping that cache entry must not force users to restart before deleting an asset.
			Result = UnloadPackage(Path);
			if (!Result) return Result;
		}

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

		Registry.Remove(Path);
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

	auto FAssetManager::LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.Load");
		if (!bAcceptingRequests)
		{
			OutAsset = nullptr;
			return Error(
				EAssetError::ShuttingDown,
				"Asset loading is closed while the asset manager is shutting down.");
		}
		if (OutReport) *OutReport = {.PackagePath = Path};
		const bool bRootLoad = LoadDepth++ == 0;
		if (bRootLoad) TransactionPackages.clear();
		FAssetLoadReport* PreviousLoadReport = GActiveAssetLoadReport;
		if (bRootLoad) GActiveAssetLoadReport = OutReport;
		DPackage* Package = nullptr;
		FAssetResult Result = LoadPackageInternal(Path, Package, OutReport);
		if (bRootLoad) GActiveAssetLoadReport = PreviousLoadReport;
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
			if (Result) bPackageLoadStarted = true;
		}
		OutAsset = Result && Package ? Package->GetAsset() : nullptr;
		return Result;
	}

	auto FAssetManager::LoadPackageInternal(
		const FAssetPath& Path,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.LoadPackage");
		if (auto It = LoadedPackages.find(Path); It != LoadedPackages.end())
		{
			OutPackage = It->second;
			return {};
		}
		std::vector<uint8> Bytes;
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty()) return Error(EAssetError::InvalidPath, "Asset path cannot be resolved in the selected package mode.");
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
			CompatibilityRiskPackages.erase(Package);
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

		Package->ClearDirty();
		FAssetMigrationContext MigrationContext{Objects};
		for (const FObjectRecord& Record : File.Objects)
		{
			DObject* Object = Objects[Record.Id - 1];
			std::vector<FAssetLegacyField> LegacyFields;
			for (const FFieldRecord& Field : Record.Fields)
			{
				DClass* DeclaringClass = FindClassByQualifiedName(Field.DeclaringClass);
				FProperty* Property = DeclaringClass && Object->IsA(DeclaringClass)
					? DeclaringClass->FindPropertyByName(FName(Field.Name), false)
					: nullptr;
				if (!Property || Property->GetKind() != Field.Kind
					|| !IsSerializedTypeSignatureCompatible(Property, Field.TypeSignature))
				{
					LegacyFields.push_back({
						.DeclaringClass = Field.DeclaringClass,
						.Name = Field.Name,
						.Kind = Field.Kind,
						.TypeSignature = Field.TypeSignature,
						.Payload = Field.Payload});
					continue;
				}
				FByteReader FieldReader{Field.Payload};
				for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
				{
					Result = DeserializeValue(
						Property, Object, ArrayIndex, FieldReader, Objects,
						&LegacyFields, Path.GetView(), File.FormatVersion);
					if (!Result) { Rollback(); return Result; }
				}
				if (FieldReader.Offset != Field.Payload.size()) { Rollback(); return Error(EAssetError::CorruptFile, "Property payload has trailing bytes."); }
			}

			if (LegacyFields.empty()) continue;
			std::vector<FAssetCompatibilityIssue> ObjectIssues;
			const FRegisteredStructureUpgrader* RegisteredUpgrader = nullptr;
			for (DClass* Class = Object->GetClass(); Class; Class = Class->GetSuperClass())
			{
				auto It = GetStructureUpgraders().find(Class);
				if (It == GetStructureUpgraders().end()) continue;
				RegisteredUpgrader = &It->second;
				break;
			}
			if (RegisteredUpgrader)
			{
				Result = RegisteredUpgrader->Upgrader(Object, LegacyFields, MigrationContext, ObjectIssues);
				if (!Result) { Rollback(); return Result; }
				for (FAssetCompatibilityIssue& Issue : ObjectIssues)
				{
					if (Issue.ObjectPath.empty()) Issue.ObjectPath = Object->GetObjectPath();
					if (Issue.DeclaringClass.empty())
						Issue.DeclaringClass = Object->GetClass()->GetQualifiedName().ToString();
					if (Issue.HandlerId.empty()) Issue.HandlerId = RegisteredUpgrader->HandlerId;
				}
			}

			auto WasHandled = [&ObjectIssues](const FAssetLegacyField& Field) {
				return std::ranges::any_of(ObjectIssues, [&Field](const FAssetCompatibilityIssue& Issue) {
					return std::ranges::any_of(Issue.LegacyFields, [&Field](const FAssetLegacyField& Handled) {
						return Handled.DeclaringClass == Field.DeclaringClass
							&& Handled.Name == Field.Name
							&& Handled.TypeSignature == Field.TypeSignature;
					});
				});
			};
			std::unordered_map<std::string, std::vector<FAssetLegacyField>> UnknownByClass;
			for (const FAssetLegacyField& Field : LegacyFields)
			{
				if (!WasHandled(Field)) UnknownByClass[Field.DeclaringClass].push_back(Field);
			}
			for (auto& [DeclaringClass, Fields] : UnknownByClass)
			{
				DURIN_WARN(
					"Asset package '{}' contains unknown incompatible fields on object '{}'; "
					"the fields were skipped and the package was not marked dirty.",
					Path.ToString(), Object->GetObjectPath());
				ObjectIssues.push_back({
					.ObjectPath = Object->GetObjectPath(),
					.DeclaringClass = std::move(DeclaringClass),
					.LegacyFields = std::move(Fields),
					.Classification = EAssetCompatibilityClassification::UnknownIncompatible,
					.MigrationSummary = "No registered asset-structure upgrader recognizes these fields.",
					.Risk = EAssetCompatibilityRisk::UnknownNewerSchema});
			}
			for (FAssetCompatibilityIssue& Issue : ObjectIssues)
			{
				if (Issue.Classification == EAssetCompatibilityClassification::SafeCleanup
					|| Issue.Classification == EAssetCompatibilityClassification::Migrated)
				{
					Package->MarkDirty();
					ReportAssetLoadMutation(
						Object,
						Issue.HandlerId,
						Issue.MigrationSummary,
						EAssetLoadMutationKind::Upgrade);
					DURIN_WARN(
						"Asset package '{}' uses legacy serialized data: {} "
						"Save the package to upgrade its on-disk format.",
						Path.ToString(), Issue.MigrationSummary);
				}
				if (Issue.Risk != EAssetCompatibilityRisk::None)
					CompatibilityRiskPackages.insert(Package);
				if (OutReport) OutReport->CompatibilityIssues.push_back(std::move(Issue));
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

		LoadingPackages.erase(Path);
		OutPackage = Package;
		const auto LastWriteTime = std::filesystem::last_write_time(PhysicalPath);
		Registry.AddOrUpdate(FAssetData{
			.PackagePath = Path,
			.PhysicalPath = PhysicalPath,
			.AssetClassName = File.AssetClassName,
			.FormatVersion = File.FormatVersion,
			.Dependencies = File.Dependencies,
			.FileSize = std::filesystem::file_size(PhysicalPath),
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime)});
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
		CompatibilityRiskPackages.erase(Package);
		RemoveFromRoot(Package);
		MarkObjectHierarchyAsGarbage(Package);
		CollectGarbage();
		return {};
	}

	auto FAssetManager::CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot
	{
		FAssetPackageLoadSnapshot Snapshot;
		Snapshot.LoadedPackages.reserve(LoadedPackages.size());
		for (const auto& [Path, Package] : LoadedPackages) Snapshot.LoadedPackages.push_back(Path);
		std::ranges::sort(Snapshot.LoadedPackages, {}, [](const FAssetPath& Path) {
			return Path.ToString();
		});
		return Snapshot;
	}

	auto FAssetManager::ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		if (LoadDepth != 0 || !LoadingPackages.empty())
			return Error(EAssetError::InUse, "A package load is still in progress.");

		std::unordered_set<FAssetPath> Protected(
			Snapshot.LoadedPackages.begin(), Snapshot.LoadedPackages.end());
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (const auto& [Path, Package] : LoadedPackages)
			{
				if (!Protected.contains(Path)) continue;
				const FAssetData* Data = Registry.FindAsset(Path);
				if (!Data) continue;
				for (const FAssetPath& Dependency : Data->Dependencies)
					bChanged |= Protected.insert(Dependency).second;
			}
		}

		std::vector<DPackage*> ReleasedPackages;
		for (auto It = LoadedPackages.begin(); It != LoadedPackages.end();)
		{
			if (Protected.contains(It->first))
			{
				++It;
				continue;
			}
			DPackage* Package = It->second;
			CompatibilityRiskPackages.erase(Package);
			ReleasedPackages.push_back(Package);
			It = LoadedPackages.erase(It);
		}
		for (DPackage* Package : ReleasedPackages)
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!ReleasedPackages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetManager::Shutdown() -> void
	{
		StopAcceptingRequests();
		Registry.FlushPersistentSnapshot();
		std::vector<DPackage*> Packages;
		Packages.reserve(LoadedPackages.size());
		for (const auto& [Path, Package] : LoadedPackages)
		{
			if (Package) Packages.push_back(Package);
		}
		LoadedPackages.clear();
		LoadingPackages.clear();
		CompatibilityRiskPackages.clear();
		TransactionPackages.clear();
		LoadDepth = 0;
		PackageLoadContext = {};
		bPackageLoadStarted = false;
		for (DPackage* Package : Packages)
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
	}

	auto FAssetManager::Initialize() -> void
	{
		check(LoadedPackages.empty());
		check(LoadingPackages.empty());
		check(TransactionPackages.empty());
		bAcceptingRequests = true;
	}

	auto FAssetManager::StopAcceptingRequests() -> void
	{
		if (!bAcceptingRequests) return;
		bAcceptingRequests = false;
		DURIN_DEBUG("Asset manager stopped accepting new requests.");
	}

	auto FAssetManager::ConfigurePackageLoadContext(FPackageLoadContext InContext) -> FAssetResult
	{
		std::string ValidationError;
		if (!InContext.IsValid(&ValidationError)) return Error(EAssetError::InvalidPath, std::move(ValidationError));
		if (bPackageLoadStarted || !LoadedPackages.empty() || !LoadingPackages.empty() || LoadDepth != 0)
			return Error(EAssetError::InUse, "Package load context cannot change after package loading has begun.");
		PackageLoadContext = std::move(InContext);
		return {};
	}

	auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetManager::Get().LoadAsset(Path, OutAsset, OutReport);
	}
	auto SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options) -> FAssetResult
	{
		return FAssetManager::Get().SavePackage(Package, Options);
	}
	auto MoveAsset(const FAssetPath& OldPath, const FAssetPath& NewPath) -> FAssetResult { return FAssetManager::Get().MoveAsset(OldPath, NewPath); }
	auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult { return FAssetManager::Get().AnalyzeAssetDeletion(Path, OutAnalysis); }
	auto AnalyzeAssetDeletionBatch(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionBatchToken& OutToken,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		return FAssetManager::Get().AnalyzeAssetDeletionBatch(
			Paths, PhysicalRoots, OutToken, OutBlockers);
	}
	auto RevalidateAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		return FAssetManager::Get().RevalidateAssetDeletionBatch(
			Token, OutBlockers);
	}
	auto UnloadAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		return FAssetManager::Get().UnloadAssetDeletionBatch(Token);
	}
	auto ApplyAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		return FAssetManager::Get().ApplyAssetDeletionBatch(Token);
	}
	auto RemoveAssetDeletionBatchRegistryProjection(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		return FAssetManager::Get().RemoveAssetDeletionBatchRegistryProjection(Token);
	}
	auto RestoreAssetDeletionBatch(
		const FAssetDeletionBatchToken& Token) -> FAssetResult
	{
		return FAssetManager::Get().RestoreAssetDeletionBatch(Token);
	}
	auto DeleteAsset(const FAssetPath& Path) -> FAssetResult { return FAssetManager::Get().DeleteAsset(Path); }
	auto FindLoadedPackage(const FAssetPath& Path) -> DPackage* { return FAssetManager::Get().FindLoadedPackage(Path); }
	auto UnloadPackage(const FAssetPath& Path) -> FAssetResult { return FAssetManager::Get().UnloadPackage(Path); }
	auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot
	{
		return FAssetManager::Get().CapturePackageLoadSnapshot();
	}
	auto ReleasePackagesLoadedSince(const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		return FAssetManager::Get().ReleasePackagesLoadedSince(Snapshot);
	}
	auto ShutdownAssetManager() -> void { FAssetManager::Get().Shutdown(); }
	auto ConfigurePackageLoadContext(FPackageLoadContext Context) -> FAssetResult
	{
		return FAssetManager::Get().ConfigurePackageLoadContext(std::move(Context));
	}
	auto GetPackageLoadContext() -> const FPackageLoadContext&
	{
		return FAssetManager::Get().GetPackageLoadContext();
	}
	auto GetAssetRegistry() -> FAssetRegistry& { return FAssetManager::Get().GetRegistry(); }
}
