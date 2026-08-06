#include "AssetSystem.h"
#include "AssetRedirector.h"
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
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	namespace
	{
		thread_local FAssetLoadReport* GActiveAssetLoadReport = nullptr;

		auto CheckSoftObjectThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto Error(EAssetError Code, std::string Message) -> FAssetResult;

		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::NotFound:
				return Error(EAssetError::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectCycle:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString()));
			case EAssetPathResolveState::CorruptRedirector:
				return Error(EAssetError::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString()));
			}
			return Error(EAssetError::CorruptFile, "Asset resolution returned an unknown state.");
		}

		constexpr uint32 AssetMagic = 0x54534144; // DAST
		constexpr uint32 MinimumAssetVersion = 2;
		constexpr uint32 AssetVersion = 3;
		constexpr uint64 MaximumPackageStringBytes = 1024 * 1024;
		constexpr uint32 MaximumRedirectDepth = 32;
		constexpr std::string_view RedirectorClassName = "Durin::Asset::DAssetRedirector";

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
			EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
			FAssetPath RedirectDestination;
			std::vector<FAssetPath> Dependencies;
			std::vector<FObjectRecord> Objects;
		};

		auto Error(EAssetError Code, std::string Message) -> FAssetResult { return {Code, std::move(Message)}; }

		auto CorruptRedirector(std::string Message) -> FAssetResult
		{
			return Error(EAssetError::CorruptFile, std::format("CorruptRedirector: {}", Message));
		}

		auto ValidateRedirectorHeader(
			const FPackageFile& File,
			uint64 ObjectCount,
			const FAssetPath* SourcePath = nullptr) -> FAssetResult
		{
			if (File.FormatVersion == 2)
			{
				if (File.AssetClassName == RedirectorClassName)
					return CorruptRedirector("DAST v2 cannot identify a redirector.");
				return {};
			}
			if (File.EntryKind == EAssetRegistryEntryKind::Asset)
			{
				if (File.RedirectDestination.IsValid())
					return CorruptRedirector("an ordinary asset declares a redirect destination.");
				if (File.AssetClassName == RedirectorClassName)
					return CorruptRedirector("the redirector class is marked as an ordinary asset.");
				return {};
			}
			if (File.EntryKind != EAssetRegistryEntryKind::Redirector)
				return CorruptRedirector("the package declares an unknown registry entry kind.");
			if (File.AssetClassName != RedirectorClassName)
				return CorruptRedirector("the redirect entry does not use DAssetRedirector.");
			if (!File.RedirectDestination.IsValid())
				return CorruptRedirector("the redirect destination is missing or invalid.");
			if (SourcePath && *SourcePath == File.RedirectDestination)
				return CorruptRedirector("a redirector cannot target its own package.");
			if (ObjectCount != 1)
				return CorruptRedirector("a redirector package must contain exactly one object.");
			if (File.Dependencies.size() != 1
				|| File.Dependencies.front() != File.RedirectDestination)
				return CorruptRedirector(
					"the dependency table must contain only the redirect destination.");
			return {};
		}

		auto ValidateRedirectorBody(const FPackageFile& File) -> FAssetResult
		{
			if (File.EntryKind != EAssetRegistryEntryKind::Redirector) return {};
			if (File.Objects.size() != 1) return CorruptRedirector(
				"the serialized body must contain exactly one object.");
			const FObjectRecord& Object = File.Objects.front();
			if (Object.Id != 1 || Object.OuterId != 0
				|| Object.ClassName != RedirectorClassName)
				return CorruptRedirector("the main object does not match the redirect summary.");
			if (Object.Fields.size() != 1)
				return CorruptRedirector(
					"the redirector must serialize exactly one DestinationObject field.");
			const FFieldRecord& Field = Object.Fields.front();
			if (Field.DeclaringClass != RedirectorClassName
				|| Field.Name != "DestinationObject"
				|| Field.Kind != DurinCodeGen::EPropertyGenFlags::Object
				|| Field.TypeSignature != "Object:Durin::DObject:true")
				return CorruptRedirector("DestinationObject metadata is invalid.");
			FByteReader Reader{Field.Payload};
			uint8 ReferenceKind = 0;
			std::string DestinationString;
			FAssetPath Destination;
			if (!Reader.Read(ReferenceKind) || ReferenceKind != 2
				|| !Reader.ReadString(DestinationString, MaximumPackageStringBytes)
				|| !FAssetPath::TryCreate(DestinationString, Destination)
				|| Reader.Offset != Field.Payload.size()
				|| Destination != File.RedirectDestination)
				return CorruptRedirector(
					"DestinationObject is not the declared non-null external destination.");
			return {};
		}

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

		auto GetMoveExternalStores()
			-> std::map<FAssetMoveExternalStoreHandle, FAssetMoveExternalStore>&
		{
			static std::map<FAssetMoveExternalStoreHandle, FAssetMoveExternalStore> Stores;
			return Stores;
		}

		auto NextMoveExternalStoreHandle() -> FAssetMoveExternalStoreHandle&
		{
			static FAssetMoveExternalStoreHandle Handle = 1;
			return Handle;
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
			if (Kind == DurinCodeGen::EPropertyGenFlags::SoftObject)
			{
				auto* SoftObject = static_cast<FSoftObjectProperty*>(Property);
				return std::format("SoftObject:{}:v1", SoftObject->GetExpectedClass()
					? SoftObject->GetExpectedClass()->GetQualifiedName().ToString() : "DObject");
			}
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
			FProperty* Property, const void* Container, uint32 ArrayIndex, FByteWriter& Writer,
			const std::unordered_map<DObject*, uint64>& ObjectIds,
			std::unordered_set<FAssetPath>& Dependencies) -> FAssetResult;

		struct FAssetArrayVisitContext
		{
			FProperty* Inner;
			FByteWriter& Writer;
			const std::unordered_map<DObject*, uint64>& ObjectIds;
			std::unordered_set<FAssetPath>& Dependencies;
			FAssetResult Result;
		};

		auto SerializeAssetArrayElement(void* RawContext, uint64, const void* Element) -> bool
		{
			auto& Context = *static_cast<FAssetArrayVisitContext*>(RawContext);
			Context.Result = SerializeValue(
				Context.Inner, Element, 0, Context.Writer, Context.ObjectIds, Context.Dependencies);
			return Context.Result.Succeeded();
		}

		struct FCanonicalAssetMapEntry
		{
			std::vector<uint8> Token;
			const void* Key = nullptr;
			const void* Value = nullptr;
		};

		struct FCanonicalAssetMapContext
		{
			FProperty* KeyProperty;
			std::vector<FCanonicalAssetMapEntry> Entries;
			std::string Error;
		};

		auto CollectCanonicalAssetMapEntry(void* RawContext, const void* Key, const void* Value) -> bool
		{
			auto& Context = *static_cast<FCanonicalAssetMapContext*>(RawContext);
			FCanonicalAssetMapEntry Entry;
			if (!BuildCanonicalMapKeyToken(Context.KeyProperty, Key, 0, Entry.Token, &Context.Error)) return false;
			Entry.Key = Key;
			Entry.Value = Value;
			Context.Entries.push_back(std::move(Entry));
			return true;
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
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
				const FSoftObjectPtr* Reference = SoftProperty->GetSoftObjectPtr(Container, ArrayIndex);
				if (!Reference)
					return Error(EAssetError::UnsupportedProperty,
						"Soft object property has no typed value accessor.");
				if (Reference->IsNull())
				{
					Writer.Write(uint8(0));
					return {};
				}
				const std::string_view Path = Reference->GetSoftObjectPath().GetAssetPath().GetView();
				if (Path.empty() || Path.size() > MaximumPackageStringBytes)
					return Error(EAssetError::InvalidPath, "Soft object path exceeds the authored package bound.");
				Writer.Write(uint8(1));
				Writer.WriteString(Path);
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
				if (!Array->HasArrayOps() || !Array->GetInner()
					|| !Array->HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::ConstTraversal))
					return Error(EAssetError::UnsupportedProperty, "ArrayOperationUnavailable: DAST save requires Count and ConstTraversal.");
				uint64 Num = 0;
				if (Array->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty, "ArrayOperationFailed: Count failed during DAST save.");
				Writer.Write(Num);
				FAssetArrayVisitContext Context{Array->GetInner(), Writer, ObjectIds, Dependencies};
				if (Array->VisitElements(Container, &SerializeAssetArrayElement, &Context, ArrayIndex)
					!= EContainerOpResult::Success && Context.Result)
					return Error(EAssetError::UnsupportedProperty, "ArrayOperationFailed: ConstTraversal failed during DAST save.");
				if (!Context.Result) return Context.Result;
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				if (!Map->HasMapOps() || !Map->GetKeyProp() || !Map->GetValueProp()
					|| !Map->HasCapability(EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal))
					return Error(EAssetError::UnsupportedProperty, "MapOperationUnavailable: DAST save requires Count and ConstTraversal.");
				std::string CanonicalError;
				if (!ValidateCanonicalMapKeyProperty(Map->GetKeyProp(), &CanonicalError))
					return Error(EAssetError::UnsupportedProperty, std::move(CanonicalError));
				uint64 Num = 0;
				if (Map->GetNum(Container, Num, ArrayIndex) != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty, "MapOperationFailed: Count failed during DAST save.");
				Writer.Write(Num);
				FCanonicalAssetMapContext Context{Map->GetKeyProp()};
				const EContainerOpResult VisitResult = Map->VisitEntries(
					Container, &CollectCanonicalAssetMapEntry, &Context, ArrayIndex);
				if (VisitResult != EContainerOpResult::Success || !Context.Error.empty())
					return Error(EAssetError::UnsupportedProperty, Context.Error.empty()
						? "MapOperationFailed: ConstTraversal failed during DAST save." : std::move(Context.Error));
				std::ranges::sort(Context.Entries, {}, &FCanonicalAssetMapEntry::Token);
				for (size_t Index = 0; Index < Context.Entries.size(); ++Index)
				{
					if (Index > 0 && Context.Entries[Index - 1].Token == Context.Entries[Index].Token)
						return Error(EAssetError::UnsupportedProperty,
							"CanonicalMapKeyCollision: distinct entries produced the same canonical token.");
					FAssetResult Result = SerializeValue(Map->GetKeyProp(), Context.Entries[Index].Key, 0, Writer, ObjectIds, Dependencies);
					if (!Result) return Result;
					Result = SerializeValue(Map->GetValueProp(), Context.Entries[Index].Value, 0, Writer, ObjectIds, Dependencies);
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
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
				FSoftObjectPtr* Reference = SoftProperty->GetSoftObjectPtr(Container, ArrayIndex);
				if (!Reference)
					return Error(EAssetError::UnsupportedProperty,
						"Soft object property has no typed value accessor.");
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind))
					return Error(EAssetError::CorruptFile, "Truncated soft object reference.");
				if (ReferenceKind == 0)
				{
					Reference->Reset();
					return {};
				}
				if (ReferenceKind != 1)
					return Error(EAssetError::CorruptFile, "Unknown soft object reference tag.");
				std::string PathString;
				if (!Reader.ReadString(PathString, MaximumPackageStringBytes) || PathString.empty())
					return Error(EAssetError::CorruptFile, "Truncated or overlong soft object path.");
				FSoftObjectPath Path;
				std::string PathError;
				if (!FSoftObjectPath::TryCreate(PathString, Path, &PathError))
					return Error(EAssetError::InvalidPath, PathError.empty()
						? "Invalid soft object path." : std::move(PathError));
				Reference->SetPath(std::move(Path));
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
				if (!Array->HasArrayOps() || !Array->GetInner() || !Reader.Read(Num) || Num > 10000000)
					return Error(EAssetError::CorruptFile, "Invalid array payload.");
				if (!Array->HasCapability(EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit
					| EArrayOpsFlags::RandomAccess) || (Num > 0 && !Array->HasCapability(EArrayOpsFlags::DefaultGrow)))
					return Error(EAssetError::UnsupportedProperty,
						"ArrayOperationUnavailable: DAST load requires DetachedStorage, RandomAccess, DefaultGrow, and TransactionalCommit.");
				const FArrayOps& Ops = Array->GetOps();
				FDetachedContainerStorage Detached;
				EContainerOpResult OpResult = Detached.Create(Ops);
				if (OpResult == EContainerOpResult::Success) OpResult = Ops.Resize(Detached.Get(), Num);
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						std::format("ArrayOperationFailed: detached allocation/resize returned {}.", static_cast<uint32>(OpResult)));
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					void* Element = nullptr;
					OpResult = Ops.GetMutableAt(Detached.Get(), Index, &Element);
					if (OpResult != EContainerOpResult::Success)
						return Error(EAssetError::UnsupportedProperty,
							std::format("ArrayElement[{}]: mutable access returned {}.", Index, static_cast<uint32>(OpResult)));
					FAssetResult Result = DeserializeValue(
						Array->GetInner(), Element,
						0, Reader, Objects, OutLegacyFields, PackagePath, SourceVersion);
					if (!Result)
					{
						Result.Message = std::format("ArrayElement[{}]: {}", Index, Result.Message);
						return Result;
					}
				}
				OpResult = Ops.Commit(Array->GetValuePtr(Container, ArrayIndex), Detached.Get());
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						std::format("ArrayOperationFailed: Commit returned {}.", static_cast<uint32>(OpResult)));
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Num = 0;
				if (!Map->HasMapOps() || !Map->GetKeyProp() || !Map->GetValueProp()
					|| !Reader.Read(Num) || Num > 10000000)
					return Error(EAssetError::CorruptFile, "Invalid map payload.");
				if (!Map->HasCapability(EMapOpsFlags::DetachedStorage | EMapOpsFlags::TransactionalCommit | EMapOpsFlags::Insert))
					return Error(EAssetError::UnsupportedProperty,
						"MapOperationUnavailable: DAST load requires DetachedStorage, Insert, and TransactionalCommit.");
				const FMapOps& Ops = Map->GetOps();
				FDetachedContainerStorage Detached;
				EContainerOpResult OpResult = Detached.Create(Ops);
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						std::format("MapOperationFailed: detached allocation returned {}.", static_cast<uint32>(OpResult)));
				if (Ops.Reserve && (OpResult = Ops.Reserve(Detached.Get(), Num)) != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						std::format("MapOperationFailed: Reserve returned {}.", static_cast<uint32>(OpResult)));
				FReflectedValueStorage KeyStorage;
				FReflectedValueStorage ValueStorage;
				std::string StorageError;
				if (Num > 0
					&& (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError)
						|| !ValueStorage.DefaultConstruct(Map->GetValueProp(), 0, &StorageError)))
					return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
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
					if (!Result)
					{
						Result.Message = std::format("MapEntry[{}].Key: {}", Index, Result.Message);
						return Result;
					}
					Result = DeserializeValue(
						Map->GetValueProp(), ValueStorage.GetContainer(), 0, Reader, Objects, OutLegacyFields, PackagePath, SourceVersion);
					if (!Result)
					{
						Result.Message = std::format("MapEntry[{}].Value: {}", Index, Result.Message);
						return Result;
					}
					OpResult = Ops.InsertCopy(Detached.Get(), KeyStorage.GetValue(), ValueStorage.GetValue());
					if (OpResult == EContainerOpResult::DuplicateKey)
						return Error(EAssetError::CorruptFile,
							std::format("MapEntry[{}].Key: duplicate decoded key.", Index));
					if (OpResult != EContainerOpResult::Success)
						return Error(EAssetError::UnsupportedProperty,
							std::format("MapEntry[{}]: Insert returned {}.", Index, static_cast<uint32>(OpResult)));
				}
				OpResult = Ops.Commit(Map->GetValuePtr(Container, ArrayIndex), Detached.Get());
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						std::format("MapOperationFailed: Commit returned {}.", static_cast<uint32>(OpResult)));
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
			Writer.Write(uint8(File.EntryKind));
			Writer.WriteString(File.RedirectDestination.GetView());
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
			if (Version < MinimumAssetVersion || Version > AssetVersion)
				return Error(EAssetError::UnsupportedVersion, std::format("Unsupported asset version {}.", Version));
			OutFile.FormatVersion = Version;
			if (!Reader.ReadString(OutFile.AssetClassName, MaximumPackageStringBytes)) return Error(EAssetError::CorruptFile, "Invalid asset header strings.");
			if (Version >= 3)
			{
				uint8 EntryKind = 0;
				std::string RedirectDestination;
				if (!Reader.Read(EntryKind)
					|| EntryKind > uint8(EAssetRegistryEntryKind::Redirector)
					|| !Reader.ReadString(RedirectDestination, MaximumPackageStringBytes))
					return CorruptRedirector("the redirect summary is invalid or truncated.");
				OutFile.EntryKind = static_cast<EAssetRegistryEntryKind>(EntryKind);
				if (!RedirectDestination.empty()
					&& !FAssetPath::TryCreate(RedirectDestination, OutFile.RedirectDestination))
					return CorruptRedirector("the redirect destination path is invalid.");
			}
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
			return ValidateRedirectorHeader(OutFile, OutObjectCount);
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
			return ValidateRedirectorBody(OutFile);
		}

		constexpr uint32 MaximumSoftReferenceContainerDepth = 4;
		constexpr uint64 MaximumSoftReferencesPerPackage = 100000;
		constexpr uint64 MaximumSoftReferencesPerSnapshot = 1000000;
		constexpr uint64 MaximumSoftReferenceDisplayPathBytes = 4 * 1024;
		constexpr uint64 MaximumSoftReferenceRouteTokenBytes = 1024 * 1024;

		auto ContainsSoftObjectPropertyImpl(
			const FProperty* Property,
			std::unordered_set<const DStruct*>& VisitingStructs) -> bool
		{
			if (!Property) return false;
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				return true;
			case DurinCodeGen::EPropertyGenFlags::Array:
				return ContainsSoftObjectPropertyImpl(
					static_cast<const FArrayProperty*>(Property)->GetInner(), VisitingStructs);
			case DurinCodeGen::EPropertyGenFlags::Map:
				return ContainsSoftObjectPropertyImpl(
					static_cast<const FMapProperty*>(Property)->GetKeyProp(), VisitingStructs)
					|| ContainsSoftObjectPropertyImpl(
						static_cast<const FMapProperty*>(Property)->GetValueProp(), VisitingStructs);
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
				if (!Struct || !VisitingStructs.insert(Struct).second) return false;
				bool bContainsSoftObject = false;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (!bContainsSoftObject && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
						bContainsSoftObject = ContainsSoftObjectPropertyImpl(Field, VisitingStructs);
				}, false);
				VisitingStructs.erase(Struct);
				return bContainsSoftObject;
			}
			default:
				return false;
			}
		}

		auto ContainsSoftObjectProperty(const FProperty* Property) -> bool
		{
			std::unordered_set<const DStruct*> VisitingStructs;
			return ContainsSoftObjectPropertyImpl(Property, VisitingStructs);
		}

		auto CompareSoftReferenceRoute(
			std::span<const FSoftAssetReferenceRouteSegment> Left,
			std::span<const FSoftAssetReferenceRouteSegment> Right) -> int
		{
			const size_t Count = std::min(Left.size(), Right.size());
			for (size_t Index = 0; Index < Count; ++Index)
			{
				if (Left[Index].Kind != Right[Index].Kind)
					return static_cast<uint8>(Left[Index].Kind) < static_cast<uint8>(Right[Index].Kind) ? -1 : 1;
				if (Left[Index].Kind == ESoftAssetReferenceRouteKind::MapValue)
				{
					if (Left[Index].MapKeyToken != Right[Index].MapKeyToken)
						return std::ranges::lexicographical_compare(
							Left[Index].MapKeyToken, Right[Index].MapKeyToken) ? -1 : 1;
				}
				else if (Left[Index].Index != Right[Index].Index)
					return Left[Index].Index < Right[Index].Index ? -1 : 1;
			}
			if (Left.size() == Right.size()) return 0;
			return Left.size() < Right.size() ? -1 : 1;
		}

		auto SoftReferenceLess(const FSoftAssetReference& Left, const FSoftAssetReference& Right) -> bool
		{
			const auto LeftKey = std::tuple(
				Left.TargetPath.GetView(), Left.SourcePackage.GetView(), Left.SourceObjectId,
				std::string_view(Left.DeclaringType), std::string_view(Left.FieldName));
			const auto RightKey = std::tuple(
				Right.TargetPath.GetView(), Right.SourcePackage.GetView(), Right.SourceObjectId,
				std::string_view(Right.DeclaringType), std::string_view(Right.FieldName));
			if (LeftKey != RightKey) return LeftKey < RightKey;
			return CompareSoftReferenceRoute(Left.ContainerRoute, Right.ContainerRoute) < 0;
		}

		auto AppendMapTokenDisplay(std::string& Path, std::span<const uint8> Token) -> void
		{
			Path.append("[key:");
			for (const uint8 Byte : Token) Path.append(std::format("{:02x}", static_cast<uint32>(Byte)));
			Path.push_back(']');
		}

		struct FSoftReferenceExtractionContext
		{
			const FAssetPath& SourcePackage;
			const FAssetPackageFingerprint& Fingerprint;
			const FAssetPackageObjectInspection& Object;
			std::string_view DeclaringType;
			std::string_view FieldName;
			std::vector<FSoftAssetReference>& References;
		};

		auto ExtractSoftValue(
			FProperty* Property,
			FByteReader& Reader,
			const FSoftReferenceExtractionContext& Context,
			std::vector<FSoftAssetReferenceRouteSegment>& Route,
			const std::string& PropertyPath,
			uint32 ContainerDepth) -> FAssetResult;

		auto ExtractSoftPropertyValues(
			FProperty* Property,
			std::span<const uint8> Payload,
			const FSoftReferenceExtractionContext& Context,
			std::vector<FSoftAssetReferenceRouteSegment>& Route,
			const std::string& PropertyPath,
			uint32 ContainerDepth) -> FAssetResult
		{
			FByteReader Reader{Payload};
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				const bool bFixedArray = Property->GetArrayDim() > 1;
				std::string ElementPath = PropertyPath;
				if (bFixedArray)
				{
					if (ContainerDepth >= MaximumSoftReferenceContainerDepth)
						return Error(EAssetError::CorruptFile,
							"SoftReferenceIndexDepthExceeded: fixed-array route exceeds four levels.");
					Route.push_back({
						.Kind = ESoftAssetReferenceRouteKind::FixedArray,
						.Index = ArrayIndex});
					ElementPath.append(std::format("[fixed:{}]", ArrayIndex));
				}
				FAssetResult Result = ExtractSoftValue(
					Property, Reader, Context, Route, ElementPath,
					ContainerDepth + (bFixedArray ? 1 : 0));
				if (bFixedArray) Route.pop_back();
				if (!Result) return Result;
			}
			if (Reader.Offset != Payload.size())
				return Error(EAssetError::CorruptFile,
					std::format("SoftReferencePayloadTrailingBytes: {} has trailing bytes.", PropertyPath));
			return {};
		}

		auto ExtractSoftValue(
			FProperty* Property,
			FByteReader& Reader,
			const FSoftReferenceExtractionContext& Context,
			std::vector<FSoftAssetReferenceRouteSegment>& Route,
			const std::string& PropertyPath,
			uint32 ContainerDepth) -> FAssetResult
		{
			if (!Property)
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceSchemaMismatch: reflected property metadata is missing.");
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind))
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferencePayloadTruncated: {} has no reference tag.", PropertyPath));
				if (ReferenceKind == 0) return {};
				if (ReferenceKind != 1)
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferencePayloadTag: {} has unknown tag {}.", PropertyPath, ReferenceKind));
				std::string PathString;
				if (!Reader.ReadString(PathString, MaximumPackageStringBytes) || PathString.empty())
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferencePayloadPath: {} is truncated or overlong.", PropertyPath));
				FSoftObjectPath SoftPath;
				std::string PathError;
				if (!FSoftObjectPath::TryCreate(PathString, SoftPath, &PathError))
					return Error(EAssetError::InvalidPath, std::format(
						"SoftReferenceInvalidPath: {} contains '{}': {}",
						PropertyPath, PathString, PathError));
				auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
				DClass* ExpectedClass = SoftProperty->GetExpectedClass();
				if (!ExpectedClass)
					return Error(EAssetError::TypeMismatch,
						std::format("SoftReferenceSchemaMismatch: {} has no expected class.", PropertyPath));
				if (PropertyPath.size() > MaximumSoftReferenceDisplayPathBytes)
					return Error(EAssetError::CorruptFile,
						"SoftReferenceIndexDisplayPathExceeded: display path exceeds 4 KiB.");
				if (Context.References.size() >= MaximumSoftReferencesPerPackage)
					return Error(EAssetError::CorruptFile,
						"SoftReferenceIndexOccurrenceExceeded: package exceeds 100,000 occurrences.");
				Context.References.push_back({
					.SourcePackage = Context.SourcePackage,
					.SourceFingerprint = Context.Fingerprint,
					.SourceObjectId = Context.Object.Id,
					.SourceClass = Context.Object.ClassName,
					.DeclaringType = std::string(Context.DeclaringType),
					.FieldName = std::string(Context.FieldName),
					.ExpectedClass = ExpectedClass->GetQualifiedName().ToString(),
					.TargetPath = SoftPath.GetAssetPath(),
					.ContainerRoute = Route,
					.PropertyPath = PropertyPath});
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				if (ContainerDepth >= MaximumSoftReferenceContainerDepth)
					return Error(EAssetError::CorruptFile,
						"SoftReferenceIndexDepthExceeded: Array route exceeds four levels.");
				auto* Array = static_cast<FArrayProperty*>(Property);
				uint64 Count = 0;
				if (!Array->GetInner() || !Reader.Read(Count) || Count > 10000000)
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferenceArrayPayload: {} has an invalid count.", PropertyPath));
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					Route.push_back({
						.Kind = ESoftAssetReferenceRouteKind::ArrayElement,
						.Index = Index});
					FAssetResult Result = ExtractSoftValue(
						Array->GetInner(), Reader, Context, Route,
						std::format("{}[{}]", PropertyPath, Index), ContainerDepth + 1);
					Route.pop_back();
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				if (ContainerDepth >= MaximumSoftReferenceContainerDepth)
					return Error(EAssetError::CorruptFile,
						"SoftReferenceIndexDepthExceeded: Map route exceeds four levels.");
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Count = 0;
				if (!Map->GetKeyProp() || !Map->GetValueProp() || !Reader.Read(Count) || Count > 10000000)
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferenceMapPayload: {} has an invalid count.", PropertyPath));
				if (ContainsSoftObjectProperty(Map->GetKeyProp()))
					return Error(EAssetError::TypeMismatch,
						"SoftReferenceSchemaMismatch: soft Map keys are unsupported.");
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FReflectedValueStorage KeyStorage;
					std::string StorageError;
					if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError))
						return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
					FAssetResult KeyResult = DeserializeValue(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Reader, {});
					if (!KeyResult)
					{
						KeyResult.Message = std::format("SoftReferenceMapKey[{}]: {}", Index, KeyResult.Message);
						return KeyResult;
					}
					std::vector<uint8> KeyToken;
					if (!BuildCanonicalMapKeyToken(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, KeyToken, &StorageError))
						return Error(EAssetError::TypeMismatch, std::move(StorageError));
					if (KeyToken.size() > MaximumSoftReferenceRouteTokenBytes)
						return Error(EAssetError::CorruptFile,
							"SoftReferenceIndexRouteTokenExceeded: Map key token exceeds 1 MiB.");
					std::string ValuePath = PropertyPath;
					AppendMapTokenDisplay(ValuePath, KeyToken);
					if (ValuePath.size() > MaximumSoftReferenceDisplayPathBytes)
						return Error(EAssetError::CorruptFile,
							"SoftReferenceIndexDisplayPathExceeded: display path exceeds 4 KiB.");
					Route.push_back({
						.Kind = ESoftAssetReferenceRouteKind::MapValue,
						.MapKeyToken = std::move(KeyToken)});
					FAssetResult Result = ExtractSoftValue(
						Map->GetValueProp(), Reader, Context, Route, ValuePath, ContainerDepth + 1);
					Route.pop_back();
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				std::string StructName;
				uint64 FieldCount = 0;
				if (!Struct || !Reader.ReadString(StructName, MaximumPackageStringBytes)
					|| StructName != Struct->GetQualifiedName().ToString()
					|| !Reader.Read(FieldCount) || FieldCount > 100000)
					return Error(EAssetError::TypeMismatch,
						std::format("SoftReferenceStructPayload: {} has an incompatible header.", PropertyPath));
				for (uint64 Index = 0; Index < FieldCount; ++Index)
				{
					std::string DeclaringStruct;
					std::string FieldName;
					std::string Signature;
					uint8 Kind = 0;
					uint64 PayloadSize = 0;
					std::span<const uint8> FieldPayload;
					if (!Reader.ReadString(DeclaringStruct, MaximumPackageStringBytes)
						|| !Reader.ReadString(FieldName, MaximumPackageStringBytes)
						|| !Reader.Read(Kind)
						|| !Reader.ReadString(Signature, MaximumPackageStringBytes)
						|| !Reader.Read(PayloadSize) || PayloadSize > Reader.Bytes.size()
						|| !Reader.ReadSpan(static_cast<size_t>(PayloadSize), FieldPayload))
						return Error(EAssetError::CorruptFile,
							std::format("SoftReferenceStructPayload: {} has a malformed field.", PropertyPath));
					if (DeclaringStruct != StructName) continue;
					FProperty* Field = Struct->FindPropertyByName(FName(FieldName), false);
					if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
						|| !ContainsSoftObjectProperty(Field)) continue;
					if (static_cast<uint8>(Field->GetKind()) != Kind
						|| !IsSerializedTypeSignatureCompatible(Field, Signature))
						return Error(EAssetError::TypeMismatch, std::format(
							"SoftReferenceSchemaMismatch: {}.{} has an incompatible signature.",
							PropertyPath, FieldName));
					FAssetResult Result = ExtractSoftPropertyValues(
						Field, FieldPayload, Context, Route,
						std::format("{}.{}", PropertyPath, FieldName), ContainerDepth);
					if (!Result) return Result;
				}
				return {};
			}
			default:
				return Error(EAssetError::TypeMismatch, std::format(
					"SoftReferenceSchemaMismatch: {} does not contain a supported soft value.", PropertyPath));
			}
		}

		struct FLoadedSoftReferenceCollector
		{
			const FAssetPath& TargetPath;
			std::vector<FSoftObjectPtr*>& Values;
		};

		struct FLoadedSoftContainerVisitContext
		{
			FLoadedSoftReferenceCollector& Collector;
			FProperty* Inner = nullptr;
			uint32 ContainerDepth = 0;
			FAssetResult Result;
		};

		auto CollectLoadedSoftValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			FLoadedSoftReferenceCollector& Collector,
			uint32 ContainerDepth) -> FAssetResult;

		auto CollectLoadedSoftArrayElement(
			void* RawContext,
			uint64,
			void* Element) -> bool
		{
			auto& Context = *static_cast<FLoadedSoftContainerVisitContext*>(RawContext);
			Context.Result = CollectLoadedSoftValue(
				Context.Inner, Element, 0, Context.Collector,
				Context.ContainerDepth + 1);
			return Context.Result.Succeeded();
		}

		auto CollectLoadedSoftMapValue(
			void* RawContext,
			const void*,
			void* Value) -> bool
		{
			auto& Context = *static_cast<FLoadedSoftContainerVisitContext*>(RawContext);
			Context.Result = CollectLoadedSoftValue(
				Context.Inner, Value, 0, Context.Collector,
				Context.ContainerDepth + 1);
			return Context.Result.Succeeded();
		}

		auto CollectLoadedSoftValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			FLoadedSoftReferenceCollector& Collector,
			uint32 ContainerDepth) -> FAssetResult
		{
			if (!Property || !Container)
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceMoveSchemaMismatch: live property metadata is unavailable.");
			if (ContainerDepth > MaximumSoftReferenceContainerDepth)
				return Error(EAssetError::CorruptFile,
					"SoftReferenceMoveDepthExceeded: live value exceeds four container levels.");
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				auto* Value = static_cast<FSoftObjectProperty*>(Property)
					->GetSoftObjectPtr(Container, ArrayIndex);
				if (!Value)
					return Error(EAssetError::TypeMismatch,
						"SoftReferenceMoveSchemaMismatch: live soft value accessor is unavailable.");
				if (!Value->IsNull()
					&& Value->GetSoftObjectPath().GetAssetPath() == Collector.TargetPath)
					Collector.Values.push_back(Value);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct || !Struct->HasCompleteAuthoredFields())
					return Error(EAssetError::TypeMismatch,
						"SoftReferenceMoveSchemaMismatch: live struct metadata is incomplete.");
				void* StructValue = Property->GetValuePtr(Container, ArrayIndex);
				FAssetResult Result;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (!Result || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
						|| !ContainsSoftObjectProperty(Field)) return;
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim(); ++FieldIndex)
					{
						Result = CollectLoadedSoftValue(
							Field, StructValue, FieldIndex, Collector,
							ContainerDepth + (Field->GetArrayDim() > 1 ? 1 : 0));
						if (!Result) return;
					}
				}, false);
				return Result;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* Array = static_cast<FArrayProperty*>(Property);
				if (!Array->GetInner()
					|| !Array->HasCapability(EArrayOpsFlags::MutableTraversal))
					return Error(EAssetError::UnsupportedProperty,
						"SoftReferenceMoveArrayUnavailable: mutable traversal is required.");
				FLoadedSoftContainerVisitContext Context{
					Collector, Array->GetInner(), ContainerDepth};
				const EContainerOpResult VisitResult = Array->VisitMutableElements(
					Container, &CollectLoadedSoftArrayElement, &Context, ArrayIndex);
				if (!Context.Result) return Context.Result;
				if (VisitResult != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						"SoftReferenceMoveArrayFailed: mutable traversal failed.");
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				if (!Map->GetValueProp()
					|| !Map->HasCapability(EMapOpsFlags::MutableMappedTraversal))
					return Error(EAssetError::UnsupportedProperty,
						"SoftReferenceMoveMapUnavailable: mutable value traversal is required.");
				FLoadedSoftContainerVisitContext Context{
					Collector, Map->GetValueProp(), ContainerDepth};
				const EContainerOpResult VisitResult = Map->VisitMutableEntries(
					Container, &CollectLoadedSoftMapValue, &Context, ArrayIndex);
				if (!Context.Result) return Context.Result;
				if (VisitResult != EContainerOpResult::Success)
					return Error(EAssetError::UnsupportedProperty,
						"SoftReferenceMoveMapFailed: mutable value traversal failed.");
				return {};
			}
			default:
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceMoveSchemaMismatch: unsupported live soft container.");
			}
		}

		auto CollectLoadedPackageSoftReferences(
			DPackage* Package,
			const FAssetPath& TargetPath,
			std::vector<FSoftObjectPtr*>& OutValues) -> FAssetResult
		{
			if (!Package || !Package->GetAsset())
				return Error(EAssetError::InvalidObjectGraph,
					"SoftReferenceMoveInvalidPackage: loaded package has no asset.");
			std::vector<DObject*> Objects;
			GatherObjects(Package->GetAsset(), Objects);
			FLoadedSoftReferenceCollector Collector{TargetPath, OutValues};
			FAssetResult Result;
			for (DObject* Object : Objects)
			{
				Object->GetClass()->ForEachProperty([&](FProperty* Property) {
					if (!Result || !Property
						|| Property->HasAnyPropertyFlags(EPropertyFlags::Transient)
						|| !ContainsSoftObjectProperty(Property)) return;
					for (uint32 ArrayIndex = 0;
						ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
					{
						Result = CollectLoadedSoftValue(
							Property, Object, ArrayIndex, Collector,
							Property->GetArrayDim() > 1 ? 1 : 0);
						if (!Result) return;
					}
				}, true);
				if (!Result) return Result;
			}
			return {};
		}

		auto RewriteSerializedSoftValue(
			FProperty* Property,
			FByteReader& Reader,
			FByteWriter& Writer,
			const FAssetPath& OldPath,
			const FAssetPath& NewPath,
			uint64& RewriteCount,
			uint32 ContainerDepth) -> FAssetResult;

		auto RewriteSerializedSoftProperty(
			FProperty* Property,
			std::span<const uint8> Payload,
			const FAssetPath& OldPath,
			const FAssetPath& NewPath,
			std::vector<uint8>& OutPayload,
			uint64& RewriteCount,
			uint32 ContainerDepth = 0) -> FAssetResult
		{
			FByteReader Reader{Payload};
			FByteWriter Writer;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				FAssetResult Result = RewriteSerializedSoftValue(
					Property, Reader, Writer, OldPath, NewPath, RewriteCount,
					ContainerDepth + (Property->GetArrayDim() > 1 ? 1 : 0));
				if (!Result) return Result;
			}
			if (Reader.Offset != Payload.size())
				return Error(EAssetError::CorruptFile,
					"SoftReferenceMoveTrailingBytes: field payload has trailing bytes.");
			OutPayload = std::move(Writer.Bytes);
			return {};
		}

		auto RewriteSerializedSoftValue(
			FProperty* Property,
			FByteReader& Reader,
			FByteWriter& Writer,
			const FAssetPath& OldPath,
			const FAssetPath& NewPath,
			uint64& RewriteCount,
			uint32 ContainerDepth) -> FAssetResult
		{
			if (!Property || ContainerDepth > MaximumSoftReferenceContainerDepth)
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceMoveSchemaMismatch: serialized container metadata is invalid.");
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				uint8 Kind = 0;
				if (!Reader.Read(Kind))
					return Error(EAssetError::CorruptFile,
						"SoftReferenceMoveTruncated: missing reference tag.");
				Writer.Write(Kind);
				if (Kind == 0) return {};
				if (Kind != 1)
					return Error(EAssetError::CorruptFile,
						"SoftReferenceMoveTag: unknown reference tag.");
				std::string PathString;
				FSoftObjectPath Path;
				std::string PathError;
				if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
					|| PathString.empty())
					return Error(EAssetError::CorruptFile,
						"SoftReferenceMovePath: path is truncated or overlong.");
				if (!FSoftObjectPath::TryCreate(PathString, Path, &PathError))
					return Error(EAssetError::InvalidPath, std::move(PathError));
				if (Path.GetAssetPath() == OldPath)
				{
					Writer.WriteString(NewPath.GetView());
					++RewriteCount;
				}
				else Writer.WriteString(PathString);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* Array = static_cast<FArrayProperty*>(Property);
				uint64 Count = 0;
				if (!Array->GetInner() || !Reader.Read(Count) || Count > 10000000)
					return Error(EAssetError::CorruptFile,
						"SoftReferenceMoveArrayPayload: invalid count.");
				Writer.Write(Count);
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FAssetResult Result = RewriteSerializedSoftValue(
						Array->GetInner(), Reader, Writer, OldPath, NewPath,
						RewriteCount, ContainerDepth + 1);
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Count = 0;
				if (!Map->GetKeyProp() || !Map->GetValueProp()
					|| !Reader.Read(Count) || Count > 10000000
					|| ContainsSoftObjectProperty(Map->GetKeyProp()))
					return Error(EAssetError::CorruptFile,
						"SoftReferenceMoveMapPayload: invalid map schema or count.");
				Writer.Write(Count);
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FReflectedValueStorage KeyStorage;
					std::string StorageError;
					if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError))
						return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
					FAssetResult Result = DeserializeValue(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Reader, {});
					if (!Result) return Result;
					std::unordered_set<FAssetPath> Dependencies;
					Result = SerializeValue(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Writer, {}, Dependencies);
					if (!Result) return Result;
					Result = RewriteSerializedSoftValue(
						Map->GetValueProp(), Reader, Writer, OldPath, NewPath,
						RewriteCount, ContainerDepth + 1);
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				std::string StructName;
				uint64 FieldCount = 0;
				if (!Struct || !Reader.ReadString(StructName, MaximumPackageStringBytes)
					|| StructName != Struct->GetQualifiedName().ToString()
					|| !Reader.Read(FieldCount) || FieldCount > 100000)
					return Error(EAssetError::TypeMismatch,
						"SoftReferenceMoveStructPayload: incompatible header.");
				Writer.WriteString(StructName);
				Writer.Write(FieldCount);
				for (uint64 Index = 0; Index < FieldCount; ++Index)
				{
					std::string DeclaringStruct;
					std::string FieldName;
					std::string Signature;
					uint8 Kind = 0;
					uint64 PayloadSize = 0;
					std::span<const uint8> FieldPayload;
					if (!Reader.ReadString(DeclaringStruct, MaximumPackageStringBytes)
						|| !Reader.ReadString(FieldName, MaximumPackageStringBytes)
						|| !Reader.Read(Kind)
						|| !Reader.ReadString(Signature, MaximumPackageStringBytes)
						|| !Reader.Read(PayloadSize) || PayloadSize > Reader.Bytes.size()
						|| !Reader.ReadSpan(static_cast<size_t>(PayloadSize), FieldPayload))
						return Error(EAssetError::CorruptFile,
							"SoftReferenceMoveStructPayload: malformed field.");
					Writer.WriteString(DeclaringStruct);
					Writer.WriteString(FieldName);
					Writer.Write(Kind);
					Writer.WriteString(Signature);
					FProperty* Field = DeclaringStruct == StructName
						? Struct->FindPropertyByName(FName(FieldName), false) : nullptr;
					if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
						|| !ContainsSoftObjectProperty(Field))
					{
						Writer.Write(PayloadSize);
						Writer.WriteBytes(FieldPayload.data(), FieldPayload.size());
						continue;
					}
					if (static_cast<uint8>(Field->GetKind()) != Kind
						|| !IsSerializedTypeSignatureCompatible(Field, Signature))
						return Error(EAssetError::TypeMismatch,
							"SoftReferenceMoveSchemaMismatch: struct field signature changed.");
					std::vector<uint8> RewrittenPayload;
					FAssetResult Result = RewriteSerializedSoftProperty(
						Field, FieldPayload, OldPath, NewPath, RewrittenPayload,
						RewriteCount, ContainerDepth);
					if (!Result) return Result;
					Writer.Write(uint64(RewrittenPayload.size()));
					Writer.WriteBytes(RewrittenPayload.data(), RewrittenPayload.size());
				}
				return {};
			}
			default:
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceMoveSchemaMismatch: unsupported serialized soft container.");
			}
		}

		auto RewriteUnloadedSoftPackage(
			std::span<const uint8> Bytes,
			const FAssetPath& OldPath,
			const FAssetPath& NewPath,
			uint64 ExpectedRewriteCount,
			std::vector<uint8>& OutBytes) -> FAssetResult
		{
			FPackageFile File;
			FAssetResult Result = ReadPackageFile(Bytes, File, false);
			if (!Result) return Result;
			uint64 RewriteCount = 0;
			for (FObjectRecord& Object : File.Objects)
			{
				DClass* ObjectClass = FindClassByQualifiedName(FName(Object.ClassName));
				if (!ObjectClass)
					return Error(EAssetError::UnknownClass,
						"SoftReferenceMoveUnknownClass: source object class is unavailable.");
				for (FFieldRecord& Field : Object.Fields)
				{
					DClass* DeclaringClass = FindClassByQualifiedName(FName(Field.DeclaringClass));
					FProperty* Property = DeclaringClass && ObjectClass->IsChildOf(DeclaringClass)
						? DeclaringClass->FindPropertyByName(FName(Field.Name), false) : nullptr;
					if (!Property || !ContainsSoftObjectProperty(Property)) continue;
					if (Property->GetKind() != Field.Kind
						|| !IsSerializedTypeSignatureCompatible(Property, Field.TypeSignature))
						return Error(EAssetError::TypeMismatch,
							"SoftReferenceMoveSchemaMismatch: source field signature changed.");
					std::vector<uint8> RewrittenPayload;
					Result = RewriteSerializedSoftProperty(
						Property, Field.Payload, OldPath, NewPath,
						RewrittenPayload, RewriteCount);
					if (!Result) return Result;
					Field.Payload = std::move(RewrittenPayload);
				}
			}
			if (RewriteCount != ExpectedRewriteCount)
				return Error(EAssetError::InUse, std::format(
					"SoftReferenceMoveStaleIndex: expected {} occurrence(s), parsed {}.",
					ExpectedRewriteCount, RewriteCount));
			FByteWriter Writer;
			WritePackageFile(File, Writer);
			OutBytes = std::move(Writer.Bytes);
			return {};
		}

		constexpr uint64 MaximumRegistryEntries = 1000000;
		constexpr uint32 MaximumRegistryDependencies = 100000;

		struct FRegistryCacheEntry
		{
			std::string MountRoot;
			std::string RelativePath;
			std::string AssetClassName;
			EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
			FAssetPath RedirectDestination;
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
				uint8 EntryKind = 0;
				std::string RedirectDestination;
				uint32 DependencyCount = 0;
				if (!Reader.ReadString(Entry.MountRoot) || !Reader.ReadString(Entry.RelativePath)
					|| !Reader.ReadString(Entry.AssetClassName)
					|| !Reader.ReadU8(EntryKind)
					|| EntryKind > uint8(EAssetRegistryEntryKind::Redirector)
					|| !Reader.ReadString(RedirectDestination)
					|| !Reader.ReadU32(Entry.FormatVersion)
					|| !Reader.ReadU32(DependencyCount) || DependencyCount > MaximumRegistryDependencies)
				{
					OutWarning = "Ignoring corrupt asset registry cache entry.";
					OutEntries.clear();
					return false;
				}
				Entry.EntryKind = static_cast<EAssetRegistryEntryKind>(EntryKind);
				if (!RedirectDestination.empty()
					&& !FAssetPath::TryCreate(
						RedirectDestination, Entry.RedirectDestination))
				{
					OutWarning = "Ignoring invalid redirect destination in asset registry cache.";
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
					|| Entry.FormatVersion < MinimumAssetVersion
					|| Entry.FormatVersion > AssetVersion
					|| !std::ranges::binary_search(ExpectedMounts, Entry.MountRoot)
					|| std::filesystem::path(Entry.RelativePath).is_absolute()
					|| std::filesystem::path(Entry.RelativePath).extension() != ".dasset"
					|| Entry.RelativePath.starts_with("../") || Entry.RelativePath.find("/../") != std::string::npos)
				{
					OutWarning = "Ignoring invalid asset registry cache identity.";
					OutEntries.clear();
					return false;
				}
				FPackageFile HeaderFile{
					.FormatVersion = Entry.FormatVersion,
					.AssetClassName = Entry.AssetClassName,
					.EntryKind = Entry.EntryKind,
					.RedirectDestination = Entry.RedirectDestination,
					.Dependencies = Entry.Dependencies};
				const uint64 ObjectCount = 1;
				if (FAssetResult Result = ValidateRedirectorHeader(
					HeaderFile, ObjectCount); !Result)
				{
					OutWarning = "Ignoring corrupt redirect metadata in asset registry cache.";
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
				Writer.WriteU8(static_cast<uint8>(Entry.EntryKind));
				Writer.WriteString(Entry.RedirectDestination.GetView());
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
					.EntryKind = Data.EntryKind,
					.RedirectDestination = Data.RedirectDestination,
					.FormatVersion = Data.FormatVersion,
					.Dependencies = Data.Dependencies,
					.FileSize = Data.FileSize,
					.LastWriteTimeTicks = Data.LastWriteTimeTicks});
			}
			return true;
		}

		constexpr uint32 SoftReferenceIndexMagic = 0x58495253; // SRIX
		constexpr uint32 SoftReferenceIndexSchemaVersion = 1;
		constexpr uint32 SoftReferenceExtractorSchemaVersion = 1;
		constexpr uintmax_t MaximumSoftReferenceCacheBytes = 1024ull * 1024ull * 1024ull;

		struct FSoftReferenceCacheSource
		{
			FAssetPackageFingerprint Fingerprint;
			std::vector<FSoftAssetReference> References;
		};

		auto SoftReferenceCachePath() -> std::filesystem::path
		{
			return std::filesystem::path(FPaths::DerivedDataCacheDir())
				/ "AssetRegistry" / "SoftReferences.bin";
		}

		auto LoadSoftReferenceCache(
			std::unordered_map<FAssetPath, FSoftReferenceCacheSource>& OutSources,
			std::string& OutWarning) -> bool
		{
			OutSources.clear();
			const std::filesystem::path Path = SoftReferenceCachePath();
			std::error_code Ec;
			if (!std::filesystem::exists(Path, Ec)) return false;
			const uintmax_t Size = std::filesystem::file_size(Path, Ec);
			if (Ec || Size > MaximumSoftReferenceCacheBytes)
			{
				OutWarning = std::format("Ignoring invalid soft-reference cache {}.", Path.generic_string());
				return false;
			}
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutWarning = std::format("Failed to read soft-reference cache {}.", Path.generic_string());
				return false;
			}
			DerivedDataCache::FReader Reader(Bytes);
			uint32 ExtractorSchema = 0;
			uint64 SourceCount = 0;
			if (!Reader.ReadAndValidateHeader(
					SoftReferenceIndexMagic, SoftReferenceIndexSchemaVersion, AssetVersion)
				|| !Reader.ReadU32(ExtractorSchema)
				|| ExtractorSchema != SoftReferenceExtractorSchemaVersion
				|| !Reader.ReadU64(SourceCount) || SourceCount > MaximumRegistryEntries)
			{
				OutWarning = "Ignoring incompatible or corrupt soft-reference cache header.";
				return false;
			}
			uint64 TotalOccurrences = 0;
			for (uint64 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
			{
				std::string SourceString;
				FAssetPath SourcePath;
				FSoftReferenceCacheSource Source;
				uint64 FileSize = 0;
				uint64 OccurrenceCount = 0;
				if (!Reader.ReadString(SourceString, MaximumPackageStringBytes)
					|| !FAssetPath::TryCreate(SourceString, SourcePath)
					|| !Reader.ReadU64(FileSize)
					|| !Reader.ReadI64(Source.Fingerprint.LastWriteTimeTicks)
					|| !Reader.ReadU64(Source.Fingerprint.ContentHash.HashLow)
					|| !Reader.ReadU64(Source.Fingerprint.ContentHash.HashHigh)
					|| !Reader.ReadU64(OccurrenceCount)
					|| OccurrenceCount > MaximumSoftReferencesPerPackage
					|| TotalOccurrences > MaximumSoftReferencesPerSnapshot - OccurrenceCount)
				{
					OutWarning = "Ignoring corrupt soft-reference cache source record.";
					OutSources.clear();
					return false;
				}
				Source.Fingerprint.FileSize = static_cast<uintmax_t>(FileSize);
				TotalOccurrences += OccurrenceCount;
				Source.References.reserve(static_cast<size_t>(OccurrenceCount));
				for (uint64 OccurrenceIndex = 0; OccurrenceIndex < OccurrenceCount; ++OccurrenceIndex)
				{
					FSoftAssetReference Reference{
						.SourcePackage = SourcePath,
						.SourceFingerprint = Source.Fingerprint};
					std::string TargetString;
					uint32 RouteCount = 0;
					if (!Reader.ReadU64(Reference.SourceObjectId)
						|| !Reader.ReadString(Reference.SourceClass, MaximumPackageStringBytes)
						|| !Reader.ReadString(Reference.DeclaringType, MaximumPackageStringBytes)
						|| !Reader.ReadString(Reference.FieldName, MaximumPackageStringBytes)
						|| !Reader.ReadString(Reference.ExpectedClass, MaximumPackageStringBytes)
						|| !Reader.ReadString(TargetString, MaximumPackageStringBytes)
						|| !FAssetPath::TryCreate(TargetString, Reference.TargetPath)
						|| !Reader.ReadU32(RouteCount)
						|| RouteCount > MaximumSoftReferenceContainerDepth)
					{
						OutWarning = "Ignoring corrupt soft-reference cache occurrence.";
						OutSources.clear();
						return false;
					}
					Reference.ContainerRoute.reserve(RouteCount);
					for (uint32 RouteIndex = 0; RouteIndex < RouteCount; ++RouteIndex)
					{
						uint8 Kind = 0;
						uint64 TokenBytes = 0;
						FSoftAssetReferenceRouteSegment Segment;
						if (!Reader.ReadU8(Kind)
							|| Kind > static_cast<uint8>(ESoftAssetReferenceRouteKind::MapValue)
							|| !Reader.ReadU64(Segment.Index)
							|| !Reader.ReadU64(TokenBytes)
							|| !Reader.ReadBytes(
								Segment.MapKeyToken, TokenBytes, MaximumSoftReferenceRouteTokenBytes))
						{
							OutWarning = "Ignoring corrupt soft-reference cache route.";
							OutSources.clear();
							return false;
						}
						Segment.Kind = static_cast<ESoftAssetReferenceRouteKind>(Kind);
						if ((Segment.Kind == ESoftAssetReferenceRouteKind::MapValue)
							!= !Segment.MapKeyToken.empty())
						{
							OutWarning = "Ignoring inconsistent soft-reference cache route.";
							OutSources.clear();
							return false;
						}
						Reference.ContainerRoute.push_back(std::move(Segment));
					}
					if (!Reader.ReadString(
						Reference.PropertyPath, MaximumSoftReferenceDisplayPathBytes)
						|| Reference.PropertyPath.empty())
					{
						OutWarning = "Ignoring invalid soft-reference cache display path.";
						OutSources.clear();
						return false;
					}
					Source.References.push_back(std::move(Reference));
				}
				if (!OutSources.emplace(SourcePath, std::move(Source)).second)
				{
					OutWarning = "Ignoring duplicate soft-reference cache source.";
					OutSources.clear();
					return false;
				}
			}
			if (!Reader.IsAtEnd())
			{
				OutWarning = "Ignoring soft-reference cache with trailing data.";
				OutSources.clear();
				return false;
			}
			return true;
		}

		auto WriteSoftReferenceCache(
			const std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints,
			std::span<const FSoftAssetReference> References,
			std::string& OutWarning) -> bool
		{
			if (Fingerprints.size() > MaximumRegistryEntries
				|| References.size() > MaximumSoftReferencesPerSnapshot)
			{
				OutWarning = "Soft-reference index exceeds its persisted snapshot bounds.";
				return false;
			}
			std::unordered_map<FAssetPath, std::vector<const FSoftAssetReference*>> BySource;
			for (const FSoftAssetReference& Reference : References)
				BySource[Reference.SourcePackage].push_back(&Reference);
			std::vector<FAssetPath> Sources;
			Sources.reserve(Fingerprints.size());
			for (const auto& [Source, Fingerprint] : Fingerprints) Sources.push_back(Source);
			std::ranges::sort(Sources, [](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() < Right.GetView();
			});

			DerivedDataCache::FWriter Writer;
			Writer.WriteHeader({SoftReferenceIndexMagic, SoftReferenceIndexSchemaVersion, AssetVersion});
			Writer.WriteU32(SoftReferenceExtractorSchemaVersion);
			Writer.WriteU64(Sources.size());
			for (const FAssetPath& Source : Sources)
			{
				const FAssetPackageFingerprint& Fingerprint = Fingerprints.at(Source);
				const auto ReferencesIt = BySource.find(Source);
				const size_t ReferenceCount = ReferencesIt == BySource.end()
					? 0 : ReferencesIt->second.size();
				if (ReferenceCount > MaximumSoftReferencesPerPackage)
				{
					OutWarning = std::format(
						"Soft-reference source {} exceeds its occurrence bound.", Source.ToString());
					return false;
				}
				Writer.WriteString(Source.GetView());
				Writer.WriteU64(static_cast<uint64>(Fingerprint.FileSize));
				Writer.WriteI64(Fingerprint.LastWriteTimeTicks);
				Writer.WriteU64(Fingerprint.ContentHash.HashLow);
				Writer.WriteU64(Fingerprint.ContentHash.HashHigh);
				Writer.WriteU64(ReferenceCount);
				if (ReferencesIt == BySource.end()) continue;
				for (const FSoftAssetReference* Reference : ReferencesIt->second)
				{
					Writer.WriteU64(Reference->SourceObjectId);
					Writer.WriteString(Reference->SourceClass);
					Writer.WriteString(Reference->DeclaringType);
					Writer.WriteString(Reference->FieldName);
					Writer.WriteString(Reference->ExpectedClass);
					Writer.WriteString(Reference->TargetPath.GetView());
					Writer.WriteU32(static_cast<uint32>(Reference->ContainerRoute.size()));
					for (const FSoftAssetReferenceRouteSegment& Segment : Reference->ContainerRoute)
					{
						Writer.WriteU8(static_cast<uint8>(Segment.Kind));
						Writer.WriteU64(Segment.Index);
						Writer.WriteU64(Segment.MapKeyToken.size());
						Writer.WriteBytes(Segment.MapKeyToken);
					}
					Writer.WriteString(Reference->PropertyPath);
				}
			}
			std::string ErrorMessage;
			if (!DerivedDataCache::WriteFileAtomically(
				SoftReferenceCachePath(), Writer.GetBytes(), &ErrorMessage))
			{
				OutWarning = std::move(ErrorMessage);
				return false;
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
			File.FormatVersion = AssetVersion;
			File.AssetClassName = Package->GetAsset()->GetClass()->GetQualifiedName().ToString();
			if (auto* Redirector = Cast<DAssetRedirector>(Package->GetAsset()))
			{
				File.EntryKind = EAssetRegistryEntryKind::Redirector;
				DObject* DestinationObject = Redirector->GetDestinationObject();
				DPackage* DestinationPackage = DestinationObject
					? DestinationObject->GetPackage() : nullptr;
				if (!DestinationPackage || DestinationPackage->GetAsset() != DestinationObject
					|| !FAssetPath::TryCreate(
						DestinationPackage->GetPackagePath(), File.RedirectDestination))
					return CorruptRedirector(
						"DestinationObject must be a non-null package main asset.");
			}
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
			FAssetPath PackagePath;
			if (!FAssetPath::TryCreate(Package->GetPackagePath(), PackagePath))
				return Error(EAssetError::InvalidPath, "Package has an invalid asset path.");
			FAssetResult ValidationResult = ValidateRedirectorHeader(
				File, File.Objects.size(), &PackagePath);
			if (!ValidationResult) return ValidationResult;
			ValidationResult = ValidateRedirectorBody(File);
			if (!ValidationResult) return ValidationResult;

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
		OutHeader.EntryKind = File.EntryKind;
		OutHeader.RedirectDestination = std::move(File.RedirectDestination);
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
				.EntryKind = Staged.File.EntryKind,
				.RedirectDestination = Staged.File.RedirectDestination,
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
			&RootProperty, OutValue, 0, Reader, {}, nullptr, {},
			SourceFormatVersion == 0 ? AssetVersion : SourceFormatVersion)
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
		OutInspection.Header.EntryKind = HeaderFile.EntryKind;
		OutInspection.Header.RedirectDestination =
			std::move(HeaderFile.RedirectDestination);
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
					.Payload = std::move(Field.Payload),
					.SourceFormatVersion = File.FormatVersion});
			}
			OutInspection.Objects.push_back(std::move(Object));
		}
		return {};
	}

	auto ExtractSoftAssetReferences(
		const FAssetPath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FSoftAssetReference>& OutReferences) -> FAssetResult
	{
		OutReferences.clear();
		if (!SourcePackage.IsValid())
			return Error(EAssetError::InvalidPath,
				"SoftReferenceIndexInvalidSource: source package path is invalid.");
		if (Inspection.Objects.empty())
			return Error(EAssetError::InvalidObjectGraph,
				"SoftReferenceIndexInvalidPackage: package has no main object.");
		if (Inspection.Header.AssetClassName != Inspection.Objects.front().ClassName)
			return Error(EAssetError::TypeMismatch,
				"SoftReferenceIndexRuntimeTypeMismatch: header and main-object classes differ.");

		std::vector<FSoftAssetReference> References;
		for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
		{
			DClass* ObjectClass = FindClassByQualifiedName(FName(Object.ClassName));
			if (!ObjectClass)
				return Error(EAssetError::UnknownClass, std::format(
					"SoftReferenceIndexUnknownClass: {} is unavailable.", Object.ClassName));
			for (const FAssetPackageField& Field : Object.Fields)
			{
				DClass* DeclaringClass = FindClassByQualifiedName(FName(Field.DeclaringClass));
				FProperty* Property = DeclaringClass && ObjectClass->IsChildOf(DeclaringClass)
					? DeclaringClass->FindPropertyByName(FName(Field.Name), false)
					: nullptr;
				if (!Property)
				{
					// Unknown fields remain compatibility payloads. Without current reflection
					// metadata they cannot safely contribute a typed derived record.
					continue;
				}
				const bool bCurrentContainsSoftObject = ContainsSoftObjectProperty(Property);
				const bool bStoredContainsSoftObject = Field.TypeSignature.find("SoftObject:") != std::string::npos;
				if (Property->GetKind() != Field.Kind
					|| !IsSerializedTypeSignatureCompatible(Property, Field.TypeSignature))
				{
					if (bCurrentContainsSoftObject || bStoredContainsSoftObject)
						return Error(EAssetError::TypeMismatch, std::format(
							"SoftReferenceSchemaMismatch: {}::{} has incompatible kind or signature.",
							Field.DeclaringClass, Field.Name));
					continue;
				}
				if (!bCurrentContainsSoftObject) continue;
				FSoftReferenceExtractionContext Context{
					.SourcePackage = SourcePackage,
					.Fingerprint = Inspection.Fingerprint,
					.Object = Object,
					.DeclaringType = Field.DeclaringClass,
					.FieldName = Field.Name,
					.References = References};
				std::vector<FSoftAssetReferenceRouteSegment> Route;
				FAssetResult Result = ExtractSoftPropertyValues(
					Property, Field.Payload, Context, Route, Field.Name, 0);
				if (!Result) return Result;
			}
		}
		std::ranges::sort(References, &SoftReferenceLess);
		OutReferences = std::move(References);
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

	auto RegisterAssetMoveExternalStore(FAssetMoveExternalStore Store)
		-> FAssetMoveExternalStoreHandle
	{
		if (!Store) return 0;
		auto& NextHandle = NextMoveExternalStoreHandle();
		const FAssetMoveExternalStoreHandle Handle = NextHandle++;
		GetMoveExternalStores().emplace(Handle, std::move(Store));
		return Handle;
	}

	auto UnregisterAssetMoveExternalStore(
		FAssetMoveExternalStoreHandle Handle) -> void
	{
		if (Handle != 0) GetMoveExternalStores().erase(Handle);
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
		std::unordered_map<FAssetPath, FSoftReferenceCacheSource> CachedSoftSources;
		std::unordered_set<std::string> SeenCachedIdentities;
		ScanErrors.clear();
		LastScanStats = {};
		CacheWarning.clear();
		SoftReferenceErrors.clear();
		SoftReferenceIndexStats = {};
		SoftReferenceCacheWarning.clear();
		const std::vector<std::string> MountManifest = GetMountManifest();
		const bool bCacheLoaded = LoadRegistryCache(MountManifest, CachedEntries, CacheWarning);
		const bool bSoftCacheLoaded = LoadSoftReferenceCache(
			CachedSoftSources, SoftReferenceCacheWarning);
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
				EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
				FAssetPath RedirectDestination;
				uint32 FormatVersion = 0;
				std::vector<FAssetPath> Dependencies;
				const auto CachedIt = CachedEntries.find(Identity);
				if (Mode == EAssetRegistryScanMode::Incremental && CachedIt != CachedEntries.end()
					&& CachedIt->second.FileSize == FileSize && CachedIt->second.LastWriteTimeTicks == LastWriteTimeTicks)
				{
					AssetClassName = CachedIt->second.AssetClassName;
					EntryKind = CachedIt->second.EntryKind;
					RedirectDestination = CachedIt->second.RedirectDestination;
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
					EntryKind = PackageHeader.EntryKind;
					RedirectDestination = std::move(PackageHeader.RedirectDestination);
					FormatVersion = PackageHeader.FormatVersion;
					Dependencies = std::move(PackageHeader.Dependencies);
					++LastScanStats.Reparsed;
				}
				if (EntryKind == EAssetRegistryEntryKind::Redirector)
				{
					FPackageFile RedirectHeader{
						.FormatVersion = FormatVersion,
						.AssetClassName = AssetClassName,
						.EntryKind = EntryKind,
						.RedirectDestination = RedirectDestination,
						.Dependencies = Dependencies};
					FAssetResult RedirectResult = ValidateRedirectorHeader(
						RedirectHeader, 1, &DiskPath);
					if (!RedirectResult)
					{
						RedirectResult.Message = std::format(
							"{} ({})", RedirectResult.Message, It->path().generic_string());
						ScanErrors.push_back(std::move(RedirectResult));
						++LastScanStats.Failed;
						continue;
					}
					if (Mode == EAssetRegistryScanMode::FullValidation)
					{
						FAssetPackageInspection Inspection;
						RedirectResult = InspectAssetPackage(
							It->path().generic_string(), Inspection);
						if (!RedirectResult)
						{
							RedirectResult.Message = std::format(
								"{} ({})", RedirectResult.Message,
								It->path().generic_string());
							ScanErrors.push_back(std::move(RedirectResult));
							++LastScanStats.Failed;
							continue;
						}
					}
					++LastScanStats.Redirectors;
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
					.EntryKind = EntryKind,
					.RedirectDestination = RedirectDestination,
					.FormatVersion = FormatVersion,
					.Dependencies = Dependencies,
					.FileSize = FileSize,
					.LastWriteTimeTicks = LastWriteTimeTicks});
				NewAssets.emplace(DiskPath, FAssetData{
					.PackagePath = DiskPath,
					.PhysicalPath = It->path().generic_string(),
					.AssetClassName = std::move(AssetClassName),
					.EntryKind = EntryKind,
					.RedirectDestination = std::move(RedirectDestination),
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

		std::vector<const FAssetData*> SortedAssets;
		SortedAssets.reserve(NewAssets.size());
		for (const auto& [Path, Data] : NewAssets) SortedAssets.push_back(&Data);
		std::ranges::sort(SortedAssets, [](const FAssetData* Left, const FAssetData* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		std::vector<FSoftAssetReference> NewSoftReferences;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> NewSoftFingerprints;
		for (const FAssetData* Data : SortedAssets)
		{
			std::vector<uint8> Bytes;
			FAssetPackageFingerprint Fingerprint;
			if (!FFileHelper::LoadFileToArray(Bytes, Data->PhysicalPath))
			{
				SoftReferenceErrors.push_back(Error(EAssetError::IoError, std::format(
					"SoftReferenceIndexReadFailed: could not read {}.", Data->PhysicalPath)));
				++SoftReferenceIndexStats.FailedSources;
				continue;
			}
			FAssetResult FingerprintResult = MakePackageFingerprint(
				Data->PhysicalPath, Bytes, Fingerprint);
			if (!FingerprintResult)
			{
				SoftReferenceErrors.push_back(std::move(FingerprintResult));
				++SoftReferenceIndexStats.FailedSources;
				continue;
			}
			const auto CachedIt = CachedSoftSources.find(Data->PackagePath);
			if (Mode == EAssetRegistryScanMode::Incremental && bSoftCacheLoaded
				&& CachedIt != CachedSoftSources.end()
				&& CachedIt->second.Fingerprint == Fingerprint)
			{
				if (NewSoftReferences.size() > MaximumSoftReferencesPerSnapshot
					- CachedIt->second.References.size())
				{
					SoftReferenceErrors.push_back(Error(EAssetError::CorruptFile,
						"SoftReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
					++SoftReferenceIndexStats.FailedSources;
					continue;
				}
				NewSoftReferences.insert(
					NewSoftReferences.end(),
					CachedIt->second.References.begin(), CachedIt->second.References.end());
				NewSoftFingerprints.emplace(Data->PackagePath, Fingerprint);
				++SoftReferenceIndexStats.ReusedSources;
				continue;
			}

			FAssetPackageInspection Inspection;
			FAssetResult InspectionResult = InspectAssetPackage(Data->PhysicalPath, Inspection);
			std::vector<FSoftAssetReference> SourceReferences;
			if (InspectionResult)
				InspectionResult = ExtractSoftAssetReferences(
					Data->PackagePath, Inspection, SourceReferences);
			++SoftReferenceIndexStats.ExtractedSources;
			if (!InspectionResult)
			{
				InspectionResult.Message = std::format(
					"{} ({})", InspectionResult.Message, Data->PhysicalPath);
				SoftReferenceErrors.push_back(std::move(InspectionResult));
				++SoftReferenceIndexStats.FailedSources;
				continue;
			}
			if (NewSoftReferences.size() > MaximumSoftReferencesPerSnapshot
				- SourceReferences.size())
			{
				SoftReferenceErrors.push_back(Error(EAssetError::CorruptFile,
					"SoftReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
				++SoftReferenceIndexStats.FailedSources;
				continue;
			}
			NewSoftReferences.insert(
				NewSoftReferences.end(),
				std::make_move_iterator(SourceReferences.begin()),
				std::make_move_iterator(SourceReferences.end()));
			NewSoftFingerprints.emplace(Data->PackagePath, Inspection.Fingerprint);
		}
		std::ranges::sort(NewSoftReferences, &SoftReferenceLess);

		const bool bAssetsChanged = Assets != NewAssets;
		const bool bSoftReferencesChanged = SoftReferences != NewSoftReferences
			|| SoftReferenceSourceFingerprints != NewSoftFingerprints;
		if (bAssetsChanged)
		{
			Assets = std::move(NewAssets);
			RebuildRedirectorIndex();
		}
		if (bSoftReferencesChanged)
		{
			SoftReferences = std::move(NewSoftReferences);
			SoftReferenceSourceFingerprints = std::move(NewSoftFingerprints);
		}
		if (bAssetsChanged || bSoftReferencesChanged) ++Revision;
		bPersistentSnapshotDirty = !WriteRegistryCache(MountManifest, std::move(NewCacheEntries), CacheWarning);
		std::string SoftWriteWarning;
		bSoftReferenceSnapshotDirty = !WriteSoftReferenceCache(
			SoftReferenceSourceFingerprints, SoftReferences, SoftWriteWarning);
		if (!SoftWriteWarning.empty())
		{
			if (!SoftReferenceCacheWarning.empty()) SoftReferenceCacheWarning.append(" ");
			SoftReferenceCacheWarning.append(SoftWriteWarning);
		}
		LastScanStats.DurationMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - ScanStartTime).count();
		DURIN_INFO_CATEGORY("AssetRegistry",
			"Scanned {} asset package(s) in {:.3f} ms: {} redirector(s), {} reused, {} reparsed, {} header read(s), {} header byte(s), {} removed, {} failed.",
			LastScanStats.Enumerated, LastScanStats.DurationMilliseconds, LastScanStats.Redirectors,
			LastScanStats.Reused, LastScanStats.Reparsed,
			LastScanStats.HeaderReadAttempts, LastScanStats.HeaderBytesRead, LastScanStats.Removed, LastScanStats.Failed);
		if (!CacheWarning.empty()) DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
		if (!SoftReferenceCacheWarning.empty())
			DURIN_WARN_CATEGORY("AssetRegistry", "{}", SoftReferenceCacheWarning);
		for (const FAssetResult& SoftError : SoftReferenceErrors)
			DURIN_WARN_CATEGORY("AssetRegistry", "{}", SoftError.Message);
		return {};
	}

	auto FAssetRegistry::FlushPersistentSnapshot() -> void
	{
		if (bPersistentSnapshotDirty)
		{
			std::vector<FRegistryCacheEntry> Entries;
			std::string Warning;
			if (BuildRegistryCacheEntries(Assets, Entries, Warning)
				&& WriteRegistryCache(GetMountManifest(), std::move(Entries), Warning))
			{
				bPersistentSnapshotDirty = false;
				CacheWarning.clear();
			}
			else
			{
				CacheWarning = std::move(Warning);
				if (!CacheWarning.empty()) DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
			}
		}
		if (bSoftReferenceSnapshotDirty)
		{
			std::string Warning;
			if (WriteSoftReferenceCache(
				SoftReferenceSourceFingerprints, SoftReferences, Warning))
			{
				bSoftReferenceSnapshotDirty = false;
				SoftReferenceCacheWarning.clear();
			}
			else
			{
				SoftReferenceCacheWarning = std::move(Warning);
				if (!SoftReferenceCacheWarning.empty())
					DURIN_WARN_CATEGORY("AssetRegistry", "{}", SoftReferenceCacheWarning);
			}
		}
	}

	auto FAssetRegistry::FindAssetExact(const FAssetPath& Path) const -> const FAssetData*
	{
		auto It = Assets.find(Path);
		return It == Assets.end() ? nullptr : &It->second;
	}

	auto FAssetRegistry::FindAsset(const FAssetPath& Path) const -> const FAssetData*
	{
		return FindAssetExact(Path);
	}

	auto FAssetRegistry::ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		FAssetPathResolveResult Result;
		Result.RequestedPath = Path;
		FAssetPath Current = Path;
		std::unordered_set<FAssetPath> Visited;
		while (true)
		{
			const FAssetData* Data = FindAssetExact(Current);
			if (!Data)
			{
				Result.FinalPath = Current;
				Result.State = Result.RedirectChain.empty()
					? EAssetPathResolveState::NotFound
					: EAssetPathResolveState::MissingRedirectTarget;
				return Result;
			}
			if (Data->EntryKind == EAssetRegistryEntryKind::Asset)
			{
				if (Data->RedirectDestination.IsValid()
					|| Data->AssetClassName == RedirectorClassName)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::CorruptRedirector;
					return Result;
				}
				DClass* TargetClass = FindClassByQualifiedName(FName(Data->AssetClassName));
				if (!TargetClass)
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::UnknownTargetClass;
					return Result;
				}
				if (Options.ExpectedClass && !TargetClass->IsChildOf(Options.ExpectedClass))
				{
					Result.FinalPath = Current;
					Result.State = EAssetPathResolveState::RedirectTypeMismatch;
					return Result;
				}
				Result.FinalPath = Current;
				Result.FinalAssetData = *Data;
				Result.State = EAssetPathResolveState::Resolved;
				return Result;
			}
			if (Data->EntryKind != EAssetRegistryEntryKind::Redirector
				|| Data->AssetClassName != RedirectorClassName
				|| !Data->RedirectDestination.IsValid()
				|| Data->RedirectDestination == Current
				|| Data->Dependencies.size() != 1
				|| Data->Dependencies.front() != Data->RedirectDestination)
			{
				Result.FinalPath = Current;
				Result.State = EAssetPathResolveState::CorruptRedirector;
				return Result;
			}
			if (!Visited.insert(Current).second)
			{
				Result.FinalPath = Current;
				Result.State = EAssetPathResolveState::RedirectCycle;
				return Result;
			}
			if (Result.RedirectChain.size() == MaximumRedirectDepth)
			{
				Result.FinalPath = Current;
				Result.State = EAssetPathResolveState::RedirectDepthExceeded;
				return Result;
			}
			Result.RedirectChain.push_back(Current);
			Current = Data->RedirectDestination;
		}
	}

	auto FAssetRegistry::FindRedirectorsTo(
		const FAssetPath& Destination) const -> std::vector<FAssetPath>
	{
		const auto It = RedirectorsByDestination.find(Destination);
		return It == RedirectorsByDestination.end()
			? std::vector<FAssetPath>{} : It->second;
	}

	auto FAssetRegistry::RebuildRedirectorIndex() -> void
	{
		RedirectorsByDestination.clear();
		for (const auto& [Path, Data] : Assets)
		{
			if (Data.EntryKind == EAssetRegistryEntryKind::Redirector
				&& Data.RedirectDestination.IsValid())
				RedirectorsByDestination[Data.RedirectDestination].push_back(Path);
		}
		for (auto& [Destination, Redirectors] : RedirectorsByDestination)
		{
			std::ranges::sort(Redirectors, [](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() < Right.GetView();
			});
		}
	}

	auto FAssetRegistry::FindSoftReferencers(
		const FAssetPath& Target) const -> std::vector<FSoftAssetReference>
	{
		std::vector<FSoftAssetReference> Result;
		for (const FSoftAssetReference& Reference : SoftReferences)
			if (Reference.TargetPath == Target) Result.push_back(Reference);
		return Result;
	}

	auto FAssetRegistry::FindSoftTargets(const FAssetPath& Source) const -> std::vector<FAssetPath>
	{
		std::vector<FAssetPath> Result;
		for (const FSoftAssetReference& Reference : SoftReferences)
			if (Reference.SourcePackage == Source) Result.push_back(Reference.TargetPath);
		std::ranges::sort(Result, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
		return Result;
	}

	auto FAssetRegistry::BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) const -> FAssetResult
	{
		OutPackages.clear();
		std::vector<FAssetPath> Pending(Roots.begin(), Roots.end());
		std::unordered_set<FAssetPath> Visited;
		while (!Pending.empty())
		{
			std::ranges::sort(Pending, [](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() > Right.GetView();
			});
			FAssetPath Source = std::move(Pending.back());
			Pending.pop_back();
			if (!Visited.insert(Source).second) continue;
			const FAssetData* SourceData = FindAsset(Source);
			if (!SourceData)
				return Error(EAssetError::MissingDependency, std::format(
					"CookReachabilityMissingPackage: {} is not registered.", Source.ToString()));
			if (!SoftReferenceSourceFingerprints.contains(Source))
				return Error(EAssetError::StaleData, std::format(
					"CookReachabilityIncompleteSoftIndex: {} has no current source entry.", Source.ToString()));
			for (const FAssetPath& Dependency : SourceData->Dependencies)
			{
				if (!FindAsset(Dependency))
					return Error(EAssetError::MissingDependency, std::format(
						"CookReachabilityMissingHardDependency: {} references missing {}.",
						Source.ToString(), Dependency.ToString()));
				Pending.push_back(Dependency);
			}
			for (const FSoftAssetReference& Reference : SoftReferences)
			{
				if (Reference.SourcePackage != Source) continue;
				const FAssetData* TargetData = FindAsset(Reference.TargetPath);
				if (!TargetData)
					return Error(EAssetError::MissingDependency, std::format(
						"CookReachabilityMissingSoftTarget: {} references missing {} at {}.",
						Source.ToString(), Reference.TargetPath.ToString(), Reference.PropertyPath));
				DClass* ExpectedClass = FindClassByQualifiedName(FName(Reference.ExpectedClass));
				DClass* RegisteredClass = FindClassByQualifiedName(FName(TargetData->AssetClassName));
				if (!ExpectedClass || !RegisteredClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownSoftClass: {} expects {} and target {} is registered as {}.",
						Reference.PropertyPath, Reference.ExpectedClass,
						Reference.TargetPath.ToString(), TargetData->AssetClassName));
				if (!RegisteredClass->IsChildOf(ExpectedClass))
					return Error(EAssetError::TypeMismatch, std::format(
						"CookReachabilitySoftTypeMismatch: {} expects {}, but {} is {}.",
						Reference.PropertyPath, Reference.ExpectedClass,
						Reference.TargetPath.ToString(), TargetData->AssetClassName));
				Pending.push_back(Reference.TargetPath);
			}
		}
		OutPackages.assign(Visited.begin(), Visited.end());
		std::ranges::sort(OutPackages, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return {};
	}

	auto FAssetRegistry::RemoveSoftReferencesFromSource(const FAssetPath& Path) -> bool
	{
		const size_t PreviousCount = SoftReferences.size();
		std::erase_if(SoftReferences, [&](const FSoftAssetReference& Reference) {
			return Reference.SourcePackage == Path;
		});
		const bool bChanged = PreviousCount != SoftReferences.size()
			|| SoftReferenceSourceFingerprints.erase(Path) != 0;
		if (bChanged) bSoftReferenceSnapshotDirty = true;
		return bChanged;
	}

	auto FAssetRegistry::RefreshSoftReferencesForAsset(const FAssetData& Data) -> bool
	{
		const std::vector<FSoftAssetReference> PreviousReferences = SoftReferences;
		const auto PreviousFingerprints = SoftReferenceSourceFingerprints;
		RemoveSoftReferencesFromSource(Data.PackagePath);
		SoftReferenceErrors.clear();
		FAssetPackageInspection Inspection;
		FAssetResult Result = InspectAssetPackage(Data.PhysicalPath, Inspection);
		std::vector<FSoftAssetReference> SourceReferences;
		if (Result)
			Result = ExtractSoftAssetReferences(Data.PackagePath, Inspection, SourceReferences);
		if (!Result)
		{
			Result.Message = std::format("{} ({})", Result.Message, Data.PhysicalPath);
			SoftReferenceErrors.push_back(std::move(Result));
			bSoftReferenceSnapshotDirty = true;
			return PreviousReferences != SoftReferences
				|| PreviousFingerprints != SoftReferenceSourceFingerprints;
		}
		if (SoftReferences.size() > MaximumSoftReferencesPerSnapshot - SourceReferences.size())
		{
			SoftReferenceErrors.push_back(Error(EAssetError::CorruptFile,
				"SoftReferenceIndexSnapshotExceeded: mutation exceeds 1,000,000 occurrences."));
			bSoftReferenceSnapshotDirty = true;
			return PreviousReferences != SoftReferences
				|| PreviousFingerprints != SoftReferenceSourceFingerprints;
		}
		SoftReferences.insert(
			SoftReferences.end(),
			std::make_move_iterator(SourceReferences.begin()),
			std::make_move_iterator(SourceReferences.end()));
		std::ranges::sort(SoftReferences, &SoftReferenceLess);
		SoftReferenceSourceFingerprints.insert_or_assign(
			Data.PackagePath, Inspection.Fingerprint);
		const bool bChanged = PreviousReferences != SoftReferences
			|| PreviousFingerprints != SoftReferenceSourceFingerprints;
		if (bChanged) bSoftReferenceSnapshotDirty = true;
		return bChanged;
	}

	auto FAssetRegistry::AddOrUpdate(FAssetData Data) -> void
	{
		const FAssetPath Path = Data.PackagePath;
		const auto Existing = Assets.find(Path);
		const bool bAssetChanged = Existing == Assets.end() || Existing->second != Data;
		Assets.insert_or_assign(Path, std::move(Data));
		if (bAssetChanged) RebuildRedirectorIndex();
		bPersistentSnapshotDirty = true;
		const FAssetData* Stored = FindAsset(Path);
		const bool bSoftChanged = Stored && RefreshSoftReferencesForAsset(*Stored);
		if (bAssetChanged || bSoftChanged) ++Revision;
	}

	auto FAssetRegistry::Remove(const FAssetPath& Path) -> void
	{
		const bool bAssetChanged = Assets.erase(Path) != 0;
		if (bAssetChanged) RebuildRedirectorIndex();
		const bool bSoftChanged = RemoveSoftReferencesFromSource(Path);
		if (!bAssetChanged && !bSoftChanged) return;
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

	auto FAssetManager::CreateRedirector(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		OutRedirector = nullptr;
		if (!RedirectorPath.IsValid() || !DestinationPath.IsValid()
			|| RedirectorPath == DestinationPath)
			return Error(EAssetError::InvalidPath,
				"Redirector source and destination paths must be valid and distinct.");
		const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(DestinationPath);
		if (!Resolution)
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::NotFound:
			case EAssetPathResolveState::MissingRedirectTarget:
				return Error(EAssetError::NotFound,
					"Redirector destination does not resolve to a registered asset.");
			case EAssetPathResolveState::RedirectCycle:
			case EAssetPathResolveState::RedirectDepthExceeded:
				return Error(EAssetError::CircularDependency,
					"Redirector destination does not have a finite canonical target.");
			case EAssetPathResolveState::UnknownTargetClass:
				return Error(EAssetError::UnknownClass,
					"Redirector destination has an unavailable reflected class.");
			case EAssetPathResolveState::RedirectTypeMismatch:
				return Error(EAssetError::TypeMismatch,
					"Redirector destination has an incompatible asset class.");
			case EAssetPathResolveState::CorruptRedirector:
				return CorruptRedirector(
					"the requested destination traverses corrupt redirect metadata.");
			case EAssetPathResolveState::Resolved:
				break;
			}
		}
		DObject* DestinationObject = nullptr;
		FAssetResult Result = LoadAsset(Resolution.FinalPath, DestinationObject);
		if (!Result) return Result;
		DObject* CreatedObject = nullptr;
		Result = CreateAsset(
			RedirectorPath,
			DAssetRedirector::StaticClass(),
			sizeof(DAssetRedirector),
			CreatedObject);
		if (!Result) return Result;
		OutRedirector = Cast<DAssetRedirector>(CreatedObject);
		if (!OutRedirector)
			return Error(EAssetError::InvalidObjectGraph,
				"Failed to construct the redirector main asset.");
		OutRedirector->SetDestinationObject(DestinationObject);
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
			.EntryKind = File.EntryKind,
			.RedirectDestination = File.RedirectDestination,
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
		if (!OldPath.IsValid() || !NewPath.IsValid() || OldPath == NewPath)
			return Error(EAssetError::InvalidPath, "Asset move paths are invalid or identical.");
		const FAssetData* SourceData = Registry.FindAsset(OldPath);
		if (!SourceData)
			return Error(EAssetError::NotFound,
				std::format("Asset {} was not found.", OldPath.ToString()));
		const std::filesystem::path OldFile(GetPhysicalPath(OldPath));
		const std::filesystem::path NewFile(GetPhysicalPath(NewPath));
		if (Registry.FindAsset(NewPath) || LoadedPackages.contains(NewPath)
			|| std::filesystem::exists(NewFile))
			return Error(EAssetError::AlreadyExists,
				std::format("Asset {} already exists.", NewPath.ToString()));
		if (!Registry.GetSoftReferenceErrors().empty())
			return Error(EAssetError::InUse,
				"SoftReferenceMoveIndexIncomplete: repair requires a complete soft-reference index.");

		std::vector<FAssetPath> ReferrerPaths;
		for (const auto& [Path, Data] : Registry.GetAssets())
			if (Path != OldPath
				&& std::ranges::find(Data.Dependencies, OldPath) != Data.Dependencies.end())
				ReferrerPaths.push_back(Path);

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
			if (!std::filesystem::is_regular_file(From))
				return Error(EAssetError::NotFound,
					std::format("Companion file {} was not found.", From.generic_string()));
			if (std::filesystem::exists(To))
				return Error(EAssetError::AlreadyExists,
					std::format("Companion destination {} already exists.", To.generic_string()));
		}
		std::vector<DPackage*> AdditionalPackages;
		std::unordered_set<DPackage*> SeenAdditional;
		for (DPackage* Package : Contribution.AdditionalPackages)
		{
			if (!Package || Package == MovingPackage || !SeenAdditional.insert(Package).second
				|| std::ranges::find(Referrers, Package) != Referrers.end()) continue;
			AdditionalPackages.push_back(Package);
		}

		std::vector<FAssetMoveExternalStoreAction> ExternalActions;
		for (const auto& [Handle, Store] : GetMoveExternalStores())
		{
			(void)Handle;
			FAssetMoveExternalStoreAction Action;
			Result = Store(OldPath, NewPath, Action);
			if (!Result) return Result;
			if (!Action.Apply && !Action.Rollback) continue;
			if (!Action.Apply || !Action.Rollback)
				return Error(EAssetError::UnsupportedProperty,
					"Asset move external stores must provide both Apply and Rollback actions.");
			ExternalActions.push_back(std::move(Action));
		}

		struct FUnloadedSoftRewrite
		{
			FAssetData Data;
			std::filesystem::path File;
			std::vector<uint8> Bytes;
		};
		std::unordered_map<FAssetPath, std::vector<FSoftAssetReference>> IndexedBySource;
		for (FSoftAssetReference Reference : Registry.FindSoftReferencers(OldPath))
			IndexedBySource[Reference.SourcePackage].push_back(std::move(Reference));
		std::vector<FUnloadedSoftRewrite> UnloadedSoftRewrites;
		for (const auto& [SourcePath, References] : IndexedBySource)
		{
			if (LoadedPackages.contains(SourcePath)) continue;
			const FAssetData* Data = Registry.FindAsset(SourcePath);
			if (!Data)
				return Error(EAssetError::InUse,
					"SoftReferenceMoveStaleIndex: an indexed source is no longer registered.");
			for (const FSoftAssetReference& Reference : References)
				if (Reference.SourceFingerprint != References.front().SourceFingerprint)
					return Error(EAssetError::InUse,
						"SoftReferenceMoveStaleIndex: source occurrences disagree on their fingerprint.");

			const std::filesystem::path SourceFile(Data->PhysicalPath);
			std::error_code PermissionError;
			const std::filesystem::perms Permissions =
				std::filesystem::status(SourceFile, PermissionError).permissions();
			constexpr auto WritePermissions = std::filesystem::perms::owner_write
				| std::filesystem::perms::group_write
				| std::filesystem::perms::others_write;
			if (PermissionError)
				return Error(EAssetError::IoError, std::format(
					"Could not inspect soft referencer {}: {}",
					SourcePath.ToString(), PermissionError.message()));
			if ((Permissions & WritePermissions) == std::filesystem::perms::none)
				return Error(EAssetError::ReadOnlyMode, std::format(
					"Soft referencer {} is read-only.", SourcePath.ToString()));

			std::vector<uint8> SourceBytes;
			if (!FFileHelper::LoadFileToArray(SourceBytes, SourceFile.generic_string()))
				return Error(EAssetError::IoError, std::format(
					"Could not read soft referencer {}.", SourcePath.ToString()));
			FAssetPackageFingerprint CurrentFingerprint;
			Result = MakePackageFingerprint(
				SourceFile.generic_string(), SourceBytes, CurrentFingerprint);
			if (!Result) return Result;
			if (CurrentFingerprint != References.front().SourceFingerprint)
				return Error(EAssetError::InUse, std::format(
					"SoftReferenceMoveStaleFingerprint: {} changed after indexing.",
					SourcePath.ToString()));
			FUnloadedSoftRewrite Rewrite{.Data = *Data, .File = SourceFile};
			Result = RewriteUnloadedSoftPackage(
				SourceBytes, OldPath, NewPath, References.size(), Rewrite.Bytes);
			if (!Result) return Result;
			UnloadedSoftRewrites.push_back(std::move(Rewrite));
		}

		std::vector<std::pair<FSoftObjectPtr*, FSoftObjectPath>> LoadedSoftValues;
		std::vector<DPackage*> LoadedSoftPackages;
		for (const auto& [Path, Package] : LoadedPackages)
		{
			(void)Path;
			const size_t PreviousCount = LoadedSoftValues.size();
			std::vector<FSoftObjectPtr*> Values;
			Result = CollectLoadedPackageSoftReferences(Package, OldPath, Values);
			if (!Result) return Result;
			for (FSoftObjectPtr* Value : Values)
				LoadedSoftValues.emplace_back(Value, Value->GetSoftObjectPath());
			if (LoadedSoftValues.size() != PreviousCount)
				LoadedSoftPackages.push_back(Package);
		}

		const auto RegistryAssetsBackup = Registry.Assets;
		const auto RegistrySoftReferencesBackup = Registry.SoftReferences;
		const auto RegistrySoftFingerprintsBackup = Registry.SoftReferenceSourceFingerprints;
		const auto RegistrySoftErrorsBackup = Registry.SoftReferenceErrors;
		const bool RegistrySnapshotDirtyBackup = Registry.bPersistentSnapshotDirty;
		const bool RegistrySoftSnapshotDirtyBackup = Registry.bSoftReferenceSnapshotDirty;
		const uint64 RegistryRevisionBackup = Registry.Revision;
		const auto LoadedPackagesBackup = LoadedPackages;
		const std::string OldName = MovingPackage->GetAsset()->GetName();
		std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Backups;
		std::unordered_set<std::string> BackedUpFiles;
		auto Backup = [&](const std::filesystem::path& File) -> bool {
			if (!std::filesystem::exists(File)) return false;
			const std::string Key = std::filesystem::absolute(File).lexically_normal().generic_string();
			if (!BackedUpFiles.insert(Key).second) return true;
			const std::filesystem::path Copy = File.string() + ".movebak";
			std::error_code Ec;
			std::filesystem::remove(Copy, Ec);
			Ec.clear();
			std::filesystem::copy_file(File, Copy, std::filesystem::copy_options::overwrite_existing, Ec);
			if (Ec) return false;
			Backups.emplace_back(File, Copy);
			return true;
		};
		auto CleanupBackups = [&]() {
			std::error_code Ec;
			for (const auto& [Original, Copy] : Backups)
			{
				(void)Original;
				std::filesystem::remove(Copy, Ec);
				Ec.clear();
			}
		};
		auto BackupFailure = [&](std::string Message) -> FAssetResult {
			CleanupBackups();
			return Error(EAssetError::IoError, std::move(Message));
		};
		if (!Backup(OldFile)) return BackupFailure("Failed to back up source asset.");
		for (const FAssetPath& Path : ReferrerPaths)
			if (!Backup(GetPhysicalPath(Path)))
				return BackupFailure("Failed to back up an asset referrer.");
		for (DPackage* Package : AdditionalPackages)
		{
			FAssetPath AdditionalPath;
			if (!FAssetPath::TryCreate(Package->GetPackagePath(), AdditionalPath)
				|| !Backup(GetPhysicalPath(AdditionalPath)))
				return BackupFailure(
					"Failed to back up an additional move contributor package.");
		}
		for (const FUnloadedSoftRewrite& Rewrite : UnloadedSoftRewrites)
			if (!Backup(Rewrite.File))
				return BackupFailure("Failed to back up an unloaded soft referencer.");
		for (const auto& [From, To] : Contribution.Files)
			if (!Backup(From)) return BackupFailure("Failed to back up a companion file.");

		std::vector<std::pair<DPackage*, bool>> DirtyStates;
		std::unordered_set<DPackage*> SeenDirtyPackages;
		auto CaptureDirty = [&](DPackage* Package) {
			if (Package && SeenDirtyPackages.insert(Package).second)
				DirtyStates.emplace_back(Package, Package->IsDirty());
		};
		CaptureDirty(MovingPackage);
		for (DPackage* Package : Referrers) CaptureDirty(Package);
		for (DPackage* Package : AdditionalPackages) CaptureDirty(Package);
		for (DPackage* Package : LoadedSoftPackages) CaptureDirty(Package);

		size_t AppliedExternalActionCount = 0;
		bool bContributionApplied = false;
		bool bMovingPackageRelocated = false;
		auto Rollback = [&]() -> std::string {
			std::string Errors;
			if (bContributionApplied && Contribution.Rollback) Contribution.Rollback();
			if (bMovingPackageRelocated)
			{
				if (!MovingPackage->RelocateAssetPackage(OldPath))
					Errors += "Could not restore the moving package path. ";
				MovingPackage->Rename(FName(OldPath.GetAssetName()));
				MovingPackage->GetAsset()->Rename(FName(OldName));
			}
			for (auto& [Value, PreviousPath] : LoadedSoftValues)
				Value->SetPath(PreviousPath);
			LoadedPackages = LoadedPackagesBackup;
			std::error_code Ec;
			std::filesystem::remove(NewFile, Ec);
			for (const auto& [From, To] : Contribution.Files)
			{
				(void)From;
				Ec.clear();
				std::filesystem::remove(To, Ec);
			}
			for (const auto& [Original, Copy] : Backups)
			{
				Ec.clear();
				std::filesystem::copy_file(
					Copy, Original, std::filesystem::copy_options::overwrite_existing, Ec);
				if (Ec) Errors += std::format(
					"Could not restore {}: {}. ", Original.generic_string(), Ec.message());
			}
			Registry.Assets = RegistryAssetsBackup;
			Registry.SoftReferences = RegistrySoftReferencesBackup;
			Registry.SoftReferenceSourceFingerprints = RegistrySoftFingerprintsBackup;
			Registry.SoftReferenceErrors = RegistrySoftErrorsBackup;
			Registry.bPersistentSnapshotDirty = RegistrySnapshotDirtyBackup;
			Registry.bSoftReferenceSnapshotDirty = RegistrySoftSnapshotDirtyBackup;
			Registry.Revision = RegistryRevisionBackup;
			for (const auto& [Package, bWasDirty] : DirtyStates)
			{
				if (bWasDirty) Package->MarkDirty();
				else Package->ClearDirty();
			}
			for (size_t Count = AppliedExternalActionCount; Count > 0; --Count)
			{
				const FAssetMoveExternalStoreAction& Action = ExternalActions[Count - 1];
				const FAssetResult RollbackResult = Action.Rollback();
				if (!RollbackResult)
					Errors += std::format("Could not restore external store '{}': {}. ",
						Action.Name, RollbackResult.Message);
			}
			CleanupBackups();
			return Errors;
		};
		auto FailAfterMutation = [&](FAssetResult Failure) -> FAssetResult {
			const std::string RollbackErrors = Rollback();
			if (!RollbackErrors.empty())
				Failure.Message += std::format(" Rollback also failed: {}", RollbackErrors);
			return Failure;
		};

		if (!MovingPackage->RelocateAssetPackage(NewPath))
		{
			CleanupBackups();
			return Error(EAssetError::AlreadyExists,
				"The destination package path is registered.");
		}
		bMovingPackageRelocated = true;
		MovingPackage->Rename(FName(NewPath.GetAssetName()));
		if (OldPath.GetAssetName() != NewPath.GetAssetName())
			MovingPackage->GetAsset()->Rename(FName(NewPath.GetAssetName()));
		if (Contribution.Apply)
		{
			Contribution.Apply();
			bContributionApplied = true;
		}
		for (const auto& [From, To] : Contribution.Files)
		{
			std::error_code Ec;
			std::filesystem::create_directories(To.parent_path(), Ec);
			Ec.clear();
			std::filesystem::rename(From, To, Ec);
			if (Ec)
				return FailAfterMutation(Error(EAssetError::IoError,
					"Failed to move a companion file."));
		}
		LoadedPackages.erase(OldPath);
		LoadedPackages.emplace(NewPath, MovingPackage);
		Registry.Remove(OldPath);
		std::error_code DirectoryEc;
		std::filesystem::create_directories(NewFile.parent_path(), DirectoryEc);
		if (DirectoryEc)
			return FailAfterMutation(Error(EAssetError::IoError,
				"Failed to create the destination directory."));

		for (auto& [Value, PreviousPath] : LoadedSoftValues)
		{
			(void)PreviousPath;
			Value->SetPath(NewPath);
		}
		std::vector<DPackage*> PackagesToSave;
		std::unordered_set<DPackage*> SeenPackagesToSave;
		auto AddPackageToSave = [&](DPackage* Package) {
			if (Package && SeenPackagesToSave.insert(Package).second)
				PackagesToSave.push_back(Package);
		};
		AddPackageToSave(MovingPackage);
		for (DPackage* Package : Referrers) AddPackageToSave(Package);
		for (DPackage* Package : AdditionalPackages) AddPackageToSave(Package);
		for (DPackage* Package : LoadedSoftPackages) AddPackageToSave(Package);
		for (DPackage* Package : PackagesToSave)
		{
			Result = SavePackage(Package);
			if (!Result) return FailAfterMutation(std::move(Result));
		}

		for (FUnloadedSoftRewrite& Rewrite : UnloadedSoftRewrites)
		{
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Rewrite.Bytes.data()),
					Rewrite.Bytes.size()},
				Rewrite.File,
				&PublicationError))
				return FailAfterMutation(Error(EAssetError::IoError,
					PublicationError.ToString()));
			const auto LastWriteTime = std::filesystem::last_write_time(Rewrite.File);
			Rewrite.Data.FileSize = std::filesystem::file_size(Rewrite.File);
			Rewrite.Data.LastWriteTime = LastWriteTime;
			Rewrite.Data.LastWriteTimeTicks =
				DerivedDataCache::FileTimeToStableTicks(LastWriteTime);
			Registry.AddOrUpdate(std::move(Rewrite.Data));
		}

		for (size_t Index = 0; Index < ExternalActions.size(); ++Index)
		{
			AppliedExternalActionCount = Index + 1;
			Result = ExternalActions[Index].Apply();
			if (!Result) return FailAfterMutation(std::move(Result));
		}

		std::error_code Ec;
		std::filesystem::remove(OldFile, Ec);
		if (Ec)
			return FailAfterMutation(Error(EAssetError::IoError,
				"Failed to remove the old asset file."));
		CleanupBackups();
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
		return LoadAsset(Path, nullptr, OutAsset, OutReport);
	}

	auto FAssetManager::LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!bAcceptingRequests)
			return Error(
				EAssetError::ShuttingDown,
				"Asset loading is closed while the asset manager is shutting down.");
		if (ExpectedClass && !ExpectedClass->IsChildOf(DObject::StaticClass()))
			return Error(EAssetError::TypeMismatch, "An asset load requires a DObject class.");

		const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (!Resolution)
		{
			if (Resolution.State == EAssetPathResolveState::NotFound)
			{
				const auto Loaded = LoadedPackages.find(Path);
				DObject* LoadedObject = Loaded != LoadedPackages.end()
					? Loaded->second->GetAsset() : nullptr;
				if (LoadedObject && !LoadedObject->IsA<DAssetRedirector>())
				{
					if (ExpectedClass && !LoadedObject->IsA(ExpectedClass))
						return Error(EAssetError::TypeMismatch, std::format(
							"Asset {} is not a {}.", Path.ToString(),
							ExpectedClass->GetQualifiedName().ToString()));
					OutAsset = LoadedObject;
					return {};
				}

				const std::string PhysicalPath = GetPhysicalPath(Path);
				FAssetPackageHeader Header;
				FAssetResult HeaderResult = PhysicalPath.empty()
					? AssetPathResolutionError(Resolution)
					: ReadAssetPackageHeader(PhysicalPath, Header);
				if (!HeaderResult) return HeaderResult;
				if (Header.EntryKind == EAssetRegistryEntryKind::Redirector)
					return Error(EAssetError::NotFound, std::format(
						"Redirector {} must be present in the registry before public loading.",
						Path.ToString()));
				DClass* HeaderClass = FindClassByQualifiedName(FName(Header.AssetClassName));
				if (!HeaderClass)
					return Error(EAssetError::UnknownClass, std::format(
						"Asset {} has unknown class {}.",
						Path.ToString(), Header.AssetClassName));
				if (ExpectedClass && !HeaderClass->IsChildOf(ExpectedClass))
					return Error(EAssetError::TypeMismatch, std::format(
						"Asset {} is registered as {}, not a {}.",
						Path.ToString(), Header.AssetClassName,
						ExpectedClass->GetQualifiedName().ToString()));
				return LoadAssetExact(Path, ExpectedClass, OutAsset, OutReport);
			}
			return AssetPathResolutionError(Resolution);
		}
		return LoadAssetExact(
			Resolution.FinalPath, ExpectedClass, OutAsset, OutReport);
	}

	auto FAssetManager::LoadAssetExact(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
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
		if (Result && Package && ExpectedClass)
		{
			DObject* Asset = Package->GetAsset();
			if (!Asset || !Asset->IsA(ExpectedClass))
				Result = Error(EAssetError::TypeMismatch, std::format(
					"Asset {} is not a {}.", Path.ToString(),
					ExpectedClass->GetQualifiedName().ToString()));
		}
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
		Result = ValidateRedirectorHeader(File, File.Objects.size(), &Path);
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
			DObject* DependencyObject = nullptr;
			Result = LoadAsset(Dependency, DependencyObject);
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
			.EntryKind = File.EntryKind,
			.RedirectDestination = File.RedirectDestination,
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
			const FAssetData* Data = Registry.FindAssetExact(OtherPath);
			if (!Data) continue;
			for (const FAssetPath& Dependency : Data->Dependencies)
			{
				const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(Dependency);
				if (Resolution && Resolution.FinalPath == Path) return true;
			}
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
				const FAssetData* Data = Registry.FindAssetExact(Path);
				if (!Data) continue;
				for (const FAssetPath& Dependency : Data->Dependencies)
				{
					const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(Dependency);
					if (Resolution) bChanged |= Protected.insert(Resolution.FinalPath).second;
				}
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

	auto CreateAssetRedirector(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		return FAssetManager::Get().CreateRedirector(
			RedirectorPath, DestinationPath, OutRedirector);
	}

	auto FAssetManager::ResolveSoftObjectInternal(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult
	{
		CheckSoftObjectThread();
		if (!ExpectedClass || !ExpectedClass->IsChildOf(DObject::StaticClass()))
		{
			return {
				.Result = Error(EAssetError::TypeMismatch, "A soft-object resolve requires a DObject class."),
				.State = Reference.IsNull() ? ESoftObjectResolveState::Null : ESoftObjectResolveState::NotLoaded};
		}
		if (Reference.IsNull())
		{
			return NullPolicy == ESoftObjectNullPolicy::Allow
				? FSoftObjectResolveResult{.State = ESoftObjectResolveState::Null}
				: FSoftObjectResolveResult{
					.Result = Error(EAssetError::InvalidPath, "A null soft-object reference is not allowed."),
					.State = ESoftObjectResolveState::Null};
		}

		const FAssetPath& Path = Reference.GetSoftObjectPath().GetAssetPath();
		const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (!Resolution)
		{
			if (Resolution.State == EAssetPathResolveState::NotFound)
			{
				DPackage* LoadedPackage = FindLoadedPackage(Path);
				DObject* LoadedObject = LoadedPackage ? LoadedPackage->GetAsset() : nullptr;
				if (LoadedObject && !LoadedObject->IsA<DAssetRedirector>())
				{
					if (!LoadedObject->IsA(ExpectedClass))
						return {
							.Result = Error(EAssetError::TypeMismatch, std::format(
								"Asset {} is not a {}.", Path.ToString(),
								ExpectedClass->GetQualifiedName().ToString())),
							.State = ESoftObjectResolveState::NotLoaded};
					std::string ValidationError;
					if (!Reference.TrySetResolvedObject(
						LoadedObject, Path, Path, ExpectedClass, &ValidationError))
						return {
							.Result = Error(EAssetError::InvalidObjectGraph, std::move(ValidationError)),
							.State = ESoftObjectResolveState::NotLoaded};
					return {
						.State = ESoftObjectResolveState::Loaded,
						.Object = LoadedObject,
						.ResolvedPath = Path};
				}
			}
			Reference.ResetResolvedObject();
			return {
				.Result = AssetPathResolutionError(Resolution),
				.State = ESoftObjectResolveState::NotLoaded};
		}

		DPackage* Package = FindLoadedPackage(Resolution.FinalPath);
		if (!Package)
		{
			Reference.ResetResolvedObject();
			return {
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}

		DObject* Object = Package->GetAsset();
		if (!Object)
		{
			return {
				.Result = Error(EAssetError::InvalidObjectGraph, std::format(
					"Loaded package {} has no main asset.", Resolution.FinalPath.ToString())),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}
		if (!Object->IsA(ExpectedClass))
		{
			return {
				.Result = Error(EAssetError::TypeMismatch, std::format(
					"Asset {} is not a {}.",
					Resolution.FinalPath.ToString(), ExpectedClass->GetQualifiedName().ToString())),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}

		std::string ValidationError;
		if (!Reference.TrySetResolvedObject(
			Object, Path, Resolution.FinalPath, ExpectedClass, &ValidationError))
		{
			return {
				.Result = Error(EAssetError::InvalidObjectGraph, std::move(ValidationError)),
				.State = ESoftObjectResolveState::NotLoaded,
				.ResolvedPath = Resolution.FinalPath,
				.bRedirected = !Resolution.RedirectChain.empty()};
		}
		return {
			.State = ESoftObjectResolveState::Loaded,
			.Object = Object,
			.ResolvedPath = Resolution.FinalPath,
			.bRedirected = !Resolution.RedirectChain.empty()};
	}

	auto FAssetManager::LoadSoftObjectInternal(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		CheckSoftObjectThread();
		OutObject = nullptr;
		FSoftObjectResolveResult Resolved = ResolveSoftObjectInternal(
			Reference, ExpectedClass, NullPolicy);
		if (!Resolved) return Resolved.Result;
		if (Resolved.State == ESoftObjectResolveState::Null) return {};
		if (Resolved.State == ESoftObjectResolveState::Loaded)
		{
			OutObject = Resolved.Object;
			return {};
		}

		DObject* LoadedObject = nullptr;
		const FAssetPath& Path = Reference.GetSoftObjectPath().GetAssetPath();
		FAssetResult Result = LoadAsset(
			Path, ExpectedClass, LoadedObject, OutReport);
		if (!Result) return Result;
		if (!LoadedObject)
			return Error(EAssetError::InvalidObjectGraph, std::format(
				"Loaded package {} has no main asset.", Path.ToString()));
		if (!LoadedObject->IsA(ExpectedClass))
		{
			return Error(EAssetError::TypeMismatch, std::format(
				"Asset {} is not a {}.",
				Path.ToString(), ExpectedClass->GetQualifiedName().ToString()));
		}

		std::string ValidationError;
		if (!Reference.TrySetResolvedObject(
			LoadedObject, Path, Resolved.ResolvedPath, ExpectedClass, &ValidationError))
			return Error(EAssetError::InvalidObjectGraph, std::move(ValidationError));
		OutObject = LoadedObject;
		return {};
	}

	auto ResolveSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		ESoftObjectNullPolicy NullPolicy) -> FSoftObjectResolveResult
	{
		return FAssetManager::Get().ResolveSoftObjectInternal(
			Reference, ExpectedClass, NullPolicy);
	}

	auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetManager::Get().LoadSoftObjectInternal(
			Reference, ExpectedClass, OutObject, NullPolicy, OutReport);
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
