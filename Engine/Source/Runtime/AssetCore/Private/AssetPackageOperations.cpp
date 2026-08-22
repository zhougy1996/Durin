#include "AssetRuntimeStateInternal.h"
#include "AssetDeletionInternal.h"
#include "AssetCatalogPersistenceInternal.h"
#include "AssetMutationJournalInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetRelocationExtensionsInternal.h"
#include "Asset/PackageV4Reader.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"
#include "Asset/Redirector.h"
#include "Asset/AuthoredBulkStorage.h"
#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"
#include "Profiling/Profiling.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "Misc/FileHelper.h"
#include "Misc/FileTime.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	using Private::FAssetReferenceStoreRegistry;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::EAssetMutationState;
	using Private::ERelocationPublicationRole;
	using Private::FAssetMutationJournal;
	using Private::FAssetMutationJournalEntry;
	using Private::FingerprintRelocationFile;
	using Private::IsWritableRelocationPath;
	using Private::LoadRelocationBytes;
	using Private::MakePackageFingerprint;
	using Private::MakeRelocationOperationId;
	using Private::NormalizePhysicalPath;
	using Private::PublishRelocationFile;
	using Private::RebuildReferenceProjectionForPublishedEntries;
	using Private::SaveRelocationBytes;
	using Private::WriteMutationJournalState;
	using Private::AssetReferenceLess;
	using Private::BuildRegistryCacheEntries;
	using Private::FReferenceCacheSource;
	using Private::FRegistryCacheEntry;
	using Private::GetMountManifest;
	using Private::LoadReferenceCache;
	using Private::LoadRegistryCache;
	using Private::MakeRegistryIdentity;
	using Private::WriteReferenceCache;
	using Private::WriteRegistryCache;

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			std::span<const uint8> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult;

		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::NotFound:
				return {EAssetError::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::MissingRedirectTarget:
				return {EAssetError::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::RedirectCycle:
				return {EAssetError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::RedirectDepthExceeded:
				return {EAssetError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::UnknownTargetClass:
				return {EAssetError::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::RedirectTypeMismatch:
				return {EAssetError::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::CorruptRedirector:
				return {EAssetError::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			}
			return {EAssetError::CorruptFile,
				"Asset resolution returned an unknown state."};
		}
		using Private::MaximumPackageStringBytes;
		using Private::FByteReader;
		using Private::FByteWriter;
		using Private::GetSerializedTypeSignature;
		using Private::IsSerializedTypeSignatureCompatible;
		constexpr uint32 MaximumRedirectDepth = 32;
		constexpr std::string_view RedirectorClassName = "Durin::Asset::DAssetRedirector";

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

			auto Reset() -> bool
			{
				Stream.clear();
				Stream.seekg(0, std::ios::beg);
				Offset = 0;
				return !Stream.fail();
			}

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

		struct FPackageFile
		{
			uint32 FormatVersion = 0;
			std::string AssetClassName;
			EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
			FAssetPath RedirectDestination;
			std::vector<FAssetPath> Dependencies;
			std::vector<FAuthoredBulkPayload> BulkPayloads;
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

		auto IsMissingPathError(const std::error_code& ErrorCode) -> bool
		{
			return ErrorCode == std::errc::no_such_file_or_directory
				|| ErrorCode.value() == 2
				|| ErrorCode.value() == 3;
		}

		auto CleanupStaleAuthoredBulkCompanions(
			const std::filesystem::path& PackagePath,
			const std::filesystem::path& KeepPath = {}) -> void
		{
			const std::filesystem::path Parent = PackagePath.parent_path();
			const std::string Prefix = PackagePath.stem().string() + ".";
			std::error_code ErrorCode;
			for (std::filesystem::directory_iterator It(Parent, ErrorCode), End;
				!ErrorCode && It != End; It.increment(ErrorCode))
			{
				const std::filesystem::path Candidate = It->path();
				const std::string Name = Candidate.filename().string();
				if (!It->is_regular_file(ErrorCode) || ErrorCode
					|| !Name.starts_with(Prefix)
					|| !Name.ends_with(AuthoredBulkCompanionSuffix)
					|| (!KeepPath.empty() && Candidate == KeepPath))
				{
					ErrorCode.clear();
					continue;
				}
				std::filesystem::remove(Candidate, ErrorCode);
				ErrorCode.clear();
			}
		}

		auto GetPhysicalPath(const FAssetPath& Path) -> std::string
		{
			const FAssetRuntimeConfiguration& Context =
				FAssetRuntimeState::Get().GetRuntimeConfiguration();
			if (Context.IsCooked())
			{
				std::filesystem::path CookedPath;
				if (!ResolveCookedPackagePath(
					Context.GetCookRoot(), Path.GetView(), CookedPath)) return {};
				return CookedPath.generic_string();
			}
			const PathUtilities::FAssetPathResult Resolved =
				PathUtilities::ResolveAssetPath(Path.GetView(), PathUtilities::EPathExistence::AllowMissing);
			if (!Resolved)
				DURIN_WARN_CATEGORY(
					"AssetSystem", "Failed to resolve asset path {}: {}", Path.ToString(), Resolved.Message);
			return Resolved ? Resolved.PhysicalPath.generic_string() + ".dasset" : std::string{};
		}

		auto GatherObjects(DObject* Object, std::vector<DObject*>& OutObjects) -> void
		{
			if (!Object) return;
			OutObjects.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object, EObjectQueryScope::LiveOnly)) GatherObjects(Inner, OutObjects);
		}

		auto DecodeByteToolValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			FByteReader& Reader,
			const std::vector<DObject*>& Objects,
			uint32 SourceVersion = LatestAssetPackageWriterVersion) -> FAssetResult
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
					FAssetResult Result = FAssetRuntimeState::Get().GetLoadService().LoadAsset(Path, Value);
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
				std::optional<FStructProperty> DetachedProperty;
				const FProperty* StorageProperty = Property;
				if (Property->HasValueAccessors())
				{
					DetachedProperty.emplace(
						FFieldVariant(), Property->NamePrivate, EObjectFlags::Transient,
						EPropertyFlags::Transient, 1, 0, Struct);
					StorageProperty = &*DetachedProperty;
				}
				FReflectedValueStorage Storage;
				if (!Storage.DefaultConstruct(StorageProperty, 0, &StorageError))
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
					FProperty* Field = Struct->FindPropertyBySerializedName(FName(FieldName), false);
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
						FAssetResult Result = DecodeByteToolValue(
							Field, StructValue, FieldIndex, PayloadReader, Objects, SourceVersion);
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
					FAssetResult Result = DecodeByteToolValue(
						Array->GetInner(), Element,
						0, Reader, Objects, SourceVersion);
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
					FAssetResult Result = DecodeByteToolValue(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Reader, Objects, SourceVersion);
					if (!Result)
					{
						Result.Message = std::format("MapEntry[{}].Key: {}", Index, Result.Message);
						return Result;
					}
					Result = DecodeByteToolValue(
						Map->GetValueProp(), ValueStorage.GetContainer(), 0, Reader, Objects, SourceVersion);
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
				return FAssetRuntimeState::Get().GetLoadService().LoadAsset(Path, OutObject);
			}
			return Error(EAssetError::CorruptFile, "Unknown object reference kind.");
		}

		auto FindExistingInner(DObject* Outer, std::string_view Name, DClass* Class, bool& bTypeMismatch) -> DObject*
		{
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Outer, EObjectQueryScope::LiveOnly))
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
			const Private::FAssetPackageCodec* Codec =
				Private::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
			if (!Codec)
				return Error(EAssetError::UnsupportedVersion,
					"The selected ordinary asset package writer is unavailable.");
			FAssetPackageSerializationOptions EffectiveOptions = Options;
			if (OutFile) EffectiveOptions.AuthoredBulkPayloads = &OutFile->BulkPayloads;
			FAssetResult Result = Codec->Write(
				Package, OutBytes, EDefaultDeltaMode::NoDelta, EffectiveOptions);
			if (!Result) return Result;
			if (OutFile)
			{
				FAssetPackageHeader Header;
				if (FAssetResult HeaderResult = Codec->ReadHeader(
					OutBytes, OutBytes.size(), Header); !HeaderResult)
					return HeaderResult;
				OutFile->FormatVersion = Header.FormatVersion;
				OutFile->AssetClassName = std::move(Header.AssetClassName);
				OutFile->EntryKind = Header.EntryKind;
				OutFile->RedirectDestination = std::move(Header.RedirectDestination);
				OutFile->Dependencies = std::move(Header.Dependencies);
			}
			return Result;
		}

		auto ValidateOrdinarySaveVersion(
			const FAssetCatalogStore& Registry,
			const FAssetPath& Path) -> FAssetResult
		{
			const FAssetCatalogEntry Existing = Registry.FindAssetExact(Path);
			if (!Existing || Existing->FormatVersion == OrdinaryAssetPackageWriterVersion)
				return {};
			return Error(
				EAssetError::UnsupportedVersion,
				std::format(
					"Package {} uses unsupported DAST v{} while ordinary saves write DAST v{}.",
					Path.ToString(),
					Existing->FormatVersion,
					OrdinaryAssetPackageWriterVersion));
		}
	}

	namespace Private
	{
		auto ValidateMutationPackageMetadata(
			const FMutationPackageMetadata& Metadata,
			uint64 ObjectCount,
			const FAssetPath* SourcePath) -> FAssetResult
		{
			const FPackageFile File{
				.FormatVersion = Metadata.FormatVersion,
				.AssetClassName = Metadata.AssetClassName,
				.EntryKind = Metadata.EntryKind,
				.RedirectDestination = Metadata.RedirectDestination,
				.Dependencies = Metadata.Dependencies};
			return ValidateRedirectorHeader(File, ObjectCount, SourcePath);
		}

		auto InspectAssetPackageBytesForCatalog(
			std::string_view PhysicalPath,
			std::span<const uint8> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			return InspectAssetPackageBytes(
				PhysicalPath, Bytes, OutInspection);
		}

		auto DecodeReferenceByteToolValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			FByteReader& Reader,
			const std::vector<DObject*>& Objects,
			uint32 SourceVersion) -> FAssetResult
		{
			return DecodeByteToolValue(
				Property,
				Container,
				ArrayIndex,
				Reader,
				Objects,
				SourceVersion);
		}

	}

	auto ReadAssetPackageHeader(std::string_view PhysicalPath, FAssetPackageHeader& OutHeader) -> FAssetResult
	{
		OutHeader = {};
		FFileByteReader Reader(PhysicalPath);
		if (!Reader.IsOpen())
			return Error(EAssetError::IoError,
				std::format("Failed to open asset package {}.", PhysicalPath));
		if (Reader.FileSize > DastV4::MaximumPackageBytes)
			return Error(EAssetError::CorruptFile,
				"Asset package exceeds the supported byte bound.");
		const uint64 ReadSize = std::min(
			Reader.FileSize, DastV4::MaximumHeaderBytes);
		std::vector<uint8> Bytes(static_cast<size_t>(ReadSize));
		if (ReadSize != 0)
		{
			Reader.Stream.read(
				reinterpret_cast<char*>(Bytes.data()),
				static_cast<std::streamsize>(ReadSize));
			if (!Reader.Stream)
				return Error(EAssetError::IoError,
					std::format("Failed to read asset package {}.", PhysicalPath));
		}
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		FAssetResult Result = Codec->ReadHeader(Bytes, Reader.FileSize, OutHeader);
		if (Result) OutHeader.FileBytesRead = ReadSize;
		return Result;
	}

	auto ValidateAssetPackageBytes(std::span<const uint8> Bytes) -> FAssetResult
	{
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		return Codec->Validate(Bytes);
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
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.SavePackagesAtomically(Packages, Options);
	}

	auto FAssetMutationCoordinator::SavePackagesAtomically(
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
			std::filesystem::path PublishedCompanion;
			uintmax_t PublishedFileSize = 0;
			std::filesystem::file_time_type PublishedLastWriteTime{};
			bool bHadDestination = false;
			bool bPublished = false;
		};

		if (Packages.empty())
			return Error(EAssetError::InvalidPackageType, "An asset bundle must contain at least one package.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit bundle saves.");
		if (Options.RootPackage
			&& std::ranges::find(Packages, Options.RootPackage) == Packages.end())
			return Error(EAssetError::InvalidPackageType, "The root package is not part of the asset bundle.");

		std::vector<FStagedPackage> StagedPackages;
		StagedPackages.reserve(Packages.size());
		std::unordered_set<FAssetPath> Paths;
		for (DPackage* Package : Packages)
		{
			FAssetPath Path;
			if (!Package || !Package->IsAssetPackage()
				|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
				return Error(EAssetError::InvalidPackageType, "The asset bundle contains an invalid package.");
			if (!Paths.insert(Path).second)
				return Error(EAssetError::AlreadyExists, std::format(
					"The asset bundle contains duplicate package {}.", Path.ToString()));
			FAssetResult Result = ValidateOrdinarySaveVersion(Registry, Path);
			if (!Result) return Result;
			FStagedPackage& Staged = StagedPackages.emplace_back();
			Staged.Package = Package;
			Staged.Path = Path;
			Result = BuildPackageBytes(Package, Staged.Bytes, &Staged.File);
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
			if (!Staged.File.BulkPayloads.empty())
			{
				const FXxHash128 ContainerHash =
					Staged.File.BulkPayloads.front().Descriptor.ContainerHash;
				std::vector<uint8> CompanionBytes;
				std::string CompanionError;
				if (ContainerHash.IsZero()
					|| std::ranges::any_of(Staged.File.BulkPayloads,
						[&](const FAuthoredBulkPayload& Payload) {
							return Payload.Descriptor.ContainerHash != ContainerHash;
						})
					|| !BuildAuthoredBulkCompanion(Staged.File.BulkPayloads,
						ContainerHash, CompanionBytes, &CompanionError)
					|| !ResolveAuthoredBulkCompanionPath(Staged.Destination,
						ContainerHash, Staged.PublishedCompanion, &CompanionError))
				{
					CleanupStaging();
					return Error(EAssetError::CorruptFile, std::move(CompanionError));
				}
				FFileHelper::FAtomicFileError CompanionPublicationError;
				if (!FFileHelper::SaveArrayToFileAtomically(
						std::span{reinterpret_cast<const std::byte*>(CompanionBytes.data()),
							CompanionBytes.size()}, Staged.PublishedCompanion,
						&CompanionPublicationError))
				{
					CleanupStaging();
					return Error(EAssetError::IoError,
						CompanionPublicationError.ToString());
				}
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
			Registry.AddOrUpdate(FAssetData{
				.PackagePath = Staged.Path,
				.PhysicalPath = Staged.Destination.generic_string(),
				.AssetClassName = Staged.File.AssetClassName,
				.EntryKind = Staged.File.EntryKind,
				.RedirectDestination = Staged.File.RedirectDestination,
				.FormatVersion = OrdinaryAssetPackageWriterVersion,
				.Dependencies = Staged.File.Dependencies,
				.FileSize = Staged.PublishedFileSize,
				.LastWriteTime = Staged.PublishedLastWriteTime,
				.LastWriteTimeTicks = FileTime::ToStableTicks(
					Staged.PublishedLastWriteTime)});
			if (auto Resident = ResidentPackages.find(Staged.Path);
				Resident != ResidentPackages.end()
				&& Resident->second.Package == Staged.Package)
				Resident->second.PublicationState =
					EAssetPackagePublicationState::Published;
			Staged.Package->ClearDirty();
			std::error_code Ec;
			std::filesystem::remove(Staged.Backup, Ec);
			CleanupStaleAuthoredBulkCompanions(
				Staged.Destination, Staged.PublishedCompanion);
		}
		return {};
	}

	auto AdmitAssetPackageToCatalog(const FAssetPath& Path) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().AdmitAssetPackageToCatalog(Path);
	}

	auto FAssetMutationCoordinator::AdmitAssetPackageToCatalog(
		const FAssetPath& Path) -> FAssetResult
	{
		if (!Path.IsValid())
			return Error(EAssetError::InvalidPath, "The asset admission path is invalid.");
		if (Registry.FindAssetExact(Path) || FindResidentPackage(Path))
			return Error(EAssetError::AlreadyExists,
				"The asset admission path is already occupied.");
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty())
			return Error(EAssetError::InvalidPath,
				"The asset admission path is outside mounted package content.");
		FAssetPackageHeader Header;
		if (FAssetResult Result = ReadAssetPackageHeader(PhysicalPath, Header); !Result)
			return Result;
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError,
				"The asset package could not be read for admission validation.");
		if (FAssetResult Result = ValidateAssetPackageBytes(Bytes); !Result)
			return Result;
		std::error_code ErrorCode;
		const auto LastWriteTime = std::filesystem::last_write_time(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(EAssetError::IoError,
				"The asset package timestamp could not be read for admission.");
		const uintmax_t FileSize = std::filesystem::file_size(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(EAssetError::IoError,
				"The asset package size could not be read for admission.");
		Registry.AddOrUpdate(FAssetData{
			.PackagePath = Path,
			.PhysicalPath = PhysicalPath,
			.AssetClassName = Header.AssetClassName,
			.EntryKind = Header.EntryKind,
			.RedirectDestination = Header.RedirectDestination,
			.FormatVersion = Header.FormatVersion,
			.Dependencies = Header.Dependencies,
			.FileSize = FileSize,
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)});
		return {};
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

	auto FAssetPackageField::TryReadAuthoredBulkDescriptor(
		FAuthoredBulkDataDescriptor& OutValue) const -> bool
	{
		OutValue = {};
		if (Kind != DurinCodeGen::EPropertyGenFlags::BulkData) return false;
		FByteReader Reader{Payload};
		uint8 StorageKind = 0;
		uint64 HashLow = 0, HashHigh = 0, ContainerLow = 0, ContainerHigh = 0;
		if (!Reader.Read(StorageKind) || StorageKind > 1
			|| !Reader.Read(OutValue.PayloadId) || !Reader.Read(OutValue.FormatId)
			|| !Reader.Read(OutValue.FormatVersion)
			|| !Reader.Read(OutValue.LogicalByteCount)
			|| !Reader.Read(OutValue.StoredByteCount)
			|| !Reader.Read(HashLow) || !Reader.Read(HashHigh)
			|| !Reader.Read(ContainerLow) || !Reader.Read(ContainerHigh)) return false;
		OutValue.ContentHash = {HashLow, HashHigh};
		OutValue.ContainerHash = {ContainerLow, ContainerHigh};
		OutValue.StorageKind = StorageKind == 0
			? EAuthoredBulkStorageKind::Inline : EAuthoredBulkStorageKind::External;
		return OutValue.PayloadId.IsValid() && OutValue.FormatId.IsValid()
			&& OutValue.FormatVersion != 0
			&& OutValue.LogicalByteCount == OutValue.StoredByteCount
			&& (OutValue.StorageKind == EAuthoredBulkStorageKind::Inline
				? OutValue.ContainerHash.IsZero()
				: OutValue.ContainerHash.IsZero() == false && Reader.Offset == Payload.size());
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
		return DecodeByteToolValue(
			&RootProperty, OutValue, 0, Reader, {},
			SourceFormatVersion == 0 ? LatestAssetPackageWriterVersion : SourceFormatVersion)
			&& Reader.Offset == Payload.size();
	}

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			std::span<const uint8> Bytes,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			OutInspection = {};
			FAssetResult Result = MakePackageFingerprint(PhysicalPath, Bytes, OutInspection.Fingerprint);
			if (!Result) return Result;
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			FAssetPackageInspection Inspection;
			Result = Codec->Inspect(Bytes, Inspection);
			if (!Result) return Result;
			Inspection.Fingerprint = OutInspection.Fingerprint;
			OutInspection = std::move(Inspection);
			return {};
		}
	}

	auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetResult
	{
		OutInspection = {};
		std::vector<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError, std::format("Failed to open asset package {}.", PhysicalPath));
		return InspectAssetPackageBytes(PhysicalPath, Bytes, OutInspection);
	}


	auto CanonicalizeAssetPackageForCook(
		std::span<const uint8> Bytes,
		std::vector<uint8>& OutBytes) -> FAssetResult
	{
		OutBytes.clear();
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		if (!Codec->bCanMutate || Codec->FormatVersion != OrdinaryAssetPackageWriterVersion)
			return Error(EAssetError::UnsupportedVersion,
				"Cook canonicalization requires the ordinary-format mutation capability.");
		FAssetPackageInspection Inspection;
		FAssetResult Result = Codec->Inspect(Bytes, Inspection);
		if (!Result) return Result;
		if (Inspection.Header.EntryKind != EAssetRegistryEntryKind::Asset)
			return Error(EAssetError::InvalidPackageType,
				"CookCanonicalizationRedirectorPackage: redirector packages are authoring-only.");
		std::vector<FAssetReferenceEdge> References;
		Result = Private::ExtractAssetReferencesForCook(
			Inspection, References);
		if (!Result) return Result;
		const FAssetCatalogStore& Registry = GetAssetCatalogStore();
		std::vector<FAssetRedirectorFixupMapping> Mappings;
		auto ResolveReference = [&](const FAssetPath& Path,
			std::string_view ExpectedClassName, std::string_view Route) -> FAssetResult {
			DClass* ExpectedClass = nullptr;
			if (!ExpectedClassName.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(ExpectedClassName));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookCanonicalizationUnknownExpectedClass: {} expects unavailable class {}.",
						Route, ExpectedClassName));
			}
			const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(
				Path, {.ExpectedClass = ExpectedClass});
			if (!Resolution)
			{
				FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
				ResolutionError.Message = std::format(
					"CookCanonicalizationUnresolvedReference: {} at {}. {}",
					Path.ToString(), Route, ResolutionError.Message);
				return ResolutionError;
			}
			if (!Resolution.FinalAssetData
				|| Resolution.FinalAssetData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType,
					"Cook canonicalization resolved a reference to a non-asset package.");
			if (Resolution.FinalPath == Path) return {};
			const auto Existing = std::ranges::find(
				Mappings, Path, &FAssetRedirectorFixupMapping::RedirectorPath);
			if (Existing == Mappings.end())
				Mappings.push_back({.RedirectorPath = Path, .FinalPath = Resolution.FinalPath});
			else if (Existing->FinalPath != Resolution.FinalPath)
				return Error(EAssetError::StaleData,
					"Cook canonicalization observed inconsistent redirect resolution.");
			return {};
		};
		for (const FAssetPath& Dependency : Inspection.Header.Dependencies)
		{
			Result = ResolveReference(Dependency, {}, "package dependency table");
			if (!Result) return Result;
		}
		for (const FAssetReferenceEdge& Reference : References)
		{
			Result = ResolveReference(Reference.TargetPath, Reference.ExpectedClass,
				Reference.DisplayRoute);
			if (!Result) return Result;
		}
		if (Mappings.empty())
		{
			OutBytes.assign(Bytes.begin(), Bytes.end());
			return {};
		}
		return Private::RewritePackageReferencesForMutation(
			Bytes, Mappings, std::numeric_limits<uint64>::max(), OutBytes);

	}

	auto FAssetMutationCoordinator::SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions&) -> FAssetResult
	{
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit package saves.");
		FAssetPath Path;
		if (Package && Package->IsAssetPackage()
			&& !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
			return Error(EAssetError::InvalidPath, "Package path is invalid.");
		FAssetResult VersionResult = ValidateOrdinarySaveVersion(Registry, Path);
		if (!VersionResult) return VersionResult;
		const std::filesystem::path Destination(GetPhysicalPath(Path));
		FPackageFile File;
		std::vector<uint8> Bytes;
		FAssetResult SerializationResult = BuildPackageBytes(Package, Bytes, &File);
		if (!SerializationResult) return SerializationResult;
		FFileHelper::FAtomicFileError PublicationError;
		std::filesystem::path PublishedCompanion;
		if (!File.BulkPayloads.empty())
		{
			const FXxHash128 ContainerHash = File.BulkPayloads.front().Descriptor.ContainerHash;
			if (ContainerHash.IsZero()
				|| std::ranges::any_of(File.BulkPayloads, [&](const FAuthoredBulkPayload& Payload) {
					return Payload.Descriptor.ContainerHash != ContainerHash;
				}))
				return Error(EAssetError::CorruptFile,
					"Serialized authored bulk payloads disagree on container identity.");
			std::vector<uint8> CompanionBytes;
			std::string CompanionError;
			if (!BuildAuthoredBulkCompanion(
					File.BulkPayloads, ContainerHash, CompanionBytes, &CompanionError)
				|| !ResolveAuthoredBulkCompanionPath(
					Destination, ContainerHash, PublishedCompanion, &CompanionError))
				return Error(EAssetError::CorruptFile, std::move(CompanionError));
			if (!FFileHelper::SaveArrayToFileAtomically(
					std::span{reinterpret_cast<const std::byte*>(CompanionBytes.data()),
						CompanionBytes.size()}, PublishedCompanion, &PublicationError))
				return Error(EAssetError::IoError, PublicationError.ToString());
		}
		if (!FFileHelper::SaveArrayToFileAtomically(
			std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()},
			Destination,
			&PublicationError
		))
		{
			return Error(EAssetError::IoError, PublicationError.ToString());
		}
		Package->ClearDirty();
		const auto LastWriteTime = std::filesystem::last_write_time(Destination);
		Registry.AddOrUpdate(FAssetData{
			.PackagePath = Path,
			.PhysicalPath = Destination.generic_string(),
			.AssetClassName = File.AssetClassName,
			.EntryKind = File.EntryKind,
			.RedirectDestination = File.RedirectDestination,
			.FormatVersion = OrdinaryAssetPackageWriterVersion,
			.Dependencies = File.Dependencies,
			.FileSize = std::filesystem::file_size(Destination),
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)});
		if (auto Resident = ResidentPackages.find(Path);
			Resident != ResidentPackages.end()
			&& Resident->second.Package == Package)
			Resident->second.PublicationState =
				EAssetPackagePublicationState::Published;
		CleanupStaleAuthoredBulkCompanions(Destination, PublishedCompanion);
		return {};
	}

	namespace
	{
		auto RebuildReferenceProjectionForPublishedEntriesImpl(
			std::span<const FAssetMutationJournalEntry> Entries,
			const std::unordered_map<FAssetPath, FAssetData>& Assets,
			std::vector<FAssetReferenceEdge>& Edges,
			std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints)
			-> FAssetResult
		{
			for (const FAssetMutationJournalEntry& Entry : Entries)
			{
				if (!Entry.RegistryPath.IsValid()) continue;
				std::erase_if(Edges, [&](const FAssetReferenceEdge& Edge) {
					return Edge.SourcePackage == Entry.RegistryPath;
				});
				Fingerprints.erase(Entry.RegistryPath);
				const auto Data = Assets.find(Entry.RegistryPath);
				if (Data == Assets.end()) continue;
				FAssetPackageInspection Inspection;
				FAssetResult Result = InspectAssetPackage(
					Data->second.PhysicalPath, Inspection);
				if (!Result) return Result;
				std::vector<FAssetReferenceEdge> SourceEdges;
				Result = ExtractAssetReferences(
					Entry.RegistryPath, Inspection, SourceEdges);
				if (!Result) return Result;
				Edges.insert(Edges.end(),
					std::make_move_iterator(SourceEdges.begin()),
					std::make_move_iterator(SourceEdges.end()));
				Fingerprints.insert_or_assign(
					Entry.RegistryPath, Inspection.Fingerprint);
			}
			std::ranges::sort(Edges, &AssetReferenceLess);
			return {};
		}
	}

	namespace Private
	{
		auto RebuildReferenceProjectionForPublishedEntries(
			std::span<const FAssetMutationJournalEntry> Entries,
			const std::unordered_map<FAssetPath, FAssetData>& Assets,
			std::vector<FAssetReferenceEdge>& Edges,
			std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints)
			-> FAssetResult
		{
			return RebuildReferenceProjectionForPublishedEntriesImpl(
				Entries, Assets, Edges, Fingerprints);
		}
	}



}
