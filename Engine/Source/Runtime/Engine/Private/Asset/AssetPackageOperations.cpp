#include "AssetLiveLoadGuard.h"
#include "Asset/RegistryOperations.h"
#include "AssetRuntimeStateInternal.h"
#include "AssetMutationRegistryInternal.h"
#include "AssetMutationJournalInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetRelocationExtensionsInternal.h"
#include "AssetRegistryResultAdapter.h"
#include "Asset/PackageSerialization.h"
#include "DObject/ObjectGraphReplacement.h"
#include "AssetPackageCodec.h"
#include "Asset/PackageVersionPolicy.h"
#include "Asset/Redirector.h"
#include "Asset/EditorBulkData.h"
#include "EditorBulkDataSaveRetention.h"
#include "Asset/EditorBulkDataStorage.h"
#include "Asset/PackageResource.h"
#include "AssetPackageArchive.h"
#include "AssetPropertyKindTraits.h"
#include "DObject/PackageValueCodec.h"
#include "DObject/DefaultDeltaPlan.h"
#include "Profiling/Profiling.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "DObject/PackagePersistence.h"
#include "Misc/FileTime.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Threading/RunnableThread.h"
#include "Threading/TaskComposition.h"
#include "DObject/StrongObjectPtr.h"
#include "Misc/Guid.h"

namespace Durin
{
	namespace
	{
		auto ProjectTopLevelAssetData(const FAssetPackageHeader& Header)
			-> std::vector<FTopLevelAssetData>
		{
			std::vector<FTopLevelAssetData> Result;
			Result.reserve(Header.TopLevelAssets.size());
			for (const FAssetPackageTopLevelAssetHeader& Asset : Header.TopLevelAssets)
				Result.push_back({Asset.AssetPath, Asset.AssetClassName,
					Asset.RedirectDestination});
			return Result;
		}

	}

