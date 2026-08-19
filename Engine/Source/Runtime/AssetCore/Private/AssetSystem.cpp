#include "AssetSystemInternal.h"
#include "AssetPackageV4Reader.h"
#include "AssetPackageCodec.h"
#include "AssetPackageVersionPolicy.h"
#include "AssetRedirector.h"
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
#include "Misc/DerivedDataCache.h"
#include "Misc/LexicalPath.h"
#include "Misc/Paths.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
{
	namespace
	{
		thread_local FAssetLoadReport* GActiveAssetLoadReport = nullptr;
		thread_local uint64 GActivePackageFileReadCount = 0;

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

		struct FRegisteredOwnedPayloadRelocator
		{
			FAssetOwnedPayloadRelocatorHandle Handle = 0;
			FModuleOwnedResourceLease OwnerResource;
			FAssetOwnedPayloadRelocator Relocator;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto GetOwnedPayloadRelocators()
			-> std::unordered_map<DClass*, FRegisteredOwnedPayloadRelocator>&
		{
			static std::unordered_map<DClass*, FRegisteredOwnedPayloadRelocator> Relocators;
			return Relocators;
		}

		auto NextOwnedPayloadRelocatorHandle()
			-> FAssetOwnedPayloadRelocatorHandle&
		{
			static FAssetOwnedPayloadRelocatorHandle Handle = 1;
			return Handle;
		}

		struct FRegisteredAssetMoveObserver
		{
			FModuleOwnedResourceLease OwnerResource;
			IAssetMoveObserver* Observer = nullptr;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto GetMoveObservers()
			-> std::map<FAssetMoveObserverHandle, FRegisteredAssetMoveObserver>&
		{
			static std::map<FAssetMoveObserverHandle, FRegisteredAssetMoveObserver> Observers;
			return Observers;
		}

		auto NextMoveObserverHandle() -> FAssetMoveObserverHandle&
		{
			static FAssetMoveObserverHandle Handle = 1;
			return Handle;
		}

		struct FAssetReferenceStoreRegistry
		{
			struct FEntry
			{
				FModuleOwnedResourceLease OwnerResource;
				IAssetReferenceStore* Store = nullptr;
				FModuleOwnedCallbackGate OwnerGate;
			};
			std::map<FAssetReferenceStoreHandle, FEntry> Stores;
			FAssetReferenceStoreHandle NextHandle = 1;
			uint64 Revision = 1;
		};

		auto GetAssetReferenceStoreRegistry() -> FAssetReferenceStoreRegistry&
		{
			static FAssetReferenceStoreRegistry Registry;
			return Registry;
		}

		struct FRelocationFailureInjection
		{
			std::map<EAssetRelocationFailurePoint, uint32> RemainingOccurrences;
		};

		auto GetRelocationFailureInjection() -> FRelocationFailureInjection&
		{
			static FRelocationFailureInjection Injection;
			return Injection;
		}

		auto ConsumeRelocationFailure(
			EAssetRelocationFailurePoint Point) -> bool
		{
			FRelocationFailureInjection& Injection =
				GetRelocationFailureInjection();
			auto Injected = Injection.RemainingOccurrences.find(Point);
			if (Injected == Injection.RemainingOccurrences.end()
				|| Injected->second == 0)
				return false;
			if (--Injected->second != 0) return false;
			Injection.RemainingOccurrences.erase(Injected);
			return true;
		}

		struct FFixupFailureInjection
		{
			std::map<EAssetRedirectorFixupFailurePoint, uint32> RemainingOccurrences;
		};

		auto GetFixupFailureInjection() -> FFixupFailureInjection&
		{
			static FFixupFailureInjection Injection;
			return Injection;
		}

		auto ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint Point) -> bool
		{
			auto& Remaining = GetFixupFailureInjection().RemainingOccurrences;
			auto Injected = Remaining.find(Point);
			if (Injected == Remaining.end() || Injected->second == 0) return false;
			if (--Injected->second != 0) return false;
			Remaining.erase(Injected);
			return true;
		}

		struct FRegisteredDeleteContributor
		{
			FAssetDeleteContributorHandle Handle = 0;
			FModuleOwnedResourceLease OwnerResource;
			FAssetDeleteContributor Contributor;
			FModuleOwnedCallbackGate OwnerGate;
		};

		auto GetDeleteContributors()
			-> std::unordered_map<DClass*, FRegisteredDeleteContributor>&
		{
			static std::unordered_map<DClass*, FRegisteredDeleteContributor> Contributors;
			return Contributors;
		}

		auto NextDeleteContributorHandle() -> FAssetDeleteContributorHandle&
		{
			static FAssetDeleteContributorHandle Handle = 1;
			return Handle;
		}

		auto InspectAssetCompanionFiles(
			const FAssetData& Data,
			std::vector<std::filesystem::path>& OutFiles,
			bool* OutHasContributor = nullptr) -> FAssetResult
		{
			OutFiles.clear();
			if (OutHasContributor) *OutHasContributor = false;
			DClass* AssetClass = FindClassByQualifiedName(FName(Data.AssetClassName));
			for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
			{
				const auto It = GetDeleteContributors().find(Class);
				if (It == GetDeleteContributors().end()) continue;
				if (OutHasContributor) *OutHasContributor = true;
				FAssetPackageInspection Inspection;
				auto Call = It->second.OwnerGate.TryEnter();
				if (It->second.OwnerGate.IsValid() && !Call)
					return Error(EAssetError::StaleData,
						"The asset deletion contributor is unavailable.");
				FAssetResult Result = InspectAssetPackage(Data.PhysicalPath, Inspection);
				if (!Result) return Result;
				FAssetDeleteContribution Contribution;
				Result = It->second.Contributor(Data, Inspection, Contribution);
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
			uint32 Magic = 0;
			uint32 Version = 0;
			if (Bytes.size() >= sizeof(Magic) + sizeof(Version))
			{
				std::memcpy(&Magic, Bytes.data(), sizeof(Magic));
				std::memcpy(&Version, Bytes.data() + sizeof(Magic), sizeof(Version));
			}
			OutFingerprint = {
				.FileSize = Bytes.size(),
				.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime),
				.ContentHash = FXxHash128::HashBuffer(Bytes),
				.ReaderVersion = Magic == DastPackageMagic ? Version : 0};
			return {};
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
					FAssetResult Result = FAssetRuntimeState::Get().LoadAsset(Path, Value);
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
				return FAssetRuntimeState::Get().LoadAsset(Path, OutObject);
			}
			return Error(EAssetError::CorruptFile, "Unknown object reference kind.");
		}

		constexpr uint32 MaximumReferenceContainerDepth = 4;
		constexpr uint64 MaximumReferencesPerPackage = 100000;
		constexpr uint64 MaximumReferencesPerSnapshot = 1000000;
		constexpr uint64 MaximumReferenceDisplayRouteBytes = 4 * 1024;
		constexpr uint64 MaximumReferenceRouteTokenBytes = 1024 * 1024;

		auto ContainsAssetReferencePropertyImpl(
			const FProperty* Property,
			std::unordered_set<const DStruct*>& VisitingStructs) -> bool
		{
			if (!Property) return false;
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Object:
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
				return true;
			case DurinCodeGen::EPropertyGenFlags::Array:
				return ContainsAssetReferencePropertyImpl(
					static_cast<const FArrayProperty*>(Property)->GetInner(), VisitingStructs);
			case DurinCodeGen::EPropertyGenFlags::Map:
				return ContainsAssetReferencePropertyImpl(
					static_cast<const FMapProperty*>(Property)->GetKeyProp(), VisitingStructs)
					|| ContainsAssetReferencePropertyImpl(
						static_cast<const FMapProperty*>(Property)->GetValueProp(), VisitingStructs);
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
				if (!Struct || !VisitingStructs.insert(Struct).second) return false;
				bool bContainsReference = false;
				Struct->ForEachProperty([&](FProperty* Field) {
					if (!bContainsReference && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
						bContainsReference = ContainsAssetReferencePropertyImpl(Field, VisitingStructs);
				}, false);
				VisitingStructs.erase(Struct);
				return bContainsReference;
			}
			default:
				return false;
			}
		}

		auto ContainsAssetReferenceProperty(const FProperty* Property) -> bool
		{
			std::unordered_set<const DStruct*> VisitingStructs;
			return ContainsAssetReferencePropertyImpl(Property, VisitingStructs);
		}

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
					if (!bContainsSoftObject && Field
						&& !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
						bContainsSoftObject = ContainsSoftObjectPropertyImpl(
							Field, VisitingStructs);
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

		auto CompareReferenceRoute(
			std::span<const FAssetReferenceRouteSegment> Left,
			std::span<const FAssetReferenceRouteSegment> Right) -> int
		{
			const size_t Count = std::min(Left.size(), Right.size());
			for (size_t Index = 0; Index < Count; ++Index)
			{
				if (Left[Index].Kind != Right[Index].Kind)
					return static_cast<uint8>(Left[Index].Kind) < static_cast<uint8>(Right[Index].Kind) ? -1 : 1;
				if (Left[Index].Kind == EAssetReferenceRouteKind::MapValue)
				{
					if (Left[Index].MapKeyToken != Right[Index].MapKeyToken)
						return std::ranges::lexicographical_compare(
							Left[Index].MapKeyToken, Right[Index].MapKeyToken) ? -1 : 1;
				}
				else if (Left[Index].Kind == EAssetReferenceRouteKind::StructField)
				{
					const auto LeftField = std::tie(
						Left[Index].DeclaringType, Left[Index].FieldName);
					const auto RightField = std::tie(
						Right[Index].DeclaringType, Right[Index].FieldName);
					if (LeftField != RightField) return LeftField < RightField ? -1 : 1;
				}
				else if (Left[Index].Index != Right[Index].Index)
					return Left[Index].Index < Right[Index].Index ? -1 : 1;
			}
			if (Left.size() == Right.size()) return 0;
			return Left.size() < Right.size() ? -1 : 1;
		}

		auto AssetReferenceLess(const FAssetReferenceEdge& Left, const FAssetReferenceEdge& Right) -> bool
		{
			const auto LeftKey = std::tuple(
				Left.TargetPath.GetView(), Left.SourcePackage.GetView(), Left.SourceObjectId,
				std::string_view(Left.DeclaringType), std::string_view(Left.FieldName), Left.Kind);
			const auto RightKey = std::tuple(
				Right.TargetPath.GetView(), Right.SourcePackage.GetView(), Right.SourceObjectId,
				std::string_view(Right.DeclaringType), std::string_view(Right.FieldName), Right.Kind);
			if (LeftKey != RightKey) return LeftKey < RightKey;
			return CompareReferenceRoute(Left.Route, Right.Route) < 0;
		}

		auto AppendMapTokenDisplay(std::string& Path, std::span<const uint8> Token) -> void
		{
			Path.append("[key:");
			for (const uint8 Byte : Token) Path.append(std::format("{:02x}", static_cast<uint32>(Byte)));
			Path.push_back(']');
		}

		struct FReferenceExtractionContext
		{
			const FAssetPath& SourcePackage;
			const FAssetPackageFingerprint& Fingerprint;
			const FAssetPackageObjectInspection& Object;
			std::string_view DeclaringType;
			std::string_view FieldName;
			EAssetReferenceKind ObjectKind = EAssetReferenceKind::HardObject;
			std::vector<FAssetReferenceEdge>& References;
		};

		auto ExtractReferenceValue(
			FProperty* Property,
			FByteReader& Reader,
			const FReferenceExtractionContext& Context,
			std::vector<FAssetReferenceRouteSegment>& Route,
			const std::string& PropertyPath,
			uint32 ContainerDepth) -> FAssetResult;

		auto ExtractReferencePropertyValues(
			FProperty* Property,
			std::span<const uint8> Payload,
			const FReferenceExtractionContext& Context,
			std::vector<FAssetReferenceRouteSegment>& Route,
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
					if (ContainerDepth >= MaximumReferenceContainerDepth)
						return Error(EAssetError::CorruptFile,
							"AssetReferenceIndexDepthExceeded: fixed-array route exceeds four levels.");
					Route.push_back({
						.Kind = EAssetReferenceRouteKind::FixedArray,
						.Index = ArrayIndex});
					ElementPath.append(std::format("[fixed:{}]", ArrayIndex));
				}
				FAssetResult Result = ExtractReferenceValue(
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

		auto ExtractReferenceValue(
			FProperty* Property,
			FByteReader& Reader,
			const FReferenceExtractionContext& Context,
			std::vector<FAssetReferenceRouteSegment>& Route,
			const std::string& PropertyPath,
			uint32 ContainerDepth) -> FAssetResult
		{
			if (!Property)
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceSchemaMismatch: reflected property metadata is missing.");
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				if (!ObjectProperty->IsObjectPtrWrapper())
					return Error(EAssetError::UnsupportedProperty,
						"AssetReferenceSchemaMismatch: raw object pointers are unsupported.");
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind))
					return Error(EAssetError::CorruptFile,
						std::format("AssetReferencePayloadTruncated: {} has no reference tag.", PropertyPath));
				if (ReferenceKind == 0) return {};
				if (ReferenceKind == 1)
				{
					uint64 ObjectId = 0;
					if (!Reader.Read(ObjectId) || ObjectId == 0)
						return Error(EAssetError::InvalidObjectGraph,
							std::format("AssetReferenceInternalObject: {} has an invalid object id.", PropertyPath));
					return {};
				}
				if (ReferenceKind != 2)
					return Error(EAssetError::CorruptFile,
						std::format("AssetReferencePayloadTag: {} has unknown tag {}.", PropertyPath, ReferenceKind));
				std::string PathString;
				FAssetPath TargetPath;
				if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
					|| !FAssetPath::TryCreate(PathString, TargetPath))
					return Error(EAssetError::InvalidPath,
						std::format("AssetReferenceInvalidPath: {} has an invalid external path.", PropertyPath));
				DClass* ExpectedClass = ObjectProperty->GetReferencedClass();
				if (!ExpectedClass)
					return Error(EAssetError::TypeMismatch,
						std::format("AssetReferenceSchemaMismatch: {} has no referenced class.", PropertyPath));
				if (PropertyPath.size() > MaximumReferenceDisplayRouteBytes)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexDisplayRouteExceeded: display route exceeds 4 KiB.");
				if (Context.References.size() >= MaximumReferencesPerPackage)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexOccurrenceExceeded: package exceeds 100,000 occurrences.");
				Context.References.push_back({
					.SourcePackage = Context.SourcePackage,
					.SourceFingerprint = Context.Fingerprint,
					.SourceObjectId = Context.Object.Id,
					.SourceClass = Context.Object.ClassName,
					.DeclaringType = std::string(Context.DeclaringType),
					.FieldName = std::string(Context.FieldName),
					.Kind = Context.ObjectKind,
					.ExpectedClass = ExpectedClass->GetQualifiedName().ToString(),
					.TargetPath = std::move(TargetPath),
					.Route = Route,
					.DisplayRoute = PropertyPath});
				return {};
			}
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
				if (PropertyPath.size() > MaximumReferenceDisplayRouteBytes)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexDisplayRouteExceeded: display route exceeds 4 KiB.");
				if (Context.References.size() >= MaximumReferencesPerPackage)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexOccurrenceExceeded: package exceeds 100,000 occurrences.");
				Context.References.push_back({
					.SourcePackage = Context.SourcePackage,
					.SourceFingerprint = Context.Fingerprint,
					.SourceObjectId = Context.Object.Id,
					.SourceClass = Context.Object.ClassName,
					.DeclaringType = std::string(Context.DeclaringType),
					.FieldName = std::string(Context.FieldName),
					.Kind = EAssetReferenceKind::SoftObject,
					.ExpectedClass = ExpectedClass->GetQualifiedName().ToString(),
					.TargetPath = SoftPath.GetAssetPath(),
					.Route = Route,
					.DisplayRoute = PropertyPath});
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				if (ContainerDepth >= MaximumReferenceContainerDepth)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexDepthExceeded: Array route exceeds four levels.");
				auto* Array = static_cast<FArrayProperty*>(Property);
				uint64 Count = 0;
				if (!Array->GetInner() || !Reader.Read(Count) || Count > 10000000)
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferenceArrayPayload: {} has an invalid count.", PropertyPath));
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					Route.push_back({
						.Kind = EAssetReferenceRouteKind::ArrayElement,
						.Index = Index});
					FAssetResult Result = ExtractReferenceValue(
						Array->GetInner(), Reader, Context, Route,
						std::format("{}[{}]", PropertyPath, Index), ContainerDepth + 1);
					Route.pop_back();
					if (!Result) return Result;
				}
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				if (ContainerDepth >= MaximumReferenceContainerDepth)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexDepthExceeded: Map route exceeds four levels.");
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Count = 0;
				if (!Map->GetKeyProp() || !Map->GetValueProp() || !Reader.Read(Count) || Count > 10000000)
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferenceMapPayload: {} has an invalid count.", PropertyPath));
				if (ContainsAssetReferenceProperty(Map->GetKeyProp()))
					return Error(EAssetError::TypeMismatch,
						"AssetReferenceSchemaMismatch: reference Map keys are unsupported.");
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FReflectedValueStorage KeyStorage;
					std::string StorageError;
					if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError))
						return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
					FAssetResult KeyResult = DecodeByteToolValue(
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
					if (KeyToken.size() > MaximumReferenceRouteTokenBytes)
						return Error(EAssetError::CorruptFile,
							"AssetReferenceIndexRouteTokenExceeded: Map key token exceeds 1 MiB.");
					std::string ValuePath = PropertyPath;
					AppendMapTokenDisplay(ValuePath, KeyToken);
					if (ValuePath.size() > MaximumReferenceDisplayRouteBytes)
						return Error(EAssetError::CorruptFile,
							"AssetReferenceIndexDisplayPathExceeded: display path exceeds 4 KiB.");
					Route.push_back({
						.Kind = EAssetReferenceRouteKind::MapValue,
						.MapKeyToken = std::move(KeyToken)});
					FAssetResult Result = ExtractReferenceValue(
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
					FProperty* Field = Struct->FindPropertyBySerializedName(FName(FieldName), false);
					if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
						|| !ContainsAssetReferenceProperty(Field)) continue;
					if (static_cast<uint8>(Field->GetKind()) != Kind
						|| !IsSerializedTypeSignatureCompatible(Field, Signature))
						return Error(EAssetError::TypeMismatch, std::format(
							"SoftReferenceSchemaMismatch: {}.{} has an incompatible signature.",
							PropertyPath, FieldName));
					Route.push_back({
						.Kind = EAssetReferenceRouteKind::StructField,
						.DeclaringType = DeclaringStruct,
						.FieldName = FieldName});
					FAssetResult Result = ExtractReferencePropertyValues(
						Field, FieldPayload, Context, Route,
						std::format("{}.{}", PropertyPath, FieldName), ContainerDepth);
					Route.pop_back();
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
			if (ContainerDepth > MaximumReferenceContainerDepth)
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

		auto FindFixupDestination(
			const FAssetPath& Source,
			std::span<const FAssetRedirectorFixupMapping> Mappings) -> const FAssetPath*
		{
			const auto It = std::ranges::find(Mappings, Source,
				&FAssetRedirectorFixupMapping::RedirectorPath);
			return It == Mappings.end() ? nullptr : &It->FinalPath;
		}

		auto RewriteSerializedReferenceValue(
			FProperty* Property,
			FByteReader& Reader,
			FByteWriter& Writer,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64& RewriteCount,
			uint32 ContainerDepth) -> FAssetResult;

		auto RewriteSerializedReferenceProperty(
			FProperty* Property,
			std::span<const uint8> Payload,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			std::vector<uint8>& OutPayload,
			uint64& RewriteCount,
			uint32 ContainerDepth = 0) -> FAssetResult
		{
			FByteReader Reader{Payload};
			FByteWriter Writer;
			for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
			{
				FAssetResult Result = RewriteSerializedReferenceValue(
					Property, Reader, Writer, Mappings, RewriteCount,
					ContainerDepth + (Property->GetArrayDim() > 1 ? 1 : 0));
				if (!Result) return Result;
			}
			if (Reader.Offset != Payload.size())
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupTrailingBytes: field payload has trailing bytes.");
			OutPayload = std::move(Writer.Bytes);
			return {};
		}

		auto RewriteSerializedReferenceValue(
			FProperty* Property,
			FByteReader& Reader,
			FByteWriter& Writer,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64& RewriteCount,
			uint32 ContainerDepth) -> FAssetResult
		{
			if (!Property || ContainerDepth > MaximumReferenceContainerDepth)
				return Error(EAssetError::TypeMismatch,
					"AssetReferenceFixupSchemaMismatch: serialized container metadata is invalid.");
			switch (Property->GetKind())
			{
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				uint8 Kind = 0;
				if (!Reader.Read(Kind))
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupTruncated: missing object reference tag.");
				Writer.Write(Kind);
				if (Kind == 0) return {};
				if (Kind == 1)
				{
					uint64 ObjectId = 0;
					if (!Reader.Read(ObjectId) || ObjectId == 0)
						return Error(EAssetError::InvalidObjectGraph,
							"AssetReferenceFixupInternalObject: invalid object id.");
					Writer.Write(ObjectId);
					return {};
				}
				if (Kind != 2)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupTag: unknown object reference tag.");
				std::string PathString;
				FAssetPath Path;
				if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
					|| !FAssetPath::TryCreate(PathString, Path))
					return Error(EAssetError::InvalidPath,
						"AssetReferenceFixupPath: invalid external object path.");
				if (const FAssetPath* Destination = FindFixupDestination(Path, Mappings))
				{
					Writer.WriteString(Destination->GetView());
					++RewriteCount;
				}
				else Writer.WriteString(PathString);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				uint8 Kind = 0;
				if (!Reader.Read(Kind))
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupTruncated: missing soft reference tag.");
				Writer.Write(Kind);
				if (Kind == 0) return {};
				if (Kind != 1)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupTag: unknown soft reference tag.");
				std::string PathString;
				FSoftObjectPath Path;
				std::string PathError;
				if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
					|| PathString.empty())
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupPath: soft path is truncated or overlong.");
				if (!FSoftObjectPath::TryCreate(PathString, Path, &PathError))
					return Error(EAssetError::InvalidPath, std::move(PathError));
				if (const FAssetPath* Destination = FindFixupDestination(
					Path.GetAssetPath(), Mappings))
				{
					Writer.WriteString(Destination->GetView());
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
						"AssetReferenceFixupArrayPayload: invalid count.");
				Writer.Write(Count);
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					FAssetResult Result = RewriteSerializedReferenceValue(
						Array->GetInner(), Reader, Writer, Mappings,
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
					|| ContainsAssetReferenceProperty(Map->GetKeyProp()))
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupMapPayload: invalid map schema or count.");
				Writer.Write(Count);
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					const size_t KeyOffset = Reader.Offset;
					FReflectedValueStorage KeyStorage;
					std::string StorageError;
					if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError))
						return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
					FAssetResult Result = DecodeByteToolValue(
						Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Reader, {});
					if (!Result) return Result;
					Writer.WriteBytes(Reader.Bytes.subspan(KeyOffset, Reader.Offset - KeyOffset));
					Result = RewriteSerializedReferenceValue(
						Map->GetValueProp(), Reader, Writer, Mappings,
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
						"AssetReferenceFixupStructPayload: incompatible header.");
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
							"AssetReferenceFixupStructPayload: malformed field.");
					Writer.WriteString(DeclaringStruct);
					Writer.WriteString(FieldName);
					Writer.Write(Kind);
					Writer.WriteString(Signature);
					FProperty* Field = DeclaringStruct == StructName
						? Struct->FindPropertyBySerializedName(FName(FieldName), false) : nullptr;
					if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
						|| !ContainsAssetReferenceProperty(Field))
					{
						Writer.Write(PayloadSize);
						Writer.WriteBytes(FieldPayload.data(), FieldPayload.size());
						continue;
					}
					if (static_cast<uint8>(Field->GetKind()) != Kind
						|| !IsSerializedTypeSignatureCompatible(Field, Signature))
						return Error(EAssetError::TypeMismatch,
							"AssetReferenceFixupSchemaMismatch: struct field signature changed.");
					std::vector<uint8> RewrittenPayload;
					FAssetResult Result = RewriteSerializedReferenceProperty(
						Field, FieldPayload, Mappings, RewrittenPayload,
						RewriteCount, ContainerDepth);
					if (!Result) return Result;
					Writer.Write(uint64(RewrittenPayload.size()));
					Writer.WriteBytes(RewrittenPayload.data(), RewrittenPayload.size());
				}
				return {};
			}
			default:
				return Error(EAssetError::TypeMismatch,
					"AssetReferenceFixupSchemaMismatch: unsupported serialized reference container.");
			}
		}

		auto RewritePackageReferences(
			std::span<const uint8> Bytes,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedRewriteCount,
			std::vector<uint8>& OutBytes) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			if (!Codec->bCanMutate || Codec->FormatVersion != OrdinaryAssetPackageWriterVersion)
				return Error(EAssetError::UnsupportedVersion,
					"Reference rewrite requires the ordinary-format mutation capability.");
			return Codec->RewriteReferences(Bytes, Mappings, ExpectedRewriteCount, OutBytes);
		}

		auto ReadPackageMetadata(
			std::span<const uint8> Bytes, FPackageFile& OutFile) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			FAssetPackageHeader Header;
			if (FAssetResult Result = Codec->ReadHeader(
				Bytes, Bytes.size(), Header); !Result)
				return Result;
			FPackageFile File{
				.FormatVersion = Header.FormatVersion,
				.AssetClassName = std::move(Header.AssetClassName),
				.EntryKind = Header.EntryKind};
			File.RedirectDestination = std::move(Header.RedirectDestination);
			File.Dependencies = std::move(Header.Dependencies);
			OutFile = std::move(File);
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
			if (!Reader.ReadAndValidateHeader(DerivedDataCache::AssetRegistryMagic, DerivedDataCache::AssetRegistrySchemaVersion, AssetPackageReaderPolicyFingerprint)
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
					|| !IsSupportedAssetPackageReaderVersion(Entry.FormatVersion)
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
			Writer.WriteHeader({DerivedDataCache::AssetRegistryMagic, DerivedDataCache::AssetRegistrySchemaVersion, AssetPackageReaderPolicyFingerprint});
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

		constexpr uint32 AssetReferenceIndexMagic = 0x58495241; // ARIX
		constexpr uint32 AssetReferenceIndexSchemaVersion = 1;
		constexpr uint32 AssetReferenceExtractorSchemaVersion = 1;
		constexpr uintmax_t MaximumReferenceCacheBytes = 1024ull * 1024ull * 1024ull;

		struct FReferenceCacheSource
		{
			FAssetPackageFingerprint Fingerprint;
			std::vector<FAssetReferenceEdge> References;
		};

		auto ReferenceCachePath() -> std::filesystem::path
		{
			return std::filesystem::path(FPaths::DerivedDataCacheDir())
				/ "AssetRegistry" / "References.bin";
		}

		auto LoadReferenceCache(
			std::unordered_map<FAssetPath, FReferenceCacheSource>& OutSources,
			std::string& OutWarning) -> bool
		{
			OutSources.clear();
			const std::filesystem::path Path = ReferenceCachePath();
			std::error_code Ec;
			if (!std::filesystem::exists(Path, Ec)) return false;
			const uintmax_t Size = std::filesystem::file_size(Path, Ec);
			if (Ec || Size > MaximumReferenceCacheBytes)
			{
				OutWarning = std::format("Ignoring invalid asset-reference cache {}.", Path.generic_string());
				return false;
			}
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Path.generic_string()))
			{
				OutWarning = std::format("Failed to read asset-reference cache {}.", Path.generic_string());
				return false;
			}
			DerivedDataCache::FReader Reader(Bytes);
			uint32 ExtractorSchema = 0;
			uint64 SourceCount = 0;
			if (!Reader.ReadAndValidateHeader(
					AssetReferenceIndexMagic, AssetReferenceIndexSchemaVersion, AssetPackageReaderPolicyFingerprint)
				|| !Reader.ReadU32(ExtractorSchema)
				|| ExtractorSchema != AssetReferenceExtractorSchemaVersion
				|| !Reader.ReadU64(SourceCount) || SourceCount > MaximumRegistryEntries)
			{
				OutWarning = "Ignoring incompatible or corrupt asset-reference cache header.";
				return false;
			}
			uint64 TotalOccurrences = 0;
			for (uint64 SourceIndex = 0; SourceIndex < SourceCount; ++SourceIndex)
			{
				std::string SourceString;
				FAssetPath SourcePath;
				FReferenceCacheSource Source;
				uint64 FileSize = 0;
				uint64 OccurrenceCount = 0;
				if (!Reader.ReadString(SourceString, MaximumPackageStringBytes)
					|| !FAssetPath::TryCreate(SourceString, SourcePath)
					|| !Reader.ReadU64(FileSize)
					|| !Reader.ReadI64(Source.Fingerprint.LastWriteTimeTicks)
					|| !Reader.ReadU64(Source.Fingerprint.ContentHash.HashLow)
					|| !Reader.ReadU64(Source.Fingerprint.ContentHash.HashHigh)
					|| !Reader.ReadU32(Source.Fingerprint.ReaderVersion)
					|| !IsSupportedAssetPackageReaderVersion(Source.Fingerprint.ReaderVersion)
					|| !Reader.ReadU64(OccurrenceCount)
					|| OccurrenceCount > MaximumReferencesPerPackage
					|| TotalOccurrences > MaximumReferencesPerSnapshot - OccurrenceCount)
				{
					OutWarning = "Ignoring corrupt asset-reference cache source record.";
					OutSources.clear();
					return false;
				}
				Source.Fingerprint.FileSize = static_cast<uintmax_t>(FileSize);
				TotalOccurrences += OccurrenceCount;
				Source.References.reserve(static_cast<size_t>(OccurrenceCount));
				for (uint64 OccurrenceIndex = 0; OccurrenceIndex < OccurrenceCount; ++OccurrenceIndex)
				{
					FAssetReferenceEdge Reference{
						.SourcePackage = SourcePath,
						.SourceFingerprint = Source.Fingerprint};
					std::string TargetString;
					uint32 RouteCount = 0;
					uint8 ReferenceKind = 0;
					if (!Reader.ReadU64(Reference.SourceObjectId)
						|| !Reader.ReadString(Reference.SourceClass, MaximumPackageStringBytes)
						|| !Reader.ReadString(Reference.DeclaringType, MaximumPackageStringBytes)
						|| !Reader.ReadString(Reference.FieldName, MaximumPackageStringBytes)
						|| !Reader.ReadU8(ReferenceKind)
						|| ReferenceKind > static_cast<uint8>(EAssetReferenceKind::Redirect)
						|| !Reader.ReadString(Reference.ExpectedClass, MaximumPackageStringBytes)
						|| !Reader.ReadString(TargetString, MaximumPackageStringBytes)
						|| !FAssetPath::TryCreate(TargetString, Reference.TargetPath)
						|| !Reader.ReadU32(RouteCount)
						|| RouteCount > MaximumReferenceContainerDepth)
					{
						OutWarning = "Ignoring corrupt asset-reference cache occurrence.";
						OutSources.clear();
						return false;
					}
					Reference.Kind = static_cast<EAssetReferenceKind>(ReferenceKind);
					Reference.Route.reserve(RouteCount);
					for (uint32 RouteIndex = 0; RouteIndex < RouteCount; ++RouteIndex)
					{
						uint8 Kind = 0;
						uint64 TokenBytes = 0;
						FAssetReferenceRouteSegment Segment;
						if (!Reader.ReadU8(Kind)
							|| Kind > static_cast<uint8>(EAssetReferenceRouteKind::StructField)
							|| !Reader.ReadU64(Segment.Index)
							|| !Reader.ReadU64(TokenBytes)
							|| !Reader.ReadBytes(
								Segment.MapKeyToken, TokenBytes, MaximumReferenceRouteTokenBytes)
							|| !Reader.ReadString(Segment.DeclaringType, MaximumPackageStringBytes)
							|| !Reader.ReadString(Segment.FieldName, MaximumPackageStringBytes))
						{
							OutWarning = "Ignoring corrupt asset-reference cache route.";
							OutSources.clear();
							return false;
						}
						Segment.Kind = static_cast<EAssetReferenceRouteKind>(Kind);
						if ((Segment.Kind == EAssetReferenceRouteKind::MapValue)
							!= !Segment.MapKeyToken.empty()
							|| (Segment.Kind == EAssetReferenceRouteKind::StructField)
							!= (!Segment.DeclaringType.empty() && !Segment.FieldName.empty()))
						{
							OutWarning = "Ignoring inconsistent asset-reference cache route.";
							OutSources.clear();
							return false;
						}
						Reference.Route.push_back(std::move(Segment));
					}
					if (!Reader.ReadString(
						Reference.DisplayRoute, MaximumReferenceDisplayRouteBytes)
						|| Reference.DisplayRoute.empty())
					{
						OutWarning = "Ignoring invalid asset-reference cache display route.";
						OutSources.clear();
						return false;
					}
					Source.References.push_back(std::move(Reference));
				}
				if (!OutSources.emplace(SourcePath, std::move(Source)).second)
				{
					OutWarning = "Ignoring duplicate asset-reference cache source.";
					OutSources.clear();
					return false;
				}
			}
			if (!Reader.IsAtEnd())
			{
				OutWarning = "Ignoring asset-reference cache with trailing data.";
				OutSources.clear();
				return false;
			}
			return true;
		}

		auto WriteReferenceCache(
			const std::unordered_map<FAssetPath, FAssetPackageFingerprint>& Fingerprints,
			std::span<const FAssetReferenceEdge> References,
			std::string& OutWarning) -> bool
		{
			if (Fingerprints.size() > MaximumRegistryEntries
				|| References.size() > MaximumReferencesPerSnapshot)
			{
				OutWarning = "Asset-reference index exceeds its persisted snapshot bounds.";
				return false;
			}
			std::unordered_map<FAssetPath, std::vector<const FAssetReferenceEdge*>> BySource;
			for (const FAssetReferenceEdge& Reference : References)
				BySource[Reference.SourcePackage].push_back(&Reference);
			std::vector<FAssetPath> Sources;
			Sources.reserve(Fingerprints.size());
			for (const auto& [Source, Fingerprint] : Fingerprints) Sources.push_back(Source);
			std::ranges::sort(Sources, [](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() < Right.GetView();
			});

			DerivedDataCache::FWriter Writer;
			Writer.WriteHeader({AssetReferenceIndexMagic, AssetReferenceIndexSchemaVersion, AssetPackageReaderPolicyFingerprint});
			Writer.WriteU32(AssetReferenceExtractorSchemaVersion);
			Writer.WriteU64(Sources.size());
			for (const FAssetPath& Source : Sources)
			{
				const FAssetPackageFingerprint& Fingerprint = Fingerprints.at(Source);
				const auto ReferencesIt = BySource.find(Source);
				const size_t ReferenceCount = ReferencesIt == BySource.end()
					? 0 : ReferencesIt->second.size();
				if (ReferenceCount > MaximumReferencesPerPackage)
				{
					OutWarning = std::format(
						"Asset-reference source {} exceeds its occurrence bound.", Source.ToString());
					return false;
				}
				Writer.WriteString(Source.GetView());
				Writer.WriteU64(static_cast<uint64>(Fingerprint.FileSize));
				Writer.WriteI64(Fingerprint.LastWriteTimeTicks);
				Writer.WriteU64(Fingerprint.ContentHash.HashLow);
				Writer.WriteU64(Fingerprint.ContentHash.HashHigh);
				Writer.WriteU32(Fingerprint.ReaderVersion);
				Writer.WriteU64(ReferenceCount);
				if (ReferencesIt == BySource.end()) continue;
				for (const FAssetReferenceEdge* Reference : ReferencesIt->second)
				{
					Writer.WriteU64(Reference->SourceObjectId);
					Writer.WriteString(Reference->SourceClass);
					Writer.WriteString(Reference->DeclaringType);
					Writer.WriteString(Reference->FieldName);
					Writer.WriteU8(static_cast<uint8>(Reference->Kind));
					Writer.WriteString(Reference->ExpectedClass);
					Writer.WriteString(Reference->TargetPath.GetView());
					Writer.WriteU32(static_cast<uint32>(Reference->Route.size()));
					for (const FAssetReferenceRouteSegment& Segment : Reference->Route)
					{
						Writer.WriteU8(static_cast<uint8>(Segment.Kind));
						Writer.WriteU64(Segment.Index);
						Writer.WriteU64(Segment.MapKeyToken.size());
						Writer.WriteBytes(Segment.MapKeyToken);
						Writer.WriteString(Segment.DeclaringType);
						Writer.WriteString(Segment.FieldName);
					}
					Writer.WriteString(Reference->DisplayRoute);
				}
			}
			std::string ErrorMessage;
			if (!DerivedDataCache::WriteFileAtomically(
				ReferenceCachePath(), Writer.GetBytes(), &ErrorMessage))
			{
				OutWarning = std::move(ErrorMessage);
				return false;
			}
			return true;
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
			FAssetResult Result = Codec->Write(
				Package, OutBytes, EDefaultDeltaMode::NoDelta, Options);
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
		FAssetRuntimeState& Manager = FAssetRuntimeState::Get();
		if (Manager.RuntimeConfiguration.IsCooked())
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
			FAssetResult Result = ValidateOrdinarySaveVersion(Manager.GetRegistry(), Path);
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
			Manager.GetRegistry().AddOrUpdate(FAssetData{
				.PackagePath = Staged.Path,
				.PhysicalPath = Staged.Destination.generic_string(),
				.AssetClassName = Staged.File.AssetClassName,
				.EntryKind = Staged.File.EntryKind,
				.RedirectDestination = Staged.File.RedirectDestination,
				.FormatVersion = OrdinaryAssetPackageWriterVersion,
				.Dependencies = Staged.File.Dependencies,
				.FileSize = Staged.PublishedFileSize,
				.LastWriteTime = Staged.PublishedLastWriteTime,
				.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(
					Staged.PublishedLastWriteTime)});
			if (auto Resident = Manager.ResidentPackages.find(Staged.Path);
				Resident != Manager.ResidentPackages.end()
				&& Resident->second.Package == Staged.Package)
				Resident->second.PublicationState =
					EAssetPackagePublicationState::Published;
			Staged.Package->ClearDirty();
			std::error_code Ec;
			std::filesystem::remove(Staged.Backup, Ec);
		}
		return {};
	}

	auto AdmitAssetPackageToCatalog(const FAssetPath& Path) -> FAssetResult
	{
		return FAssetRuntimeState::Get().AdmitAssetPackageToCatalog(Path);
	}

	auto FAssetRuntimeState::AdmitAssetPackageToCatalog(
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
			.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime)});
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

	namespace
	{
		auto ExtractAssetReferencesInternal(
			const FAssetPath& SourcePackage,
			const FAssetPackageInspection& Inspection,
			std::vector<FAssetReferenceEdge>& OutReferences,
			bool bRequireValidSource) -> FAssetResult
		{
			OutReferences.clear();
			if (bRequireValidSource && !SourcePackage.IsValid())
				return Error(EAssetError::InvalidPath,
					"AssetReferenceIndexInvalidSource: source package path is invalid.");
			if (Inspection.Objects.empty())
				return Error(EAssetError::InvalidObjectGraph,
					"AssetReferenceIndexInvalidPackage: package has no main object.");
			if (Inspection.Header.AssetClassName != Inspection.Objects.front().ClassName)
				return Error(EAssetError::TypeMismatch,
					"AssetReferenceIndexRuntimeTypeMismatch: header and main-object classes differ.");

			std::vector<FAssetReferenceEdge> References;
			for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			{
				DClass* ObjectClass = FindClassByQualifiedName(FName(Object.ClassName));
				if (!ObjectClass)
					return Error(EAssetError::UnknownClass, std::format(
						"AssetReferenceIndexUnknownClass: {} is unavailable.", Object.ClassName));
				for (const FAssetPackageField& Field : Object.Fields)
				{
					DClass* DeclaringClass = FindClassByQualifiedName(FName(Field.DeclaringClass));
					FProperty* Property = DeclaringClass && ObjectClass->IsChildOf(DeclaringClass)
						? DeclaringClass->FindPropertyBySerializedName(FName(Field.Name), false)
						: nullptr;
					if (!Property)
					{
						if (Field.TypeSignature.find("SoftObject:") != std::string::npos
							|| Field.TypeSignature.find("Object:") != std::string::npos)
							return Error(EAssetError::TypeMismatch, std::format(
								"AssetReferenceSchemaMismatch: {}::{} has no current property metadata.",
								Field.DeclaringClass, Field.Name));
						continue;
					}
					const bool bCurrentContainsReference = ContainsAssetReferenceProperty(Property);
					const bool bStoredContainsReference =
						Field.TypeSignature.find("SoftObject:") != std::string::npos
						|| Field.TypeSignature.find("Object:") != std::string::npos;
					if (Property->GetKind() != Field.Kind
						|| !IsSerializedTypeSignatureCompatible(Property, Field.TypeSignature))
					{
						if (bCurrentContainsReference || bStoredContainsReference)
							return Error(EAssetError::TypeMismatch, std::format(
								"AssetReferenceSchemaMismatch: {}::{} has incompatible kind or signature.",
								Field.DeclaringClass, Field.Name));
						continue;
					}
					if (!bCurrentContainsReference) continue;
					FReferenceExtractionContext Context{
						.SourcePackage = SourcePackage,
						.Fingerprint = Inspection.Fingerprint,
						.Object = Object,
						.DeclaringType = Field.DeclaringClass,
						.FieldName = Field.Name,
						.ObjectKind = Inspection.Header.EntryKind
							== EAssetRegistryEntryKind::Redirector
							? EAssetReferenceKind::Redirect
							: EAssetReferenceKind::HardObject,
						.References = References};
					std::vector<FAssetReferenceRouteSegment> Route;
					FAssetResult Result = ExtractReferencePropertyValues(
						Property, Field.Payload, Context, Route, Field.Name, 0);
					if (!Result) return Result;
				}
			}
			std::ranges::sort(References, &AssetReferenceLess);
			OutReferences = std::move(References);
			return {};
		}
	}

	auto ExtractAssetReferences(
		const FAssetPath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult
	{
		return ExtractAssetReferencesInternal(
			SourcePackage, Inspection, OutReferences, true);
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
		Result = ExtractAssetReferencesInternal({}, Inspection, References, false);
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
		return RewritePackageReferences(
			Bytes, Mappings, std::numeric_limits<uint64>::max(), OutBytes);

	}

	auto FAssetLoadReport::HasNonUpgradeMutations() const -> bool
	{
		return std::ranges::any_of(Mutations, [](const FAssetLoadMutation& Mutation) {
			return Mutation.Kind == EAssetLoadMutationKind::NonUpgrade;
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

	auto RegisterAssetOwnedPayloadRelocator(
		DClass* Class,
		FAssetOwnedPayloadRelocator Relocator,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetOwnedPayloadRelocatorHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (!Class || !Relocator || (OwnerGate.IsValid() && !Call)) return 0;
		auto& Relocators = GetOwnedPayloadRelocators();
		if (Relocators.contains(Class)) return 0;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		auto& NextHandle = NextOwnedPayloadRelocatorHandle();
		const FAssetOwnedPayloadRelocatorHandle Handle = NextHandle++;
		Relocators.emplace(Class, FRegisteredOwnedPayloadRelocator{
			.Handle = Handle,
			.OwnerResource = std::move(Resource),
			.Relocator = std::move(Relocator),
			.OwnerGate = std::move(OwnerGate),
		});
		return Handle;
	}

	auto UnregisterAssetOwnedPayloadRelocator(
		FAssetOwnedPayloadRelocatorHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Relocators = GetOwnedPayloadRelocators();
		std::erase_if(Relocators, [Handle](const auto& Pair) {
			return Pair.second.Handle == Handle;
		});
	}

	auto RegisterAssetMoveObserver(
		IAssetMoveObserver* Observer,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetMoveObserverHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return 0;
		if (!Observer) return 0;
		auto& NextHandle = NextMoveObserverHandle();
		const FAssetMoveObserverHandle Handle = NextHandle++;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		GetMoveObservers().emplace(Handle,
			FRegisteredAssetMoveObserver{
				std::move(Resource), Observer, std::move(OwnerGate)});
		return Handle;
	}

	auto UnregisterAssetMoveObserver(FAssetMoveObserverHandle Handle) -> void
	{
		if (Handle != 0) GetMoveObservers().erase(Handle);
	}

	auto RegisterAssetReferenceStore(
		IAssetReferenceStore* Store,
		FModuleOwnedCallbackGate OwnerGate)
		-> FAssetReferenceStoreHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (OwnerGate.IsValid() && !Call) return 0;
		if (!Store) return 0;
		auto& Registry = GetAssetReferenceStoreRegistry();
		const FAssetReferenceStoreHandle Handle = Registry.NextHandle++;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		Registry.Stores.emplace(Handle,
			FAssetReferenceStoreRegistry::FEntry{
				std::move(Resource), Store, std::move(OwnerGate)});
		++Registry.Revision;
		return Handle;
	}

	auto UnregisterAssetReferenceStore(FAssetReferenceStoreHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Registry = GetAssetReferenceStoreRegistry();
		if (Registry.Stores.erase(Handle) != 0) ++Registry.Revision;
	}

	auto SetAssetRelocationFailurePointForTesting(
		EAssetRelocationFailurePoint Point,
		uint32 Occurrence) -> void
	{
		auto& Injection = GetRelocationFailureInjection();
		if (Point == EAssetRelocationFailurePoint::None)
		{
			Injection.RemainingOccurrences.clear();
			return;
		}
		Injection.RemainingOccurrences.insert_or_assign(
			Point, std::max(Occurrence, 1u));
	}

	auto SetAssetRedirectorFixupFailurePointForTesting(
		EAssetRedirectorFixupFailurePoint Point,
		uint32 Occurrence) -> void
	{
		auto& Injection = GetFixupFailureInjection();
		if (Point == EAssetRedirectorFixupFailurePoint::None)
		{
			Injection.RemainingOccurrences.clear();
			return;
		}
		Injection.RemainingOccurrences.insert_or_assign(
			Point, std::max(Occurrence, 1u));
	}

	auto RegisterAssetDeleteContributor(
		DClass* Class,
		FAssetDeleteContributor Contributor,
		FModuleOwnedCallbackGate OwnerGate) -> FAssetDeleteContributorHandle
	{
		auto Call = OwnerGate.TryEnter();
		if (!Class || !Contributor || (OwnerGate.IsValid() && !Call)) return 0;
		auto& Contributors = GetDeleteContributors();
		if (Contributors.contains(Class)) return 0;
		FModuleOwnedResourceLease Resource = OwnerGate.RetainResource();
		if (OwnerGate.IsValid() && !Resource) return 0;
		auto& NextHandle = NextDeleteContributorHandle();
		const FAssetDeleteContributorHandle Handle = NextHandle++;
		Contributors.emplace(Class, FRegisteredDeleteContributor{
			.Handle = Handle,
			.OwnerResource = std::move(Resource),
			.Contributor = std::move(Contributor),
			.OwnerGate = std::move(OwnerGate),
		});
		return Handle;
	}

	auto UnregisterAssetDeleteContributor(
		FAssetDeleteContributorHandle Handle) -> void
	{
		if (Handle == 0) return;
		auto& Contributors = GetDeleteContributors();
		std::erase_if(Contributors, [Handle](const auto& Pair) {
			return Pair.second.Handle == Handle;
		});
	}

	auto QueryAssetCompanionOwnership(
		const std::filesystem::path& PhysicalPath,
		FAssetCompanionOwnership& OutOwnership) -> FAssetResult
	{
		OutOwnership = {};
		const std::filesystem::path Candidate =
			std::filesystem::absolute(PhysicalPath).lexically_normal();
		for (const auto& [Path, Data] : GetAssetCatalogStore().GetAssets())
		{
			std::error_code ExistenceError;
			const bool bPackageExists =
				std::filesystem::is_regular_file(Data.PhysicalPath, ExistenceError);
			if (!bPackageExists
				&& (!ExistenceError
					|| ExistenceError == std::errc::no_such_file_or_directory
					|| ExistenceError == std::errc::not_a_directory))
				continue;
			if (ExistenceError)
				return {
					EAssetError::IoError,
					std::format(
						"Could not inspect companion owner package {}: {}",
						Path.ToString(), ExistenceError.message())};
			std::vector<std::filesystem::path> CompanionFiles;
			bool bHasContributor = false;
			const FAssetResult Result = InspectAssetCompanionFiles(
				Data, CompanionFiles, &bHasContributor);
			if (!Result)
				return {
					Result.Error,
					std::format(
						"Could not inspect companion ownership for {}: {}",
						Path.ToString(), Result.Message)};
			if (bHasContributor
				&& std::ranges::find(CompanionFiles, Candidate)
					!= CompanionFiles.end())
				OutOwnership.Owners.push_back(Path);
		}
		std::ranges::sort(
			OutOwnership.Owners,
			[](const FAssetPath& A, const FAssetPath& B) {
				return A.GetView() < B.GetView();
			});
		OutOwnership.State = OutOwnership.Owners.empty()
			? EAssetCompanionOwnershipState::Unclaimed
			: OutOwnership.Owners.size() == 1
			? EAssetCompanionOwnershipState::Owned
			: EAssetCompanionOwnershipState::Ambiguous;
		return {};
	}

	auto FAssetCatalogStore::ScanMountedContent(EAssetRegistryScanMode Mode) -> FAssetResult
	{
		RefreshMountedContent(Mode);
		return {};
	}

	auto FAssetCatalogStore::RefreshMountedContent(
		EAssetRegistryScanMode Mode) -> FAssetCatalogRefreshResult
	{
		const uint64 PriorRevision = Revision;
		const FAssetReferenceIndex PriorReferenceIndex = ReferenceIndex;
		const auto ScanStartTime = std::chrono::steady_clock::now();
		std::unordered_map<FAssetPath, FAssetData> NewAssets;
		std::vector<FRegistryCacheEntry> NewCacheEntries;
		std::unordered_map<std::string, FRegistryCacheEntry> CachedEntries;
		std::unordered_map<FAssetPath, FReferenceCacheSource> CachedReferenceSources;
		std::unordered_map<FAssetPath, FAssetPackageInspection> FullValidationInspections;
		std::unordered_set<std::string> SeenCachedIdentities;
		ScanErrors.clear();
		LastScanStats = {};
		CacheWarning.clear();
		ReferenceIndex.Errors.clear();
		ReferenceIndex.Stats = {};
		ReferenceIndex.CacheWarning.clear();
		const std::vector<std::string> MountManifest = GetMountManifest();
		const bool bCacheLoaded = LoadRegistryCache(MountManifest, CachedEntries, CacheWarning);
		const bool bReferenceCacheLoaded = LoadReferenceCache(
			CachedReferenceSources, ReferenceIndex.CacheWarning);
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
					LastScanStats.HeaderFileBytesRead += PackageHeader.FileBytesRead;
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
						std::vector<uint8> Bytes;
						++ReferenceIndex.Stats.PayloadReadAttempts;
						if (!FFileHelper::LoadFileToArray(Bytes, It->path().generic_string()))
							RedirectResult = Error(EAssetError::IoError, std::format(
								"Failed to open asset package {}.", It->path().generic_string()));
						else
						{
							ReferenceIndex.Stats.PayloadBytesRead += Bytes.size();
							RedirectResult = InspectAssetPackageBytes(
								It->path().generic_string(), Bytes, Inspection);
						}
						if (!RedirectResult)
						{
							RedirectResult.Message = std::format(
								"{} ({})", RedirectResult.Message,
								It->path().generic_string());
							ScanErrors.push_back(std::move(RedirectResult));
							++LastScanStats.Failed;
							continue;
						}
						FullValidationInspections.emplace(DiskPath, std::move(Inspection));
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
		std::vector<FAssetReferenceEdge> NewReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> NewReferenceFingerprints;
		for (const FAssetData* Data : SortedAssets)
		{
			const auto CachedIt = CachedReferenceSources.find(Data->PackagePath);
			if (Mode == EAssetRegistryScanMode::Incremental && bReferenceCacheLoaded
				&& CachedIt != CachedReferenceSources.end()
				&& CachedIt->second.Fingerprint.FileSize == Data->FileSize
				&& CachedIt->second.Fingerprint.LastWriteTimeTicks == Data->LastWriteTimeTicks
				&& CachedIt->second.Fingerprint.ReaderVersion == Data->FormatVersion)
			{
				if (NewReferenceEdges.size() > MaximumReferencesPerSnapshot
					- CachedIt->second.References.size())
				{
					ReferenceIndex.Errors.push_back(Error(EAssetError::CorruptFile,
						"AssetReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
					++ReferenceIndex.Stats.FailedSources;
					continue;
				}
				NewReferenceEdges.insert(
					NewReferenceEdges.end(),
					CachedIt->second.References.begin(), CachedIt->second.References.end());
				NewReferenceFingerprints.emplace(Data->PackagePath, CachedIt->second.Fingerprint);
				++ReferenceIndex.Stats.ReusedSources;
				continue;
			}

			FAssetPackageInspection Inspection;
			FAssetResult InspectionResult;
			const auto PreparedIt = FullValidationInspections.find(Data->PackagePath);
			if (PreparedIt != FullValidationInspections.end())
				Inspection = std::move(PreparedIt->second);
			else
			{
				std::vector<uint8> Bytes;
				++ReferenceIndex.Stats.PayloadReadAttempts;
				if (!FFileHelper::LoadFileToArray(Bytes, Data->PhysicalPath))
					InspectionResult = Error(EAssetError::IoError, std::format(
						"AssetReferenceIndexReadFailed: could not read {}.", Data->PhysicalPath));
				else
				{
					ReferenceIndex.Stats.PayloadBytesRead += Bytes.size();
					InspectionResult = InspectAssetPackageBytes(
						Data->PhysicalPath, Bytes, Inspection);
				}
			}
			std::vector<FAssetReferenceEdge> SourceReferences;
			if (InspectionResult)
				InspectionResult = ExtractAssetReferences(
					Data->PackagePath, Inspection, SourceReferences);
			++ReferenceIndex.Stats.ExtractedSources;
			if (!InspectionResult)
			{
				InspectionResult.Message = std::format(
					"{} ({})", InspectionResult.Message, Data->PhysicalPath);
				ReferenceIndex.Errors.push_back(std::move(InspectionResult));
				++ReferenceIndex.Stats.FailedSources;
				continue;
			}
			if (NewReferenceEdges.size() > MaximumReferencesPerSnapshot
				- SourceReferences.size())
			{
				ReferenceIndex.Errors.push_back(Error(EAssetError::CorruptFile,
					"AssetReferenceIndexSnapshotExceeded: scan exceeds 1,000,000 occurrences."));
				++ReferenceIndex.Stats.FailedSources;
				continue;
			}
			NewReferenceEdges.insert(
				NewReferenceEdges.end(),
				std::make_move_iterator(SourceReferences.begin()),
				std::make_move_iterator(SourceReferences.end()));
			NewReferenceFingerprints.emplace(Data->PackagePath, Inspection.Fingerprint);
		}
		std::ranges::sort(NewReferenceEdges, &AssetReferenceLess);
		ReferenceIndex.bComplete = ReferenceIndex.Errors.empty()
			&& NewReferenceFingerprints.size() == NewAssets.size();
		if (!ScanErrors.empty() || !ReferenceIndex.bComplete)
		{
			LastScanStats.DurationMilliseconds =
				std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - ScanStartTime).count();
			FAssetCatalogRefreshResult Result{
				.Mode = Mode,
				.bCatalogComplete = ScanErrors.empty(),
				.bReferenceIndexComplete = ReferenceIndex.bComplete,
				.bPublished = false,
				.bRetainedPriorRevision = true,
				.PriorRevision = PriorRevision,
				.ResultingRevision = PriorRevision,
				.CatalogStats = LastScanStats,
				.ReferenceStats = ReferenceIndex.Stats,
				.Errors = ScanErrors,
				.CatalogCacheWarning = CacheWarning,
				.ReferenceCacheWarning = ReferenceIndex.CacheWarning};
			Result.Errors.insert(
				Result.Errors.end(), ReferenceIndex.Errors.begin(),
				ReferenceIndex.Errors.end());
			ReferenceIndex = PriorReferenceIndex;
			DURIN_WARN_CATEGORY(
				"AssetRegistry",
				"Retained catalog revision {} after incomplete refresh with {} catalog error(s) and {} reference error(s).",
				PriorRevision, ScanErrors.size(),
				Result.Errors.size() - ScanErrors.size());
			return Result;
		}

		const bool bAssetsChanged = Assets != NewAssets;
		const bool bReferencesChanged = ReferenceIndex.Edges != NewReferenceEdges
			|| ReferenceIndex.SourceFingerprints != NewReferenceFingerprints;
		if (bAssetsChanged)
		{
			Assets = std::move(NewAssets);
			RebuildRedirectorIndex();
		}
		if (bReferencesChanged)
		{
			ReferenceIndex.Edges = std::move(NewReferenceEdges);
			ReferenceIndex.SourceFingerprints = std::move(NewReferenceFingerprints);
		}
		if (bAssetsChanged || bReferencesChanged) ++Revision;
		bPersistentSnapshotDirty = !WriteRegistryCache(MountManifest, std::move(NewCacheEntries), CacheWarning);
		std::string SoftWriteWarning;
		ReferenceIndex.bSnapshotDirty = !WriteReferenceCache(
			ReferenceIndex.SourceFingerprints, ReferenceIndex.Edges, SoftWriteWarning);
		if (!SoftWriteWarning.empty())
		{
			if (!ReferenceIndex.CacheWarning.empty()) ReferenceIndex.CacheWarning.append(" ");
			ReferenceIndex.CacheWarning.append(SoftWriteWarning);
		}
		LastScanStats.DurationMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - ScanStartTime).count();
		DURIN_INFO_CATEGORY("AssetRegistry",
			"Scanned {} asset package(s) in {:.3f} ms: {} redirector(s), {} reused, {} reparsed, {} header read(s), {} logical header byte(s), {} file header byte(s), {} reference payload read(s), {} reference payload byte(s), {} removed, {} failed.",
			LastScanStats.Enumerated, LastScanStats.DurationMilliseconds, LastScanStats.Redirectors,
			LastScanStats.Reused, LastScanStats.Reparsed,
			LastScanStats.HeaderReadAttempts, LastScanStats.HeaderBytesRead,
			LastScanStats.HeaderFileBytesRead,
			ReferenceIndex.Stats.PayloadReadAttempts, ReferenceIndex.Stats.PayloadBytesRead,
			LastScanStats.Removed, LastScanStats.Failed);
		if (!CacheWarning.empty())
		{
			if (bPersistentSnapshotDirty)
				DURIN_WARN_CATEGORY("AssetRegistry", "{}", CacheWarning);
			else
				DURIN_INFO_CATEGORY("AssetRegistry",
					"Rebuilt asset registry cache after a recoverable cache read issue: {}",
					CacheWarning);
		}
		if (!ReferenceIndex.CacheWarning.empty())
		{
			if (ReferenceIndex.bSnapshotDirty)
				DURIN_WARN_CATEGORY("AssetRegistry", "{}", ReferenceIndex.CacheWarning);
			else
				DURIN_INFO_CATEGORY("AssetRegistry",
					"Rebuilt asset-reference cache after a recoverable cache read issue: {}",
					ReferenceIndex.CacheWarning);
		}
		for (const FAssetResult& ReferenceError : ReferenceIndex.Errors)
			DURIN_WARN_CATEGORY("AssetRegistry", "{}", ReferenceError.Message);
		FAssetCatalogRefreshResult Result{
			.Mode = Mode,
			.bCatalogComplete = ScanErrors.empty(),
			.bReferenceIndexComplete = ReferenceIndex.IsComplete(),
			.bPublished = true,
			.bRetainedPriorRevision = false,
			.PriorRevision = PriorRevision,
			.ResultingRevision = Revision,
			.CatalogStats = LastScanStats,
			.ReferenceStats = ReferenceIndex.GetStats(),
			.Errors = ScanErrors,
			.CatalogCacheWarning = CacheWarning,
			.ReferenceCacheWarning = ReferenceIndex.CacheWarning};
		Result.Errors.insert(
			Result.Errors.end(), ReferenceIndex.Errors.begin(),
			ReferenceIndex.Errors.end());
		return Result;
	}

	auto FAssetCatalogStore::FlushPersistentSnapshot() -> void
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
		if (ReferenceIndex.bSnapshotDirty)
		{
			std::string Warning;
			if (WriteReferenceCache(
				ReferenceIndex.SourceFingerprints, ReferenceIndex.Edges, Warning))
			{
				ReferenceIndex.bSnapshotDirty = false;
				ReferenceIndex.CacheWarning.clear();
			}
			else
			{
				ReferenceIndex.CacheWarning = std::move(Warning);
				if (!ReferenceIndex.CacheWarning.empty())
					DURIN_WARN_CATEGORY("AssetRegistry", "{}", ReferenceIndex.CacheWarning);
			}
		}
	}

	auto FAssetCatalogStore::FindAssetExactPointer(
		const FAssetPath& Path) const -> const FAssetData*
	{
		auto It = Assets.find(Path);
		return It == Assets.end() ? nullptr : &It->second;
	}

	auto FAssetCatalogStore::FindAssetExact(
		const FAssetPath& Path) const -> FAssetCatalogEntry
	{
		const FAssetData* Data = FindAssetExactPointer(Path);
		return {.Revision = Revision,
			.Data = Data ? std::optional<FAssetData>(*Data) : std::nullopt};
	}

	auto FAssetCatalogStore::CaptureSnapshot() const -> FAssetCatalogSnapshot
	{
		return {.Revision = Revision, .Assets = Assets};
	}

	auto FAssetCatalogStore::ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) const -> FAssetPathResolveResult
	{
		FAssetPathResolveResult Result;
		Result.CatalogRevision = Revision;
		Result.RequestedPath = Path;
		FAssetPath Current = Path;
		std::unordered_set<FAssetPath> Visited;
		while (true)
		{
			const FAssetData* Data = FindAssetExactPointer(Current);
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

	auto FAssetCatalogStore::FindRedirectorsTo(
		const FAssetPath& Destination) const -> std::vector<FAssetPath>
	{
		const auto It = RedirectorsByDestination.find(Destination);
		return It == RedirectorsByDestination.end()
			? std::vector<FAssetPath>{} : It->second;
	}

	auto FAssetCatalogStore::RebuildRedirectorIndex() -> void
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

	auto FAssetReferenceIndex::FindReferencers(
		const FAssetPath& Target) const -> std::vector<FAssetReferenceEdge>
	{
		std::vector<FAssetReferenceEdge> Result;
		for (const FAssetReferenceEdge& Reference : Edges)
			if (Reference.TargetPath == Target) Result.push_back(Reference);
		return Result;
	}

	auto FAssetReferenceIndex::FindTargets(
		const FAssetPath& Source) const -> std::vector<FAssetPath>
	{
		std::vector<FAssetPath> Result;
		for (const FAssetReferenceEdge& Reference : Edges)
			if (Reference.SourcePackage == Source) Result.push_back(Reference.TargetPath);
		std::ranges::sort(Result, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		Result.erase(std::unique(Result.begin(), Result.end()), Result.end());
		return Result;
	}

	auto FAssetCatalogStore::BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) const -> FAssetResult
	{
		OutPackages.clear();
		struct FPendingCookPath
		{
			FAssetPath Path;
			std::string ExpectedClass;
			std::string Source;
		};
		std::vector<FPendingCookPath> Pending;
		Pending.reserve(Roots.size());
		for (const FAssetPath& Root : Roots)
			Pending.push_back({Root, {}, "explicit Cook root"});
		for (const auto& [Handle, Entry] : GetAssetReferenceStoreRegistry().Stores)
		{
			(void)Handle;
			IAssetReferenceStore* Store = Entry.Store;
			if (!Store) continue;
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.OwnerGate.IsValid() && !Call) continue;
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetResult StoreResult = Store->CaptureSnapshot(Snapshot);
			if (!StoreResult)
			{
				StoreResult.Message = std::format(
					"CookReachabilityExternalRootProviderFailed: {}",
					StoreResult.Message);
				return StoreResult;
			}
			for (const FAssetReferenceStoreOccurrence& Occurrence :
				Snapshot.Occurrences)
				if (Occurrence.bCookRoot)
					Pending.push_back({
						Occurrence.TargetPath,
						Occurrence.ExpectedClass,
						Occurrence.DisplayRoute});
		}
		std::unordered_set<FAssetPath> Visited;
		while (!Pending.empty())
		{
			std::ranges::sort(Pending, [](const FPendingCookPath& Left,
				const FPendingCookPath& Right) {
				return Left.Path.GetView() > Right.Path.GetView();
			});
			FPendingCookPath Requested = std::move(Pending.back());
			Pending.pop_back();
			DClass* ExpectedClass = nullptr;
			if (!Requested.ExpectedClass.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(Requested.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownRootClass: {} expects unavailable class {}.",
						Requested.Source, Requested.ExpectedClass));
			}
			const FAssetPathResolveResult SourceResolution = ResolveAssetPath(
				Requested.Path, {.ExpectedClass = ExpectedClass});
			if (!SourceResolution)
			{
				FAssetResult ResolutionError = AssetPathResolutionError(SourceResolution);
				if (ResolutionError.Error == EAssetError::NotFound)
					ResolutionError.Error = EAssetError::MissingDependency;
				ResolutionError.Message = std::format(
					"CookReachabilityUnresolvedRoot: {} from {}. {}",
					Requested.Path.ToString(), Requested.Source,
					ResolutionError.Message);
				return ResolutionError;
			}
			const FAssetPath Source = SourceResolution.FinalPath;
			if (!Visited.insert(Source).second) continue;
			const FAssetData* SourceData = FindAssetExactPointer(Source);
			if (!SourceData || SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType, std::format(
					"CookReachabilityNonAssetPackage: {} is not a real asset.", Source.ToString()));
			if (!ReferenceIndex.SourceFingerprints.contains(Source))
				return Error(EAssetError::StaleData, std::format(
					"CookReachabilityIncompleteReferenceIndex: {} has no current source entry.", Source.ToString()));
			for (const FAssetPath& Dependency : SourceData->Dependencies)
			{
				const FAssetPathResolveResult Resolution = ResolveAssetPath(Dependency);
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedHardDependency: {} references {}. {}",
						Source.ToString(), Dependency.ToString(), ResolutionError.Message);
					return ResolutionError;
				}
				Pending.push_back({
					Resolution.FinalPath, {},
					std::format("hard dependency of {}", Source.ToString())});
			}
			for (const FAssetReferenceEdge& Reference : ReferenceIndex.Edges)
			{
				if (Reference.SourcePackage != Source
					|| Reference.Kind == EAssetReferenceKind::Redirect) continue;
				DClass* ExpectedClass = FindClassByQualifiedName(FName(Reference.ExpectedClass));
				if (!ExpectedClass)
					return Error(EAssetError::UnknownClass, std::format(
						"CookReachabilityUnknownReferenceClass: {} expects unavailable class {}.",
						Reference.DisplayRoute, Reference.ExpectedClass));
				const FAssetPathResolveResult Resolution = ResolveAssetPath(
					Reference.TargetPath, {.ExpectedClass = ExpectedClass});
				if (!Resolution)
				{
					FAssetResult ResolutionError = AssetPathResolutionError(Resolution);
					if (ResolutionError.Error == EAssetError::NotFound)
						ResolutionError.Error = EAssetError::MissingDependency;
					ResolutionError.Message = std::format(
						"CookReachabilityUnresolvedReference: {} references {} at {}. {}",
						Source.ToString(), Reference.TargetPath.ToString(),
						Reference.DisplayRoute, ResolutionError.Message);
					return ResolutionError;
				}
				Pending.push_back({
					Resolution.FinalPath, {}, Reference.DisplayRoute});
			}
		}
		OutPackages.assign(Visited.begin(), Visited.end());
		std::ranges::sort(OutPackages, [](const FAssetPath& Left, const FAssetPath& Right) {
			return Left.GetView() < Right.GetView();
		});
		return {};
	}

	auto FAssetCatalogStore::RemoveReferencesFromSource(const FAssetPath& Path) -> bool
	{
		const size_t PreviousCount = ReferenceIndex.Edges.size();
		std::erase_if(ReferenceIndex.Edges, [&](const FAssetReferenceEdge& Reference) {
			return Reference.SourcePackage == Path;
		});
		const bool bChanged = PreviousCount != ReferenceIndex.Edges.size()
			|| ReferenceIndex.SourceFingerprints.erase(Path) != 0;
		if (bChanged) ReferenceIndex.bSnapshotDirty = true;
		return bChanged;
	}

	auto FAssetCatalogStore::RefreshReferencesForAsset(const FAssetData& Data) -> bool
	{
		const std::vector<FAssetReferenceEdge> PreviousReferences = ReferenceIndex.Edges;
		const auto PreviousFingerprints = ReferenceIndex.SourceFingerprints;
		RemoveReferencesFromSource(Data.PackagePath);
		ReferenceIndex.Errors.clear();
		FAssetPackageInspection Inspection;
		FAssetResult Result = InspectAssetPackage(Data.PhysicalPath, Inspection);
		std::vector<FAssetReferenceEdge> SourceReferences;
		if (Result)
			Result = ExtractAssetReferences(Data.PackagePath, Inspection, SourceReferences);
		if (!Result)
		{
			Result.Message = std::format("{} ({})", Result.Message, Data.PhysicalPath);
			ReferenceIndex.Errors.push_back(std::move(Result));
			ReferenceIndex.bSnapshotDirty = true;
			ReferenceIndex.bComplete = false;
			return PreviousReferences != ReferenceIndex.Edges
				|| PreviousFingerprints != ReferenceIndex.SourceFingerprints;
		}
		if (ReferenceIndex.Edges.size() > MaximumReferencesPerSnapshot - SourceReferences.size())
		{
			ReferenceIndex.Errors.push_back(Error(EAssetError::CorruptFile,
				"AssetReferenceIndexSnapshotExceeded: mutation exceeds 1,000,000 occurrences."));
			ReferenceIndex.bSnapshotDirty = true;
			ReferenceIndex.bComplete = false;
			return PreviousReferences != ReferenceIndex.Edges
				|| PreviousFingerprints != ReferenceIndex.SourceFingerprints;
		}
		ReferenceIndex.Edges.insert(
			ReferenceIndex.Edges.end(),
			std::make_move_iterator(SourceReferences.begin()),
			std::make_move_iterator(SourceReferences.end()));
		std::ranges::sort(ReferenceIndex.Edges, &AssetReferenceLess);
		ReferenceIndex.SourceFingerprints.insert_or_assign(
			Data.PackagePath, Inspection.Fingerprint);
		const bool bChanged = PreviousReferences != ReferenceIndex.Edges
			|| PreviousFingerprints != ReferenceIndex.SourceFingerprints;
		ReferenceIndex.bComplete = ReferenceIndex.Errors.empty()
			&& ReferenceIndex.SourceFingerprints.size() == Assets.size();
		if (bChanged) ReferenceIndex.bSnapshotDirty = true;
		return bChanged;
	}

	auto FAssetCatalogStore::AddOrUpdate(FAssetData Data) -> void
	{
		const FAssetPath Path = Data.PackagePath;
		const auto Existing = Assets.find(Path);
		const bool bAssetChanged = Existing == Assets.end() || Existing->second != Data;
		Assets.insert_or_assign(Path, std::move(Data));
		if (bAssetChanged) RebuildRedirectorIndex();
		bPersistentSnapshotDirty = true;
		const FAssetData* Stored = FindAssetExactPointer(Path);
		const bool bReferencesChanged = Stored && RefreshReferencesForAsset(*Stored);
		if (bAssetChanged || bReferencesChanged) ++Revision;
	}

	auto FAssetCatalogStore::Remove(const FAssetPath& Path) -> void
	{
		const bool bAssetChanged = Assets.erase(Path) != 0;
		if (bAssetChanged) RebuildRedirectorIndex();
		const bool bReferencesChanged = RemoveReferencesFromSource(Path);
		ReferenceIndex.bComplete = ReferenceIndex.Errors.empty()
			&& ReferenceIndex.SourceFingerprints.size() == Assets.size();
		if (!bAssetChanged && !bReferencesChanged) return;
		bPersistentSnapshotDirty = true;
		++Revision;
	}

	auto FAssetRuntimeState::Get() -> FAssetRuntimeState&
	{
		static FAssetRuntimeState Instance;
		return Instance;
	}

	FAssetRuntimeState::FAssetRuntimeState() = default;

	auto FAssetRuntimeState::CreateAsset(const FAssetPath& Path, DClass* Class, size_t Size, DObject*& OutAsset) -> FAssetResult
	{
		OutAsset = nullptr;
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown, "Asset creation is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset creation.");
		if (!Path.IsValid() || !Class || !Class->ClassConstructor) return Error(EAssetError::InvalidPath, "Invalid asset path or class.");
		if (const FAssetData* Existing = Registry.FindAssetExactPointer(Path))
			return Existing->EntryKind == EAssetRegistryEntryKind::Redirector
				? Error(EAssetError::AlreadyExists, std::format(
					"Asset {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
					Path.ToString(), Existing->RedirectDestination.ToString()))
				: Error(EAssetError::AlreadyExists, std::format(
					"Asset {} already exists. Choose another destination or delete the existing asset.",
					Path.ToString()));
		if (ResidentPackages.contains(Path))
			return Error(EAssetError::AlreadyExists, std::format(
				"A loaded package already uses {}. Close it or choose another destination.",
				Path.ToString()));

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
		ResidentPackages.emplace(
			Path, Package, EAssetPackagePublicationState::NewlyCreated);
		return {};
	}

	auto FAssetRuntimeState::CreateRedirector(
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

	auto FAssetRuntimeState::SavePackage(
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
			.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(LastWriteTime)});
		if (auto Resident = ResidentPackages.find(Path);
			Resident != ResidentPackages.end()
			&& Resident->second.Package == Package)
			Resident->second.PublicationState =
				EAssetPackagePublicationState::Published;
		return {};
	}

	namespace
	{
		enum class EAssetMutationState : uint8
		{
			Planned,
			Prepared,
			Publishing,
			Committed,
			Compensating,
			Restored,
			RecoveryRequired,
		};

		enum class ERelocationPublicationRole : uint8
		{
			RealAsset,
			OwnedPayload,
			Redirector,
		};

		struct FAssetMutationJournalEntry
		{
			std::filesystem::path PhysicalPath;
			FAssetPath RegistryPath;
			ERelocationPublicationRole Role = ERelocationPublicationRole::RealAsset;
			uint64 PublicationOrder = std::numeric_limits<uint64>::max();
			bool bPreExists = false;
			bool bPostExists = false;
			bool bCompleted = false;
			bool bCompensated = false;
			std::filesystem::path StagedPrePath;
			std::filesystem::path StagedPostPath;
			FXxHash128 StagedPreHash;
			FXxHash128 StagedPostHash;
			FAssetPackageFingerprint ExpectedPreFingerprint;
			FAssetPackageFingerprint ExpectedPostFingerprint;
		};

		// Retains every byte image required to compensate, undo, or redo one
		// authored mutation. Recovery-required roots deliberately outlive tokens.
		struct FAssetMutationJournal
		{
			std::string OperationId;
			std::string OperationType = "relocation";
			std::vector<std::filesystem::path> Roots;
			std::filesystem::path LocatorPath;
			std::vector<FAssetMutationJournalEntry> Entries;
			EAssetMutationState State = EAssetMutationState::Planned;

			~FAssetMutationJournal()
			{
				if (State == EAssetMutationState::RecoveryRequired) return;
				const std::string ExpectedOwner = std::format(
					"durin-asset-mutation\n{}\n", OperationId);
				for (const std::filesystem::path& Root : Roots)
				{
					if (Root.filename() != std::format("operation-{}", OperationId)
						|| Root.parent_path().filename() != ".durin-asset-mutation")
						continue;
					std::error_code ErrorCode;
					if (!std::filesystem::is_regular_file(Root / "owner", ErrorCode))
						continue;
					std::vector<uint8> OwnerBytes;
					if (!FFileHelper::LoadFileToArray(
							OwnerBytes, (Root / "owner").generic_string())
						|| std::string_view(
							reinterpret_cast<const char*>(OwnerBytes.data()),
							OwnerBytes.size()) != ExpectedOwner)
						continue;
					std::filesystem::remove_all(Root, ErrorCode);
				}
				if (LocatorPath.filename() == std::format("operation-{}", OperationId)
					&& LocatorPath.parent_path().filename() == "AssetMutationRecovery")
				{
					std::error_code ErrorCode;
					std::filesystem::remove(LocatorPath, ErrorCode);
					std::filesystem::remove(LocatorPath.parent_path(), ErrorCode);
				}
			}
		};

		struct FLoadedRelocationState
		{
			FAssetRelocationMapping Mapping;
			DPackage* Package = nullptr;
			std::string PrePackageName;
			std::string PreAssetName;
		};

		auto MakeRelocationOperationId() -> std::string
		{
			static std::atomic<uint64> Counter = 1;
			const uint64 Time = static_cast<uint64>(
				std::chrono::steady_clock::now().time_since_epoch().count());
			return std::format("{:016x}{:016x}", Time, Counter++);
		}

		auto NormalizePhysicalPath(const std::filesystem::path& Path)
			-> std::filesystem::path
		{
			return std::filesystem::absolute(Path).lexically_normal();
		}

		auto LoadRelocationBytes(
			const std::filesystem::path& Path,
			std::vector<uint8>& OutBytes) -> FAssetResult
		{
			OutBytes.clear();
			if (!FFileHelper::LoadFileToArray(OutBytes, Path.generic_string()))
				return Error(EAssetError::IoError, std::format(
					"Could not read relocation input {}.", Path.generic_string()));
			return {};
		}

		auto SaveRelocationBytes(
			const std::filesystem::path& Path,
			std::span<const uint8> Bytes) -> FAssetResult
		{
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
					std::span{reinterpret_cast<const std::byte*>(Bytes.data()),
						Bytes.size()},
					Path,
					&PublicationError))
				return Error(EAssetError::IoError, PublicationError.ToString());
			return {};
		}

		auto FingerprintRelocationFile(
			const std::filesystem::path& Path,
			FAssetPackageFingerprint& OutFingerprint) -> FAssetResult
		{
			std::vector<uint8> Bytes;
			FAssetResult Result = LoadRelocationBytes(Path, Bytes);
			if (!Result) return Result;
			return MakePackageFingerprint(
				Path.generic_string(), Bytes, OutFingerprint);
		}

		auto IsWritableRelocationPath(
			const std::filesystem::path& Path,
			const PathUtilities::FMountPoint*& OutMount,
			std::string& OutError) -> bool
		{
			OutMount = nullptr;
			const std::filesystem::path Normalized = NormalizePhysicalPath(Path);
			for (const PathUtilities::FMountPoint& Mount :
				PathUtilities::GetRegisteredMountPoints())
			{
				const std::filesystem::path Content =
					NormalizePhysicalPath(Mount.GetContentDir());
				if (!PathUtilities::IsLexicalDescendantPath(
						Normalized.generic_string(), Content.generic_string(), true))
					continue;
				if (!Mount.bAuthoringWritable)
				{
					OutError = std::format(
						"Content mount {} is read-only.", Mount.VirtualRoot);
					return false;
				}
				for (std::filesystem::path Current = Normalized.parent_path();
					!Current.empty(); Current = Current.parent_path())
				{
					std::error_code StatusError;
					const auto Status = std::filesystem::symlink_status(
						Current, StatusError);
					if (!StatusError && std::filesystem::is_symlink(Status))
					{
						OutError = std::format(
							"Relocation path traverses a reparse point: {}.",
							Current.generic_string());
						return false;
					}
					if (Current == Content) break;
					if (Current == Current.root_path()) break;
				}
				OutMount = &Mount;
				return true;
			}
			OutError = std::format(
				"Relocation path is outside writable mounted content: {}.",
				Path.generic_string());
			return false;
		}

		auto BuildMovedPackageBytes(
			std::span<const uint8> SourceBytes,
			const FAssetPath& DestinationPath,
			std::vector<uint8>& OutBytes) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(SourceBytes, Codec); !Result)
				return Result;
			if (!Codec->bCanMutate || Codec->FormatVersion != OrdinaryAssetPackageWriterVersion)
				return Error(EAssetError::UnsupportedVersion,
					"Relocation requires the ordinary-format mutation capability.");
			if (FAssetResult Result = Codec->Relocate(
				SourceBytes, DestinationPath, OutBytes); !Result)
				return Result;
			return Codec->Validate(OutBytes);
		}

		auto BuildRedirectorPackageBytes(
			const FAssetPath& SourcePath,
			const FAssetPath& DestinationPath,
			std::vector<uint8>& OutBytes) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec =
				Private::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
			if (!Codec || !Codec->bCanMutate)
				return Error(EAssetError::UnsupportedVersion,
					"Redirector creation requires the ordinary-format mutation capability.");
			return Codec->WriteRedirector(SourcePath, DestinationPath, OutBytes);
		}

		auto WriteMutationJournalState(FAssetMutationJournal& Journal) -> void
		{
			std::string Text = std::format(
				"version=1\noperation={}\ntype={}\nstate={}\nentries={}\n",
				Journal.OperationId,
				Journal.OperationType,
				static_cast<uint32>(Journal.State),
				Journal.Entries.size());
			for (size_t Index = 0; Index < Journal.Entries.size(); ++Index)
			{
				const FAssetMutationJournalEntry& Entry = Journal.Entries[Index];
				Text += std::format(
					"entry.{}.role={}\n"
					"entry.{}.order={}\n"
					"entry.{}.registry={}\n"
					"entry.{}.original={}\n"
					"entry.{}.staged_pre={}\n"
					"entry.{}.staged_post={}\n"
					"entry.{}.published={}\n"
					"entry.{}.pre_exists={}\n"
					"entry.{}.post_exists={}\n"
					"entry.{}.pre_fingerprint={}:{}:{}\n"
					"entry.{}.post_fingerprint={}:{}:{}\n"
					"entry.{}.staged_pre_hash={}\n"
					"entry.{}.staged_post_hash={}\n"
					"entry.{}.completed={}\n"
					"entry.{}.compensated={}\n",
					Index, static_cast<uint32>(Entry.Role),
					Index, Entry.PublicationOrder,
					Index, Entry.RegistryPath.ToString(),
					Index, Entry.PhysicalPath.generic_string(),
					Index, Entry.StagedPrePath.generic_string(),
					Index, Entry.StagedPostPath.generic_string(),
					Index, Entry.PhysicalPath.generic_string(),
					Index, Entry.bPreExists,
					Index, Entry.bPostExists,
					Index, Entry.ExpectedPreFingerprint.FileSize,
					Entry.ExpectedPreFingerprint.LastWriteTimeTicks,
					Entry.ExpectedPreFingerprint.ContentHash.ToString(),
					Index, Entry.ExpectedPostFingerprint.FileSize,
					Entry.ExpectedPostFingerprint.LastWriteTimeTicks,
					Entry.ExpectedPostFingerprint.ContentHash.ToString(),
					Index, Entry.StagedPreHash.ToString(),
					Index, Entry.StagedPostHash.ToString(),
					Index, Entry.bCompleted,
					Index, Entry.bCompensated);
			}
			const std::span Bytes{
				reinterpret_cast<const uint8*>(Text.data()), Text.size()};
			for (const std::filesystem::path& Root : Journal.Roots)
			{
				std::string Ignored;
				DerivedDataCache::WriteFileAtomically(
					Root / "journal", Bytes, &Ignored);
			}

			std::string Locator = std::format(
				"version=1\noperation={}\nroots={}\n",
				Journal.OperationId, Journal.Roots.size());
			for (const std::filesystem::path& Root : Journal.Roots)
				Locator += std::format("root={}\n", Root.generic_string());
			std::error_code DirectoryError;
			std::filesystem::create_directories(
				Journal.LocatorPath.parent_path(), DirectoryError);
			if (!DirectoryError)
			{
				const std::span LocatorBytes{
					reinterpret_cast<const uint8*>(Locator.data()), Locator.size()};
				std::string Ignored;
				DerivedDataCache::WriteFileAtomically(
					Journal.LocatorPath, LocatorBytes, &Ignored);
			}
		}

		auto RebuildReferenceProjectionForPublishedEntries(
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

	struct FAssetRelocationState
	{
		uint64 ExpectedRegistryRevision = 0;
		std::vector<FAssetRelocationMapping> Mappings;
		FAssetMutationJournal Journal;
		std::vector<FLoadedRelocationState> ResidentPackages;
		std::vector<FAssetOwnedPayloadRelocation> OwnedPayloads;
		std::unordered_map<FAssetPath, FAssetData> PreAssets;
		std::unordered_map<FAssetPath, FAssetData> PostAssets;
		std::unordered_map<FAssetPath, FAssetData> ExpectedAssets;
		std::vector<FAssetReferenceEdge> PreReferenceEdges;
		std::vector<FAssetReferenceEdge> PostReferenceEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> PreReferenceFingerprints;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> PostReferenceFingerprints;
	};

	struct FAssetMutationTransaction::FState
	{
		FAssetMutationSummary Summary;
		std::function<FAssetResult()> CommitOperation;
		std::function<FAssetResult()> UndoOperation;
		std::function<FAssetResult()> RedoOperation;
		std::function<bool()> IsRecoveryRequired;
		std::function<void(FAssetMutationResultDetails&)> PopulateResultDetails;
		EAssetMutationTransactionState State =
			EAssetMutationTransactionState::Prepared;
		FAssetMutationResultDetails LastResult;
	};

	struct FAssetDeletionTransaction::FState
	{
		uint64 RegistryRevision = 0;
		uint64 ReferenceStoreRevision = 0;
		std::vector<FAssetDeletionBatchEntry> Entries;
		std::vector<FAssetDeletionBatchWarning> Warnings;
		std::vector<std::filesystem::path> PhysicalRoots;
		EAssetMutationTransactionState TransactionState =
			EAssetMutationTransactionState::Prepared;
	};

	auto FAssetDeletionTransaction::GetRegistryRevision() const -> uint64
	{
		return State ? State->RegistryRevision : 0;
	}

	auto FAssetDeletionTransaction::GetEntries() const
		-> std::span<const FAssetDeletionBatchEntry>
	{
		return State ? std::span<const FAssetDeletionBatchEntry>(State->Entries)
			: std::span<const FAssetDeletionBatchEntry>{};
	}

	auto FAssetDeletionTransaction::GetWarnings() const
		-> std::span<const FAssetDeletionBatchWarning>
	{
		return State ? std::span<const FAssetDeletionBatchWarning>(State->Warnings)
			: std::span<const FAssetDeletionBatchWarning>{};
	}

	auto FAssetDeletionTransaction::GetState() const
		-> EAssetMutationTransactionState
	{
		return State ? State->TransactionState
			: EAssetMutationTransactionState::Empty;
	}

	auto FAssetMutationTransaction::GetSummary() const
		-> const FAssetMutationSummary&
	{
		static const FAssetMutationSummary EmptySummary;
		return State ? State->Summary : EmptySummary;
	}

	auto FAssetMutationTransaction::GetState() const
		-> EAssetMutationTransactionState
	{
		return State ? State->State : EAssetMutationTransactionState::Empty;
	}

	auto FAssetMutationTransaction::GetLastResultDetails() const
		-> FAssetMutationResultDetails
	{
		return State ? State->LastResult : FAssetMutationResultDetails{};
	}

	auto FAssetMutationTransaction::Commit() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction is empty.");
		if (State->State != EAssetMutationTransactionState::Prepared)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only a prepared asset mutation transaction can be committed.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}

		if (!State->CommitOperation)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction has no commit operation.");
		FAssetResult Result = State->CommitOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationTransactionState::Committed;
		else if (bRecoveryRequired)
			State->State = EAssetMutationTransactionState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
			.bStateRestored = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}

	auto FAssetMutationTransaction::Undo() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction is empty.");
		if (State->State != EAssetMutationTransactionState::Committed)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only a committed asset mutation transaction can be undone.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}
		if (!State->UndoOperation)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"This asset mutation does not support editor-history undo.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}

		FAssetResult Result = State->UndoOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationTransactionState::Undone;
		else if (bRecoveryRequired)
			State->State = EAssetMutationTransactionState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
			.bStateRestored = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}

	auto FAssetMutationTransaction::Redo() -> FAssetResult
	{
		if (!State)
			return Error(EAssetError::StaleData,
				"The asset mutation transaction is empty.");
		if (State->State != EAssetMutationTransactionState::Undone)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"Only an undone asset mutation transaction can be redone.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}
		if (!State->RedoOperation)
		{
			FAssetResult Result = Error(EAssetError::StaleData,
				"This asset mutation does not support editor-history redo.");
			State->LastResult = {
				.Result = Result,
				.State = State->State,
				.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
				.bStateRestored = true,
			};
			return Result;
		}

		FAssetResult Result = State->RedoOperation();
		const bool bRecoveryRequired = State->IsRecoveryRequired
			&& State->IsRecoveryRequired();
		if (Result)
			State->State = EAssetMutationTransactionState::Committed;
		else if (bRecoveryRequired)
			State->State = EAssetMutationTransactionState::RecoveryRequired;
		State->LastResult = {
			.Result = Result,
			.State = State->State,
			.RegistryRevision = FAssetRuntimeState::Get().GetRegistry().GetRevision(),
			.bStateRestored = !Result && !bRecoveryRequired,
			.bRecoveryRequired = bRecoveryRequired,
		};
		if (State->PopulateResultDetails)
			State->PopulateResultDetails(State->LastResult);
		return Result;
	}

	auto FAssetRuntimeState::PrepareAssetRelocationState(
		std::span<const FAssetRelocationMapping> Mappings,
		std::shared_ptr<FAssetRelocationState>& OutState) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		OutState.reset();
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown,
				"Asset relocation is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit asset relocation.");
		if (Mappings.empty())
			return Error(EAssetError::InvalidPath,
				"An asset relocation batch must not be empty.");

		auto State = std::make_shared<FAssetRelocationState>();
		State->ExpectedRegistryRevision = Registry.GetRevision();
		State->Mappings.assign(Mappings.begin(), Mappings.end());
		std::ranges::sort(State->Mappings,
			[](const FAssetRelocationMapping& A,
				const FAssetRelocationMapping& B) {
				return A.SourcePath.GetView() < B.SourcePath.GetView();
			});
		State->PreAssets = Registry.Assets;
		State->PostAssets = State->PreAssets;
		State->ExpectedAssets = State->PreAssets;
		State->PreReferenceEdges = Registry.ReferenceIndex.Edges;
		State->PostReferenceEdges = State->PreReferenceEdges;
		State->PreReferenceFingerprints = Registry.ReferenceIndex.SourceFingerprints;
		State->PostReferenceFingerprints = State->PreReferenceFingerprints;
		State->Journal.OperationId = MakeRelocationOperationId();
		const std::string RecoveryBase = FPaths::ProjectDir().empty()
			? FPaths::LaunchDir() : FPaths::ProjectDir();
		State->Journal.LocatorPath = NormalizePhysicalPath(RecoveryBase)
			/ "Saved" / "AssetMutationRecovery"
			/ std::format("operation-{}", State->Journal.OperationId);

		std::unordered_set<FAssetPath> Sources;
		std::unordered_set<FAssetPath> Destinations;
		std::unordered_map<std::string, size_t> FileEntries;
		std::unordered_map<std::string, const PathUtilities::FMountPoint*> EntryMounts;
		auto AddFileEntry = [&](const std::filesystem::path& PhysicalPath,
			const FAssetPath& RegistryPath,
			ERelocationPublicationRole Role,
			std::optional<std::vector<uint8>> PreBytes,
			std::optional<std::vector<uint8>> PostBytes) -> FAssetResult {
			const std::filesystem::path Normalized =
				NormalizePhysicalPath(PhysicalPath);
			const std::string Key = Normalized.generic_string();
			if (FileEntries.contains(Key))
				return Error(EAssetError::AlreadyExists, std::format(
					"Relocation participants claim the same file {}.", Key));
			std::string PathError;
			const PathUtilities::FMountPoint* Mount = nullptr;
			if (!IsWritableRelocationPath(Normalized, Mount, PathError))
				return Error(EAssetError::ReadOnlyMode, std::move(PathError));
			if (PreBytes)
			{
				std::error_code PermissionError;
				const std::filesystem::perms Permissions =
					std::filesystem::status(
						Normalized, PermissionError).permissions();
				constexpr auto WritePermissions =
					std::filesystem::perms::owner_write
					| std::filesystem::perms::group_write
					| std::filesystem::perms::others_write;
				if (PermissionError
					|| (Permissions & WritePermissions)
						== std::filesystem::perms::none)
					return Error(EAssetError::ReadOnlyMode, std::format(
						"Relocation input is read-only: {}.", Key));
			}
			FAssetMutationJournalEntry Entry{
				.PhysicalPath = Normalized,
				.RegistryPath = RegistryPath,
				.Role = Role,
				.bPreExists = PreBytes.has_value(),
				.bPostExists = PostBytes.has_value()};
			if (PreBytes)
				Entry.StagedPreHash = FXxHash128::HashBuffer(*PreBytes);
			if (PostBytes)
			{
				Entry.StagedPostHash = FXxHash128::HashBuffer(*PostBytes);
				Entry.ExpectedPostFingerprint.FileSize = PostBytes->size();
				Entry.ExpectedPostFingerprint.ContentHash = Entry.StagedPostHash;
			}
			FileEntries.emplace(Key, State->Journal.Entries.size());
			EntryMounts.emplace(Key, Mount);
			State->Journal.Entries.push_back(std::move(Entry));
			FAssetMutationJournalEntry& Stored = State->Journal.Entries.back();
			if (Stored.bPreExists)
			{
				FAssetResult Result = MakePackageFingerprint(
					Normalized.generic_string(), *PreBytes,
					Stored.ExpectedPreFingerprint);
				if (!Result) return Result;
			}
			const size_t Index = State->Journal.Entries.size() - 1;
			const std::filesystem::path Content =
				NormalizePhysicalPath(Mount->GetContentDir());
			const std::filesystem::path Root = Content
				/ ".durin-asset-mutation"
				/ std::format("operation-{}", State->Journal.OperationId);
			if (std::ranges::find(State->Journal.Roots, Root)
				== State->Journal.Roots.end())
			{
				std::error_code DirectoryError;
				std::filesystem::create_directories(Root, DirectoryError);
				if (DirectoryError)
					return Error(EAssetError::IoError, std::format(
						"Could not create relocation staging root: {}",
						DirectoryError.message()));
				const std::string Marker = std::format(
					"durin-asset-mutation\n{}\n", State->Journal.OperationId);
				FAssetResult MarkerResult = SaveRelocationBytes(
					Root / "owner",
					std::span{reinterpret_cast<const uint8*>(Marker.data()),
						Marker.size()});
				if (!MarkerResult) return MarkerResult;
				State->Journal.Roots.push_back(Root);
			}
			Stored.StagedPrePath = Root / std::format("pre-{:08}", Index);
			Stored.StagedPostPath = Root / std::format("post-{:08}", Index);
			if (ConsumeRelocationFailure(
					EAssetRelocationFailurePoint::PrepareOutput))
				return Error(EAssetError::IoError,
					"Injected relocation output-preparation failure.");
			if (PreBytes)
			{
				FAssetResult Result = SaveRelocationBytes(
					Stored.StagedPrePath, *PreBytes);
				if (!Result) return Result;
			}
			if (PostBytes)
			{
				FAssetResult Result = SaveRelocationBytes(
					Stored.StagedPostPath, *PostBytes);
				if (!Result) return Result;
			}
			return {};
		};

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			if (!Mapping.SourcePath.IsValid()
				|| !Mapping.DestinationPath.IsValid()
				|| Mapping.SourcePath == Mapping.DestinationPath)
				return Error(EAssetError::InvalidPath,
					"Asset relocation paths are invalid or identical.");
			if (!Sources.insert(Mapping.SourcePath).second
				|| !Destinations.insert(Mapping.DestinationPath).second)
				return Error(EAssetError::InvalidPath,
					"An asset relocation batch contains duplicate paths.");
		}

		for (const FAssetRelocationMapping& Mapping : State->Mappings)
		{
			const FAssetData* SourceData =
				Registry.FindAssetExactPointer(Mapping.SourcePath);
			if (!SourceData)
				return Error(EAssetError::NotFound, std::format(
					"Asset {} was not found.", Mapping.SourcePath.ToString()));
			if (SourceData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetError::InvalidPackageType,
					"Redirectors cannot be used as relocation sources.");
			if (LoadingPackages.contains(Mapping.SourcePath))
				return Error(EAssetError::InUse,
					"A relocation source is currently loading.");
			if (DPackage* Loaded = FindResidentPackage(Mapping.SourcePath))
			{
				if (Loaded->IsDirty())
					return Error(EAssetError::InUse,
						"A dirty loaded asset must be saved before relocation.");
				State->ResidentPackages.push_back({
					.Mapping = Mapping,
					.Package = Loaded,
					.PrePackageName = Loaded->GetName(),
					.PreAssetName = Loaded->GetAsset()->GetName()});
			}

			bool bReclaimDestinationRedirector = false;
			if (const FAssetData* DestinationData =
				Registry.FindAssetExactPointer(Mapping.DestinationPath))
			{
				if (DestinationData->EntryKind
						!= EAssetRegistryEntryKind::Redirector)
					return Error(EAssetError::AlreadyExists, std::format(
						"Asset {} already exists.",
						Mapping.DestinationPath.ToString()));
				const FAssetPathResolveResult DestinationResolution =
					Registry.ResolveAssetPath(Mapping.DestinationPath);
				if (!DestinationResolution
					|| DestinationResolution.FinalPath != Mapping.SourcePath)
					return Error(EAssetError::AlreadyExists, std::format(
						"The destination {} is occupied by a redirector to {}. Run Fix Up Redirectors or choose another destination.",
						Mapping.DestinationPath.ToString(),
						DestinationData->RedirectDestination.ToString()));
				if (ResidentPackages.contains(Mapping.DestinationPath))
					return Error(EAssetError::InUse,
						"A loaded destination redirector cannot be reclaimed.");
				bReclaimDestinationRedirector = true;
			}

			const std::filesystem::path SourceFile =
				NormalizePhysicalPath(SourceData->PhysicalPath);
			const std::filesystem::path DestinationFile =
				NormalizePhysicalPath(GetPhysicalPath(Mapping.DestinationPath));
			std::vector<uint8> SourceBytes;
			FAssetResult Result = LoadRelocationBytes(SourceFile, SourceBytes);
			if (!Result) return Result;
			std::vector<uint8> DestinationPreBytes;
			if (bReclaimDestinationRedirector)
			{
				Result = LoadRelocationBytes(
					DestinationFile, DestinationPreBytes);
				if (!Result) return Result;
			}
			else if (std::filesystem::exists(DestinationFile))
				return Error(EAssetError::AlreadyExists, std::format(
					"Relocation destination file {} already exists.",
					DestinationFile.generic_string()));

			std::vector<uint8> MovedBytes;
			Result = BuildMovedPackageBytes(
				SourceBytes, Mapping.DestinationPath, MovedBytes);
			if (!Result) return Result;
			std::vector<uint8> SourceRedirectorBytes;
			Result = BuildRedirectorPackageBytes(
				Mapping.SourcePath, Mapping.DestinationPath,
				SourceRedirectorBytes);
			if (!Result) return Result;

			Result = AddFileEntry(
				DestinationFile,
				Mapping.DestinationPath,
				ERelocationPublicationRole::RealAsset,
				bReclaimDestinationRedirector
					? std::optional<std::vector<uint8>>(DestinationPreBytes)
					: std::nullopt,
				std::move(MovedBytes));
			if (!Result) return Result;
			Result = AddFileEntry(
				SourceFile,
				Mapping.SourcePath,
				ERelocationPublicationRole::Redirector,
				SourceBytes,
				std::move(SourceRedirectorBytes));
			if (!Result) return Result;

			FAssetData MovedData = *SourceData;
			MovedData.PackagePath = Mapping.DestinationPath;
			MovedData.PhysicalPath = DestinationFile.generic_string();
			State->PostAssets.erase(Mapping.SourcePath);
			State->PostAssets.erase(Mapping.DestinationPath);
			State->PostAssets.emplace(Mapping.DestinationPath,
				std::move(MovedData));
			State->PostAssets.emplace(Mapping.SourcePath, FAssetData{
				.PackagePath = Mapping.SourcePath,
				.PhysicalPath = SourceFile.generic_string(),
				.AssetClassName = std::string(RedirectorClassName),
				.EntryKind = EAssetRegistryEntryKind::Redirector,
				.RedirectDestination = Mapping.DestinationPath,
				.FormatVersion = OrdinaryAssetPackageWriterVersion,
				.Dependencies = {Mapping.DestinationPath}});

			for (const auto& [AliasPath, AliasData] : State->PreAssets)
			{
				if (AliasData.EntryKind != EAssetRegistryEntryKind::Redirector
					|| AliasPath == Mapping.DestinationPath)
					continue;
				const FAssetPathResolveResult AliasResolution =
					Registry.ResolveAssetPath(AliasPath);
				if (!AliasResolution
					|| AliasResolution.FinalPath != Mapping.SourcePath)
					continue;
				if (ResidentPackages.contains(AliasPath))
					return Error(EAssetError::InUse,
						"A loaded upstream redirector cannot be retargeted.");
				std::vector<uint8> AliasPreBytes;
				Result = LoadRelocationBytes(
					AliasData.PhysicalPath, AliasPreBytes);
				if (!Result) return Result;
				std::vector<uint8> AliasPostBytes;
				Result = BuildRedirectorPackageBytes(
					AliasPath, Mapping.DestinationPath, AliasPostBytes);
				if (!Result) return Result;
				Result = AddFileEntry(
					AliasData.PhysicalPath,
					AliasPath,
					ERelocationPublicationRole::Redirector,
					std::move(AliasPreBytes),
					std::move(AliasPostBytes));
				if (!Result) return Result;
				FAssetData& PostAlias = State->PostAssets.at(AliasPath);
				PostAlias.RedirectDestination = Mapping.DestinationPath;
				PostAlias.Dependencies = {Mapping.DestinationPath};
			}

			for (FAssetReferenceEdge& Reference : State->PostReferenceEdges)
				if (Reference.SourcePackage == Mapping.SourcePath)
					Reference.SourcePackage = Mapping.DestinationPath;
			if (auto ReferenceSource = State->PostReferenceFingerprints.find(
					Mapping.SourcePath);
				ReferenceSource != State->PostReferenceFingerprints.end())
			{
				State->PostReferenceFingerprints.insert_or_assign(
					Mapping.DestinationPath, ReferenceSource->second);
				State->PostReferenceFingerprints.erase(ReferenceSource);
			}

			DClass* AssetClass = FindClassByQualifiedName(
				FName(SourceData->AssetClassName));
			for (DClass* Class = AssetClass; Class; Class = Class->GetSuperClass())
			{
				auto Relocator = GetOwnedPayloadRelocators().find(Class);
				if (Relocator == GetOwnedPayloadRelocators().end()) continue;
				auto Call = Relocator->second.OwnerGate.TryEnter();
				if (Relocator->second.OwnerGate.IsValid() && !Call)
					return Error(EAssetError::StaleData,
						"The owned-payload relocator is unavailable.");
				DObject* AssetObject = nullptr;
				Result = LoadAsset(Mapping.SourcePath, AssetObject);
				if (!Result) return Result;
				if (std::ranges::none_of(
						State->ResidentPackages,
						[&](const FLoadedRelocationState& Loaded) {
							return Loaded.Mapping.SourcePath
								== Mapping.SourcePath;
						}))
				{
					DPackage* LoadedPackage = AssetObject->GetPackage();
					State->ResidentPackages.push_back({
						.Mapping = Mapping,
						.Package = LoadedPackage,
						.PrePackageName = LoadedPackage->GetName(),
						.PreAssetName = AssetObject->GetName()});
				}
				FAssetOwnedPayloadRelocation Payload;
				Result = Relocator->second.Relocator(
					AssetObject, Mapping.SourcePath,
					Mapping.DestinationPath, Payload);
				if (!Result) return Result;
				for (const auto& [From, To] : Payload.Files)
				{
					const std::filesystem::path SourcePayload =
						NormalizePhysicalPath(From);
					const std::filesystem::path DestinationPayload =
						NormalizePhysicalPath(To);
					if (SourcePayload == DestinationPayload)
						return Error(EAssetError::InvalidPath,
							"An owned payload relocation has identical paths.");
					std::vector<uint8> PayloadBytes;
					Result = LoadRelocationBytes(SourcePayload, PayloadBytes);
					if (!Result) return Result;
					if (std::filesystem::exists(DestinationPayload))
						return Error(EAssetError::AlreadyExists,
							"An owned payload destination already exists.");
					Result = AddFileEntry(
						DestinationPayload, {},
						ERelocationPublicationRole::OwnedPayload,
						std::nullopt, PayloadBytes);
					if (!Result) return Result;
					Result = AddFileEntry(
						SourcePayload, {},
						ERelocationPublicationRole::OwnedPayload,
						std::move(PayloadBytes), std::nullopt);
					if (!Result) return Result;
				}
				State->OwnedPayloads.push_back(std::move(Payload));
				break;
			}
		}

		State->Journal.State = EAssetMutationState::Prepared;
		WriteMutationJournalState(State->Journal);
		OutState = std::move(State);
		return {};
	}

	auto FAssetRuntimeState::PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		OutSummary = {};
		OutTransaction = {};
		std::shared_ptr<FAssetRelocationState> Relocation;
		FAssetResult Result = PrepareAssetRelocationState(Mappings, Relocation);
		if (!Result) return Result;

		std::vector<FAssetPath> Scope;
		Scope.reserve(Mappings.size() * 2);
		for (const FAssetRelocationMapping& Mapping : Mappings)
		{
			Scope.push_back(Mapping.SourcePath);
			Scope.push_back(Mapping.DestinationPath);
		}
		OutSummary = FAssetMutationSummary(
			EAssetMutationOperationKind::Relocation,
			Relocation->ExpectedRegistryRevision,
			std::move(Scope));
		auto TransactionState = std::make_shared<FAssetMutationTransaction::FState>();
		TransactionState->Summary = OutSummary;
		TransactionState->CommitOperation = [Relocation] {
			return FAssetRuntimeState::Get().ApplyAssetRelocation(Relocation);
		};
		TransactionState->UndoOperation = [Relocation] {
			return FAssetRuntimeState::Get().RestoreAssetRelocation(Relocation);
		};
		TransactionState->RedoOperation = TransactionState->CommitOperation;
		TransactionState->IsRecoveryRequired = [Relocation] {
			return Relocation->Journal.State
				== EAssetMutationState::RecoveryRequired;
		};
		TransactionState->LastResult.State =
			EAssetMutationTransactionState::Prepared;
		TransactionState->LastResult.RegistryRevision =
			Registry.GetRevision();
		OutTransaction.State = std::move(TransactionState);
		return {};
	}

	auto FAssetRuntimeState::RevalidateAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation transaction state is empty.");
		const auto& State = *Relocation;
		if (State.Journal.State == EAssetMutationState::RecoveryRequired)
			return Error(EAssetError::IoError,
				"AssetMutationRecoveryRequired: the relocation journal requires recovery.");
		if (State.Journal.State != EAssetMutationState::Prepared
			&& State.Journal.State != EAssetMutationState::Committed
			&& State.Journal.State != EAssetMutationState::Restored)
			return Error(EAssetError::StaleData,
				"The relocation token is not in a revalidatable state.");
		if (Registry.GetRevision() != State.ExpectedRegistryRevision
			|| Registry.Assets != State.ExpectedAssets)
			return Error(EAssetError::StaleData,
				"The asset registry changed after relocation analysis.");
		const bool bExpectPost =
			State.Journal.State == EAssetMutationState::Committed;
		for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			const bool bExpectedExists = bExpectPost
				? Entry.bPostExists : Entry.bPreExists;
			std::error_code ExistsError;
			const bool bExists = std::filesystem::exists(
				Entry.PhysicalPath, ExistsError);
			if (ExistsError || bExists != bExpectedExists)
				return Error(EAssetError::StaleData, std::format(
					"Relocation participant occupancy changed: {}.",
					Entry.PhysicalPath.generic_string()));
			if (bExists)
			{
				FAssetPackageFingerprint Fingerprint;
				FAssetResult Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Fingerprint);
				if (!Result) return Result;
				const FAssetPackageFingerprint& Expected = bExpectPost
					? Entry.ExpectedPostFingerprint
					: Entry.ExpectedPreFingerprint;
				if (Fingerprint != Expected)
					return Error(EAssetError::StaleData, std::format(
						"Relocation participant changed: {}.",
						Entry.PhysicalPath.generic_string()));
			}
			const bool bOutputExists = bExpectPost
				? Entry.bPreExists : Entry.bPostExists;
			const std::filesystem::path& Staged = bExpectPost
				? Entry.StagedPrePath : Entry.StagedPostPath;
			const FXxHash128& ExpectedHash = bExpectPost
				? Entry.StagedPreHash : Entry.StagedPostHash;
			if (bOutputExists)
			{
				std::vector<uint8> StagedBytes;
				FAssetResult Result = LoadRelocationBytes(Staged, StagedBytes);
				if (!Result || FXxHash128::HashBuffer(StagedBytes) != ExpectedHash)
					return Error(EAssetError::StaleData,
						"A staged relocation output changed.");
			}
		}
		for (const FLoadedRelocationState& Loaded : State.ResidentPackages)
		{
			const FAssetPath& ExpectedPath = bExpectPost
				? Loaded.Mapping.DestinationPath
				: Loaded.Mapping.SourcePath;
			if (FindResidentPackage(ExpectedPath) != Loaded.Package)
				return Error(EAssetError::StaleData,
					"A loaded relocation participant changed identity.");
		}
		return {};
	}

	namespace
	{
		auto PublishRelocationFile(
			const FAssetMutationJournalEntry& Entry,
			bool bForward) -> FAssetResult
		{
			const bool bExists = bForward
				? Entry.bPostExists : Entry.bPreExists;
			const std::filesystem::path& Staged = bForward
				? Entry.StagedPostPath : Entry.StagedPrePath;
			if (!bExists)
			{
				std::error_code RemoveError;
				if (!std::filesystem::remove(Entry.PhysicalPath, RemoveError)
					&& RemoveError)
					return Error(EAssetError::IoError, std::format(
						"Could not remove relocation input {}: {}",
						Entry.PhysicalPath.generic_string(),
						RemoveError.message()));
				return {};
			}
			std::vector<uint8> Bytes;
			FAssetResult Result = LoadRelocationBytes(Staged, Bytes);
			if (!Result) return Result;
			std::error_code DirectoryError;
			std::filesystem::create_directories(
				Entry.PhysicalPath.parent_path(), DirectoryError);
			if (DirectoryError)
				return Error(EAssetError::IoError, std::format(
					"Could not create relocation destination directory: {}",
					DirectoryError.message()));
			return SaveRelocationBytes(Entry.PhysicalPath, Bytes);
		}

		auto FailurePointForRole(ERelocationPublicationRole Role)
			-> EAssetRelocationFailurePoint
		{
			switch (Role)
			{
			case ERelocationPublicationRole::RealAsset:
				return EAssetRelocationFailurePoint::PublishRealAsset;
			case ERelocationPublicationRole::OwnedPayload:
				return EAssetRelocationFailurePoint::PublishOwnedPayload;
			case ERelocationPublicationRole::Redirector:
				return EAssetRelocationFailurePoint::PublishRedirector;
			}
			return EAssetRelocationFailurePoint::PublishRealAsset;
		}
	}

	auto FAssetRuntimeState::ApplyAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation transaction state is empty.");
		auto& State = *Relocation;
		if (State.Journal.State != EAssetMutationState::Prepared
			&& State.Journal.State != EAssetMutationState::Restored)
			return Error(EAssetError::StaleData,
				"Only a prepared or restored relocation can be applied.");
		FAssetResult Result = RevalidateAssetRelocation(Relocation);
		if (!Result) return Result;
		if (ConsumeRelocationFailure(
				EAssetRelocationFailurePoint::StageOriginal))
			return Error(EAssetError::IoError,
				"Injected relocation original-staging failure.");

		State.Journal.State = EAssetMutationState::Publishing;
		WriteMutationJournalState(State.Journal);
		std::vector<size_t> Order(State.Journal.Entries.size());
		for (size_t Index = 0; Index < Order.size(); ++Index)
			Order[Index] = Index;
		std::ranges::stable_sort(Order, [&](size_t A, size_t B) {
			return State.Journal.Entries[A].Role
				< State.Journal.Entries[B].Role;
		});
		for (size_t OrderIndex = 0; OrderIndex < Order.size(); ++OrderIndex)
		{
			FAssetMutationJournalEntry& Entry =
				State.Journal.Entries[Order[OrderIndex]];
			Entry.PublicationOrder = static_cast<uint64>(OrderIndex);
			Entry.bCompleted = false;
			Entry.bCompensated = false;
		}
		WriteMutationJournalState(State.Journal);
		std::vector<size_t> Published;
		size_t RelocatedLoadedCount = 0;
		size_t AppliedPayloadCount = 0;
		auto EnterRecovery = [&](std::string Message) -> FAssetResult {
			State.Journal.State = EAssetMutationState::RecoveryRequired;
			WriteMutationJournalState(State.Journal);
			return Error(EAssetError::IoError,
				std::format("AssetMutationRecoveryRequired: {}", Message));
		};
		auto Compensate = [&](FAssetResult Failure) -> FAssetResult {
			State.Journal.State = EAssetMutationState::Compensating;
			WriteMutationJournalState(State.Journal);
			for (size_t Count = AppliedPayloadCount; Count > 0; --Count)
				if (State.OwnedPayloads[Count - 1].Restore)
					State.OwnedPayloads[Count - 1].Restore();
			for (size_t Count = RelocatedLoadedCount; Count > 0; --Count)
			{
				if (ConsumeRelocationFailure(
						EAssetRelocationFailurePoint::CompensateLoadedPackage))
					return EnterRecovery(
						"loaded-package compensation was interrupted.");
				FLoadedRelocationState& Loaded = State.ResidentPackages[Count - 1];
				if (!Loaded.Package->RelocateAssetPackage(
						Loaded.Mapping.SourcePath))
					return EnterRecovery(
						"a loaded package path could not be restored.");
				Loaded.Package->Rename(FName(Loaded.PrePackageName));
				Loaded.Package->GetAsset()->Rename(FName(Loaded.PreAssetName));
				Loaded.Package->ClearDirty();
				ResidentPackages.erase(Loaded.Mapping.DestinationPath);
				ResidentPackages.emplace(Loaded.Mapping.SourcePath, Loaded.Package);
			}
			for (auto It = Published.rbegin(); It != Published.rend(); ++It)
			{
				if (ConsumeRelocationFailure(
						EAssetRelocationFailurePoint::CompensateFile))
					return EnterRecovery(
						"file compensation was interrupted.");
				FAssetResult RestoreResult = PublishRelocationFile(
					State.Journal.Entries[*It], false);
				if (!RestoreResult)
					return EnterRecovery(RestoreResult.Message);
				State.Journal.Entries[*It].bCompensated = true;
				WriteMutationJournalState(State.Journal);
			}
			for (FAssetMutationJournalEntry& Entry : State.Journal.Entries)
			{
				if (!Entry.bPreExists) continue;
				FAssetResult FingerprintResult = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPreFingerprint);
				if (!FingerprintResult)
					return EnterRecovery(FingerprintResult.Message);
			}
			State.Journal.State = EAssetMutationState::Prepared;
			WriteMutationJournalState(State.Journal);
			return Failure;
		};

		for (size_t Index : Order)
		{
			FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
			if (ConsumeRelocationFailure(FailurePointForRole(Entry.Role)))
				return Compensate(Error(EAssetError::IoError,
					"Injected relocation publication failure."));
			Result = PublishRelocationFile(Entry, true);
			if (!Result) return Compensate(std::move(Result));
			Published.push_back(Index);
			Entry.bCompleted = true;
			WriteMutationJournalState(State.Journal);
		}

		for (FLoadedRelocationState& Loaded : State.ResidentPackages)
		{
			if (ConsumeRelocationFailure(
					EAssetRelocationFailurePoint::UpdateLoadedPackage))
				return Compensate(Error(EAssetError::IoError,
					"Injected loaded-package relocation failure."));
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.DestinationPath))
				return Compensate(Error(EAssetError::AlreadyExists,
					"A loaded relocation destination became occupied."));
			Loaded.Package->Rename(FName(
				Loaded.Mapping.DestinationPath.GetAssetName()));
			Loaded.Package->GetAsset()->Rename(FName(
				Loaded.Mapping.DestinationPath.GetAssetName()));
			Loaded.Package->ClearDirty();
			ResidentPackages.erase(Loaded.Mapping.SourcePath);
			ResidentPackages.emplace(
				Loaded.Mapping.DestinationPath, Loaded.Package);
			++RelocatedLoadedCount;
		}
		for (FAssetOwnedPayloadRelocation& Payload : State.OwnedPayloads)
		{
			if (Payload.Apply) Payload.Apply();
			++AppliedPayloadCount;
		}
		if (ConsumeRelocationFailure(
				EAssetRelocationFailurePoint::PublishRegistry))
			return Compensate(Error(EAssetError::IoError,
				"Injected relocation registry-publication failure."));

		for (FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			if (Entry.bPostExists)
			{
				Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPostFingerprint);
				if (!Result) return Compensate(std::move(Result));
			}
			if (!Entry.RegistryPath.IsValid()) continue;
			auto Data = State.PostAssets.find(Entry.RegistryPath);
			if (Data == State.PostAssets.end()) continue;
			std::error_code MetadataError;
			Data->second.FileSize = std::filesystem::file_size(
				Entry.PhysicalPath, MetadataError);
			Data->second.LastWriteTime = std::filesystem::last_write_time(
				Entry.PhysicalPath, MetadataError);
			if (MetadataError)
				return Compensate(Error(EAssetError::IoError,
					"Could not read relocated package metadata."));
			Data->second.LastWriteTimeTicks =
				DerivedDataCache::FileTimeToStableTicks(
					Data->second.LastWriteTime);
		}
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			const FAssetMutationJournalEntry* DestinationEntry = nullptr;
			for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
				if (Entry.RegistryPath == Mapping.DestinationPath)
				{
					DestinationEntry = &Entry;
					break;
				}
			if (!DestinationEntry) continue;
			for (FAssetReferenceEdge& Reference : State.PostReferenceEdges)
				if (Reference.SourcePackage == Mapping.DestinationPath)
					Reference.SourceFingerprint =
						DestinationEntry->ExpectedPostFingerprint;
			if (State.PostReferenceFingerprints.contains(Mapping.DestinationPath))
				State.PostReferenceFingerprints.insert_or_assign(
					Mapping.DestinationPath,
					DestinationEntry->ExpectedPostFingerprint);
		}
		Result = RebuildReferenceProjectionForPublishedEntries(
			State.Journal.Entries, State.PostAssets,
			State.PostReferenceEdges, State.PostReferenceFingerprints);
		if (!Result) return Compensate(std::move(Result));

		Registry.Assets = State.PostAssets;
		Registry.ReferenceIndex.Edges = State.PostReferenceEdges;
		Registry.ReferenceIndex.SourceFingerprints = State.PostReferenceFingerprints;
		Registry.ReferenceIndex.bComplete = Registry.ReferenceIndex.Errors.empty()
			&& Registry.ReferenceIndex.SourceFingerprints.size()
				== Registry.Assets.size();
		Registry.RebuildRedirectorIndex();
		Registry.bPersistentSnapshotDirty = true;
		Registry.ReferenceIndex.bSnapshotDirty = true;
		++Registry.Revision;
		State.ExpectedRegistryRevision = Registry.Revision;
		State.ExpectedAssets = Registry.Assets;
		State.Journal.State = EAssetMutationState::Committed;
		WriteMutationJournalState(State.Journal);
		for (const auto& [Handle, Entry] : GetMoveObservers())
		{
			(void)Handle;
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.Observer && (!Entry.OwnerGate.IsValid() || Call))
				Entry.Observer->OnAssetsRelocated(State.Mappings);
		}
		return {};
	}

	auto FAssetRuntimeState::RestoreAssetRelocation(
		const std::shared_ptr<FAssetRelocationState>& Relocation) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Relocation)
			return Error(EAssetError::StaleData,
				"The relocation transaction state is empty.");
		auto& State = *Relocation;
		if (State.Journal.State != EAssetMutationState::Committed)
			return Error(EAssetError::StaleData,
				"Only a committed relocation can be restored.");
		FAssetResult Result = RevalidateAssetRelocation(Relocation);
		if (!Result) return Result;

		State.Journal.State = EAssetMutationState::Publishing;
		WriteMutationJournalState(State.Journal);
		std::vector<size_t> RestoredFiles;
		for (size_t Count = State.Journal.Entries.size(); Count > 0; --Count)
		{
			const size_t Index = Count - 1;
			Result = PublishRelocationFile(
				State.Journal.Entries[Index], false);
			if (!Result)
			{
				State.Journal.State = EAssetMutationState::RecoveryRequired;
				WriteMutationJournalState(State.Journal);
				return Error(EAssetError::IoError, std::format(
					"AssetMutationRecoveryRequired: {}", Result.Message));
			}
			RestoredFiles.push_back(Index);
			State.Journal.Entries[Index].bCompensated = true;
			WriteMutationJournalState(State.Journal);
		}
		for (size_t Count = State.OwnedPayloads.size(); Count > 0; --Count)
			if (State.OwnedPayloads[Count - 1].Restore)
				State.OwnedPayloads[Count - 1].Restore();
		for (size_t Count = State.ResidentPackages.size(); Count > 0; --Count)
		{
			FLoadedRelocationState& Loaded = State.ResidentPackages[Count - 1];
			if (!Loaded.Package->RelocateAssetPackage(
					Loaded.Mapping.SourcePath))
			{
				State.Journal.State = EAssetMutationState::RecoveryRequired;
				WriteMutationJournalState(State.Journal);
				return Error(EAssetError::IoError,
					"AssetMutationRecoveryRequired: a loaded package could not be restored.");
			}
			Loaded.Package->Rename(FName(Loaded.PrePackageName));
			Loaded.Package->GetAsset()->Rename(FName(Loaded.PreAssetName));
			Loaded.Package->ClearDirty();
			ResidentPackages.erase(Loaded.Mapping.DestinationPath);
			ResidentPackages.emplace(Loaded.Mapping.SourcePath, Loaded.Package);
		}

		for (FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			if (Entry.bPreExists)
			{
				Result = FingerprintRelocationFile(
					Entry.PhysicalPath, Entry.ExpectedPreFingerprint);
				if (!Result)
				{
					State.Journal.State = EAssetMutationState::RecoveryRequired;
					WriteMutationJournalState(State.Journal);
					return Error(EAssetError::IoError,
						"AssetMutationRecoveryRequired: restored package metadata is unavailable.");
				}
			}
			if (!Entry.RegistryPath.IsValid()) continue;
			auto Data = State.PreAssets.find(Entry.RegistryPath);
			if (Data == State.PreAssets.end()) continue;
			std::error_code MetadataError;
			Data->second.FileSize = std::filesystem::file_size(
				Entry.PhysicalPath, MetadataError);
			Data->second.LastWriteTime = std::filesystem::last_write_time(
				Entry.PhysicalPath, MetadataError);
			if (MetadataError)
			{
				State.Journal.State = EAssetMutationState::RecoveryRequired;
				WriteMutationJournalState(State.Journal);
				return Error(EAssetError::IoError,
					"AssetMutationRecoveryRequired: restored package metadata is unavailable.");
			}
			Data->second.LastWriteTimeTicks =
				DerivedDataCache::FileTimeToStableTicks(
					Data->second.LastWriteTime);
		}
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
		{
			const FAssetMutationJournalEntry* SourceEntry = nullptr;
			for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
				if (Entry.RegistryPath == Mapping.SourcePath)
				{
					SourceEntry = &Entry;
					break;
				}
			if (!SourceEntry) continue;
			for (FAssetReferenceEdge& Reference : State.PreReferenceEdges)
				if (Reference.SourcePackage == Mapping.SourcePath)
					Reference.SourceFingerprint =
						SourceEntry->ExpectedPreFingerprint;
			if (State.PreReferenceFingerprints.contains(Mapping.SourcePath))
				State.PreReferenceFingerprints.insert_or_assign(
					Mapping.SourcePath,
					SourceEntry->ExpectedPreFingerprint);
		}
		FAssetResult ProjectionResult = RebuildReferenceProjectionForPublishedEntries(
			State.Journal.Entries, State.PreAssets,
			State.PreReferenceEdges, State.PreReferenceFingerprints);
		if (!ProjectionResult)
		{
			State.Journal.State = EAssetMutationState::RecoveryRequired;
			WriteMutationJournalState(State.Journal);
			return Error(EAssetError::CorruptFile, std::format(
				"AssetMutationRecoveryRequired: restored reference projection failed: {}",
				ProjectionResult.Message));
		}

		Registry.Assets = State.PreAssets;
		Registry.ReferenceIndex.Edges = State.PreReferenceEdges;
		Registry.ReferenceIndex.SourceFingerprints = State.PreReferenceFingerprints;
		Registry.ReferenceIndex.bComplete = Registry.ReferenceIndex.Errors.empty()
			&& Registry.ReferenceIndex.SourceFingerprints.size()
				== Registry.Assets.size();
		Registry.RebuildRedirectorIndex();
		Registry.bPersistentSnapshotDirty = true;
		Registry.ReferenceIndex.bSnapshotDirty = true;
		++Registry.Revision;
		State.ExpectedRegistryRevision = Registry.Revision;
		State.ExpectedAssets = Registry.Assets;
		State.Journal.State = EAssetMutationState::Restored;
		WriteMutationJournalState(State.Journal);
		std::vector<FAssetRelocationMapping> Inverse;
		Inverse.reserve(State.Mappings.size());
		for (const FAssetRelocationMapping& Mapping : State.Mappings)
			Inverse.push_back({
				.SourcePath = Mapping.DestinationPath,
				.DestinationPath = Mapping.SourcePath});
		for (const auto& [Handle, Entry] : GetMoveObservers())
		{
			(void)Handle;
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.Observer && (!Entry.OwnerGate.IsValid() || Call))
				Entry.Observer->OnAssetsRelocated(Inverse);
		}
		return {};
	}

	namespace
	{
		struct FFixupPackageState
		{
			FAssetPath SourcePath;
			size_t JournalEntry = 0;
			DPackage* LoadedPackage = nullptr;
		};

		struct FFixupLiveSoftReference
		{
			FSoftObjectPtr* Value = nullptr;
			FAssetPath PrePath;
			FAssetPath PostPath;
		};

		struct FFixupStoreState
		{
			FModuleOwnedResourceLease OwnerResource;
			FModuleOwnedCallbackGate OwnerGate;
			FAssetReferenceStoreHandle Handle = 0;
			IAssetReferenceStore* Store = nullptr;
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetReferenceStoreRewriteContribution Contribution;
			bool bApplied = false;
		};

		auto IsReadOnlyMutationInput(const std::filesystem::path& Path) -> bool
		{
			std::error_code ErrorCode;
			const std::filesystem::perms Permissions =
				std::filesystem::status(Path, ErrorCode).permissions();
			constexpr auto WritePermissions = std::filesystem::perms::owner_write
				| std::filesystem::perms::group_write
				| std::filesystem::perms::others_write;
			return ErrorCode || (Permissions & WritePermissions)
				== std::filesystem::perms::none;
		}
	}

	struct FAssetRedirectorFixupState
	{
		EAssetRedirectorFixupMode Mode = EAssetRedirectorFixupMode::RewriteAndDelete;
		uint64 ExpectedRegistryRevision = 0;
		uint64 ExpectedStoreRevision = 0;
		std::vector<FAssetPath> Redirectors;
		std::vector<FAssetRedirectorFixupMapping> Mappings;
		std::vector<FAssetReferenceEdge> PackageOccurrences;
		std::vector<FAssetReferenceStoreOccurrence> StoreOccurrences;
		std::vector<FAssetPath> DeletableRedirectors;
		std::vector<FFixupPackageState> Packages;
		std::vector<FFixupLiveSoftReference> LiveSoftReferences;
		std::vector<FFixupStoreState> Stores;
		FAssetMutationJournal Journal;
		std::unordered_map<FAssetPath, FAssetData> ExpectedAssets;
		std::unordered_map<FAssetPath, FAssetData> PostAssets;
		std::vector<FAssetReferenceEdge> PostEdges;
		std::unordered_map<FAssetPath, FAssetPackageFingerprint> PostFingerprints;
		std::vector<FAssetResult> PostErrors;
		bool bPostIndexComplete = false;
	};

	auto FAssetRedirectorFixupSummary::GetMode() const
		-> EAssetRedirectorFixupMode
	{
		return Mode;
	}

	auto FAssetRedirectorFixupSummary::GetRegistryRevision() const -> uint64
	{
		return RegistryRevision;
	}

	auto FAssetRedirectorFixupSummary::GetRedirectors() const
		-> std::span<const FAssetPath>
	{
		return Redirectors;
	}

	auto FAssetRedirectorFixupSummary::GetFinalPathMappings() const
		-> std::span<const FAssetRedirectorFixupMapping>
	{
		return FinalPathMappings;
	}

	auto FAssetRedirectorFixupSummary::GetPackageOccurrences() const
		-> std::span<const FAssetReferenceEdge>
	{
		return PackageOccurrences;
	}

	auto FAssetRedirectorFixupSummary::GetStoreOccurrences() const
		-> std::span<const FAssetReferenceStoreOccurrence>
	{
		return StoreOccurrences;
	}

	auto FAssetRedirectorFixupSummary::GetDeletableRedirectors() const
		-> std::span<const FAssetPath>
	{
		return DeletableRedirectors;
	}

	auto FAssetRuntimeState::PrepareRedirectorFixupState(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		std::shared_ptr<FAssetRedirectorFixupState>& OutState) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		OutState.reset();
		if (!bAcceptingRequests)
			return Error(EAssetError::ShuttingDown,
				"Redirector Fix Up is closed while the asset manager is shutting down.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode,
				"Cooked runtime package mode does not permit redirector Fix Up.");
		if (Redirectors.empty())
			return Error(EAssetError::InvalidPath,
				"Redirector Fix Up requires at least one redirector.");
		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
			&& !Registry.ReferenceIndex.IsComplete())
			return Error(EAssetError::StaleData,
				"Redirector Fix Up cannot delete aliases because the reference index is incomplete.");

		auto State = std::make_shared<FAssetRedirectorFixupState>();
		State->Mode = Mode;
		State->ExpectedRegistryRevision = Registry.GetRevision();
		State->ExpectedAssets = Registry.Assets;
		State->PostAssets = Registry.Assets;
		State->PostEdges = Registry.ReferenceIndex.Edges;
		State->PostFingerprints = Registry.ReferenceIndex.SourceFingerprints;
		State->PostErrors = Registry.ReferenceIndex.Errors;
		State->bPostIndexComplete = Registry.ReferenceIndex.bComplete;
		State->Journal.OperationId = MakeRelocationOperationId();
		State->Journal.OperationType = "fixup";
		const std::string RecoveryBase = FPaths::ProjectDir().empty()
			? FPaths::LaunchDir() : FPaths::ProjectDir();
		State->Journal.LocatorPath = NormalizePhysicalPath(RecoveryBase)
			/ "Saved" / "AssetMutationRecovery"
			/ std::format("operation-{}", State->Journal.OperationId);

		std::unordered_set<FAssetPath> Closure;
		std::vector<FAssetPath> Pending(Redirectors.begin(), Redirectors.end());
		while (!Pending.empty())
		{
			FAssetPath Alias = std::move(Pending.back());
			Pending.pop_back();
			if (!Alias.IsValid())
				return Error(EAssetError::InvalidPath,
					"Redirector Fix Up contains an invalid path.");
			if (!Closure.insert(Alias).second) continue;
			const FAssetData* Data = Registry.FindAssetExactPointer(Alias);
			if (!Data)
				return Error(EAssetError::NotFound, std::format(
					"Fix Up redirector {} is not registered.", Alias.ToString()));
			if (Data->EntryKind != EAssetRegistryEntryKind::Redirector)
				return Error(EAssetError::InvalidPackageType, std::format(
					"Fix Up selection {} is not a redirector.", Alias.ToString()));
			if (LoadingPackages.contains(Alias))
				return Error(EAssetError::InUse,
					"A selected redirector is currently loading.");
			if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
				&& ResidentPackages.contains(Alias))
				return Error(EAssetError::InUse,
					"A loaded redirector must be unloaded before Fix Up deletion.");
			for (FAssetPath Upstream : Registry.FindRedirectorsTo(Alias))
				Pending.push_back(std::move(Upstream));
		}
		State->Redirectors.assign(Closure.begin(), Closure.end());
		std::ranges::sort(State->Redirectors,
			[](const FAssetPath& Left, const FAssetPath& Right) {
				return Left.GetView() < Right.GetView();
			});
		for (const FAssetPath& Alias : State->Redirectors)
		{
			const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(Alias);
			if (!Resolution)
				return Error(EAssetError::CorruptFile, std::format(
					"Fix Up could not resolve {} (state {}).", Alias.ToString(),
					static_cast<uint32>(Resolution.State)));
			State->Mappings.push_back({Alias, Resolution.FinalPath});
		}
		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
			State->DeletableRedirectors = State->Redirectors;

		std::map<FAssetPath, uint64, decltype([](const FAssetPath& Left,
			const FAssetPath& Right) { return Left.GetView() < Right.GetView(); })>
			PackageRewriteCounts;
		for (const FAssetReferenceEdge& Edge : Registry.ReferenceIndex.Edges)
		{
			if (!Closure.contains(Edge.TargetPath)) continue;
			State->PackageOccurrences.push_back(Edge);
			if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete
				&& Closure.contains(Edge.SourcePackage))
				continue;
			++PackageRewriteCounts[Edge.SourcePackage];
		}

		std::unordered_set<std::string> FilePaths;
		auto AddJournalEntry = [&](const std::filesystem::path& PhysicalPath,
			const FAssetPath& RegistryPath,
			ERelocationPublicationRole Role,
			std::optional<std::vector<uint8>> PreBytes,
			std::optional<std::vector<uint8>> PostBytes,
			size_t& OutIndex) -> FAssetResult {
			const std::filesystem::path Normalized = NormalizePhysicalPath(PhysicalPath);
			if (!FilePaths.insert(Normalized.generic_string()).second)
				return Error(EAssetError::AlreadyExists,
					"Redirector Fix Up has duplicate physical participants.");
			const PathUtilities::FMountPoint* Mount = nullptr;
			std::string PathError;
			if (!IsWritableRelocationPath(Normalized, Mount, PathError))
				return Error(EAssetError::ReadOnlyMode, std::move(PathError));
			if (PreBytes && IsReadOnlyMutationInput(Normalized))
				return Error(EAssetError::ReadOnlyMode, std::format(
					"Redirector Fix Up input is read-only: {}.",
					Normalized.generic_string()));
			FAssetMutationJournalEntry Entry{
				.PhysicalPath = Normalized,
				.RegistryPath = RegistryPath,
				.Role = Role,
				.bPreExists = PreBytes.has_value(),
				.bPostExists = PostBytes.has_value()};
			if (PreBytes)
			{
				Entry.StagedPreHash = FXxHash128::HashBuffer(*PreBytes);
				FAssetResult Result = MakePackageFingerprint(
					Normalized.generic_string(), *PreBytes,
					Entry.ExpectedPreFingerprint);
				if (!Result) return Result;
			}
			if (PostBytes)
			{
				Entry.StagedPostHash = FXxHash128::HashBuffer(*PostBytes);
				Entry.ExpectedPostFingerprint.FileSize = PostBytes->size();
				Entry.ExpectedPostFingerprint.ContentHash = Entry.StagedPostHash;
			}
			const std::filesystem::path Root =
				NormalizePhysicalPath(Mount->GetContentDir())
				/ ".durin-asset-mutation"
				/ std::format("operation-{}", State->Journal.OperationId);
			if (std::ranges::find(State->Journal.Roots, Root)
				== State->Journal.Roots.end())
			{
				std::error_code DirectoryError;
				std::filesystem::create_directories(Root, DirectoryError);
				if (DirectoryError)
					return Error(EAssetError::IoError, std::format(
						"Could not create Fix Up staging root: {}",
						DirectoryError.message()));
				const std::string Marker = std::format(
					"durin-asset-mutation\n{}\n", State->Journal.OperationId);
				FAssetResult MarkerResult = SaveRelocationBytes(
					Root / "owner",
					std::span{reinterpret_cast<const uint8*>(Marker.data()),
						Marker.size()});
				if (!MarkerResult) return MarkerResult;
				State->Journal.Roots.push_back(Root);
			}
			OutIndex = State->Journal.Entries.size();
			Entry.StagedPrePath = Root / std::format("pre-{:08}", OutIndex);
			Entry.StagedPostPath = Root / std::format("post-{:08}", OutIndex);
			if (PreBytes)
			{
				FAssetResult Result = SaveRelocationBytes(Entry.StagedPrePath, *PreBytes);
				if (!Result) return Result;
			}
			if (PostBytes)
			{
				FAssetResult Result = SaveRelocationBytes(Entry.StagedPostPath, *PostBytes);
				if (!Result) return Result;
			}
			State->Journal.Entries.push_back(std::move(Entry));
			return {};
		};

		for (const auto& [SourcePath, ExpectedCount] : PackageRewriteCounts)
		{
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PreparePackage))
				return Error(EAssetError::IoError,
					"Injected Fix Up package-preparation failure.");
			const FAssetData* Data = Registry.FindAssetExactPointer(SourcePath);
			if (!Data)
				return Error(EAssetError::StaleData,
					"A package referencer is no longer registered.");
			if (LoadingPackages.contains(SourcePath))
				return Error(EAssetError::InUse,
					"A package referencer is currently loading.");
			DPackage* Loaded = FindResidentPackage(SourcePath);
			if (Loaded && Loaded->IsDirty())
				return Error(EAssetError::InUse,
					"A dirty loaded package blocks redirector Fix Up.");
			std::vector<uint8> PreBytes;
			FAssetResult Result = LoadRelocationBytes(Data->PhysicalPath, PreBytes);
			if (!Result) return Result;
			const auto Fingerprint = Registry.ReferenceIndex.SourceFingerprints.find(SourcePath);
			if (Fingerprint == Registry.ReferenceIndex.SourceFingerprints.end())
				return Error(EAssetError::StaleData,
					"A package referencer has no complete index fingerprint.");
			FAssetPackageFingerprint CurrentFingerprint;
			Result = MakePackageFingerprint(Data->PhysicalPath, PreBytes, CurrentFingerprint);
			if (!Result) return Result;
			if (CurrentFingerprint != Fingerprint->second)
				return Error(EAssetError::StaleData,
					"A package referencer changed after reference indexing.");
			std::vector<uint8> PostBytes;
			Result = RewritePackageReferences(
				PreBytes, State->Mappings, ExpectedCount, PostBytes);
			if (!Result) return Result;
			size_t JournalEntry = 0;
			Result = AddJournalEntry(
				Data->PhysicalPath, SourcePath,
				ERelocationPublicationRole::RealAsset,
				std::move(PreBytes), PostBytes, JournalEntry);
			if (!Result) return Result;
			State->Packages.push_back({SourcePath, JournalEntry, Loaded});

			FPackageFile PostFile;
			Result = ReadPackageMetadata(PostBytes, PostFile);
			if (!Result) return Result;
			FAssetData& PostData = State->PostAssets.at(SourcePath);
			PostData.AssetClassName = PostFile.AssetClassName;
			PostData.EntryKind = PostFile.EntryKind;
			PostData.RedirectDestination = PostFile.RedirectDestination;
			PostData.FormatVersion = PostFile.FormatVersion;
			PostData.Dependencies = PostFile.Dependencies;

			if (Loaded)
			{
				std::unordered_set<FSoftObjectPtr*> Seen;
				for (const FAssetRedirectorFixupMapping& Mapping : State->Mappings)
				{
					std::vector<FSoftObjectPtr*> Values;
					Result = CollectLoadedPackageSoftReferences(
						Loaded, Mapping.RedirectorPath, Values);
					if (!Result) return Result;
					for (FSoftObjectPtr* Value : Values)
					{
						if (!Value || !Seen.insert(Value).second) continue;
						State->LiveSoftReferences.push_back({
							.Value = Value,
							.PrePath = Mapping.RedirectorPath,
							.PostPath = Mapping.FinalPath});
					}
				}
			}
		}

		auto& StoreRegistry = GetAssetReferenceStoreRegistry();
		State->ExpectedStoreRevision = StoreRegistry.Revision;
		for (const auto& [Handle, Entry] : StoreRegistry.Stores)
		{
			IAssetReferenceStore* Store = Entry.Store;
			if (!Store)
				return Error(EAssetError::StaleData,
					"A registered asset reference store is unavailable.");
			auto Call = Entry.OwnerGate.TryEnter();
			if (Entry.OwnerGate.IsValid() && !Call)
				return Error(EAssetError::StaleData,
					"An asset reference store owner is retiring.");
			FModuleOwnedResourceLease Resource = Entry.OwnerGate.RetainResource();
			if (Entry.OwnerGate.IsValid() && !Resource)
				return Error(EAssetError::StaleData,
					"An asset reference store owner is retiring.");
			FFixupStoreState StoreState{
				.OwnerResource = std::move(Resource),
				.OwnerGate = Entry.OwnerGate,
				.Handle = Handle,
				.Store = Store};
			FAssetResult Result = Store->CaptureSnapshot(StoreState.Snapshot);
			if (!Result) return Result;
			if (StoreState.Snapshot.ProviderId.empty()
				|| StoreState.Snapshot.ProviderVersion == 0
				|| StoreState.Snapshot.Fingerprint.empty())
				return Error(EAssetError::StaleData,
					"An asset reference store returned an invalid identity or fingerprint.");
			std::ranges::sort(StoreState.Snapshot.Occurrences,
				[](const FAssetReferenceStoreOccurrence& Left,
					const FAssetReferenceStoreOccurrence& Right) {
					if (Left.StableId != Right.StableId)
						return Left.StableId < Right.StableId;
					return Left.TargetPath.GetView() < Right.TargetPath.GetView();
				});
			std::vector<FAssetReferenceRewrite> Rewrites;
			for (const FAssetReferenceStoreOccurrence& Occurrence :
				StoreState.Snapshot.Occurrences)
			{
				if (Occurrence.ProviderId != StoreState.Snapshot.ProviderId
					|| Occurrence.StableId.empty())
					return Error(EAssetError::StaleData,
						"An asset reference store returned an invalid occurrence.");
				if (const FAssetPath* Destination = FindFixupDestination(
						Occurrence.TargetPath, State->Mappings))
				{
					State->StoreOccurrences.push_back(Occurrence);
					Rewrites.push_back({
						.StableId = Occurrence.StableId,
						.SourcePath = Occurrence.TargetPath,
						.DestinationPath = *Destination});
				}
			}
			if (!Rewrites.empty())
			{
				if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PrepareStore))
					return Error(EAssetError::IoError,
						"Injected Fix Up store-preparation failure.");
				Result = Store->PrepareRewrite(
					Rewrites, StoreState.Snapshot.Fingerprint,
					StoreState.Contribution);
				if (!Result) return Result;
				if (StoreState.Contribution.Fingerprint
						!= StoreState.Snapshot.Fingerprint
					|| StoreState.Contribution.Rewrites != Rewrites
					|| !StoreState.Contribution.Revalidate
					|| !StoreState.Contribution.Apply
					|| !StoreState.Contribution.Restore
					|| !StoreState.Contribution.Verify)
					return Error(EAssetError::StaleData,
						"An asset reference store returned an incomplete rewrite contribution.");
				for (FAssetReferenceStorePackageRewrite& PackageRewrite :
					StoreState.Contribution.PackageRewrites)
				{
					const FAssetData* Data = Registry.FindAssetExactPointer(
						PackageRewrite.PackagePath);
					if (!PackageRewrite.PackagePath.IsValid() || !Data
						|| Data->EntryKind == EAssetRegistryEntryKind::Redirector)
						return Error(EAssetError::StaleData,
							"An asset reference store returned an invalid package participant.");
					if (LoadingPackages.contains(PackageRewrite.PackagePath))
						return Error(EAssetError::InUse,
							"An asset reference-store package is currently loading.");
					DPackage* Loaded = FindResidentPackage(PackageRewrite.PackagePath);
					if (Loaded && Loaded->IsDirty())
						return Error(EAssetError::InUse,
							"A dirty external-reference package blocks redirector Fix Up.");
					std::vector<uint8> CurrentBytes;
					Result = LoadRelocationBytes(Data->PhysicalPath, CurrentBytes);
					if (!Result) return Result;
					if (CurrentBytes != PackageRewrite.PreBytes)
						return Error(EAssetError::StaleData,
							"An asset reference-store package changed during rewrite preparation.");
					Result = ValidateAssetPackageBytes(PackageRewrite.PostBytes);
					if (!Result) return Result;
					size_t JournalEntry = 0;
					Result = AddJournalEntry(
						Data->PhysicalPath, PackageRewrite.PackagePath,
						ERelocationPublicationRole::RealAsset,
						std::move(PackageRewrite.PreBytes),
						PackageRewrite.PostBytes, JournalEntry);
					if (!Result) return Result;
					State->Packages.push_back({
						PackageRewrite.PackagePath, JournalEntry, Loaded});

					FPackageFile PostFile;
					Result = ReadPackageMetadata(
						PackageRewrite.PostBytes, PostFile);
					if (!Result) return Result;
					FAssetData& PostData = State->PostAssets.at(
						PackageRewrite.PackagePath);
					PostData.AssetClassName = PostFile.AssetClassName;
					PostData.EntryKind = PostFile.EntryKind;
					PostData.RedirectDestination =
						PostFile.RedirectDestination;
					PostData.FormatVersion = PostFile.FormatVersion;
					PostData.Dependencies = PostFile.Dependencies;
				}
			}
			State->Stores.push_back(std::move(StoreState));
		}
		std::ranges::sort(State->Stores,
			[](const FFixupStoreState& Left, const FFixupStoreState& Right) {
				return Left.Snapshot.ProviderId < Right.Snapshot.ProviderId;
			});
		for (size_t Index = 1; Index < State->Stores.size(); ++Index)
			if (State->Stores[Index - 1].Snapshot.ProviderId
				== State->Stores[Index].Snapshot.ProviderId)
				return Error(EAssetError::AlreadyExists,
					"Asset reference store provider ids must be unique.");
		std::ranges::sort(State->StoreOccurrences,
			[](const FAssetReferenceStoreOccurrence& Left,
				const FAssetReferenceStoreOccurrence& Right) {
				if (Left.ProviderId != Right.ProviderId)
					return Left.ProviderId < Right.ProviderId;
				if (Left.StableId != Right.StableId)
					return Left.StableId < Right.StableId;
				return Left.TargetPath.GetView() < Right.TargetPath.GetView();
			});

		if (Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			for (const FAssetPath& Alias : State->Redirectors)
			{
				const FAssetData& Data = State->ExpectedAssets.at(Alias);
				std::vector<uint8> PreBytes;
				FAssetResult Result = LoadRelocationBytes(Data.PhysicalPath, PreBytes);
				if (!Result) return Result;
				size_t Ignored = 0;
				Result = AddJournalEntry(
					Data.PhysicalPath, Alias,
					ERelocationPublicationRole::Redirector,
					std::move(PreBytes), std::nullopt, Ignored);
				if (!Result) return Result;
				State->PostAssets.erase(Alias);
			}
		}

		State->Journal.State = EAssetMutationState::Prepared;
		WriteMutationJournalState(State->Journal);
		OutState = std::move(State);
		return {};
	}

	auto FAssetRuntimeState::PrepareRedirectorFixupTransaction(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		OutSummary = {};
		OutTransaction = {};
		std::shared_ptr<FAssetRedirectorFixupState> Fixup;
		FAssetResult Result = PrepareRedirectorFixupState(
			Redirectors, Mode, Fixup);
		if (!Result) return Result;

		OutSummary.Mode = Fixup->Mode;
		OutSummary.RegistryRevision = Fixup->ExpectedRegistryRevision;
		OutSummary.Redirectors = Fixup->Redirectors;
		OutSummary.FinalPathMappings = Fixup->Mappings;
		OutSummary.PackageOccurrences = Fixup->PackageOccurrences;
		OutSummary.StoreOccurrences = Fixup->StoreOccurrences;
		OutSummary.DeletableRedirectors = Fixup->DeletableRedirectors;

		auto TransactionState = std::make_shared<FAssetMutationTransaction::FState>();
		TransactionState->Summary = FAssetMutationSummary(
			EAssetMutationOperationKind::RedirectorFixup,
			Fixup->ExpectedRegistryRevision,
			Fixup->Redirectors);
		TransactionState->CommitOperation = [Fixup] {
			return FAssetRuntimeState::Get().CommitRedirectorFixup(Fixup);
		};
		TransactionState->IsRecoveryRequired = [Fixup] {
			return Fixup->Journal.State == EAssetMutationState::RecoveryRequired;
		};
		TransactionState->PopulateResultDetails = [Fixup](
			FAssetMutationResultDetails& Details) {
			if (!Details.Result)
			{
				Details.FailedPaths = Fixup->Redirectors;
				return;
			}
			for (const FAssetReferenceEdge& Occurrence :
				Fixup->PackageOccurrences)
				Details.RewrittenPaths.push_back(Occurrence.SourcePackage);
			std::ranges::sort(Details.RewrittenPaths,
				[](const FAssetPath& Left, const FAssetPath& Right) {
					return Left.GetView() < Right.GetView();
				});
			Details.RewrittenPaths.erase(std::ranges::unique(
				Details.RewrittenPaths).begin(), Details.RewrittenPaths.end());
			if (Fixup->Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
				Details.DeletedPaths = Fixup->DeletableRedirectors;
			else
				Details.RetainedPaths = Fixup->Redirectors;
		};
		TransactionState->LastResult.State =
			EAssetMutationTransactionState::Prepared;
		TransactionState->LastResult.RegistryRevision = Registry.GetRevision();
		OutTransaction.State = std::move(TransactionState);
		return {};
	}

	auto FAssetRuntimeState::ValidateRedirectorFixupCommit(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Error(EAssetError::StaleData,
				"The redirector Fix Up transaction state is empty.");
		const auto& State = *Fixup;
		if (State.Journal.State == EAssetMutationState::RecoveryRequired)
			return Error(EAssetError::IoError,
				"AssetMutationRecoveryRequired: the Fix Up journal requires recovery.");
		if (State.Journal.State != EAssetMutationState::Prepared)
			return Error(EAssetError::StaleData,
				"The redirector Fix Up plan is no longer prepared.");
		if (Registry.GetRevision() != State.ExpectedRegistryRevision
			|| Registry.Assets != State.ExpectedAssets)
			return Error(EAssetError::StaleData,
				"The asset registry changed after redirector Fix Up analysis.");
		const auto& Stores = GetAssetReferenceStoreRegistry();
		if (Stores.Revision != State.ExpectedStoreRevision
			|| Stores.Stores.size() != State.Stores.size())
			return Error(EAssetError::StaleData,
				"Asset reference store registration changed after Fix Up analysis.");
		for (const FFixupStoreState& StoreState : State.Stores)
		{
			const auto Current = Stores.Stores.find(StoreState.Handle);
			if (Current == Stores.Stores.end()
				|| Current->second.Store != StoreState.Store)
				return Error(EAssetError::StaleData,
					"An asset reference store became unavailable.");
			auto Call = StoreState.OwnerGate.TryEnter();
			if (StoreState.OwnerGate.IsValid() && !Call)
				return Error(EAssetError::StaleData,
					"An asset reference store owner is retiring.");
			FAssetReferenceStoreSnapshot Snapshot;
			FAssetResult Result = StoreState.Store->CaptureSnapshot(Snapshot);
			if (!Result) return Result;
			std::ranges::sort(Snapshot.Occurrences,
				[](const FAssetReferenceStoreOccurrence& Left,
					const FAssetReferenceStoreOccurrence& Right) {
					if (Left.StableId != Right.StableId)
						return Left.StableId < Right.StableId;
					return Left.TargetPath.GetView() < Right.TargetPath.GetView();
				});
			if (Snapshot.ProviderId != StoreState.Snapshot.ProviderId
				|| Snapshot.ProviderVersion != StoreState.Snapshot.ProviderVersion
				|| Snapshot.Fingerprint != StoreState.Snapshot.Fingerprint
				|| Snapshot.Occurrences != StoreState.Snapshot.Occurrences)
				return Error(EAssetError::StaleData,
					"An asset reference store changed after Fix Up analysis.");
			if (StoreState.Contribution.Revalidate)
			{
				Result = StoreState.Contribution.Revalidate();
				if (!Result) return Result;
			}
		}
		for (const FFixupPackageState& PackageState : State.Packages)
		{
			if (PackageState.LoadedPackage)
			{
				if (FindResidentPackage(PackageState.SourcePath)
						!= PackageState.LoadedPackage
					|| PackageState.LoadedPackage->IsDirty())
					return Error(EAssetError::StaleData,
						"A loaded Fix Up package changed after analysis.");
			}
		}
		for (const FAssetMutationJournalEntry& Entry : State.Journal.Entries)
		{
			std::error_code ExistsError;
			if (!Entry.bPreExists
				|| !std::filesystem::exists(Entry.PhysicalPath, ExistsError)
				|| ExistsError)
				return Error(EAssetError::StaleData,
					"A Fix Up file participant changed occupancy.");
			FAssetPackageFingerprint Fingerprint;
			FAssetResult Result = FingerprintRelocationFile(
				Entry.PhysicalPath, Fingerprint);
			if (!Result) return Result;
			if (Fingerprint != Entry.ExpectedPreFingerprint)
				return Error(EAssetError::StaleData,
					"A Fix Up file participant changed after analysis.");
			if (Entry.bPostExists)
			{
				std::vector<uint8> StagedBytes;
				Result = LoadRelocationBytes(Entry.StagedPostPath, StagedBytes);
				if (!Result || FXxHash128::HashBuffer(StagedBytes)
						!= Entry.StagedPostHash)
					return Error(EAssetError::StaleData,
						"A staged Fix Up output changed after analysis.");
			}
		}
		return {};
	}

	auto FAssetRuntimeState::CommitRedirectorFixup(
		const std::shared_ptr<FAssetRedirectorFixupState>& Fixup) -> FAssetResult
	{
		if (GIsGameThreadIdInitialized) CheckGameThread();
		if (!Fixup)
			return Error(EAssetError::StaleData,
				"The redirector Fix Up transaction state is empty.");
		auto& State = *Fixup;
		FAssetResult Result = ValidateRedirectorFixupCommit(Fixup);
		if (!Result) return Result;
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::StageOriginal))
			return Error(EAssetError::IoError,
				"Injected Fix Up original-staging failure.");

		State.Journal.State = EAssetMutationState::Publishing;
		WriteMutationJournalState(State.Journal);
		std::vector<size_t> PublishedPackages;
		std::vector<size_t> PublishedRedirectors;
		size_t ChangedLiveCount = 0;
		auto EnterRecovery = [&](std::string Message) -> FAssetResult {
			State.Journal.State = EAssetMutationState::RecoveryRequired;
			WriteMutationJournalState(State.Journal);
			return Error(EAssetError::IoError,
				std::format("AssetMutationRecoveryRequired: {}", Message));
		};
		auto Compensate = [&](FAssetResult Failure) -> FAssetResult {
			State.Journal.State = EAssetMutationState::Compensating;
			WriteMutationJournalState(State.Journal);
			for (auto It = PublishedRedirectors.rbegin();
				It != PublishedRedirectors.rend(); ++It)
			{
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::CompensatePackage))
					return EnterRecovery("redirector compensation was interrupted.");
				Result = PublishRelocationFile(State.Journal.Entries[*It], false);
				if (!Result) return EnterRecovery(Result.Message);
				State.Journal.Entries[*It].bCompensated = true;
				WriteMutationJournalState(State.Journal);
			}
			for (size_t Count = ChangedLiveCount; Count > 0; --Count)
			{
				FFixupLiveSoftReference& Live = State.LiveSoftReferences[Count - 1];
				FSoftObjectPath Path;
				FSoftObjectPath::TryCreate(Live.PrePath.GetView(), Path);
				Live.Value->SetPath(std::move(Path));
			}
			for (auto It = State.Stores.rbegin(); It != State.Stores.rend(); ++It)
			{
				if (!It->bApplied) continue;
				auto Call = It->OwnerGate.TryEnter();
				if (It->OwnerGate.IsValid() && !Call)
					return EnterRecovery("reference-store owner retired before compensation.");
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::CompensateStore))
					return EnterRecovery("reference-store compensation was interrupted.");
				FAssetResult RestoreResult = It->Contribution.Restore();
				if (!RestoreResult) return EnterRecovery(RestoreResult.Message);
				It->bApplied = false;
			}
			for (auto It = PublishedPackages.rbegin();
				It != PublishedPackages.rend(); ++It)
			{
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::CompensatePackage))
					return EnterRecovery("package compensation was interrupted.");
				Result = PublishRelocationFile(State.Journal.Entries[*It], false);
				if (!Result) return EnterRecovery(Result.Message);
				State.Journal.Entries[*It].bCompensated = true;
				WriteMutationJournalState(State.Journal);
			}
			State.Journal.State = EAssetMutationState::Restored;
			WriteMutationJournalState(State.Journal);
			return Failure;
		};

		uint64 PublicationOrder = 0;
		for (size_t Index = 0; Index < State.Journal.Entries.size(); ++Index)
		{
			FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
			if (Entry.Role == ERelocationPublicationRole::Redirector) continue;
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishPackage))
				return Compensate(Error(EAssetError::IoError,
					"Injected Fix Up package-publication failure."));
			Entry.PublicationOrder = PublicationOrder++;
			Result = PublishRelocationFile(Entry, true);
			if (!Result) return Compensate(std::move(Result));
			Entry.bCompleted = true;
			PublishedPackages.push_back(Index);
			WriteMutationJournalState(State.Journal);
		}
		for (FFixupStoreState& Store : State.Stores)
		{
			if (Store.Contribution.Rewrites.empty()) continue;
			auto Call = Store.OwnerGate.TryEnter();
			if (Store.OwnerGate.IsValid() && !Call)
				return Compensate(Error(EAssetError::StaleData,
					"An asset reference store owner is retiring."));
			if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::ApplyStore))
				return Compensate(Error(EAssetError::IoError,
					"Injected Fix Up store-publication failure."));
			Result = Store.Contribution.Apply();
			if (!Result) return Compensate(std::move(Result));
			Store.bApplied = true;
		}
		for (FFixupLiveSoftReference& Live : State.LiveSoftReferences)
		{
			FSoftObjectPath Path;
			if (!FSoftObjectPath::TryCreate(Live.PostPath.GetView(), Path))
				return Compensate(Error(EAssetError::InvalidPath,
					"A prepared live soft-reference destination became invalid."));
			Live.Value->SetPath(std::move(Path));
			++ChangedLiveCount;
		}

		Result = RebuildReferenceProjectionForPublishedEntries(
			State.Journal.Entries, State.PostAssets,
			State.PostEdges, State.PostFingerprints);
		if (!Result) return Compensate(std::move(Result));
		if (State.Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			std::erase_if(State.PostEdges, [&](const FAssetReferenceEdge& Edge) {
				return std::ranges::binary_search(
					State.Redirectors, Edge.SourcePackage,
					[](const FAssetPath& Left, const FAssetPath& Right) {
						return Left.GetView() < Right.GetView();
					});
			});
			for (const FAssetPath& Alias : State.Redirectors)
				State.PostFingerprints.erase(Alias);
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::Verify))
			return Compensate(Error(EAssetError::IoError,
				"Injected Fix Up verification failure."));
		for (const FAssetReferenceEdge& Edge : State.PostEdges)
			if (FindFixupDestination(Edge.TargetPath, State.Mappings))
				return Compensate(Error(EAssetError::InUse, std::format(
					"Fix Up verification found a remaining package occurrence at {}:{}.",
					Edge.SourcePackage.ToString(), Edge.DisplayRoute)));
		for (FFixupStoreState& Store : State.Stores)
		{
			auto Call = Store.OwnerGate.TryEnter();
			if (Store.OwnerGate.IsValid() && !Call)
				return Compensate(Error(EAssetError::StaleData,
					"An asset reference store owner is retiring."));
			if (Store.Contribution.Verify)
			{
				Result = Store.Contribution.Verify();
				if (!Result) return Compensate(std::move(Result));
			}
			FAssetReferenceStoreSnapshot Snapshot;
			Result = Store.Store->CaptureSnapshot(Snapshot);
			if (!Result) return Compensate(std::move(Result));
			for (const FAssetReferenceStoreOccurrence& Occurrence : Snapshot.Occurrences)
				if (FindFixupDestination(Occurrence.TargetPath, State.Mappings))
					return Compensate(Error(EAssetError::InUse,
						"Fix Up verification found a remaining external occurrence."));
		}

		if (State.Mode == EAssetRedirectorFixupMode::RewriteAndDelete)
		{
			for (size_t Index = 0; Index < State.Journal.Entries.size(); ++Index)
			{
				FAssetMutationJournalEntry& Entry = State.Journal.Entries[Index];
				if (Entry.Role != ERelocationPublicationRole::Redirector) continue;
				if (ConsumeFixupFailure(
						EAssetRedirectorFixupFailurePoint::DeleteRedirector))
					return Compensate(Error(EAssetError::IoError,
						"Injected Fix Up redirector-deletion failure."));
				Entry.PublicationOrder = PublicationOrder++;
				Result = PublishRelocationFile(Entry, true);
				if (!Result) return Compensate(std::move(Result));
				Entry.bCompleted = true;
				PublishedRedirectors.push_back(Index);
				WriteMutationJournalState(State.Journal);
			}
		}
		if (ConsumeFixupFailure(EAssetRedirectorFixupFailurePoint::PublishRegistry))
			return Compensate(Error(EAssetError::IoError,
				"Injected Fix Up registry-publication failure."));

		for (const FFixupPackageState& PackageState : State.Packages)
		{
			const FAssetMutationJournalEntry& Entry =
				State.Journal.Entries[PackageState.JournalEntry];
			FAssetData& Data = State.PostAssets.at(PackageState.SourcePath);
			std::error_code MetadataError;
			Data.FileSize = std::filesystem::file_size(Entry.PhysicalPath, MetadataError);
			Data.LastWriteTime = std::filesystem::last_write_time(
				Entry.PhysicalPath, MetadataError);
			if (MetadataError)
				return Compensate(Error(EAssetError::IoError,
					"Could not inspect a published Fix Up package."));
			Data.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(
				Data.LastWriteTime);
		}
		Registry.Assets = State.PostAssets;
		Registry.ReferenceIndex.Edges = State.PostEdges;
		Registry.ReferenceIndex.SourceFingerprints = State.PostFingerprints;
		Registry.ReferenceIndex.Errors = State.PostErrors;
		Registry.ReferenceIndex.bComplete = State.Mode
			== EAssetRedirectorFixupMode::RewriteAndDelete
			? true : State.bPostIndexComplete;
		Registry.ReferenceIndex.bSnapshotDirty = true;
		Registry.RebuildRedirectorIndex();
		Registry.bPersistentSnapshotDirty = true;
		++Registry.Revision;
		State.ExpectedRegistryRevision = Registry.Revision;
		State.ExpectedAssets = Registry.Assets;
		State.Journal.State = EAssetMutationState::Committed;
		WriteMutationJournalState(State.Journal);
		return {};
	}

	auto FAssetRuntimeState::AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult
	{
		OutAnalysis = {};
		OutAnalysis.AssetPath = Path;
		const FAssetData* Data = Registry.FindAssetExactPointer(Path);
		if (!Path.IsValid() || !Data) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));
		OutAnalysis.bRedirector =
			Data->EntryKind == EAssetRegistryEntryKind::Redirector;
		OutAnalysis.RedirectDestination = Data->RedirectDestination;

		for (const auto& [OtherPath, OtherData] : Registry.GetAssets())
		{
			if (OtherPath != Path && std::ranges::find(OtherData.Dependencies, Path) != OtherData.Dependencies.end())
				OutAnalysis.DirectReferencers.push_back(OtherPath);
		}
		std::ranges::sort(OutAnalysis.DirectReferencers, [](const FAssetPath& A, const FAssetPath& B) { return A.GetView() < B.GetView(); });
		OutAnalysis.bLoaded = ResidentPackages.contains(Path);
		OutAnalysis.bLoading = LoadingPackages.contains(Path);

		const FAssetResult CompanionResult =
			InspectAssetCompanionFiles(*Data, OutAnalysis.CompanionFiles);
		if (!CompanionResult)
		{
			OutAnalysis.Warning = std::format(
				"Could not determine companion files: {} Only the main asset file will be deleted.",
				CompanionResult.Message);
		}
		return {};
	}

	auto FAssetRuntimeState::PrepareAssetDeletionTransaction(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionTransaction& OutTransaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		OutTransaction = {};
		OutTransaction.State = std::make_shared<FAssetDeletionTransaction::FState>();
		auto& OutToken = *OutTransaction.State;
		OutBlockers.clear();
		if (RuntimeConfiguration.IsCooked())
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
		OutToken.ReferenceStoreRevision =
			GetAssetReferenceStoreRegistry().Revision;
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

		for (const FAssetPath& Path : SortedPaths)
		{
			const FAssetData* Data = Registry.FindAssetExactPointer(Path);
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
				.bLoaded = ResidentPackages.contains(Path)};
			if (LoadingPackages.contains(Path))
				AddBlocker(
					EAssetDeletionBatchBlocker::LoadingPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset is currently loading.");
			if (const auto Loaded = ResidentPackages.find(Path);
				Loaded != ResidentPackages.end() && Loaded->second
				&& Loaded->second->IsDirty())
				AddBlocker(
					EAssetDeletionBatchBlocker::DirtyPackage,
					Path,
					{},
					Data->PhysicalPath,
					"Asset has unsaved changes.");

			const FAssetResult CompanionResult =
				InspectAssetCompanionFiles(*Data, Entry.CompanionFiles);
			if (!CompanionResult)
				AddBlocker(
					EAssetDeletionBatchBlocker::CompanionInspectionFailed,
					Path,
					{},
					Data->PhysicalPath,
					CompanionResult.Message);
			OutToken.Entries.push_back(std::move(Entry));
		}

		// Deletion is closed over final targets and every alias that resolves to them.
		// This prevents an alias-only delete from silently invalidating authored old paths,
		// and prevents a target delete from leaving redirectors with no destination.
		std::unordered_map<FAssetPath, std::vector<FAssetPath>> RedirectorsByTarget;
		for (const auto& [AliasPath, AliasData] : Registry.GetAssets())
		{
			if (AliasData.EntryKind != EAssetRegistryEntryKind::Redirector) continue;
			const FAssetPathResolveResult Resolution =
				Registry.ResolveAssetPath(AliasPath);
			if (!Resolution) continue;
			RedirectorsByTarget[Resolution.FinalPath].push_back(AliasPath);
		}
		for (auto& [TargetPath, Redirectors] : RedirectorsByTarget)
			std::ranges::sort(
				Redirectors,
				[](const FAssetPath& A, const FAssetPath& B) {
					return A.GetView() < B.GetView();
				});

		for (const FAssetPath& Path : SortedPaths)
		{
			const FAssetData* Data = Registry.FindAssetExactPointer(Path);
			if (!Data) continue;
			if (Data->EntryKind == EAssetRegistryEntryKind::Redirector)
			{
				const FAssetPathResolveResult Resolution =
					Registry.ResolveAssetPath(Path);
				if (!Resolution || !DeletionSet.contains(Resolution.FinalPath))
				{
					const FAssetPath Related = Resolution.FinalPath.IsValid()
						? Resolution.FinalPath
						: Data->RedirectDestination;
					AddBlocker(
						EAssetDeletionBatchBlocker::RedirectorTargetNotSelected,
						Path,
						Related,
						Data->PhysicalPath,
						Resolution
							? std::format(
								"Redirector {} cannot be deleted alone. Select its final target {} and every alias to that target, or run Fix Up Redirectors.",
								Path.ToString(), Resolution.FinalPath.ToString())
							: std::format(
								"Redirector {} cannot be deleted because its target does not resolve. Repair the redirector before deleting it.",
								Path.ToString()));
				}
				continue;
			}

			const auto Found = RedirectorsByTarget.find(Path);
			if (Found == RedirectorsByTarget.end()) continue;
			std::vector<FAssetPath> SelectedRedirectors;
			for (const FAssetPath& Redirector : Found->second)
			{
				if (DeletionSet.contains(Redirector))
					SelectedRedirectors.push_back(Redirector);
				else
				{
					const FAssetData* RedirectorData =
						Registry.FindAssetExactPointer(Redirector);
					AddBlocker(
						EAssetDeletionBatchBlocker::TargetRedirectorsNotSelected,
						Path,
						Redirector,
						RedirectorData
							? std::filesystem::path(RedirectorData->PhysicalPath)
							: std::filesystem::path{},
						std::format(
							"Asset {} still has redirector {}. Reveal redirectors and include every alias, or run Fix Up Redirectors before deleting the target.",
							Path.ToString(), Redirector.ToString()));
				}
			}
			if (!SelectedRedirectors.empty())
				OutToken.Warnings.push_back({
					.TargetPath = Path,
					.RedirectorPaths = std::move(SelectedRedirectors),
					.Details = std::format(
						"Deleting {} together with {} redirector(s) permanently invalidates every authored old path after the operation leaves Undo history.",
						Path.ToString(), Found->second.size())});
		}

		const FAssetReferenceIndex& ReferenceIndex = Registry.GetReferenceIndex();
		for (const FAssetPath& Path : SortedPaths)
		{
			std::vector<FAssetPath> SoftReferencers;
			for (const FAssetReferenceEdge& Edge :
				 ReferenceIndex.FindReferencers(Path))
			{
				if (Edge.Kind != EAssetReferenceKind::SoftObject
					|| DeletionSet.contains(Edge.SourcePackage))
					continue;
				SoftReferencers.push_back(Edge.SourcePackage);
			}
			std::ranges::sort(
				SoftReferencers,
				[](const FAssetPath& A, const FAssetPath& B) {
					return A.GetView() < B.GetView();
				});
			SoftReferencers.erase(
				std::unique(SoftReferencers.begin(), SoftReferencers.end()),
				SoftReferencers.end());
			if (!SoftReferencers.empty())
			{
				const size_t SoftReferencerCount = SoftReferencers.size();
				OutToken.Warnings.push_back({
					.TargetPath = Path,
					.SoftReferencerPaths = std::move(SoftReferencers),
					.Details = std::format(
						"Deleting {} leaves {} package(s) with dangling soft references. Review the referencers before confirming.",
						Path.ToString(),
						SoftReferencerCount)});
			}
		}
		if (!SortedPaths.empty() && !ReferenceIndex.IsComplete())
			OutToken.Warnings.push_back({
				.TargetPath = SortedPaths.front(),
				.Details = std::format(
					"The package reference index is incomplete ({} error{}), so soft-reference warning counts may be incomplete.",
					ReferenceIndex.GetErrors().size(),
					ReferenceIndex.GetErrors().size() == 1 ? "" : "s")});

		auto& StoreRegistry = GetAssetReferenceStoreRegistry();
		if (!SortedPaths.empty())
		{
			for (const auto& [Handle, Entry] : StoreRegistry.Stores)
			{
				(void)Handle;
				FAssetReferenceStoreSnapshot Snapshot;
				auto Call = Entry.OwnerGate.TryEnter();
				const bool bAdmitted = !Entry.OwnerGate.IsValid() || Call;
				const FAssetResult SnapshotResult = Entry.Store && bAdmitted
					? Entry.Store->CaptureSnapshot(Snapshot)
					: Error(EAssetError::StaleData,
						"A registered asset reference store is unavailable.");
				if (!SnapshotResult)
				{
					AddBlocker(
						EAssetDeletionBatchBlocker::ReferenceStoreInspectionFailed,
						SortedPaths.front(),
						{},
						{},
						std::format(
							"A persistent asset-reference owner could not be inspected: {}",
							SnapshotResult.Message));
					continue;
				}
				for (const FAssetPath& Path : SortedPaths)
				{
					std::vector<std::string> Occurrences;
					for (const FAssetReferenceStoreOccurrence& Occurrence :
						 Snapshot.Occurrences)
					{
						if (Occurrence.TargetPath != Path) continue;
						Occurrences.push_back(std::format(
							"{}:{} ({})",
							Occurrence.ProviderId,
							Occurrence.StableId,
							Occurrence.DisplayRoute));
					}
					std::ranges::sort(Occurrences);
					if (!Occurrences.empty())
					{
						const size_t OccurrenceCount = Occurrences.size();
						OutToken.Warnings.push_back({
							.TargetPath = Path,
							.ExternalOccurrences = std::move(Occurrences),
							.Details = std::format(
								"Deleting {} leaves {} persistent external owner occurrence(s) dangling. Run Fix Up Redirectors or update those owners before confirming.",
								Path.ToString(), OccurrenceCount)});
					}
				}
			}
		}
		std::ranges::sort(
			OutToken.Warnings,
			[](const FAssetDeletionBatchWarning& A,
				const FAssetDeletionBatchWarning& B) {
				if (A.TargetPath.GetView() != B.TargetPath.GetView())
					return A.TargetPath.GetView() < B.TargetPath.GetView();
				return A.Details < B.Details;
			});

		for (const auto& [OtherPath, OtherData] : Registry.GetAssets())
		{
			if (DeletionSet.contains(OtherPath)) continue;
			for (const FAssetPath& Dependency : OtherData.Dependencies)
			{
				if (!DeletionSet.contains(Dependency)) continue;
				// Redirect hard blockers have dedicated actionable closure diagnostics.
				if (OtherData.EntryKind == EAssetRegistryEntryKind::Redirector)
					continue;
				const bool bLoadedReference = ResidentPackages.contains(OtherPath);
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
			if (!InspectAssetCompanionFiles(OwnerData, Files)) continue;
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

	auto FAssetRuntimeState::ValidateAssetDeletionTransaction(
		const FAssetDeletionTransaction& Transaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		if (!Transaction.State)
			return Error(EAssetError::StaleData,
				"The asset deletion transaction is empty.");
		const auto& Token = *Transaction.State;
		std::vector<FAssetPath> Paths;
		Paths.reserve(Token.Entries.size());
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			Paths.push_back(Entry.RegistryEntry.PackagePath);

		FAssetDeletionTransaction CurrentTransaction;
		FAssetResult Result = PrepareAssetDeletionTransaction(
			Paths, Token.PhysicalRoots, CurrentTransaction, OutBlockers);
		if (!Result || !OutBlockers.empty()) return Result;
		const auto& Current = *CurrentTransaction.State;
		if (Current.Entries.size() != Token.Entries.size())
			return Error(EAssetError::InUse,
				"The asset deletion set changed after confirmation.");
		if (Current.ReferenceStoreRevision != Token.ReferenceStoreRevision)
			return Error(EAssetError::InUse,
				"Persistent asset-reference owners changed after deletion confirmation.");
		if (Current.Warnings != Token.Warnings)
			return Error(EAssetError::InUse,
				"Asset references changed after deletion confirmation.");
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

	auto FAssetRuntimeState::UnloadAssetDeletionTransaction(
		const FAssetDeletionTransaction& Transaction) -> FAssetResult
	{
		const auto& Token = *Transaction.State;
		std::vector<DPackage*> Packages;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FAssetPath& Path = Entry.RegistryEntry.PackagePath;
			if (LoadingPackages.contains(Path))
				return Error(EAssetError::InUse, std::format(
					"Asset {} is currently loading.", Path.ToString()));
			const auto Loaded = ResidentPackages.find(Path);
			if (Loaded == ResidentPackages.end()) continue;
			if (Loaded->second && Loaded->second->IsDirty())
				return Error(EAssetError::InUse, std::format(
					"Asset {} has unsaved changes.", Path.ToString()));
			if (Loaded->second) Packages.push_back(Loaded->second);
		}
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			ResidentPackages.erase(Entry.RegistryEntry.PackagePath);
		for (DPackage* Package : Packages)
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!Packages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetRuntimeState::RemoveAssetDeletionRegistryProjection(
		const FAssetDeletionTransaction& Transaction) -> FAssetResult
	{
		const auto& Token = *Transaction.State;
		std::unordered_set<FAssetPath> DeletionSet;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
			DeletionSet.insert(Entry.RegistryEntry.PackagePath);
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FAssetData* Current = Registry.FindAssetExactPointer(
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

	auto FAssetRuntimeState::RestoreAssetDeletionRegistryProjection(
		const FAssetDeletionTransaction& Transaction) -> FAssetResult
	{
		const auto& Token = *Transaction.State;
		for (const FAssetDeletionBatchEntry& Entry : Token.Entries)
		{
			const FAssetPath& Path = Entry.RegistryEntry.PackagePath;
			if (Registry.FindAssetExactPointer(Path) || ResidentPackages.contains(Path))
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

	auto FAssetDeletionTransaction::Commit(
		const FAssetDeletionPhysicalTransition& Transition) -> FAssetResult
	{
		if (!State || State->TransactionState != EAssetMutationTransactionState::Prepared)
			return Error(EAssetError::StaleData,
				"Only a prepared asset deletion transaction can be committed.");
		if (!Transition.Stage || !Transition.Restore)
			return Error(EAssetError::StaleData,
				"The asset deletion transaction has no physical transition.");

		FAssetRuntimeState& Runtime = FAssetRuntimeState::Get();
		std::vector<FAssetDeletionBatchBlocker> Blockers;
		FAssetResult Result = Runtime.ValidateAssetDeletionTransaction(*this, Blockers);
		if (!Result) return Result;
		if (!Blockers.empty())
			return Error(EAssetError::InUse, Blockers.front().Details);
		Result = Runtime.UnloadAssetDeletionTransaction(*this);
		if (!Result) return Result;
		Result = Transition.Stage();
		if (!Result)
		{
			if (Transition.IsRecoveryRequired && Transition.IsRecoveryRequired())
				State->TransactionState = EAssetMutationTransactionState::RecoveryRequired;
			return Result;
		}
		Result = Runtime.RemoveAssetDeletionRegistryProjection(*this);
		if (!Result)
		{
			const FAssetResult Restore = Transition.Restore();
			if (!Restore)
			{
				State->TransactionState = EAssetMutationTransactionState::RecoveryRequired;
				return Error(EAssetError::IoError, std::format(
					"{} Physical compensation also failed: {}", Result.Message, Restore.Message));
			}
			return Result;
		}
		State->TransactionState = EAssetMutationTransactionState::Committed;
		return {};
	}

	auto FAssetDeletionTransaction::Undo(
		const FAssetDeletionPhysicalTransition& Transition) -> FAssetResult
	{
		if (!State || State->TransactionState != EAssetMutationTransactionState::Committed)
			return Error(EAssetError::StaleData,
				"Only a committed asset deletion transaction can be undone.");
		if (!Transition.Stage || !Transition.Restore)
			return Error(EAssetError::StaleData,
				"The asset deletion transaction has no physical transition.");

		FAssetResult Result = Transition.Restore();
		if (!Result)
		{
			if (Transition.IsRecoveryRequired && Transition.IsRecoveryRequired())
				State->TransactionState = EAssetMutationTransactionState::RecoveryRequired;
			return Result;
		}
		Result = FAssetRuntimeState::Get().RestoreAssetDeletionRegistryProjection(*this);
		if (!Result)
		{
			const FAssetResult Restage = Transition.Stage();
			if (!Restage)
			{
				State->TransactionState = EAssetMutationTransactionState::RecoveryRequired;
				return Error(EAssetError::IoError, std::format(
					"{} Physical compensation also failed: {}", Result.Message, Restage.Message));
			}
			return Result;
		}
		State->TransactionState = EAssetMutationTransactionState::Undone;
		return {};
	}

	auto FAssetDeletionTransaction::Redo(
		const FAssetDeletionPhysicalTransition& Transition) -> FAssetResult
	{
		if (!State || State->TransactionState != EAssetMutationTransactionState::Undone)
			return Error(EAssetError::StaleData,
				"Only an undone asset deletion transaction can be redone.");
		State->TransactionState = EAssetMutationTransactionState::Prepared;
		const FAssetResult Result = Commit(Transition);
		if (!Result && State->TransactionState == EAssetMutationTransactionState::Prepared)
			State->TransactionState = EAssetMutationTransactionState::Undone;
		return Result;
	}

	auto FAssetRuntimeState::DeleteAssetForTesting(const FAssetPath& Path)
		-> FAssetResult
	{
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit asset deletion.");
		FAssetDeleteAnalysis Analysis;
		FAssetResult Result = AnalyzeAssetDeletion(Path, Analysis);
		if (!Result) return Result;
		if (Analysis.bRedirector)
			return Error(EAssetError::InUse, std::format(
				"Redirector {} cannot be deleted alone. Select its final target and every alias in one batch, or run Fix Up Redirectors.",
				Path.ToString()));
		if (!Analysis.DirectReferencers.empty())
			return Error(EAssetError::InUse, std::format("Asset {} is referenced by {} asset(s).", Path.ToString(), Analysis.DirectReferencers.size()));
		if (Analysis.bLoading) return Error(EAssetError::InUse, "Asset is currently loading.");
		if (Analysis.bLoaded)
		{
			// ResidentPackages is a residency cache. Once persistent package references are gone,
			// keeping that cache entry must not force users to restart before deleting an asset.
			Result = UnloadPackage(Path);
			if (!Result) return Result;
		}

		const FAssetData* Data = Registry.FindAssetExactPointer(Path);
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

	auto FAssetRuntimeState::LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return LoadAsset(Path, nullptr, OutAsset, OutReport);
	}

	auto FAssetRuntimeState::LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		OutAsset = nullptr;
		if (OutReport) *OutReport = {
			.RequestedPath = Path,
			.PackagePath = Path};
		auto Finish = [&](FAssetResult Result) -> FAssetResult
		{
			if (OutReport)
			{
				OutReport->Error = Result.Error;
				OutReport->ErrorMessage = Result.Message;
			}
			return Result;
		};
		if (!bAcceptingRequests)
			return Finish(Error(
				EAssetError::ShuttingDown,
				"Asset loading is closed while the asset manager is shutting down."));
		if (ExpectedClass && !ExpectedClass->IsChildOf(DObject::StaticClass()))
			return Finish(Error(EAssetError::TypeMismatch, "An asset load requires a DObject class."));
		if (DPackage* Resident = FindResidentPackage(Path))
		{
			DObject* Asset = Resident->GetAsset();
			if (Asset && !Asset->IsA<DAssetRedirector>())
			{
				if (ExpectedClass && !Asset->IsA(ExpectedClass))
					return Finish(Error(EAssetError::TypeMismatch, std::format(
						"Asset {} is not a {}.", Path.ToString(),
						ExpectedClass->GetQualifiedName().ToString())));
				OutAsset = Asset;
				if (OutReport)
				{
					OutReport->CatalogRevision = Registry.GetRevision();
					OutReport->FinalPath = Path;
					OutReport->FinalAssetClassName =
						Asset->GetClass()->GetQualifiedName().ToString();
				}
				return Finish({});
			}
		}

		const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(
			Path, {.ExpectedClass = ExpectedClass});
		if (OutReport)
		{
			OutReport->CatalogRevision = Resolution.CatalogRevision;
			OutReport->FinalPath = Resolution.FinalPath;
			OutReport->RedirectChain = Resolution.RedirectChain;
			if (Resolution.FinalAssetData)
				OutReport->FinalAssetClassName = Resolution.FinalAssetData->AssetClassName;
		}
		if (!Resolution)
			return Finish(AssetPathResolutionError(Resolution));
		check(Resolution.FinalAssetData.has_value());
		return Finish(LoadAssetFromCatalog(
			*Resolution.FinalAssetData, ExpectedClass, OutAsset, OutReport));
	}

	auto FAssetRuntimeState::LoadAssetFromCatalog(
		const FAssetData& Data,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return LoadAssetFromPhysicalPath(
			Data.PackagePath, Data.PhysicalPath, ExpectedClass, OutAsset, OutReport);
	}

	auto FAssetRuntimeState::LoadAssetFromPhysicalPath(
		const FAssetPath& Path,
		std::string_view PhysicalPath,
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
		if (OutReport)
		{
			if (!OutReport->RequestedPath.IsValid())
				*OutReport = {
					.RequestedPath = Path,
					.FinalPath = Path,
					.PackagePath = Path};
			else OutReport->PackagePath = Path;
		}
		const bool bRootLoad = LoadDepth++ == 0;
		if (bRootLoad) GActivePackageFileReadCount = 0;
		FAssetLoadReport FailureReport;
		if (bRootLoad)
		{
			if (OutReport) FailureReport = *OutReport;
			TransactionPackages.clear();
		}
		FAssetLoadReport* PreviousLoadReport = GActiveAssetLoadReport;
		if (bRootLoad) GActiveAssetLoadReport = OutReport;
		DPackage* Package = nullptr;
		FAssetResult Result = LoadPackageInternal(
			Path, PhysicalPath, Package, OutReport);
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
					auto LoadedIt = ResidentPackages.find(*It);
					if (LoadedIt == ResidentPackages.end()) continue;
					DPackage* TransactionPackage = LoadedIt->second;
					ResidentPackages.erase(LoadedIt);
					LoadingPackages.erase(*It);
					RemoveFromRoot(TransactionPackage);
					MarkObjectHierarchyAsGarbage(TransactionPackage);
					bDiscardedPackage = true;
				}
				if (bDiscardedPackage) CollectGarbage();
				if (OutReport) *OutReport = std::move(FailureReport);
			}
			TransactionPackages.clear();
		}
		OutAsset = Result && Package ? Package->GetAsset() : nullptr;
		return Result;
	}

	auto FAssetRuntimeState::LoadPackageInternal(
		const FAssetPath& Path,
		std::string_view PhysicalPath,
		DPackage*& OutPackage,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		DURIN_PROFILE_CPU_ZONE_NAMED("Asset.LoadPackage");
		if (auto It = ResidentPackages.find(Path); It != ResidentPackages.end())
		{
			OutPackage = It->second;
			return {};
		}
		FAssetLoadReport LocalReport{.PackagePath = Path};
		FAssetLoadReport* CodecReport = OutReport ? OutReport : &LocalReport;
		std::vector<uint8> Bytes;
		if (PhysicalPath.empty()) return Error(EAssetError::InvalidPath, "Asset path cannot be resolved in the selected package mode.");
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath)) return Error(EAssetError::NotFound, std::format("Asset {} was not found.", Path.ToString()));
		++GActivePackageFileReadCount;
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(
				Bytes, Codec); !Result)
			return Result;
		{
			FAssetPackageHeader Header;
			FAssetResult Result = Codec->ReadHeader(
				Bytes, Bytes.size(), Header);
			if (!Result) return Result;
			FPackageFile HeaderFile{
				.FormatVersion = Header.FormatVersion,
				.AssetClassName = Header.AssetClassName,
				.EntryKind = Header.EntryKind,
				.RedirectDestination = Header.RedirectDestination,
				.Dependencies = Header.Dependencies};
			Result = ValidateRedirectorHeader(HeaderFile, Header.ObjectCount, &Path);
			if (!Result) return Result;

			DPackage* Package = nullptr;
			Result = Codec->Load(
				Bytes, Path, Package, CodecReport,
				[&](DPackage* LoadedPackage) -> FAssetResult {
					if (!LoadedPackage || ResidentPackages.contains(Path))
						return Error(EAssetError::AlreadyExists,
							"The package skeleton is already resident.");
					ResidentPackages.emplace(Path, LoadedPackage);
					LoadingPackages.insert(Path);
					if (LoadDepth > 0) TransactionPackages.push_back(Path);
					return {};
				},
				[&](DPackage* LoadedPackage) {
					LoadingPackages.erase(Path);
					ResidentPackages.erase(Path);
				});
			CodecReport->PackageFileReadCount = GActivePackageFileReadCount;
			if (!Result) return Result;
			LoadingPackages.erase(Path);
			OutPackage = Package;
			return {};
		}
	}

	auto FAssetRuntimeState::FindResidentPackage(const FAssetPath& Path) const -> DPackage*
	{
		auto It = ResidentPackages.find(Path);
		return It == ResidentPackages.end() ? nullptr : It->second.Package;
	}

	auto FAssetRuntimeState::GetResidentPackagePublicationState(
		const FAssetPath& Path) const
		-> std::optional<EAssetPackagePublicationState>
	{
		const auto It = ResidentPackages.find(Path);
		return It == ResidentPackages.end()
			? std::nullopt
			: std::optional{It->second.PublicationState};
	}

	auto FAssetRuntimeState::IsPackageReferenced(const DPackage* Package) const -> bool
	{
		if (!Package) return false;
		FAssetPath Path;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), Path)) return false;
		for (const auto& [OtherPath, OtherPackage] : ResidentPackages)
		{
			if (OtherPackage == Package) continue;
			const FAssetData* Data = Registry.FindAssetExactPointer(OtherPath);
			if (!Data) continue;
			for (const FAssetPath& Dependency : Data->Dependencies)
			{
				const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(Dependency);
				if (Resolution && Resolution.FinalPath == Path) return true;
			}
		}
		return false;
	}

	auto FAssetRuntimeState::UnloadPackage(
		const FAssetPath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		auto It = ResidentPackages.find(Path);
		if (It == ResidentPackages.end())
			return Error(EAssetError::NotFound, "Package is not resident.");
		if (LoadingPackages.contains(Path) || IsPackageReferenced(It->second)) return Error(EAssetError::InUse, "Package is still referenced.");
		DPackage* Package = It->second.Package;
		const bool bHasUnsavedState =
			It->second.PublicationState
				== EAssetPackagePublicationState::NewlyCreated
			|| (Package && Package->IsDirty());
		if (bHasUnsavedState
			&& Policy == EAssetPackageUnloadPolicy::RejectUnsaved)
			return Error(EAssetError::InUse,
				"Package has unsaved state; explicit discard policy is required.");
		ResidentPackages.erase(It);
		RemoveFromRoot(Package);
		MarkObjectHierarchyAsGarbage(Package);
		CollectGarbage();
		return {};
	}

	auto FAssetRuntimeState::CapturePackageLoadSnapshot() const -> FAssetPackageLoadSnapshot
	{
		FAssetPackageLoadSnapshot Snapshot;
		Snapshot.ResidentPackages.reserve(ResidentPackages.size());
		for (const auto& [Path, Package] : ResidentPackages) Snapshot.ResidentPackages.push_back(Path);
		std::ranges::sort(Snapshot.ResidentPackages, {}, [](const FAssetPath& Path) {
			return Path.ToString();
		});
		return Snapshot;
	}

	auto FAssetRuntimeState::ReleasePackagesLoadedSince(
		const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		if (LoadDepth != 0 || !LoadingPackages.empty())
			return Error(EAssetError::InUse, "A package load is still in progress.");

		std::unordered_set<FAssetPath> Protected(
			Snapshot.ResidentPackages.begin(), Snapshot.ResidentPackages.end());
		bool bChanged = true;
		while (bChanged)
		{
			bChanged = false;
			for (const auto& [Path, Package] : ResidentPackages)
			{
				if (!Protected.contains(Path)) continue;
				const FAssetData* Data = Registry.FindAssetExactPointer(Path);
				if (!Data) continue;
				for (const FAssetPath& Dependency : Data->Dependencies)
				{
					const FAssetPathResolveResult Resolution = Registry.ResolveAssetPath(Dependency);
					if (Resolution) bChanged |= Protected.insert(Resolution.FinalPath).second;
				}
			}
		}

		std::vector<DPackage*> ReleasedPackages;
		for (auto It = ResidentPackages.begin(); It != ResidentPackages.end();)
		{
			if (Protected.contains(It->first)
				|| It->second.PublicationState
					== EAssetPackagePublicationState::NewlyCreated
				|| (It->second.Package && It->second.Package->IsDirty()))
			{
				++It;
				continue;
			}
			DPackage* Package = It->second;
			ReleasedPackages.push_back(Package);
			It = ResidentPackages.erase(It);
		}
		for (DPackage* Package : ReleasedPackages)
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
		if (!ReleasedPackages.empty()) CollectGarbage();
		return {};
	}

	auto FAssetRuntimeState::Shutdown() -> void
	{
		StopAcceptingRequests();
		Registry.FlushPersistentSnapshot();
		std::vector<DPackage*> Packages;
		Packages.reserve(ResidentPackages.size());
		for (const auto& [Path, Package] : ResidentPackages)
		{
			if (Package) Packages.push_back(Package);
		}
		ResidentPackages.clear();
		LoadingPackages.clear();
		TransactionPackages.clear();
		LoadDepth = 0;
		for (DPackage* Package : Packages)
		{
			RemoveFromRoot(Package);
			MarkObjectHierarchyAsGarbage(Package);
		}
	}

	auto FAssetRuntimeState::Initialize(FAssetRuntimeConfiguration Configuration)
		-> FAssetResult
	{
		if (bAcceptingRequests)
		{
			if (RuntimeConfiguration == Configuration) return {};
			return Error(EAssetError::InUse,
				"Asset runtime configuration cannot be replaced while AssetCore is initialized.");
		}
		check(ResidentPackages.empty());
		check(LoadingPackages.empty());
		check(TransactionPackages.empty());
		RuntimeConfiguration = std::move(Configuration);
		bAcceptingRequests = true;
		return {};
	}

	auto FAssetRuntimeState::StopAcceptingRequests() -> void
	{
		if (!bAcceptingRequests) return;
		bAcceptingRequests = false;
		DURIN_DEBUG("Asset manager stopped accepting new requests.");
	}

	auto LoadAsset(
		const FAssetPath& Path,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().LoadAsset(Path, OutAsset, OutReport);
	}

	auto LoadAsset(
		const FAssetPath& Path,
		const DClass* ExpectedClass,
		DObject*& OutAsset,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().LoadAsset(
			Path, ExpectedClass, OutAsset, OutReport);
	}

	auto CreateAsset(
		const FAssetPath& Path,
		DClass* Class,
		size_t Size,
		DObject*& OutAsset) -> FAssetResult
	{
		return FAssetRuntimeState::Get().CreateAsset(Path, Class, Size, OutAsset);
	}

	auto CreateAssetRedirectorForTesting(
		const FAssetPath& RedirectorPath,
		const FAssetPath& DestinationPath,
		DAssetRedirector*& OutRedirector) -> FAssetResult
	{
		return FAssetRuntimeState::Get().CreateRedirector(
			RedirectorPath, DestinationPath, OutRedirector);
	}

	auto FAssetRuntimeState::ResolveSoftObjectInternal(
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
				DPackage* LoadedPackage = FindResidentPackage(Path);
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

		DPackage* Package = FindResidentPackage(Resolution.FinalPath);
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

	auto FAssetRuntimeState::LoadSoftObjectInternal(
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
		return FAssetRuntimeState::Get().ResolveSoftObjectInternal(
			Reference, ExpectedClass, NullPolicy);
	}

	auto LoadSoftObject(
		FSoftObjectPtr& Reference,
		const DClass* ExpectedClass,
		DObject*& OutObject,
		ESoftObjectNullPolicy NullPolicy,
		FAssetLoadReport* OutReport) -> FAssetResult
	{
		return FAssetRuntimeState::Get().LoadSoftObjectInternal(
			Reference, ExpectedClass, OutObject, NullPolicy, OutReport);
	}
	auto SavePackage(
		DPackage* Package,
		const FAssetPackageSaveOptions& Options) -> FAssetResult
	{
		return FAssetRuntimeState::Get().SavePackage(Package, Options);
	}
	auto PrepareAssetRelocationTransaction(
		std::span<const FAssetRelocationMapping> Mappings,
		FAssetMutationSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		return FAssetRuntimeState::Get().PrepareAssetRelocationTransaction(
			Mappings, OutSummary, OutTransaction);
	}
	auto PrepareRedirectorFixupTransaction(
		std::span<const FAssetPath> Redirectors,
		EAssetRedirectorFixupMode Mode,
		FAssetRedirectorFixupSummary& OutSummary,
		FAssetMutationTransaction& OutTransaction) -> FAssetResult
	{
		return FAssetRuntimeState::Get().PrepareRedirectorFixupTransaction(
			Redirectors, Mode, OutSummary, OutTransaction);
	}
	auto AnalyzeAssetDeletion(const FAssetPath& Path, FAssetDeleteAnalysis& OutAnalysis) -> FAssetResult { return FAssetRuntimeState::Get().AnalyzeAssetDeletion(Path, OutAnalysis); }
	auto PrepareAssetDeletionTransaction(
		std::span<const FAssetPath> Paths,
		std::span<const std::filesystem::path> PhysicalRoots,
		FAssetDeletionTransaction& OutTransaction,
		std::vector<FAssetDeletionBatchBlocker>& OutBlockers) -> FAssetResult
	{
		return FAssetRuntimeState::Get().PrepareAssetDeletionTransaction(
			Paths, PhysicalRoots, OutTransaction, OutBlockers);
	}
	auto DeleteAssetForTesting(const FAssetPath& Path) -> FAssetResult
	{
		return FAssetRuntimeState::Get().DeleteAssetForTesting(Path);
	}
	auto FindResidentPackage(const FAssetPath& Path) -> DPackage* { return FAssetRuntimeState::Get().FindResidentPackage(Path); }
	auto GetResidentPackagePublicationState(const FAssetPath& Path)
		-> std::optional<EAssetPackagePublicationState>
	{
		return FAssetRuntimeState::Get().GetResidentPackagePublicationState(Path);
	}
	auto UnloadPackage(
		const FAssetPath& Path,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		return FAssetRuntimeState::Get().UnloadPackage(Path, Policy);
	}
	auto UnloadPackage(
		DPackage* Package,
		EAssetPackageUnloadPolicy Policy) -> FAssetResult
	{
		FAssetPath Path;
		if (!Package || !Package->IsAssetPackage()
			|| !FAssetPath::TryCreate(Package->GetPackagePath(), Path))
			return Error(EAssetError::InvalidPackageType,
				"The package to unload is invalid.");
		FAssetRuntimeState& State = FAssetRuntimeState::Get();
		if (State.FindResidentPackage(Path) != Package)
			return Error(EAssetError::NotFound,
				"The package is not the resident package at its path.");
		return State.UnloadPackage(Path, Policy);
	}
	auto CapturePackageLoadSnapshot() -> FAssetPackageLoadSnapshot
	{
		return FAssetRuntimeState::Get().CapturePackageLoadSnapshot();
	}
	auto ReleasePackagesLoadedSince(const FAssetPackageLoadSnapshot& Snapshot) -> FAssetResult
	{
		return FAssetRuntimeState::Get().ReleasePackagesLoadedSince(Snapshot);
	}
	auto ShutdownAssetManager() -> void { FAssetRuntimeState::Get().Shutdown(); }
	auto InitializeAssetManager(FAssetRuntimeConfiguration Configuration) -> FAssetResult
	{
		return FAssetRuntimeState::Get().Initialize(std::move(Configuration));
	}
	auto GetAssetRuntimeConfiguration() -> const FAssetRuntimeConfiguration&
	{
		return FAssetRuntimeState::Get().GetRuntimeConfiguration();
	}
	auto GetAssetCatalogStore() -> FAssetCatalogStore& { return FAssetRuntimeState::Get().GetRegistry(); }
	auto FindAssetExact(const FAssetPath& Path) -> FAssetCatalogEntry
	{
		return FAssetRuntimeState::Get().GetRegistry().FindAssetExact(Path);
	}
	auto ResolveAssetPath(
		const FAssetPath& Path,
		const FAssetPathResolveOptions& Options) -> FAssetPathResolveResult
	{
		return FAssetRuntimeState::Get().GetRegistry().ResolveAssetPath(Path, Options);
	}
	auto CaptureAssetCatalogSnapshot() -> FAssetCatalogSnapshot
	{
		return FAssetRuntimeState::Get().GetRegistry().CaptureSnapshot();
	}
	auto GetAssetCatalogRevision() -> uint64
	{
		return FAssetRuntimeState::Get().GetRegistry().GetRevision();
	}
	auto CaptureAssetReferenceIndex() -> FAssetReferenceIndex
	{
		return FAssetRuntimeState::Get().GetRegistry().GetReferenceIndex();
	}
	auto FindRedirectorsTo(const FAssetPath& Destination)
		-> std::vector<FAssetPath>
	{
		return FAssetRuntimeState::Get().GetRegistry().FindRedirectorsTo(Destination);
	}
	auto BuildCookReachability(
		std::span<const FAssetPath> Roots,
		std::vector<FAssetPath>& OutPackages) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetRegistry().BuildCookReachability(
			Roots, OutPackages);
	}
	auto FlushAssetCatalogSnapshotForTesting() -> void
	{
		FAssetRuntimeState::Get().GetRegistry().FlushPersistentSnapshot();
	}
	auto IsAssetCatalogSnapshotDirtyForTesting() -> bool
	{
		return FAssetRuntimeState::Get().GetRegistry().IsPersistentSnapshotDirty();
	}
	auto GetAssetCatalogCacheWarningForTesting() -> std::string
	{
		return FAssetRuntimeState::Get().GetRegistry().GetCacheWarning();
	}
	auto RefreshAssetCatalog(
		EAssetRegistryScanMode Mode) -> FAssetCatalogRefreshResult
	{
		return FAssetRuntimeState::Get().GetRegistry().RefreshMountedContent(Mode);
	}
}