	using AssetPrivate::FAssetReferenceStoreRegistry;
	using AssetPrivate::GetAssetReferenceStoreRegistry;
	using AssetPrivate::EAssetMutationState;
	using AssetPrivate::FAssetMutationJournal;
	using AssetPrivate::FAssetMutationJournalEntry;
	using AssetPrivate::FingerprintRelocationFile;
	using AssetPrivate::LoadRelocationBytes;
	using AssetPrivate::MakePackageFingerprint;
	using AssetPrivate::NormalizePhysicalPath;
	using AssetPrivate::PublishRelocationFile;
	using AssetPrivate::SaveRelocationBytes;
	using AssetPrivate::WriteMutationJournalState;

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			FByteView Bytes,
			const FPackagePath& PackagePath,
			FAssetPackageInspection& OutInspection) -> FAssetReadResult;

		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetReadResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return {EAssetReadError::ProjectionPending,
					std::format("Registry projection for package {} is pending synchronization.",
						Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::NotFound:
				return {EAssetReadError::NotFound, std::format(
					"Asset {} is not present in the registry.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::MissingRedirectTarget:
				return {EAssetReadError::NotFound, std::format(
					"Asset redirect {} has a missing target {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::RedirectCycle:
				return {EAssetReadError::CircularDependency, std::format(
					"Asset redirect {} contains a cycle at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::RedirectDepthExceeded:
				return {EAssetReadError::CircularDependency, std::format(
					"Asset redirect {} exceeds the maximum redirect depth at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			case EAssetPathResolveState::UnknownTargetClass:
				return {EAssetReadError::UnknownClass, std::format(
					"Asset {} resolves to a target with an unavailable reflected class.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::RedirectTypeMismatch:
				return {EAssetReadError::TypeMismatch, std::format(
					"Asset {} resolves to a target with an incompatible class.",
					Resolution.RequestedPath.ToString())};
			case EAssetPathResolveState::CorruptRedirector:
				return {EAssetReadError::CorruptFile, std::format(
					"CorruptRedirector: asset {} traverses invalid redirect metadata at {}.",
					Resolution.RequestedPath.ToString(), Resolution.FinalPath.ToString())};
			}
			return {EAssetReadError::CorruptFile,
				"Asset resolution returned an unknown state."};
		}
		constexpr uint32 MaximumRedirectDepth = 32;
		constexpr std::string_view RedirectorClassName = "Durin::DAssetRedirector";

		struct FPackageFile
		{
			uint32 FormatVersion = 0;
			std::vector<FTopLevelAssetData> TopLevelAssets;
			std::string AssetClassName;
			EAssetRegistryEntryKind EntryKind = EAssetRegistryEntryKind::Asset;
			FPackagePath RedirectDestination;
			std::vector<FPackagePath> Dependencies;
			std::vector<FPackagePath> SoftDependencies;
			std::vector<std::string> SearchableNames;
			uint64 ObjectCount = 0;
			uint64 BulkSegmentExtent = 0;
			FXxHash128 BulkSegmentDigest;
			FByteBuffer BulkBytes;
		};

		auto BuildSavedAssetMetadata(const FPackageFile& File, const FPackagePath& Path,
			const std::filesystem::path& Destination, uintmax_t FileSize,
			std::filesystem::file_time_type LastWriteTime) -> FAssetData
		{
			// The captured file describes the saved revision, even if live objects change.
			return {
				.PackagePath = Path,
				.PhysicalPath = Destination.generic_string(),
				.TopLevelAssets = File.TopLevelAssets,
				.AssetClassName = File.AssetClassName,
				.EntryKind = File.EntryKind,
				.RedirectDestination = File.RedirectDestination,
				.FormatVersion = File.FormatVersion,
				.Dependencies = File.Dependencies,
				.SoftDependencies = File.SoftDependencies,
				.SearchableNames = File.SearchableNames,
				.ObjectCount = File.ObjectCount,
				.BulkSegmentExtent = File.BulkSegmentExtent,
				.BulkSegmentDigest = File.BulkSegmentDigest,
				.FileSize = FileSize,
				.LastWriteTime = LastWriteTime,
				.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)};
		}

		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult { return {Code, std::move(Message)}; }
		auto Error(EAssetWriteError Code, std::string Message) -> FAssetWriteResult { return {Code, std::move(Message)}; }

		auto LoadPackageBulkBytes(std::string_view PhysicalPath,
			FByteBuffer& OutBytes) -> FAssetReadResult
		{
			OutBytes.clear();
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(BulkPath, ErrorCode))
			{
				if (ErrorCode && ErrorCode != std::errc::no_such_file_or_directory)
					return Error(EAssetReadError::IoError,
						"Failed to inspect the package bulk companion.");
				return {};
			}
			if (!FFileHelper::LoadFileToArray(OutBytes, BulkPath))
				return Error(EAssetReadError::IoError,
					"Failed to read the package bulk companion.");
			return {};
		}

		auto ClassifyPackageIdentity(std::string_view PhysicalPath,
			FPackagePath& OutPath) -> bool
		{
			std::filesystem::path Path(PhysicalPath);
			Path.replace_extension();
			const FAssetPathResult Classified = FMountPaths::ClassifyAssetPath(Path);
			return Classified && FPackagePath::TryCreate(
				Classified.NormalizedVirtualPath, OutPath);
		}

		auto ValidateAssetPackageClosure(FByteView Bytes,
			FByteView BulkBytes, const FPackagePath& PackagePath)
			-> FAssetReadResult
		{
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (auto Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			return Codec->Validate({.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = PackagePath, .PhysicalPackageBytes = Bytes.size()});
		}

		auto ValidatePackageWriteAdmission(const FPackagePath& Path) -> FAssetWriteResult
		{
			if (IsPackageLoading(Path)) return Error(EAssetReadError::InUse, "An incomplete package cannot be saved.");
			const FMountLookupResult Mount =
				FMountPaths::FindMountForVirtualPath(Path.GetView());
			if (!Mount)
				return Error(EAssetReadError::InvalidPath,
					std::format("Package {} does not use a registered content mount.",
						Path.ToString()));
			if (!Mount.Mount->bContentWritable)
				return Error(EAssetWriteError::ReadOnlyMode,
					std::format("Content mount {} is read-only.",
						Mount.Mount->VirtualRoot));
			return {};
		}

		auto CorruptRedirector(std::string Message) -> FAssetReadResult
		{
			return Error(EAssetReadError::CorruptFile, std::format("CorruptRedirector: {}", Message));
		}

		auto ValidateRedirectorHeader(
			const FPackageFile& File,
			uint64 ObjectCount,
			const FPackagePath* SourcePath = nullptr) -> FAssetReadResult
		{
			const FPackagePath PackagePath = SourcePath ? *SourcePath
				: (File.TopLevelAssets.empty() ? FPackagePath{}
					: File.TopLevelAssets.front().AssetPath.GetPackagePath());
			if (!ArePackageAssetsValid(File.TopLevelAssets, PackagePath,
				ObjectCount, File.Dependencies))
				return CorruptRedirector("the package contains invalid exact asset metadata.");
			return {};
		}

		auto GetPhysicalPath(const FPackagePath& Path) -> std::string
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
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(Path.GetView(), EMountPathExistence::AllowMissing);
			if (!Resolved)
				DURIN_WARN_CATEGORY(
					"AssetSystem", "Failed to resolve asset path {}: {}", Path.ToString(), Resolved.Message);
			return Resolved ? Resolved.PhysicalPath.generic_string() + ".dasset" : std::string{};
		}

		auto DecodeByteToolValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			Durin::PackagePrivate::FByteReader& Reader,
			const std::vector<DObject*>& Objects,
			uint32 SourceVersion = ObjectPackage::DastV10FormatVersion) -> FAssetReadResult
		{
			const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
			if (AssetPrivate::IsByteToolRawScalarKind(Kind))
				return Reader.ReadBytes(Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize()) ? FAssetReadResult{} : Error(EAssetReadError::CorruptFile, "Truncated property payload.");
			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::String:
			{
				std::string Value;
				if (!Reader.ReadString(Value)) return Error(EAssetReadError::CorruptFile, "Truncated string property.");
				*static_cast<FStringProperty*>(Property)->GetStringValuePtr(Container, ArrayIndex) = std::move(Value);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Name:
			{
				std::string Value;
				if (!Reader.ReadString(Value)) return Error(EAssetReadError::CorruptFile, "Truncated name property.");
				*static_cast<FNameProperty*>(Property)->GetNameValuePtr(Container, ArrayIndex) = FName(Value);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Guid:
			{
				FGuid Value;
				if (!Reader.Read(Value.A) || !Reader.Read(Value.B) || !Reader.Read(Value.C) || !Reader.Read(Value.D))
					return Error(EAssetReadError::CorruptFile, "Truncated GUID property.");
				*static_cast<FGuidProperty*>(Property)->GetGuidValuePtr(Container, ArrayIndex) = Value;
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Object:
			{
				auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind)) return Error(EAssetReadError::CorruptFile, "Truncated object reference.");
				DObject* Value = nullptr;
				if (ReferenceKind == 1)
				{
					uint64 Id = 0;
					if (!Reader.Read(Id) || Id == 0 || Id > Objects.size()) return Error(EAssetReadError::InvalidObjectGraph, "Invalid internal object reference.");
					Value = Objects[static_cast<size_t>(Id - 1)];
				}
				else if (ReferenceKind == 2)
				{
					std::string PathString;
					FObjectPath Path;
					if (!Reader.ReadString(PathString) || !FObjectPath::TryCreate(PathString, Path)) return Error(EAssetReadError::InvalidPath, "Invalid external object reference.");
					auto Result = FAssetRuntimeState::Get().GetLoadService().LoadObject(
						Path, ObjectProperty->GetReferencedClass(), Value);
					if (!Result) return Error(EAssetReadError::MissingDependency, Result.Message);
				}
				else if (ReferenceKind != 0) return Error(EAssetReadError::CorruptFile, "Unknown object reference kind.");
				if (Value && ObjectProperty->GetReferencedClass() && !Value->IsA(ObjectProperty->GetReferencedClass())) return Error(EAssetReadError::TypeMismatch, "Object reference class mismatch.");
				ObjectProperty->SetObjectPropertyValue(Container, Value, ArrayIndex);
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::SoftObject:
			{
				auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
				FSoftObjectPtr* Reference = SoftProperty->GetSoftObjectPtr(Container, ArrayIndex);
				if (!Reference)
					return Error(EAssetReadError::UnsupportedProperty,
						"Soft object property has no typed value accessor.");
				uint8 ReferenceKind = 0;
				if (!Reader.Read(ReferenceKind))
					return Error(EAssetReadError::CorruptFile, "Truncated soft object reference.");
				if (ReferenceKind == 0)
				{
					Reference->Reset();
					return {};
				}
				if (ReferenceKind != 1)
					return Error(EAssetReadError::CorruptFile, "Unknown soft object reference tag.");
				std::string PathString;
				if (!Reader.ReadString(PathString, Durin::PackagePrivate::MaximumPackageStringBytes) || PathString.empty())
					return Error(EAssetReadError::CorruptFile, "Truncated or overlong soft object path.");
				FObjectPath Path;
				if (const auto PathValidation = FObjectPath::TryCreateWithDiagnostic(PathString, Path); !PathValidation)
				{
					return Error(EAssetReadError::InvalidPath, FormatObjectError(PathValidation.Error));
				}
				Reference->SetPath(std::move(Path));
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				auto* StructProperty = static_cast<FStructProperty*>(Property);
				DStruct* Struct = StructProperty->GetStruct();
				if (!Struct)
					return Error(EAssetReadError::UnsupportedProperty, "Struct property has no reflected type.");
				if (!Struct->HasCompleteAuthoredFields())
					return Error(
						EAssetReadError::UnsupportedProperty,
						std::format(
							"CustomStructCodecRequired: '{}' does not declare a complete authored "
							"field representation.",
							Struct->GetQualifiedName()));
				if (!Struct->CanDefaultConstruct() || !Struct->CanDestroy()
					|| !Struct->CanCopyAssign())
					return Error(
						EAssetReadError::UnsupportedProperty,
						std::format(
							"DStructOperationUnavailable: authored loading requires "
							"DefaultConstruct, Destroy, and CopyAssign for '{}'.",
							Struct->GetQualifiedName()));
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
				if (!([&] {
					const auto ValueResult = Storage.DefaultConstruct(StorageProperty, 0);
					StorageError = Durin::FormatPropertyValueError(ValueResult.Error);
					return ValueResult.Succeeded();
				}()))
					return Error(EAssetReadError::UnsupportedProperty, std::move(StorageError));
				std::string StructName;
				uint64 FieldCount = 0;
				if (!Reader.ReadString(StructName) || StructName != Struct->GetQualifiedName().ToString() || !Reader.Read(FieldCount) || FieldCount > 100000)
					return Error(EAssetReadError::CorruptFile, "Invalid struct payload header.");
				void* StructValue = Storage.GetValue();
				for (uint64 Index = 0; Index < FieldCount; ++Index)
				{
					std::string DeclaringStruct, FieldName, Signature;
					uint8 Kind = 0;
					uint64 PayloadSize = 0;
					FByteView Payload;
					if (!Reader.ReadString(DeclaringStruct) || !Reader.ReadString(FieldName) || !Reader.Read(Kind) || !Reader.ReadString(Signature) || !Reader.Read(PayloadSize) || PayloadSize > Reader.Bytes.size() || !Reader.ReadSpan(static_cast<size_t>(PayloadSize), Payload))
						return Error(EAssetReadError::CorruptFile, "Invalid struct field record.");
					if (DeclaringStruct != StructName) continue;
					FProperty* Field = Struct->FindPropertyBySerializedName(FName(FieldName), false);
					if (!Field)
					{
						DURIN_WARN("Skipping unknown struct field {}::{}", StructName, FieldName);
						continue;
					}
					if (static_cast<uint8>(Field->GetKind()) != Kind
						|| !Durin::PackagePrivate::IsSerializedTypeSignatureCompatible(Field, Signature))
						return Error(
							EAssetReadError::TypeMismatch,
							std::format(
								"Serialized struct field {}::{} is incompatible with the current schema.",
								StructName,
								FieldName));
					Durin::PackagePrivate::FByteReader PayloadReader{Payload};
					for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim(); ++FieldIndex)
					{
						auto Result = DecodeByteToolValue(
							Field, StructValue, FieldIndex, PayloadReader, Objects, SourceVersion);
						if (!Result) return Result;
					}
					if (PayloadReader.Offset != Payload.size()) return Error(EAssetReadError::CorruptFile, "Struct field payload has trailing bytes.");
				}
				if (Struct->HasPostDeserialize())
				{
					FDStructPostDeserializeContext Context{
						.Source = EDStructDeserializeSource::AuthoredAsset,
						.SourceVersion = SourceVersion};
					auto Validation = Struct->GetOps().PostDeserialize(StructValue, Context);
					if (!Validation)
					{
						Validation.Error.StructName = Struct->GetQualifiedName().ToString();
						Validation.Error.SourceVersion = SourceVersion;
						auto Result = Error(EAssetReadError::CorruptFile, FormatObjectValidationError(Validation.Error));
						return Result;
					}
				}
				if (!([&] {
					const auto ValueResult = Property->CopyAssignValue(Property->GetValuePtr(Container, ArrayIndex), StructValue);
					StorageError = Durin::FormatPropertyValueError(ValueResult.Error);
					return ValueResult.Succeeded();
				}()))
					return Error(EAssetReadError::UnsupportedProperty, std::move(StorageError));
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				auto* Array = static_cast<FArrayProperty*>(Property);
				uint64 Num = 0;
				if (!Array->HasArrayOps() || !Array->GetInner() || !Reader.Read(Num) || Num > 10000000)
					return Error(EAssetReadError::CorruptFile, "Invalid array payload.");
				if (!Array->HasCapability(EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit
					| EArrayOpsFlags::RandomAccess) || (Num > 0 && !Array->HasCapability(EArrayOpsFlags::DefaultGrow)))
					return Error(EAssetReadError::UnsupportedProperty,
						"ArrayOperationUnavailable: DAST load requires DetachedStorage, RandomAccess, DefaultGrow, and TransactionalCommit.");
				const FArrayOps& Ops = Array->GetOps();
				FDetachedContainerStorage Detached;
				EContainerOpResult OpResult = Detached.Create(Ops);
				if (OpResult == EContainerOpResult::Success) OpResult = Ops.Resize(Detached.Get(), Num);
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetReadError::UnsupportedProperty,
						std::format("ArrayOperationFailed: detached allocation/resize returned {}.", static_cast<uint32>(OpResult)));
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					void* Element = nullptr;
					OpResult = Ops.GetMutableAt(Detached.Get(), Index, &Element);
					if (OpResult != EContainerOpResult::Success)
						return Error(EAssetReadError::UnsupportedProperty,
							std::format("ArrayElement[{}]: mutable access returned {}.", Index, static_cast<uint32>(OpResult)));
					auto Result = DecodeByteToolValue(
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
					return Error(EAssetReadError::UnsupportedProperty,
						std::format("ArrayOperationFailed: Commit returned {}.", static_cast<uint32>(OpResult)));
				return {};
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				uint64 Num = 0;
				if (!Map->HasMapOps() || !Map->GetKeyProp() || !Map->GetValueProp()
					|| !Reader.Read(Num) || Num > 10000000)
					return Error(EAssetReadError::CorruptFile, "Invalid map payload.");
				if (!Map->HasCapability(EMapOpsFlags::DetachedStorage | EMapOpsFlags::TransactionalCommit | EMapOpsFlags::Insert))
					return Error(EAssetReadError::UnsupportedProperty,
						"MapOperationUnavailable: DAST load requires DetachedStorage, Insert, and TransactionalCommit.");
				const FMapOps& Ops = Map->GetOps();
				FDetachedContainerStorage Detached;
				EContainerOpResult OpResult = Detached.Create(Ops);
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetReadError::UnsupportedProperty,
						std::format("MapOperationFailed: detached allocation returned {}.", static_cast<uint32>(OpResult)));
				if (Ops.Reserve && (OpResult = Ops.Reserve(Detached.Get(), Num)) != EContainerOpResult::Success)
					return Error(EAssetReadError::UnsupportedProperty,
						std::format("MapOperationFailed: Reserve returned {}.", static_cast<uint32>(OpResult)));
				FReflectedValueStorage KeyStorage;
				FReflectedValueStorage ValueStorage;
				std::string StorageError;
				if (Num > 0
					&& (!([&] {
						const auto ValueResult = KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0);
						StorageError = Durin::FormatPropertyValueError(ValueResult.Error);
						return ValueResult.Succeeded();
					}())
						|| !([&] {
							const auto ValueResult = ValueStorage.DefaultConstruct(Map->GetValueProp(), 0);
							StorageError = Durin::FormatPropertyValueError(ValueResult.Error);
							return ValueResult.Succeeded();
						}())))
					return Error(EAssetReadError::UnsupportedProperty, std::move(StorageError));
				for (uint64 Index = 0; Index < Num; ++Index)
				{
					if (Index > 0)
					{
						KeyStorage.Reset();
						ValueStorage.Reset();
						if (!([&] {
							const auto ValueResult = KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0);
							StorageError = Durin::FormatPropertyValueError(ValueResult.Error);
							return ValueResult.Succeeded();
						}())
							|| !([&] {
								const auto ValueResult = ValueStorage.DefaultConstruct(Map->GetValueProp(), 0);
								StorageError = Durin::FormatPropertyValueError(ValueResult.Error);
								return ValueResult.Succeeded();
							}()))
							return Error(EAssetReadError::UnsupportedProperty, std::move(StorageError));
					}
					auto Result = DecodeByteToolValue(
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
						return Error(EAssetReadError::CorruptFile,
							std::format("MapEntry[{}].Key: duplicate decoded key.", Index));
					if (OpResult != EContainerOpResult::Success)
						return Error(EAssetReadError::UnsupportedProperty,
							std::format("MapEntry[{}]: Insert returned {}.", Index, static_cast<uint32>(OpResult)));
				}
				OpResult = Ops.Commit(Map->GetValuePtr(Container, ArrayIndex), Detached.Get());
				if (OpResult != EContainerOpResult::Success)
					return Error(EAssetReadError::UnsupportedProperty,
						std::format("MapOperationFailed: Commit returned {}.", static_cast<uint32>(OpResult)));
				return {};
			}
			default:
				return Error(EAssetReadError::UnsupportedProperty, "Unsupported property kind.");
			}
		}

		auto BuildPackageBytes(
			DPackage* Package,
			FByteBuffer& OutBytes,
			FPackageFile* OutFile = nullptr,
			const FAssetPackageSerializationOptions& Options = {}) -> FAssetWriteResult
		{
			const AssetPrivate::FAssetPackageCodec* Codec =
				AssetPrivate::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
			if (!Codec)
				return Error(EAssetReadError::UnsupportedVersion,
					"The ordinary asset package writer is unavailable.");
			FAssetPackageSerializationOptions EffectiveOptions = Options;
			std::vector<FPackageBulkStoragePayload> BulkPayloads;
			if (OutFile) EffectiveOptions.EditorBulkDataStoragePayloads = &BulkPayloads;
			AssetPrivate::FAssetPackageEncodedClosure Closure;
			auto Result = Codec->Write(
				Package, Closure, EffectiveOptions.Mode == EAssetPackageSaveMode::Complete
					? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, EffectiveOptions);
			if (!Result) return Result;
			// Codec::Write already validates its encoded closure before returning it.
			FPackagePath PackagePath;
			if (!Package || !FPackagePath::TryCreate(Package->GetPackagePath(), PackagePath))
				return Error(EAssetReadError::InvalidPath, "Package path is invalid.");
			if (OutFile)
			{
				OutFile->BulkBytes = std::move(Closure.BulkBytes);
				FAssetPackageHeader Header;
				const AssetPrivate::FAssetPackageReadContext Context{
					.PackageBytes = Closure.PackageBytes,
					.BulkBytes = OutFile->BulkBytes,
					.PackagePath = PackagePath,
					.PhysicalPackageBytes = Closure.PackageBytes.size()};
				if (auto HeaderResult = Codec->ReadHeader(Context, Header); !HeaderResult)
					return HeaderResult;
				OutFile->FormatVersion = Header.FormatVersion;
				OutFile->TopLevelAssets = ProjectTopLevelAssetData(Header);
				OutFile->AssetClassName = std::move(Header.AssetClassName);
				OutFile->EntryKind = Header.EntryKind;
				OutFile->RedirectDestination = std::move(Header.RedirectDestination);
				OutFile->Dependencies = std::move(Header.Dependencies);
				OutFile->SoftDependencies = std::move(Header.SoftDependencies);
				OutFile->SearchableNames = std::move(Header.SearchableNames);
				OutFile->ObjectCount = Header.ObjectCount;
				OutFile->BulkSegmentExtent = Header.BulkSegmentExtent;
				OutFile->BulkSegmentDigest = Header.BulkSegmentDigest;
			}
			OutBytes = std::move(Closure.PackageBytes);
			return Result;
		}

		auto ValidateSaveVersion(
			const FPackagePath& Path) -> FAssetReadResult
		{
			const FAssetCatalogEntry Existing = Durin::FindAssetExact(Path);
			if (!Existing || ObjectPackage::IsSupportedPackageReaderVersion(Existing->FormatVersion))
				return {};
			return Error(
				EAssetReadError::UnsupportedVersion,
				std::format(
					"Package {} uses unsupported DAST v{} while ordinary saves write DAST v{}.",
					Path.ToString(),
					Existing->FormatVersion,
					OrdinaryAssetPackageWriterVersion));
		}
	}

	namespace AssetPrivate
	{
		auto ValidateMutationPackageMetadata(
			const FMutationPackageMetadata& Metadata,
			uint64 ObjectCount,
			const FPackagePath* SourcePath) -> FAssetReadResult
		{
			const FPackageFile File{
				.FormatVersion = Metadata.FormatVersion,
				.TopLevelAssets = Metadata.TopLevelAssets,
				.AssetClassName = Metadata.AssetClassName,
				.EntryKind = Metadata.EntryKind,
				.RedirectDestination = Metadata.RedirectDestination,
				.Dependencies = Metadata.Dependencies};
			return ValidateRedirectorHeader(File, ObjectCount, SourcePath);
		}

		auto DecodeReferenceByteToolValue(
			FProperty* Property,
			void* Container,
			uint32 ArrayIndex,
			Durin::PackagePrivate::FByteReader& Reader,
			const std::vector<DObject*>& Objects,
			uint32 SourceVersion) -> FAssetReadResult
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

	auto ValidateAssetPackageBytes(FByteView Bytes,
		const FPackagePath& PackagePath, FByteView BulkBytes) -> FAssetReadResult
	{
		return ValidateAssetPackageClosure(Bytes, BulkBytes, PackagePath);
	}

	auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteBuffer& OutBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetWriteResult
	{
		return BuildPackageBytes(Package, OutBytes, nullptr, Options);
	}

	auto SerializeAssetPackageClosure(
		DPackage* Package,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetWriteResult
	{
		FPackageFile File;
		auto Result = BuildPackageBytes(Package, OutBytes, &File, Options);
		if (!Result) return Result;
		OutBulkBytes = std::move(File.BulkBytes);
		return {};
	}

	namespace
	{
		auto ToAssetWriteResult(FPackageWriteResult Result) -> FAssetWriteResult
		{
			FAssetWriteResult Out;
			if (!Result) Out.Error = Result.Error == EPackageWriteError::StaleData ? EAssetWriteError::StaleData
				: Result.Error == EPackageWriteError::CorruptFile ? EAssetWriteError::InvalidData : EAssetWriteError::IoError;
			Out.Message = std::move(Result.Message);
			Out.AffectedFiles = std::move(Result.AffectedFiles);
			if (Result.State == EPackageWriteState::PartiallyWritten)
				Out.Disposition = EAssetWriteDisposition::PartiallyWritten;
			if (Result.State == EPackageWriteState::RecoveryRequired
				|| (!Result && Result.State == EPackageWriteState::Committed))
				Out.Disposition = EAssetWriteDisposition::RecoveryRequired;
			if (!Result.RecoveryFiles.empty()) Out.RecoveryLocation = Result.RecoveryFiles.front();
			return Out;
		}
		auto BeginAssetWrite(const std::filesystem::path& Destination, FByteBuffer Bytes, FByteBuffer Bulk,
			const FFilePublicationStamp& MainStamp, const FFilePublicationStamp& BulkStamp, bool bDirect = false)
			-> std::unique_ptr<IPackageWriteOperation>
		{
			// Each writer owns only its transaction's siblings; abandoned files from
			// earlier saves neither participate in recovery nor reserve these paths.
			const auto Suffix = ".save-" + FGuid::NewGuid().ToString();
			auto Companion = Destination; Companion.replace_extension(".dbulk");
			const auto MainPrefix = Destination.string() + Suffix;
			const auto BulkPrefix = Companion.string() + Suffix;
			std::vector<FPackageWriteFile> Files;
			Files.push_back({{Companion, Bulk.empty() ? std::filesystem::path{} : std::filesystem::path{BulkPrefix + ".stage"},
				BulkPrefix + ".backup"}, BulkStamp, std::move(Bulk)});
			Files.push_back({{Destination, MainPrefix + ".stage", MainPrefix + ".backup"}, MainStamp, std::move(Bytes)});
			return (bDirect ? GetDirectFilePackageWriter() : GetFilePackageWriter())->Begin(std::move(Files));
		}
		struct FPreparedPackageSave
		{
			DPackage* Package = nullptr;
			FPackagePath Path;
			uint64 Revision = 0;
			uint64 DetachedBytes = 0;
			FPackageFile File;
			std::filesystem::path Destination;
			std::unique_ptr<IPackageWriteOperation> Write;
		};

		auto PreparePackageSave(DPackage* Package, const FPackagePath& Path,
			EAssetPackageSaveMode Mode, FPreparedPackageSave& Out, bool bDirect = false) -> FAssetWriteResult
		{
			if (auto Result = ValidatePackageWriteAdmission(Path); !Result) return Result;
			if (auto Result = ValidateSaveVersion(Path); !Result) return Result;
			FPreparedPackageSave Prepared;
			Prepared.Package = Package;
			Prepared.Path = Path;
			Prepared.Destination = GetPhysicalPath(Path);
			if (Prepared.Destination.empty()) return Error(EAssetWriteError::InvalidPath, "Cannot resolve package destination.");
			auto ReadAccess = FPackageFileAccess::TryReadPackage(Prepared.Destination);
			if (!ReadAccess) return Error(EAssetWriteError::InUse, "Package output is being written.");
			auto Companion = Prepared.Destination; Companion.replace_extension(".dbulk");
			FFilePublicationStamp MainStamp, BulkStamp;
			if (!FFilePublicationStamp::Inspect(Prepared.Destination, MainStamp) || !FFilePublicationStamp::Inspect(Companion, BulkStamp))
				return Error(EAssetWriteError::AlreadyExists, "Package destination is unavailable or occupied.");
			Prepared.Revision = Package->GetEditRevision();
			FByteBuffer Bytes;
			FAssetPackageSerializationOptions Serialization; Serialization.Mode = Mode;
			AssetPrivate::FScopedBulkSaveRetention RetainSources(bDirect);
			if (auto Result = BuildPackageBytes(Package, Bytes, &Prepared.File, Serialization); !Result) return Result;
			if (Package->GetEditRevision() != Prepared.Revision || Package->GetPackagePathIdentity() != Prepared.Path
				|| GetPhysicalPath(Prepared.Path) != Prepared.Destination.generic_string())
				return Error(EAssetWriteError::StaleData, "Package or destination changed during capture.");
			Prepared.DetachedBytes = Bytes.size() + Prepared.File.BulkBytes.size();
			if (bDirect)
			{
				if (auto Result = PackageSavePrivate::CheckAsyncAdmission(Prepared.DetachedBytes); !Result)
					return Error(EAssetWriteError::InUse, Result.Message);
				ReadAccess.reset();
				GetPackageResourceManager().RetirePackage(Path.ToString());
			}
			Prepared.Write = BeginAssetWrite(Prepared.Destination, std::move(Bytes), std::move(Prepared.File.BulkBytes),
				MainStamp, BulkStamp, bDirect);
			if (auto Result = ToAssetWriteResult(Prepared.Write->GetAdmissionResult()); !Result) return Result;
			// Catch registration which finished between the first retirement and
			// exclusive physical admission. New registration is now excluded.
			if (bDirect) GetPackageResourceManager().RetirePackage(Path.ToString());
			Out = std::move(Prepared);
			return {};
		}

		using FSaveParticipants = std::unordered_map<FPackagePath, std::optional<FAssetData>>;
		auto SaveParticipantsChanged(const FSaveParticipants& Participants, const FPackagePath& Path) -> bool
		{
			return std::ranges::any_of(Participants, [&](const auto& Entry) {
				const auto Current = FindAssetExact(Entry.first).Data;
				// The saved package remains an exact participant. Dependencies only need
				// stable identities and reference routing, not unchanged content metadata.
				if (Entry.first == Path) return Current != Entry.second;
				if (IsAssetRegistryProjectionFenced(Entry.first)) return true;
				if (Current.has_value() != Entry.second.has_value()) return true;
				if (!Current) return false;
				return Current->PhysicalPath != Entry.second->PhysicalPath
					|| !std::ranges::is_permutation(Current->TopLevelAssets, Entry.second->TopLevelAssets);
			});
		}

		auto RollbackAssetWrite(IPackageWriteOperation& Write, FAssetWriteResult Failure) -> FAssetWriteResult
		{
			auto Restored = Write.Rollback();
			auto Rollback = ToAssetWriteResult(Restored);
			if (Restored.State == EPackageWriteState::RecoveryRequired)
			{
				Failure.Disposition = Rollback.Disposition;
				Failure.Message += "; rollback: " + Rollback.Message;
				Failure.RecoveryLocation = Rollback.RecoveryLocation;
			}
			return Failure;
		}
		auto FinishOrdinarySave(DPackage* Package, const FPackagePath& Path, uint64 Revision,
			const FPackageFile& File, const std::filesystem::path& Destination,
			IPackageWriteOperation& Write, const FAssetBundleSaveOptions& Options,
			const FAssetRegistryPublication& Expected) -> FAssetWriteResult
		{
			if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return Guard;
			if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
				return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
			if (auto Admission = ValidatePackageWriteAdmission(Path); !Admission) return Admission;
			if (auto Version = ValidateSaveVersion(Path); !Version) return Version;
			if (Package->GetEditRevision() != Revision || Package->GetPackagePathIdentity() != Path)
				return Error(EAssetWriteError::StaleData, "Package changed during save preparation.");
			if (Options.RootPackage && Options.RootPackage != Package)
				return Error(EAssetWriteError::InvalidData, "The root package is not the saved package.");
			if (Options.ShouldFail)
				for (auto Phase : {EAssetBundleSavePhase::CreateDirectories, EAssetBundleSavePhase::PublishCompanion,
					EAssetBundleSavePhase::StagePackage, Options.RootPackage == Package
						? EAssetBundleSavePhase::PublishRootPackage : EAssetBundleSavePhase::PublishPackage})
					if (Options.ShouldFail(Phase, 0)) return Error(EAssetWriteError::IoError, "Injected save publication failure.");
			if (auto Result = ToAssetWriteResult(Write.Commit()); !Result) return Result;
			FFilePublicationStamp Stamp;
			if (!FFilePublicationStamp::Inspect(Destination, Stamp) || !Stamp.Exists)
				return RollbackAssetWrite(Write, Error(EAssetWriteError::IoError, "Cannot inspect committed package."));
			const bool Inject = Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::PublishRegistry, 1);
			auto RegistryResult = Inject ? Error(EAssetWriteError::StaleData, "Injected Registry publication failure.")
				: AssetPrivate::ToAssetResult(AssetsSaved({BuildSavedAssetMetadata(File, Path, Destination, Stamp.Size, Stamp.Time)}, Expected));
			if (!RegistryResult && Options.bRollbackOnRegistryFailure) return RollbackAssetWrite(Write, RegistryResult);
			if (FindResidentPackage(Path) == Package) Package->MarkAsPublished();
			if (Package->GetEditRevision() == Revision) Package->ClearDirty();
			auto Finalized = ToAssetWriteResult(Write.Finalize());
			if (!RegistryResult)
			{
				FenceAssetRegistryProjection(std::span(&Path, 1));
				RegistryResult.Disposition = EAssetWriteDisposition::ContentCommittedProjectionPending;
				RegistryResult.Message = "ContentCommittedProjectionPending: " + RegistryResult.Message;
				if (!Finalized) { RegistryResult.Message += "; " + Finalized.Message; RegistryResult.RecoveryLocation = Finalized.RecoveryLocation; }
				return RegistryResult;
			}
			return Finalized;
		}
	}

	class FProtectedAssetSave
	{
	public:
		static auto Begin(DPackage*, FAssetWriteResult&, const FAssetBundleSaveOptions&) -> std::shared_ptr<FProtectedAssetSave>;
		~FProtectedAssetSave();
		auto Stage() -> void;
		auto Complete() -> FAssetWriteResult;
		auto GetDetachedBytes() const -> uint64;
	private:
		FProtectedAssetSave();
		struct FState;
		std::unique_ptr<FState> State;
	};
	struct FProtectedAssetSave::FState
	{
		TStrongObjectPtr<DPackage> Package;
		FPreparedPackageSave Prepared;
		std::unordered_map<FPackagePath, std::optional<FAssetData>> Participants;
		FAssetBundleSaveOptions Options;
		FAssetWriteResult StagingResult;
		std::optional<FAssetWriteResult> Result;
		bool bCommitting = false;
	};

	FProtectedAssetSave::FProtectedAssetSave() : State(std::make_unique<FState>()) {}
	FProtectedAssetSave::~FProtectedAssetSave()
	{
		check(IsInGameThread());

	}

	auto FProtectedAssetSave::Begin(DPackage* Package, FAssetWriteResult& OutResult, const FAssetBundleSaveOptions& Options)
		-> std::shared_ptr<FProtectedAssetSave>
	{
		check(IsInGameThread());
		if (!IsTaskSchedulerRunning() || !FAssetRuntimeState::Get().IsAcceptingRequests())
		{
			OutResult = Error(EAssetWriteError::StaleData, "Asset saves are not accepting asynchronous work.");
			return {};
		}
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate() || !Package->IsDirty())
		{
			OutResult = Error(EAssetWriteError::InvalidData, "Async save requires a loaded dirty asset package.");
			return {};
		}
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard)
		{ OutResult = Guard; return {}; }
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
		{ OutResult = Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved."); return {}; }
		auto Operation = std::shared_ptr<FProtectedAssetSave>(new FProtectedAssetSave());
		auto& Data = *Operation->State;
		Data.Package = Package;
		Data.Options = Options;
		FPackagePath Path;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path))
		{ OutResult = Error(EAssetWriteError::InvalidPath, "Invalid package path."); return {}; }
		if (FindResidentPackage(Path) != Package)
		{ OutResult = Error(EAssetWriteError::StaleData, "Package destination is unavailable."); return {}; }
		OutResult = PreparePackageSave(Package, Path, Options.Mode, Data.Prepared);
		if (!OutResult) return {};
		Data.Participants.emplace(Path, FindAssetExact(Path).Data);
		for (const auto& Dependency : Data.Prepared.File.Dependencies)
			Data.Participants.emplace(Dependency, FindAssetExact(Dependency).Data);
		return Operation;
	}

	auto FProtectedAssetSave::Stage() -> void { State->StagingResult = ToAssetWriteResult(State->Prepared.Write->Stage()); }
	auto FProtectedAssetSave::GetDetachedBytes() const -> uint64 { return State->Prepared.DetachedBytes; }
	auto FProtectedAssetSave::Complete() -> FAssetWriteResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (Data.bCommitting) return Error(EAssetWriteError::StaleData, "Save publication is already running.");
		const auto& Options = Data.Options;
		if (!Data.StagingResult) return *(Data.Result = Data.StagingResult);
		const bool ParticipantsChanged = SaveParticipantsChanged(Data.Participants, Data.Prepared.Path);
		if (FindResidentPackage(Data.Prepared.Path) != Data.Package.Get()
			|| Data.Package->GetPackagePath() != Data.Prepared.Path.GetView()
			|| Data.Package->GetEditRevision() != Data.Prepared.Revision
			|| ParticipantsChanged
			|| GetPhysicalPath(Data.Prepared.Path) != Data.Prepared.Destination.generic_string())
			return *(Data.Result = Error(EAssetWriteError::StaleData,
				"Asset or destination changed while saving; retry the current version."));
		DPackage* Package = Data.Package.Get();
		Data.bCommitting = true;
		Data.Result = FinishOrdinarySave(Package, Data.Prepared.Path, Data.Prepared.Revision, Data.Prepared.File,
			Data.Prepared.Destination, *Data.Prepared.Write, Options, CaptureAssetRegistryPublication());
		Data.bCommitting = false;
		return *Data.Result;
	}

	auto FAssetPackageSaveContext::SaveAsync(DPackage* Package, FAssetWriteResult& Admission) const
		-> Tasks::TTask<FAssetWriteResult>
	{
		if (Flags != SAVE_None || Options.PreparedPublication || (Options.RootPackage && Options.RootPackage != Package))
		{ Admission = Error(EAssetWriteError::InvalidData, "Invalid protected async save policy."); return {}; }
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(); !Result)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Result.Message); return {}; }
		auto Operation = FProtectedAssetSave::Begin(Package, Admission, Options);
		if (!Operation) return {};
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(Operation->GetDetachedBytes()); !Result)
		{ Admission = Error(EAssetWriteError::ShuttingDown, Result.Message); return {}; }
		auto Source = Tasks::TCompletionSource<FAssetWriteResult>::Create({.DebugName = "Asset.SaveAsync"});
		auto Task = Source.TakeTask();
		auto Submitted = PackageSavePrivate::SubmitAsyncSave(Operation->GetDetachedBytes(),
			[Operation] { Operation->Stage(); },
			[Operation, Source, Cancellation = Cancellation](bool bSucceeded) mutable {
				const bool bCancelled = Cancellation.IsCancellationRequested()
					|| Private::FTaskRuntimeAccess::IsCancellationRequested(Source.GetCompletion().GetTaskHandle());
				auto Result = bCancelled ? Error(EAssetWriteError::Cancelled, "Save was cancelled before publication.")
					: !bSucceeded ? Error(EAssetWriteError::IoError, "Save I/O task failed.")
					: Operation->Complete();
				Operation.reset();
				Source.TrySetValue(std::move(Result));
			});
		if (!Submitted)
		{ Admission = Error(EAssetWriteError::InUse, Submitted.Message); Source.TrySetValue(Admission); return {}; }
		Admission = {};
		return Task;
	}
	namespace
	{
		FAsyncPackageSaveSink AsyncSaveSink;
		bool bFailDirectPublication = false;
		struct FDirectAssetSave
		{
			TStrongObjectPtr<DPackage> Package;
			FPreparedPackageSave Prepared;
			FPackageWriteResult Written;
			FSaveParticipants Participants;
			bool bStarted = false;
		};
	}
	auto SetAsyncPackageSaveSink(FAsyncPackageSaveSink Sink) -> void
	{ check(IsInGameThread()); AsyncSaveSink = std::move(Sink); }
	namespace AssetPrivate
	{
		auto SetAsyncSavePublicationFailureForTests(bool bFail) -> void
		{ check(IsInGameThread()); bFailDirectPublication = bFail; }
	}
	auto SavePackage(DPackage* Package, EPackageSaveFlags Flags, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		if (Flags == SAVE_None) return SavePackage(Package, Mode);
		if (Flags != SAVE_Async) return Error(EAssetWriteError::InvalidData, "Unknown package save flags.");
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard) return Guard;
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
			return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (!FAssetRuntimeState::Get().IsAcceptingRequests()) return Error(EAssetWriteError::ShuttingDown, "Asset runtime is shutting down.");
		if (auto Result = PackageSavePrivate::CheckAsyncAdmission(); !Result) return Error(EAssetWriteError::InUse, Result.Message);
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate()
			|| FindResidentPackage(Package->GetPackagePathIdentity()) != Package)
			return Error(EAssetWriteError::InvalidData, "A resident asset package is required.");
		auto Data = std::make_shared<FDirectAssetSave>();
		Data->Package = Package;
		if (auto Result = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Mode, Data->Prepared, true); !Result) return Result;
		Data->Participants.emplace(Data->Prepared.Path, FindAssetExact(Data->Prepared.Path).Data);
		for (const auto& Dependency : Data->Prepared.File.Dependencies)
			Data->Participants.emplace(Dependency, FindAssetExact(Dependency).Data);
		auto Submitted = PackageSavePrivate::SubmitAsyncSave(Data->Prepared.DetachedBytes,
			[Data] { Data->bStarted = true; Data->Written = Data->Prepared.Write->Stage(); },
			[Data, Sink = AsyncSaveSink, bFailPublication = bFailDirectPublication](bool bSucceeded) mutable {
				auto& Prepared = Data->Prepared;
				const auto Path = Prepared.Path;
				auto Result = ToAssetWriteResult(Data->Written);
				if (!bSucceeded)
				{
					Result = Error(EAssetWriteError::IoError, "Direct write task failed; the closure requires reconciliation.");
					Result.Disposition = Data->bStarted ? EAssetWriteDisposition::PartiallyWritten : EAssetWriteDisposition::Default;
					auto Bulk = Prepared.Destination; Bulk.replace_extension(".dbulk");
					Result.AffectedFiles = {Bulk, Prepared.Destination};
				}
				if (Result)
				{
					FFilePublicationStamp Stamp;
					if (SaveParticipantsChanged(Data->Participants, Path)
						|| FindResidentPackage(Path) != Data->Package.Get() || Data->Package->GetPackagePathIdentity() != Path
						|| GetPhysicalPath(Path) != Prepared.Destination.generic_string()
						|| !FFilePublicationStamp::Inspect(Prepared.Destination, Stamp) || !Stamp.Exists)
						Result = Error(EAssetWriteError::StaleData, "Written package identity or destination changed before publication.");
					else
						Result = bFailPublication ? Error(EAssetWriteError::StaleData, "Injected Registry publication failure.")
							: AssetPrivate::ToAssetResult(AssetsSaved({BuildSavedAssetMetadata(Prepared.File,
							Path, Prepared.Destination, Stamp.Size, Stamp.Time)}, CaptureAssetRegistryPublication()));
					if (!Result) Result.Disposition = EAssetWriteDisposition::ContentCommittedProjectionPending;
					else
					{
						Data->Package->MarkAsPublished();
						if (Data->Package->GetEditRevision() == Prepared.Revision) Data->Package->ClearDirty();
					}
				}
				if (!Result && Result.Disposition != EAssetWriteDisposition::Default)
				{
					if (!Data->Package->IsDirty()) Data->Package->MarkDirty();
					FenceAssetRegistryProjection(std::span(&Path, 1));
				}
				Data.reset();
				if (!Result)
				{
					DURIN_ERROR("Async save {} failed: {}", Path.ToString(), Result.Message);
					for (const auto& File : Result.AffectedFiles) DURIN_ERROR("Affected package file: {}", File.string());
				}
				if (Sink) Sink(Path, Result);
			});
		return Submitted ? FAssetWriteResult{} : Error(EAssetWriteError::InUse, Submitted.Message);
	}

	auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options) -> FAssetWriteResult
	{
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Result) return Result;
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.SavePackagesAtomically(Packages, Options);
	}

	auto FAssetMutationCoordinator::SavePackagesAtomically(
		std::span<DPackage* const> Packages, const FAssetBundleSaveOptions& Options) -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		if (Packages.empty()) return Error(EAssetWriteError::InvalidData, "An asset bundle must contain at least one package.");
		if (Options.PreparedPublication && !Options.bRollbackOnRegistryFailure)
			return Error(EAssetWriteError::InvalidData, "Prepared publication requires rollback on Registry failure.");
		if (RuntimeConfiguration.IsCooked()) return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (Options.RootPackage && std::ranges::find(Packages, Options.RootPackage) == Packages.end())
			return Error(EAssetWriteError::InvalidData, "The root package is not part of the asset bundle.");
		const auto Expected = CaptureAssetRegistryPublication();
		if (Options.PreparedPublication && (!Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty()))
			return Error(EAssetWriteError::StaleData, "Prepared publication requires a complete Registry projection.");
		std::vector<FPreparedPackageSave> StagedPackages;
		StagedPackages.reserve(Packages.size());
		std::unordered_set<FPackagePath> Paths;
		auto Rollback = [&](FAssetWriteResult Failure) {
			for (auto It = StagedPackages.rbegin(); It != StagedPackages.rend(); ++It)
				if (It->Write) Failure = RollbackAssetWrite(*It->Write, std::move(Failure));
			return Failure;
		};
		for (DPackage* Package : Packages)
		{
			FPackagePath Path;
			const bool Owned = Package && Options.PreparedPublication && Options.PreparedPublication->OwnsPreparedPackage(*Package);
			if (!Package || !Package->IsAssetPackage() || (Package->IsGraphPrivate() && !Owned)
				|| !FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				return Error(EAssetWriteError::InvalidData, "The asset bundle contains an invalid package.");
			if (!Paths.insert(Path).second) return Error(EAssetWriteError::AlreadyExists, "The asset bundle contains a duplicate package.");
			if (auto Result = PreparePackageSave(Package, Path, Options.Mode, StagedPackages.emplace_back()); !Result) return Result;
		}
		for (size_t Index = 0; Index < StagedPackages.size(); ++Index)
		{
			auto& Staged = StagedPackages[Index];
			if (Options.ShouldFail)
				for (auto Phase : {EAssetBundleSavePhase::CreateDirectories, EAssetBundleSavePhase::PublishCompanion, EAssetBundleSavePhase::StagePackage})
					if ((Phase != EAssetBundleSavePhase::PublishCompanion || Staged.File.BulkSegmentExtent)
						&& Options.ShouldFail(Phase, Index)) return Rollback(Error(EAssetWriteError::IoError, "Injected bundle staging failure."));
			if (auto Result = ToAssetWriteResult(Staged.Write->Stage()); !Result) return Rollback(Result);
		}
		std::stable_sort(StagedPackages.begin(), StagedPackages.end(), [&](const auto& A, const auto& B) {
			return A.Package != Options.RootPackage && B.Package == Options.RootPackage;
		});
		std::vector<FAssetData> Metadata;
		for (auto& Staged : StagedPackages)
			if (auto Result = ToAssetWriteResult(Staged.Write->ReserveCommit()); !Result) return Rollback(Result);
		for (size_t Index = 0; Index < StagedPackages.size(); ++Index)
		{
			auto& Staged = StagedPackages[Index];
			if (Staged.Package->GetEditRevision() != Staged.Revision || Staged.Package->GetPackagePathIdentity() != Staged.Path)
				return Rollback(Error(EAssetWriteError::StaleData, "Package changed during bundle capture."));
			const auto Phase = Staged.Package == Options.RootPackage ? EAssetBundleSavePhase::PublishRootPackage : EAssetBundleSavePhase::PublishPackage;
			if (Options.ShouldFail && Options.ShouldFail(Phase, Index)) return Rollback(Error(EAssetWriteError::IoError, "Injected bundle publication failure."));
			if (auto Result = ToAssetWriteResult(Staged.Write->Commit()); !Result) return Rollback(Result);
			FFilePublicationStamp Stamp;
			if (!FFilePublicationStamp::Inspect(Staged.Destination, Stamp) || !Stamp.Exists)
				return Rollback(Error(EAssetWriteError::IoError, "Cannot inspect committed package."));
			Metadata.push_back(BuildSavedAssetMetadata(Staged.File, Staged.Path, Staged.Destination, Stamp.Size, Stamp.Time));
		}
		const bool Inject = Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::PublishRegistry, StagedPackages.size());
		auto RegistryResult = Inject ? Error(EAssetWriteError::StaleData, "Injected Registry publication failure.")
			: AssetPrivate::ToAssetResult(AssetsSaved(std::move(Metadata), Expected));
		if (!RegistryResult && Options.bRollbackOnRegistryFailure) return Rollback(RegistryResult);
		FAssetWriteResult Finalized;
		for (auto& Staged : StagedPackages)
		{
			if (FindResidentPackage(Staged.Path) == Staged.Package) Staged.Package->MarkAsPublished();
			if (Staged.Package->GetEditRevision() == Staged.Revision) Staged.Package->ClearDirty();
			if (auto Result = ToAssetWriteResult(Staged.Write->Finalize()); !Result) Finalized = Result;
		}
		if (!RegistryResult)
		{
			std::vector<FPackagePath> Fenced(Paths.begin(), Paths.end());
			FenceAssetRegistryProjection(Fenced);
			RegistryResult.Disposition = EAssetWriteDisposition::ContentCommittedProjectionPending;
			RegistryResult.Message = "ContentCommittedProjectionPending: " + RegistryResult.Message;
			if (!Finalized) { RegistryResult.Message += "; " + Finalized.Message; RegistryResult.RecoveryLocation = Finalized.RecoveryLocation; }
			return RegistryResult;
		}
		return Finalized;
	}

	auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> FAssetWriteResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().AdmitAssetPackageToCatalog(Path);
	}

	auto FAssetMutationCoordinator::AdmitAssetPackageToCatalog(
		const FPackagePath& Path) -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		if (!Path.IsValid())
			return Error(EAssetWriteError::InvalidPath, "The asset admission path is invalid.");
		if (Durin::FindAssetExact(Path) || FindResidentPackage(Path))
			return Error(EAssetWriteError::AlreadyExists,
				"The asset admission path is already occupied.");
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty())
			return Error(EAssetWriteError::InvalidPath,
				"The asset admission path is outside mounted package content.");
		FAssetPackageHeader Header;
		if (FAssetRegistryResult Result = ReadAssetPackageHeader(
			PhysicalPath, Path, Header); !Result)
			return AssetPrivate::ToAssetResult(std::move(Result));
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(EAssetWriteError::InUse, "Package output is being written.");
		FByteBuffer Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetWriteError::IoError,
				"The asset package could not be read for admission validation.");
		FByteBuffer BulkBytes;
		if (FAssetWriteResult Result = LoadPackageBulkBytes(PhysicalPath, BulkBytes); !Result)
			return Result;
		if (FAssetWriteResult Result = ValidateAssetPackageBytes(Bytes, Path, BulkBytes); !Result)
			return Result;
		std::error_code ErrorCode;
		const auto LastWriteTime = std::filesystem::last_write_time(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(EAssetWriteError::IoError,
				"The asset package timestamp could not be read for admission.");
		const uintmax_t FileSize = std::filesystem::file_size(
			PhysicalPath, ErrorCode);
		if (ErrorCode)
			return Error(EAssetWriteError::IoError,
				"The asset package size could not be read for admission.");
		const auto Expected = CaptureAssetRegistryPublication();
		if (!Expected.bReferenceIndexComplete || !Expected.ReferenceErrors.empty())
			return Error(EAssetWriteError::StaleData, "Asset metadata cannot publish while the reference index is incomplete.");
		return AssetPrivate::ToAssetResult(AssetsSaved({FAssetData{
			.PackagePath = Path,
			.PhysicalPath = PhysicalPath,
			.TopLevelAssets = ProjectTopLevelAssetData(Header),
			.AssetClassName = Header.AssetClassName,
			.EntryKind = Header.EntryKind,
			.RedirectDestination = Header.RedirectDestination,
			.FormatVersion = Header.FormatVersion,
			.Dependencies = Header.Dependencies,
			.SoftDependencies = Header.SoftDependencies,
			.SearchableNames = Header.SearchableNames,
			.ObjectCount = Header.ObjectCount,
			.BulkSegmentExtent = Header.BulkSegmentExtent,
			.BulkSegmentDigest = Header.BulkSegmentDigest,
			.FileSize = FileSize,
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)}}, Expected));
	}

	auto FAssetPackageField::TryReadString(std::string& OutValue) const -> bool
	{
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return Reader.ReadString(OutValue, Durin::PackagePrivate::MaximumPackageStringBytes) && Reader.Offset == Payload.size();
	}

	namespace
	{
		auto ReadInspectedObjectReference(
			Durin::PackagePrivate::FByteReader& Reader,
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
			return Reader.ReadString(PathString, Durin::PackagePrivate::MaximumPackageStringBytes)
				&& FObjectPath::TryCreate(PathString, OutValue.ExternalPath);
		}
	}

	auto FAssetPackageField::TryReadObjectReference(
		FAssetPackageObjectReference& OutValue) const -> bool
	{
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return ReadInspectedObjectReference(Reader, OutValue)
			&& Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryReadObjectReferenceArray(
		std::vector<FAssetPackageObjectReference>& OutValues) const -> bool
	{
		OutValues.clear();
		Durin::PackagePrivate::FByteReader Reader{Payload};
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

	auto FAssetPackageField::TryReadBulkDataStorageDescriptor(
		FPackageBulkStorageDescriptor& OutValue, bool bValidateInlinePayload) const -> bool
	{
		OutValue = {};
		if (Kind != DurinCodeGen::EPropertyGenFlags::BulkData
			|| !ObjectPackage::IsSupportedPackageReaderVersion(SourceFormatVersion)) return false;
		Durin::PackagePrivate::FByteReader Reader{Payload};
		uint32 Version = 0;
		uint8 Placement = 0;
		uint8 Reserved = 0;
		uint16 Alignment = 0;
		uint32 ElementSize = 0;
		uint64 FieldIndex = 0;
		uint64 HashLow = 0;
		uint64 HashHigh = 0;
		if (!Reader.Read(Version) || Version != 1
			|| !Reader.Read(Placement) || Placement > 1
			|| !Reader.Read(Reserved) || Reserved != 0
			|| !Reader.Read(Alignment) || Alignment == 0 || Alignment > 4096
			|| (Alignment & (Alignment - 1)) != 0
			|| !Reader.Read(ElementSize) || ElementSize == 0
			|| !Reader.Read(FieldIndex) || FieldIndex == 0
			|| !Reader.Read(OutValue.PayloadId)
			|| !Reader.Read(HashLow) || !Reader.Read(HashHigh)
			|| !Reader.Read(OutValue.LogicalByteCount)
			|| !Reader.Read(OutValue.StoredByteCount)
			|| !Reader.Read(OutValue.SegmentOffset)) return false;
		OutValue.ContentHash = {HashLow, HashHigh};
		OutValue.StorageKind = Placement == 0
			? EPackageBulkStorageKind::Inline
			: EPackageBulkStorageKind::External;
		OutValue.Alignment = Alignment;
		if (!OutValue.PayloadId.IsValid() || OutValue.ContentHash.IsZero()
			|| OutValue.LogicalByteCount != OutValue.StoredByteCount) return false;
		if (Placement == 1)
			return OutValue.SegmentOffset % Alignment == 0
				&& Reader.Offset == Payload.size();
		if (Alignment != 1 || OutValue.SegmentOffset != 0
			|| Reader.Offset > Payload.size()
			|| OutValue.StoredByteCount != Payload.size() - Reader.Offset) return false;
		return !bValidateInlinePayload || FXxHash128::HashBuffer(
			FByteView(Payload).subspan(Reader.Offset))
			== OutValue.ContentHash;
	}

	auto FAssetPackageField::TryReadEditorBulkDataStorageDescriptor(
		FPackageBulkStorageDescriptor& OutValue) const -> bool
	{
		return TryReadBulkDataStorageDescriptor(OutValue);
	}

	namespace
	{
		auto ReadInspectedStructFields(Durin::PackagePrivate::FByteReader& Reader, uint32 SourceFormatVersion,
			std::vector<FAssetPackageField>& OutFields) -> bool
		{
			std::string StructName;
			uint64 FieldCount = 0;
			if (!Reader.ReadString(StructName, Durin::PackagePrivate::MaximumPackageStringBytes)
				|| !Reader.Read(FieldCount) || FieldCount > 100000) return false;
			OutFields.reserve(static_cast<size_t>(FieldCount));
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				FAssetPackageField Field;
				uint8 FieldKind = 0;
				uint64 PayloadSize = 0;
				if (!Reader.ReadString(Field.DeclaringClass, Durin::PackagePrivate::MaximumPackageStringBytes)
					|| !Reader.ReadString(Field.Name, Durin::PackagePrivate::MaximumPackageStringBytes)
					|| !Reader.Read(FieldKind)
					|| !Reader.ReadString(Field.TypeSignature, Durin::PackagePrivate::MaximumPackageStringBytes)
					|| !Reader.Read(PayloadSize)
					|| Reader.Offset > Reader.Bytes.size()
					|| PayloadSize > Reader.Bytes.size() - Reader.Offset) return false;
				Field.Kind = static_cast<DurinCodeGen::EPropertyGenFlags>(FieldKind);
				Field.SourceFormatVersion = SourceFormatVersion;
				Field.Payload.resize(static_cast<size_t>(PayloadSize));
				if (PayloadSize != 0
					&& !Reader.ReadBytes(Field.Payload.data(), static_cast<size_t>(PayloadSize)))
					return false;
				OutFields.push_back(std::move(Field));
			}
			return true;
		}
	}

	auto FAssetPackageField::TryInspectStructFields(
		std::vector<FAssetPackageField>& OutFields) const -> bool
	{
		OutFields.clear();
		if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return false;
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return ReadInspectedStructFields(Reader, SourceFormatVersion, OutFields)
			&& Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryInspectStructArray(
		std::vector<std::vector<FAssetPackageField>>& OutElements) const -> bool
	{
		OutElements.clear();
		if (Kind != DurinCodeGen::EPropertyGenFlags::Array
			|| !TypeSignature.starts_with("Array<Struct<")) return false;
		Durin::PackagePrivate::FByteReader Reader{Payload};
		uint64 Count = 0;
		if (!Reader.Read(Count) || Count > 100000 || Count > Payload.size() / 16) return false;
		std::vector<std::vector<FAssetPackageField>> Elements(static_cast<size_t>(Count));
		for (auto& Fields : Elements)
			if (!ReadInspectedStructFields(Reader, SourceFormatVersion, Fields)) return false;
		if (Reader.Offset != Payload.size()) return false;
		OutElements = std::move(Elements);
		return true;
	}

	auto FAssetPackageField::TryReadStruct(DStruct* Struct, void* OutValue) const -> bool
	{
		if (!Struct || !OutValue
			|| Struct->PropertiesSize == 0
			|| Struct->PropertiesSize > std::numeric_limits<uint16>::max()
			|| Struct->MinAlignment == 0
			|| (Struct->MinAlignment & (Struct->MinAlignment - 1)) != 0
			|| TypeSignature != std::format("Struct<{}>", Struct->GetQualifiedName()))
			return false;

		FStructProperty RootProperty(
			FFieldVariant(), FName("InspectedStructValue"), EObjectFlags::NoFlags,
			EPropertyFlags::None, 1, 0, Struct);
		Durin::PackagePrivate::FByteReader Reader{Payload};
		return DecodeByteToolValue(
			&RootProperty, OutValue, 0, Reader, {},
			SourceFormatVersion == 0 ? ObjectPackage::DastV10FormatVersion : SourceFormatVersion)
			&& Reader.Offset == Payload.size();
	}

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			FByteView Bytes,
			const FPackagePath& PackagePath,
			FAssetPackageInspection& OutInspection) -> FAssetReadResult
		{
			OutInspection = {};
			auto Result = MakePackageFingerprint(PhysicalPath, Bytes, OutInspection.Fingerprint);
			if (!Result) return Result;
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			if (!PackagePath.IsValid())
				return Error(EAssetReadError::InvalidPath,
					"DAST v10 inspection requires a mounted package identity.");
			FByteBuffer BulkBytes;
			if (Result = LoadPackageBulkBytes(PhysicalPath, BulkBytes); !Result)
				return Result;
			const AssetPrivate::FAssetPackageReadContext Context{
				.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = PackagePath, .PhysicalPackageBytes = Bytes.size()};
			FAssetPackageInspection Inspection;
			Result = Codec->Inspect(Context, Inspection);
			if (!Result) return Result;
			Inspection.PhysicalPath = PhysicalPath;
			Inspection.Fingerprint = OutInspection.Fingerprint;
			OutInspection = std::move(Inspection);
			return {};
		}
	}

	auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetReadResult
	{
		FPackagePath PackagePath;
		if (!ClassifyPackageIdentity(PhysicalPath, PackagePath))
			return Error(EAssetReadError::InvalidPath,
				"DAST v10 inspection requires a mounted package identity.");
		return InspectAssetPackage(PhysicalPath, PackagePath, OutInspection);
	}

	auto InspectAssetPackage(std::string_view PhysicalPath,
		const FPackagePath& PackagePath,
		FAssetPackageInspection& OutInspection) -> FAssetReadResult
	{
		OutInspection = {};
		auto ReadAccess = FPackageFileAccess::TryReadPackage(std::filesystem::path(PhysicalPath));
		if (!ReadAccess) return Error(EAssetReadError::InUse, "Package output is being written.");
		FByteBuffer Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetReadError::IoError, std::format("Failed to open asset package {}.", PhysicalPath));
		return InspectAssetPackageBytes(PhysicalPath, Bytes, PackagePath, OutInspection);
	}


	auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes) -> FAssetWriteResult
	{
		return CanonicalizeAssetPackageForCook(
			Bytes, BulkBytes, PackagePath, PackagePath, OutBytes, OutBulkBytes);
	}

	auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& SourcePackagePath,
		const FPackagePath& OutputPackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes) -> FAssetWriteResult
	{
		OutBytes.clear();
		OutBulkBytes.clear();
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (auto Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		if (!Codec->bCanMutate)
			return Error(EAssetReadError::UnsupportedVersion,
				"Cook canonicalization requires package mutation capability.");
		FByteBuffer RelocatedBytes;
		FByteBuffer RelocatedBulkBytes;
		if (SourcePackagePath != OutputPackagePath)
		{
			const AssetPrivate::FAssetPackageReadContext SourceContext{
				.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = SourcePackagePath,
				.PhysicalPackageBytes = Bytes.size()};
			AssetPrivate::FAssetPackageEncodedClosure Relocated;
			auto RelocateResult = Codec->Relocate(
				SourceContext, OutputPackagePath, Relocated);
			if (!RelocateResult) return RelocateResult;
			RelocatedBytes = std::move(Relocated.PackageBytes);
			RelocatedBulkBytes = std::move(Relocated.BulkBytes);
			Bytes = RelocatedBytes;
			BulkBytes = RelocatedBulkBytes;
		}
		const AssetPrivate::FAssetPackageReadContext Context{
			.PackageBytes = Bytes, .BulkBytes = BulkBytes,
			.PackagePath = OutputPackagePath, .PhysicalPackageBytes = Bytes.size()};
		FAssetPackageHeader Header;
		auto Result = Codec->ReadHeader(Context, Header);
		if (!Result) return Result;
		if (Header.EntryKind != EAssetRegistryEntryKind::Asset)
			return Error(EAssetReadError::InvalidPackageType,
				"CookCanonicalizationRedirectorPackage: redirector packages are uncooked-only.");
		std::vector<FAssetReferenceEdge> References;
		Result = Codec->ExtractReferences(Context, References);
		if (!Result) return Result;

		std::vector<FAssetRedirectorFixupMapping> Mappings;
		auto ResolveReference = [&](const FPackagePath& Path,
			std::string_view ExpectedClassName, std::string_view Route) -> FAssetReadResult {
			DClass* ExpectedClass = nullptr;
			if (!ExpectedClassName.empty())
			{
				ExpectedClass = FindClassByQualifiedName(FName(ExpectedClassName));
				if (!ExpectedClass)
					return Error(EAssetReadError::UnknownClass, std::format(
						"CookCanonicalizationUnknownExpectedClass: {} expects unavailable class {}.",
						Route, ExpectedClassName));
			}
			const FAssetPathResolveResult Resolution = Durin::ResolveAssetPathForOperation(
				Path, {.ExpectedClass = ExpectedClass});
			if (!Resolution)
			{
				auto ResolutionError = AssetPathResolutionError(Resolution);
				ResolutionError.Message = std::format(
					"CookCanonicalizationUnresolvedReference: {} at {}. {}",
					Path.ToString(), Route, ResolutionError.Message);
				return ResolutionError;
			}
			if (!Resolution.FinalAssetData
				|| Resolution.FinalAssetData->EntryKind != EAssetRegistryEntryKind::Asset)
				return Error(EAssetReadError::InvalidPackageType,
					"Cook canonicalization resolved a reference to a non-asset package.");
			if (Resolution.FinalPath == Path) return {};
			const auto Existing = std::ranges::find(
				Mappings, Path, &FAssetRedirectorFixupMapping::RedirectorPath);
			if (Existing == Mappings.end())
				Mappings.push_back({.RedirectorPath = Path, .FinalPath = Resolution.FinalPath});
			else if (Existing->FinalPath != Resolution.FinalPath)
				return Error(EAssetReadError::StaleData,
					"Cook canonicalization observed inconsistent redirect resolution.");
			return {};
		};
		for (const FPackagePath& Dependency : Header.Dependencies)
		{
			Result = ResolveReference(Dependency, {}, "package dependency table");
			if (!Result) return Result;
		}
		for (const FAssetReferenceEdge& Reference : References)
		{
			Result = ResolveReference(Reference.TargetPath.GetPackagePath(), Reference.ExpectedClass,
				Reference.DisplayRoute);
			if (!Result) return Result;
		}
		if (Mappings.empty())
		{
			OutBytes.assign(Bytes.begin(), Bytes.end());
			OutBulkBytes.assign(BulkBytes.begin(), BulkBytes.end());
			return {};
		}
		auto Rewritten = AssetPrivate::RewritePackageReferencesForMutation(
			Bytes, BulkBytes, OutputPackagePath, Mappings,
			std::numeric_limits<uint64>::max(), OutBytes);
		if (Rewritten) OutBulkBytes.assign(BulkBytes.begin(), BulkBytes.end());
		return Rewritten;

	}

	auto FAssetMutationCoordinator::SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetWriteResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		if (RuntimeConfiguration.IsCooked()) return Error(EAssetWriteError::ReadOnlyMode, "Cooked packages cannot be saved.");
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate())
			return Error(EAssetReadError::InvalidPackageType, "A live persistent package is required.");
		const auto Expected = CaptureAssetRegistryPublication();
		FPreparedPackageSave Prepared;
		if (auto Result = PreparePackageSave(Package, Package->GetPackagePathIdentity(), Mode, Prepared); !Result) return Result;
		if (auto Result = ToAssetWriteResult(Prepared.Write->Stage()); !Result) return Result;
		return FinishOrdinarySave(Package, Prepared.Path, Prepared.Revision, Prepared.File,
			Prepared.Destination, *Prepared.Write, {.RootPackage = Package, .Mode = Mode}, Expected);
	}

}
