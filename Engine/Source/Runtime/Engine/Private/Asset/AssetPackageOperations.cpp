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
#include "Asset/EditorBulkDataStorage.h"
#include "AssetPackageArchive.h"
#include "AssetPropertyKindTraits.h"
#include "AssetPackageValueCodec.h"
#include "DObject/DefaultDeltaPlan.h"
#include "Profiling/Profiling.h"
#include "Serialization/BinaryEnvelope.h"
#include "Serialization/BinaryFormat.h"

#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "Misc/FileHelper.h"
#include "Misc/FilePublication.h"
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
			FAssetPackageInspection& OutInspection) -> FAssetResult;

		auto AssetPathResolutionError(
			const FAssetPathResolveResult& Resolution) -> FAssetResult
		{
			switch (Resolution.State)
			{
			case EAssetPathResolveState::Resolved:
				return {};
			case EAssetPathResolveState::ProjectionPending:
				return {EAssetError::StaleData,
					std::format("Registry projection for package {} is pending synchronization.",
						Resolution.FinalPath.ToString()),
					EAssetResultDisposition::ContentCommittedProjectionPending};
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
		using AssetPrivate::MaximumPackageStringBytes;
		using AssetPrivate::FByteReader;
		using AssetPrivate::FByteWriter;
		using AssetPrivate::GetSerializedTypeSignature;
		using AssetPrivate::IsSerializedTypeSignatureCompatible;
		constexpr uint32 MaximumRedirectDepth = 32;
		constexpr std::string_view RedirectorClassName = "Durin::DAssetRedirector";

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

		auto Error(EAssetError Code, std::string Message) -> FAssetResult { return {Code, std::move(Message)}; }

		auto LoadPackageBulkBytes(std::string_view PhysicalPath,
			FByteBuffer& OutBytes) -> FAssetResult
		{
			OutBytes.clear();
			std::filesystem::path BulkPath(PhysicalPath);
			BulkPath.replace_extension(".dbulk");
			std::error_code ErrorCode;
			if (!std::filesystem::is_regular_file(BulkPath, ErrorCode))
			{
				if (ErrorCode && ErrorCode != std::errc::no_such_file_or_directory)
					return Error(EAssetError::IoError,
						"Failed to inspect the package bulk companion.");
				return {};
			}
			if (!FFileHelper::LoadFileToArray(OutBytes, BulkPath))
				return Error(EAssetError::IoError,
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
			-> FAssetResult
		{
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			return Codec->Validate({.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = PackagePath, .PhysicalPackageBytes = Bytes.size()});
		}

		auto ValidatePackageWriteAdmission(const FPackagePath& Path) -> FAssetResult
		{
			const FMountLookupResult Mount =
				FMountPaths::FindMountForVirtualPath(Path.GetView());
			if (!Mount)
				return Error(EAssetError::InvalidPath,
					std::format("Package {} does not use a registered content mount.",
						Path.ToString()));
			if (!Mount.Mount->bContentWritable)
				return Error(EAssetError::ReadOnlyMode,
					std::format("Content mount {} is read-only.",
						Mount.Mount->VirtualRoot));
			return {};
		}

		auto CorruptRedirector(std::string Message) -> FAssetResult
		{
			return Error(EAssetError::CorruptFile, std::format("CorruptRedirector: {}", Message));
		}

		auto ValidateRedirectorHeader(
			const FPackageFile& File,
			uint64 ObjectCount,
			const FPackagePath* SourcePath = nullptr) -> FAssetResult
		{
			const FPackagePath PackagePath = SourcePath ? *SourcePath
				: (File.TopLevelAssets.empty() ? FPackagePath{}
					: File.TopLevelAssets.front().AssetPath.GetPackagePath());
			if (!ArePackageAssetsValid(File.TopLevelAssets, PackagePath,
				ObjectCount, File.Dependencies))
				return CorruptRedirector("the package contains invalid exact asset metadata.");
			return {};
		}

		auto IsMissingPathError(const std::error_code& ErrorCode) -> bool
		{
			return ErrorCode == std::errc::no_such_file_or_directory
				|| ErrorCode.value() == 2
				|| ErrorCode.value() == 3;
		}

		auto CleanupStaleEditorBulkDataCompanions(
			const std::filesystem::path& PackagePath,
			const std::filesystem::path& KeepPath = {}) -> void
		{
			const std::filesystem::path Parent = PackagePath.parent_path();
			const std::string Stem = PackagePath.stem().string();
			const std::string StableName = Stem + ".dbulk";
			std::error_code ErrorCode;
			for (std::filesystem::directory_iterator It(Parent, ErrorCode), End;
				!ErrorCode && It != End; It.increment(ErrorCode))
			{
				const std::filesystem::path Candidate = It->path();
				const std::string Name = Candidate.filename().string();
				if (!It->is_regular_file(ErrorCode) || ErrorCode
					|| Name != StableName
					|| (!KeepPath.empty() && Candidate == KeepPath))
				{
					ErrorCode.clear();
					continue;
				}
				std::filesystem::remove(Candidate, ErrorCode);
				ErrorCode.clear();
			}
		}

		struct FEditorBulkDataCompanionTransaction
		{
			std::filesystem::path FinalPath;
			std::filesystem::path BackupPath;
			FXxHash128 ContainerHash;
			uint64 Extent = 0;
			bool bHadFinal = false;
			bool bPublished = false;
			bool bPreparedFile = false;
			FFileReplacement Replacement;
		};

		auto PrepareEditorBulkDataCompanionState(
			const std::filesystem::path& PackagePath,
			std::string& OutError) -> bool
		{
			std::filesystem::path FinalPath;
			FinalPath = PackagePath;
			FinalPath.replace_extension(".dbulk");
			std::filesystem::path BackupPath = FinalPath;
			BackupPath += EditorBulkDataCompanionBackupSuffix;
			std::error_code ErrorCode;
			if (!std::filesystem::exists(BackupPath, ErrorCode))
			{
				if (ErrorCode && !IsMissingPathError(ErrorCode))
				{
					OutError = std::format("Failed to inspect authored bulk backup {}: {}",
						BackupPath.generic_string(), ErrorCode.message());
					return false;
				}
				OutError.clear();
				return true;
			}

			if (!std::filesystem::exists(FinalPath, ErrorCode))
				std::filesystem::rename(BackupPath, FinalPath, ErrorCode);
			else
				std::filesystem::remove(BackupPath, ErrorCode);
			if (ErrorCode)
			{
				OutError = std::format("Failed to recover package bulk segment backup: {}",
					ErrorCode.message());
				return false;
			}
			OutError.clear();
			return true;
		}

		auto PublishEditorBulkDataCompanion(
			const std::filesystem::path& PackagePath,
			FXxHash128 ContainerHash,
			uint64 Extent,
			FByteView Bytes,
			FEditorBulkDataCompanionTransaction& OutTransaction,
			std::string& OutError,
			const std::filesystem::path& PreparedFile = {}) -> bool
		{
			OutTransaction = {};
			if (!PrepareEditorBulkDataCompanionState(PackagePath, OutError)
				) return false;
			OutTransaction.FinalPath = PackagePath;
			OutTransaction.FinalPath.replace_extension(".dbulk");
			OutTransaction.BackupPath = OutTransaction.FinalPath;
			OutTransaction.BackupPath += EditorBulkDataCompanionBackupSuffix;
			OutTransaction.ContainerHash = ContainerHash;
			OutTransaction.Extent = Extent;
			OutTransaction.bPreparedFile = !PreparedFile.empty();

			std::error_code ErrorCode;
			OutTransaction.bHadFinal =
				std::filesystem::is_regular_file(OutTransaction.FinalPath, ErrorCode);
			if (ErrorCode && !IsMissingPathError(ErrorCode))
			{
				OutError = std::format("Failed to inspect authored bulk companion {}: {}",
					OutTransaction.FinalPath.generic_string(), ErrorCode.message());
				return false;
			}
			auto Staged = PreparedFile;
			const bool OwnsStage = Staged.empty();
			if (OwnsStage)
			{
				Staged = OutTransaction.FinalPath.string() + ".bulk-stage-" + FGuid::NewGuid().ToString();
				if (!StageFileVerified(Staged, Bytes, OutError))
				{
					std::filesystem::remove(Staged, ErrorCode);
					return false;
				}
			}
			OutTransaction.Replacement = {OutTransaction.FinalPath, Staged, OutTransaction.BackupPath};
			const bool Published = OutTransaction.Replacement.Publish(OutError);
			if (!Published && OwnsStage) std::filesystem::remove(Staged, ErrorCode);
			OutTransaction.bPublished = Published;
			return Published;
		}

		auto RollbackEditorBulkDataCompanion(
			FEditorBulkDataCompanionTransaction& Transaction,
			std::string& OutError) -> bool
		{
			if (!Transaction.Replacement.Rollback(OutError)) return false;
			Transaction.bPublished = false;
			return true;
		}

		auto VerifyEditorBulkDataCompanion(
			const FEditorBulkDataCompanionTransaction& Transaction,
			std::string& OutError) -> bool
		{
			if (!Transaction.bPublished || Transaction.bPreparedFile) return true;
			FByteBuffer Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Transaction.FinalPath)
				|| Bytes.size() != Transaction.Extent
				|| FXxHash128::HashBuffer(Bytes) != Transaction.ContainerHash)
			{
				OutError = "Published package bulk segment failed extent or digest verification.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto CommitEditorBulkDataCompanion(
			FEditorBulkDataCompanionTransaction& Transaction) -> void
		{
			std::string Error;
			(void)Transaction.Replacement.Finalize(Error);
			Transaction.bPublished = false;
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
			uint32 SourceVersion = ObjectPackage::DastV10FormatVersion) -> FAssetResult
		{
			const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
			if (AssetPrivate::IsByteToolRawScalarKind(Kind))
				return Reader.ReadBytes(Property->GetValuePtr(Container, ArrayIndex), Property->GetElementSize()) ? FAssetResult{} : Error(EAssetError::CorruptFile, "Truncated property payload.");
			switch (Kind)
			{
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
					FObjectPath Path;
					if (!Reader.ReadString(PathString) || !FObjectPath::TryCreate(PathString, Path)) return Error(EAssetError::InvalidPath, "Invalid external object reference.");
					FAssetResult Result = FAssetRuntimeState::Get().GetLoadService().LoadObject(
						Path, ObjectProperty->GetReferencedClass(), Value);
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
				FObjectPath Path;
				std::string PathError;
				if (!FObjectPath::TryCreate(PathString, Path, &PathError))
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
					FByteView Payload;
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
			FByteBuffer& OutBytes,
			FPackageFile* OutFile = nullptr,
			const FAssetPackageSerializationOptions& Options = {}) -> FAssetResult
		{
			const AssetPrivate::FAssetPackageCodec* Codec =
				AssetPrivate::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
			if (!Codec)
				return Error(EAssetError::UnsupportedVersion,
					"The ordinary asset package writer is unavailable.");
			FAssetPackageSerializationOptions EffectiveOptions = Options;
			std::vector<FEditorBulkDataStoragePayload> BulkPayloads;
			if (OutFile) EffectiveOptions.EditorBulkDataStoragePayloads = &BulkPayloads;
			AssetPrivate::FAssetPackageEncodedClosure Closure;
			FAssetResult Result = Codec->Write(
				Package, Closure, EffectiveOptions.Mode == EAssetPackageSaveMode::Complete
					? EDefaultDeltaMode::NoDelta : EDefaultDeltaMode::Enabled, EffectiveOptions);
			if (!Result) return Result;
			FPackagePath PackagePath;
			if (!Package || !FPackagePath::TryCreate(Package->GetPackagePath(), PackagePath))
				return Error(EAssetError::InvalidPath, "Package path is invalid.");
			if (OutFile)
			{
				OutFile->BulkBytes = std::move(Closure.BulkBytes);
				FAssetPackageHeader Header;
				const AssetPrivate::FAssetPackageReadContext Context{
					.PackageBytes = Closure.PackageBytes,
					.BulkBytes = OutFile->BulkBytes,
					.PackagePath = PackagePath,
					.PhysicalPackageBytes = Closure.PackageBytes.size()};
				if (FAssetResult HeaderResult = Codec->ReadHeader(Context, Header); !HeaderResult)
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
			const FAssetPublicationCoordinator& Registry,
			const FPackagePath& Path) -> FAssetResult
		{
			const FAssetCatalogEntry Existing = Durin::FindAssetExact(Path);
			if (!Existing || ObjectPackage::IsSupportedPackageReaderVersion(Existing->FormatVersion))
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

	namespace AssetPrivate
	{
		auto ValidateMutationPackageMetadata(
			const FMutationPackageMetadata& Metadata,
			uint64 ObjectCount,
			const FPackagePath* SourcePath) -> FAssetResult
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

	auto ValidateAssetPackageBytes(FByteView Bytes,
		const FPackagePath& PackagePath, FByteView BulkBytes) -> FAssetResult
	{
		return ValidateAssetPackageClosure(Bytes, BulkBytes, PackagePath);
	}

	auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteBuffer& OutBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetResult
	{
		return BuildPackageBytes(Package, OutBytes, nullptr, Options);
	}

	auto SerializeAssetPackageClosure(
		DPackage* Package,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetResult
	{
		FPackageFile File;
		FAssetResult Result = BuildPackageBytes(Package, OutBytes, &File, Options);
		if (!Result) return Result;
		OutBulkBytes = std::move(File.BulkBytes);
		return {};
	}

	struct FAsyncPackageSave::FState
	{
		using FStamp = FFilePublicationStamp;
		static auto Inspect(const std::filesystem::path& Path, FStamp& Stamp) -> bool
		{ return FStamp::Inspect(Path, Stamp); }
		TStrongObjectPtr<DPackage> Package;
		FPackagePath Path;
		uint64 EditRevision = 0;
		std::unordered_map<FPackagePath, std::optional<FAssetData>> Participants;
		FPackageFile File;
		FByteBuffer Bytes;
		std::filesystem::path Destination, Companion, Staged, StagedBulk;
		FStamp PackageStamp, BulkStamp, StagedStamp, StagedBulkStamp;
		Tasks::FTaskGroup Group;
		Tasks::TTask<FAssetResult> Worker;
		std::optional<FAssetResult> Result;
		bool bCommitting = false;
	};

	FAsyncPackageSave::FAsyncPackageSave() : State(std::make_unique<FState>()) {}
	FAsyncPackageSave::~FAsyncPackageSave()
	{
		check(IsInGameThread());
		if (State->Worker.IsValid()) State->Worker.Wait();
		std::error_code Ec;
		if (!State->Staged.empty()) std::filesystem::remove(State->Staged, Ec);
		if (!State->StagedBulk.empty()) std::filesystem::remove(State->StagedBulk, Ec);
	}

	auto FAsyncPackageSave::Begin(DPackage* Package, FAssetResult& OutResult)
		-> std::unique_ptr<FAsyncPackageSave>
	{
		check(IsInGameThread());
		if (!IsTaskSchedulerRunning() || !FAssetRuntimeState::Get().IsAcceptingRequests())
		{
			OutResult = Error(EAssetError::StaleData, "Asset saves are not accepting asynchronous work.");
			return {};
		}
		if (!Package || !Package->IsAssetPackage() || Package->IsGraphPrivate() || !Package->IsDirty())
		{
			OutResult = Error(EAssetError::InvalidPackageType, "Async save requires a loaded dirty asset package.");
			return {};
		}
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Guard)
		{ OutResult = Guard; return {}; }
		if (FAssetRuntimeState::Get().GetRuntimeConfiguration().IsCooked())
		{ OutResult = Error(EAssetError::ReadOnlyMode, "Cooked packages cannot be saved."); return {}; }
		auto Operation = std::unique_ptr<FAsyncPackageSave>(new FAsyncPackageSave());
		auto& Data = *Operation->State;
		Data.Package = Package;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Data.Path))
		{ OutResult = Error(EAssetError::InvalidPath, "Invalid package path."); return {}; }
		OutResult = ValidatePackageWriteAdmission(Data.Path);
		if (!OutResult) return {};
		Data.Destination = GetPhysicalPath(Data.Path);
		Data.Companion = Data.Destination;
		Data.Companion.replace_extension(".dbulk");
		if (Data.Destination.empty() || FindResidentPackage(Data.Path) != Package
			|| !FState::Inspect(Data.Destination, Data.PackageStamp)
			|| !FState::Inspect(Data.Companion, Data.BulkStamp))
		{ OutResult = Error(EAssetError::StaleData, "Package destination is unavailable."); return {}; }
		Data.EditRevision = Package->GetEditRevision();
		OutResult = BuildPackageBytes(Package, Data.Bytes, &Data.File);
		if (!OutResult) return {};
		OutResult = ValidateAssetPackageBytes(Data.Bytes, Data.Path, Data.File.BulkBytes);
		if (!OutResult) return {};
		Data.Participants.emplace(Data.Path, FindAssetExact(Data.Path).Data);
		for (const auto& Path : Data.File.Dependencies)
			Data.Participants.emplace(Path, FindAssetExact(Path).Data);
		for (const auto& Path : Data.File.SoftDependencies)
			Data.Participants.emplace(Path, FindAssetExact(Path).Data);
		const auto Suffix = ".async-save-" + FGuid::NewGuid().ToString();
		Data.Staged = Data.Destination; Data.Staged += Suffix;
		Data.StagedBulk = Data.Companion; Data.StagedBulk += Suffix;
		// Only detached bytes and paths are touched by the I/O worker. The owner
		// drains this task before releasing its state or package pin.
		Data.Worker = Tasks::LaunchTask(Data.Group, Tasks::ETaskExecutor::BlockingIO,
			{.DebugName = "Asset.SaveStaging"}, [&Data]() -> FAssetResult {
				std::error_code Ec;
				std::filesystem::create_directories(Data.Destination.parent_path(), Ec);
				if (Ec) return Error(EAssetError::IoError, Ec.message());
				auto Write = [](const std::filesystem::path& Path, FByteView Bytes) -> FAssetResult {
					std::string Error;
					return StageFileVerified(Path, Bytes, Error) ? FAssetResult{}
						: FAssetResult{EAssetError::IoError, std::move(Error)};
				};
				if (auto Result = Write(Data.Staged, Data.Bytes); !Result) return Result;
				if (!Data.File.BulkBytes.empty())
					if (auto Result = Write(Data.StagedBulk, Data.File.BulkBytes); !Result) return Result;
				if (!FState::Inspect(Data.Staged, Data.StagedStamp)
					|| !FState::Inspect(Data.StagedBulk, Data.StagedBulkStamp))
					return Error(EAssetError::IoError, "Cannot inspect staged save files.");
				return {};
			});
		return Operation;
	}

	auto FAsyncPackageSave::IsReady() const -> bool { return State->Worker.IsCompleted(); }
	auto FAsyncPackageSave::Complete(FAssetBundleSaveOptions Options) -> FAssetResult
	{
		check(IsInGameThread());
		auto& Data = *State;
		if (Data.Result) return *Data.Result;
		if (Data.bCommitting) return Error(EAssetError::StaleData, "Save publication is already running.");
		if (Options.PreparedSave || Options.Mode != EAssetPackageSaveMode::Delta || Options.PreparedPublication)
			return Error(EAssetError::StaleData, "Prepared save policy does not match its snapshot.");
		if (!IsReady()) return Error(EAssetError::StaleData, "Save staging is still running.");
		if (Data.Worker.GetCompletion().GetState() != ETaskState::Succeeded)
			return *(Data.Result = Error(EAssetError::IoError, "Save staging task failed or was canceled."));
		if (const auto Result = Data.Worker.GetResult(); !Result) return *(Data.Result = Result);
		FState::FStamp CurrentPackage, CurrentBulk, CurrentStaged, CurrentStagedBulk;
		const bool ParticipantsChanged = std::ranges::any_of(Data.Participants, [](const auto& Entry) {
			return FindAssetExact(Entry.first).Data != Entry.second;
		});
		if (FindResidentPackage(Data.Path) != Data.Package.Get()
			|| Data.Package->GetPackagePath() != Data.Path.GetView()
			|| Data.Package->GetEditRevision() != Data.EditRevision
			|| ParticipantsChanged
			|| GetPhysicalPath(Data.Path) != Data.Destination.generic_string()
			|| !FState::Inspect(Data.Destination, CurrentPackage) || CurrentPackage != Data.PackageStamp
			|| !FState::Inspect(Data.Companion, CurrentBulk) || CurrentBulk != Data.BulkStamp)
			return *(Data.Result = Error(EAssetError::StaleData,
				"Asset or destination changed while saving; retry the current version."));
		if (!FState::Inspect(Data.Staged, CurrentStaged) || CurrentStaged != Data.StagedStamp
			|| !FState::Inspect(Data.StagedBulk, CurrentStagedBulk) || CurrentStagedBulk != Data.StagedBulkStamp)
			return *(Data.Result = Error(EAssetError::StaleData, "Staged save files changed before publication."));
		DPackage* Package = Data.Package.Get();
		Data.bCommitting = true;
		Options.PreparedSave = this;
		Data.Result = SavePackagesAtomically(std::span(&Package, 1), Options);
		Data.bCommitting = false;
		return *Data.Result;
	}

	auto SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options) -> FAssetResult
	{
		if (auto Result = AssetPrivate::FAssetLiveLoadGuard::Check("save", ""); !Result) return Result;
		return FAssetRuntimeState::Get().GetMutationCoordinator()
			.SavePackagesAtomically(Packages, Options);
	}

	auto FAssetMutationCoordinator::SavePackagesAtomically(
		std::span<DPackage* const> Packages,
		const FAssetBundleSaveOptions& Options) -> FAssetResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		auto* Prepared = Options.PreparedSave ? Options.PreparedSave->State.get() : nullptr;
		if (Prepared && (!Prepared->bCommitting || Packages.size() != 1
			|| Packages.front() != Prepared->Package.Get()))
			return Error(EAssetError::StaleData, "Invalid prepared save handoff.");
		struct FStagedPackage
		{
			DPackage* Package = nullptr;
			FPackagePath Path;
			FPackageFile File;
			FByteBuffer Bytes;
			std::filesystem::path Destination;
			std::filesystem::path Staged;
			std::filesystem::path Backup;
			std::filesystem::path PublishedCompanion;
			FEditorBulkDataCompanionTransaction CompanionTransaction;
			uintmax_t PublishedFileSize = 0;
			std::filesystem::file_time_type PublishedLastWriteTime{};
			bool bHadDestination = false;
			FFileReplacement Replacement;
		};

		if (Packages.empty())
			return Error(EAssetError::InvalidPackageType, "An asset bundle must contain at least one package.");
		if (Options.PreparedPublication && !Options.bRollbackOnRegistryFailure)
			return Error(EAssetError::InvalidPackageType, "Prepared publication requires rollback on Registry failure.");
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit bundle saves.");
		if (Options.RootPackage
			&& std::ranges::find(Packages, Options.RootPackage) == Packages.end())
			return Error(EAssetError::InvalidPackageType, "The root package is not part of the asset bundle.");
		const FAssetRegistryPublication InitialProjection =
			CaptureAssetRegistryPublication();
		if (!InitialProjection.bReferenceIndexComplete
			|| !InitialProjection.ReferenceErrors.empty())
			return Error(EAssetError::StaleData,
				"Asset bundle save requires a complete initial Registry projection.");

		std::vector<FStagedPackage> StagedPackages;
		StagedPackages.reserve(Packages.size());
		std::unordered_set<FPackagePath> Paths;
		for (DPackage* Package : Packages)
		{
			FPackagePath Path;
			const bool bOwnedPrepared = Package && Options.PreparedPublication &&
				Options.PreparedPublication->OwnsPreparedPackage(*Package);
			if (!Package || !Package->IsAssetPackage() || (Package->IsGraphPrivate() && !bOwnedPrepared)
				|| !FPackagePath::TryCreate(Package->GetPackagePath(), Path))
				return Error(EAssetError::InvalidPackageType, "The asset bundle contains an invalid package.");
			if (!Paths.insert(Path).second)
				return Error(EAssetError::AlreadyExists, std::format(
					"The asset bundle contains duplicate package {}.", Path.ToString()));
			FAssetResult Result = ValidatePackageWriteAdmission(Path);
			if (!Result) return Result;
			Result = ValidateSaveVersion(Registry, Path);
			if (!Result) return Result;
			FStagedPackage& Staged = StagedPackages.emplace_back();
			Staged.Package = Package;
			Staged.Path = Path;
			FAssetPackageSerializationOptions Serialization;
			Serialization.Mode = Options.Mode;
			if (Prepared)
			{
				Staged.Bytes = std::move(Prepared->Bytes);
				Staged.File = std::move(Prepared->File);
			}
			else
			{
				Result = BuildPackageBytes(Package, Staged.Bytes, &Staged.File, Serialization);
				if (!Result) return Result;
				Result = ValidateAssetPackageBytes(Staged.Bytes, Path, Staged.File.BulkBytes);
				if (!Result) return Result;
			}
			Staged.Destination = GetPhysicalPath(Path);
			if (Staged.Destination.empty())
				return Error(EAssetError::InvalidPath, std::format(
					"Failed to resolve package {}.", Path.ToString()));
			Staged.Staged = Staged.Destination;
			Staged.Staged += ".bundle-stage";
			if (Prepared) Staged.Staged = Prepared->Staged;
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
			if ((!Prepared && std::filesystem::exists(Staged.Staged, Ec))
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
		auto RollbackCompanions = [&](FAssetResult Failure) {
			std::string RestoreError;
			for (auto It = StagedPackages.rbegin(); It != StagedPackages.rend(); ++It)
				if (!RollbackEditorBulkDataCompanion(It->CompanionTransaction, RestoreError))
				{
					Failure.Disposition = EAssetResultDisposition::RecoveryRequired;
					Failure.Message += "; companion rollback: " + RestoreError;
					Failure.RecoveryLocation = It->CompanionTransaction.BackupPath;
				}
			return Failure;
		};
		auto AbortStaging = [&](FAssetResult Failure) {
			CleanupStaging();
			return RollbackCompanions(std::move(Failure));
		};
		auto RollbackPublication = [&](FAssetResult Failure) {
			for (auto It = StagedPackages.rbegin(); It != StagedPackages.rend(); ++It)
			{
				std::error_code Ec;
				std::string RestoreError;
				if (!It->Replacement.Rollback(RestoreError))
				{
					Failure.Disposition = EAssetResultDisposition::RecoveryRequired;
					Failure.Message += "; package rollback: " + RestoreError;
					Failure.RecoveryLocation = It->Replacement.Backup;
				}
				std::filesystem::remove(It->Staged, Ec);
			}
			return RollbackCompanions(std::move(Failure));
		};

		for (size_t Index = 0; Index < StagedPackages.size(); ++Index)
		{
			FStagedPackage& Staged = StagedPackages[Index];
			if (Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::CreateDirectories, Index))
			{
				return AbortStaging(Error(EAssetError::IoError, "Injected asset-bundle directory creation failure."));
			}
			std::error_code Ec;
			std::filesystem::create_directories(Staged.Destination.parent_path(), Ec);
			if (Ec)
			{
				return AbortStaging(Error(EAssetError::IoError, std::format(
					"Failed to create package directory {}: {}",
					Staged.Destination.parent_path().generic_string(), Ec.message())));
			}
			if (!Staged.File.BulkBytes.empty())
			{
				if (Options.ShouldFail
					&& Options.ShouldFail(EAssetBundleSavePhase::PublishCompanion, Index))
				{
					return AbortStaging(Error(EAssetError::IoError,
						"Injected asset-bundle companion publication failure."));
				}
				const FByteBuffer& CompanionBytes = Staged.File.BulkBytes;
				FPackageBulkSegmentSummary SegmentSummary;
				std::string CompanionError;
				SegmentSummary = {Staged.File.BulkSegmentExtent, Staged.File.BulkSegmentDigest};
				Staged.PublishedCompanion = Staged.Destination;
				Staged.PublishedCompanion.replace_extension(".dbulk");
				if (!PublishEditorBulkDataCompanion(
						Staged.Destination, SegmentSummary.Digest, SegmentSummary.Extent, CompanionBytes,
						Staged.CompanionTransaction, CompanionError,
						Prepared ? Prepared->StagedBulk : std::filesystem::path{}))
				{
					return AbortStaging(Error(EAssetError::IoError, std::move(CompanionError)));
				}
			}
			else
			{
				std::string CompanionError;
				if (!PrepareEditorBulkDataCompanionState(
						Staged.Destination, CompanionError))
				{
					return AbortStaging(Error(EAssetError::IoError, std::move(CompanionError)));
				}
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::StagePackage, Index))
			{
				return AbortStaging(Error(EAssetError::IoError, "Injected asset-bundle package staging failure."));
			}
			std::string PublicationError;
			if (!Prepared && !StageFileVerified(Staged.Staged, Staged.Bytes, PublicationError))
			{
				return AbortStaging(Error(EAssetError::IoError, std::move(PublicationError)));
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
				return RollbackPublication(Error(EAssetError::IoError, "Injected asset-bundle package publication failure."));
			}
			Staged.Replacement = {Staged.Destination, Staged.Staged, Staged.Backup};
			std::string PublicationError;
			if (!Staged.Replacement.Publish(PublicationError))
			{
				return RollbackPublication(Error(EAssetError::IoError, std::move(PublicationError)));
			}
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
				return RollbackPublication(Error(EAssetError::IoError, std::format(
					"Failed to inspect published package {}: {}",
					Staged.Path.ToString(), Ec.message())));
			}
		}
		for (FStagedPackage& Staged : StagedPackages)
		{
			std::string CompanionError;
			if (!VerifyEditorBulkDataCompanion(
					Staged.CompanionTransaction, CompanionError))
			{
				return RollbackPublication(Error(EAssetError::CorruptFile,
					CompanionError.empty()
						? "Published authored bulk companion failed verification."
						: std::move(CompanionError)));
			}
		}
		const bool bInjectRegistryFailure = Options.ShouldFail
			&& Options.ShouldFail(
				EAssetBundleSavePhase::PublishRegistry, StagedPackages.size());

		std::vector<FAssetData> PublishedMetadata;
		PublishedMetadata.reserve(StagedPackages.size());
		for (const FStagedPackage& Staged : StagedPackages)
		{
			PublishedMetadata.push_back(FAssetData{
				.PackagePath = Staged.Path,
				.PhysicalPath = Staged.Destination.generic_string(),
				.TopLevelAssets = Staged.File.TopLevelAssets,
				.AssetClassName = Staged.File.AssetClassName,
				.EntryKind = Staged.File.EntryKind,
				.RedirectDestination = Staged.File.RedirectDestination,
				.FormatVersion = Staged.File.FormatVersion,
				.Dependencies = Staged.File.Dependencies,
				.SoftDependencies = Staged.File.SoftDependencies,
				.SearchableNames = Staged.File.SearchableNames,
				.ObjectCount = Staged.File.ObjectCount,
				.BulkSegmentExtent = Staged.File.BulkSegmentExtent,
				.BulkSegmentDigest = Staged.File.BulkSegmentDigest,
				.FileSize = Staged.PublishedFileSize,
				.LastWriteTime = Staged.PublishedLastWriteTime,
				.LastWriteTimeTicks = FileTime::ToStableTicks(
					Staged.PublishedLastWriteTime)});
		}
		FAssetRegistryDelta Delta{
			.ExpectedRevision = InitialProjection.ExpectedRevision};
		for (FAssetData& Data : PublishedMetadata)
		{
			const FPackagePath Path = Data.PackagePath;
			if (InitialProjection.Assets.contains(Path))
				Delta.Replaces.push_back(std::move(Data));
			else
				Delta.Adds.push_back(std::move(Data));
			Delta.ReferenceInvalidations.push_back(Path);
		}
		FAssetResult RegistryResult = bInjectRegistryFailure
			? Error(EAssetError::StaleData,
				"Injected asset-bundle Registry publication failure.")
			: Registry.PublishDelta(std::move(Delta));
		if (!RegistryResult && Options.bRollbackOnRegistryFailure)
		{
			return RollbackPublication(RegistryResult);
		}

		for (FStagedPackage& Staged : StagedPackages)
		{
			if (FindResidentPackage(Staged.Path) == Staged.Package)
				Staged.Package->MarkAsPublished();
			if (!Prepared || Staged.Package->GetEditRevision() == Prepared->EditRevision)
				Staged.Package->ClearDirty();
			std::error_code Ec;
			std::string FinalizeError;
			(void)Staged.Replacement.Finalize(FinalizeError);
			CommitEditorBulkDataCompanion(Staged.CompanionTransaction);
			CleanupStaleEditorBulkDataCompanions(
				Staged.Destination, Staged.PublishedCompanion);
		}
		if (!RegistryResult)
		{
			std::vector<FPackagePath> FencedPaths;
			FencedPaths.reserve(StagedPackages.size());
			for (const FStagedPackage& Staged : StagedPackages)
				FencedPaths.push_back(Staged.Path);
			FenceAssetRegistryProjection(FencedPaths);
			return {
				.Error = EAssetError::StaleData,
				.Message = std::format(
					"ContentCommittedProjectionPending: authored package closure committed; Registry reconcile is required. {}",
					RegistryResult.Message),
				.Disposition = EAssetResultDisposition::ContentCommittedProjectionPending};
		}
		return {};
	}

	auto AdmitAssetPackageToCatalog(const FPackagePath& Path) -> FAssetResult
	{
		return FAssetRuntimeState::Get().GetMutationCoordinator().AdmitAssetPackageToCatalog(Path);
	}

	auto FAssetMutationCoordinator::AdmitAssetPackageToCatalog(
		const FPackagePath& Path) -> FAssetResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		if (!Path.IsValid())
			return Error(EAssetError::InvalidPath, "The asset admission path is invalid.");
		if (Durin::FindAssetExact(Path) || FindResidentPackage(Path))
			return Error(EAssetError::AlreadyExists,
				"The asset admission path is already occupied.");
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty())
			return Error(EAssetError::InvalidPath,
				"The asset admission path is outside mounted package content.");
		FAssetPackageHeader Header;
		if (FAssetRegistryResult Result = ReadAssetPackageHeader(
			PhysicalPath, Path, Header); !Result)
			return AssetPrivate::ToAssetResult(std::move(Result));
		FByteBuffer Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError,
				"The asset package could not be read for admission validation.");
		FByteBuffer BulkBytes;
		if (FAssetResult Result = LoadPackageBulkBytes(PhysicalPath, BulkBytes); !Result)
			return Result;
		if (FAssetResult Result = ValidateAssetPackageBytes(Bytes, Path, BulkBytes); !Result)
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
		return Registry.PublishAssetMetadata(FAssetData{
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
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)});
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
				&& FObjectPath::TryCreate(PathString, OutValue.ExternalPath);
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

	auto FAssetPackageField::TryReadBulkDataStorageDescriptor(
		FEditorBulkDataStorageDescriptor& OutValue, bool bValidateInlinePayload) const -> bool
	{
		OutValue = {};
		if (Kind != DurinCodeGen::EPropertyGenFlags::BulkData
			|| !ObjectPackage::IsSupportedPackageReaderVersion(SourceFormatVersion)) return false;
		FByteReader Reader{Payload};
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
			? EEditorBulkDataStorageKind::Inline
			: EEditorBulkDataStorageKind::External;
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
		FEditorBulkDataStorageDescriptor& OutValue) const -> bool
	{
		return TryReadBulkDataStorageDescriptor(OutValue);
	}

	namespace
	{
		auto ReadInspectedStructFields(FByteReader& Reader, uint32 SourceFormatVersion,
			std::vector<FAssetPackageField>& OutFields) -> bool
		{
			std::string StructName;
			uint64 FieldCount = 0;
			if (!Reader.ReadString(StructName, MaximumPackageStringBytes)
				|| !Reader.Read(FieldCount) || FieldCount > 100000) return false;
			OutFields.reserve(static_cast<size_t>(FieldCount));
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				FAssetPackageField Field;
				uint8 FieldKind = 0;
				uint64 PayloadSize = 0;
				if (!Reader.ReadString(Field.DeclaringClass, MaximumPackageStringBytes)
					|| !Reader.ReadString(Field.Name, MaximumPackageStringBytes)
					|| !Reader.Read(FieldKind)
					|| !Reader.ReadString(Field.TypeSignature, MaximumPackageStringBytes)
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
		FByteReader Reader{Payload};
		return ReadInspectedStructFields(Reader, SourceFormatVersion, OutFields)
			&& Reader.Offset == Payload.size();
	}

	auto FAssetPackageField::TryInspectStructArray(
		std::vector<std::vector<FAssetPackageField>>& OutElements) const -> bool
	{
		OutElements.clear();
		if (Kind != DurinCodeGen::EPropertyGenFlags::Array
			|| !TypeSignature.starts_with("Array<Struct<")) return false;
		FByteReader Reader{Payload};
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
			|| TypeSignature != std::format("Struct<{}>", Struct->GetQualifiedName().ToString()))
			return false;

		FStructProperty RootProperty(
			FFieldVariant(), FName("InspectedStructValue"), EObjectFlags::NoFlags,
			EPropertyFlags::None, 1, 0, Struct);
		FByteReader Reader{Payload};
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
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			OutInspection = {};
			FAssetResult Result = MakePackageFingerprint(PhysicalPath, Bytes, OutInspection.Fingerprint);
			if (!Result) return Result;
			const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
			if (Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			if (!PackagePath.IsValid())
				return Error(EAssetError::InvalidPath,
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

	auto InspectAssetPackage(std::string_view PhysicalPath, FAssetPackageInspection& OutInspection) -> FAssetResult
	{
		FPackagePath PackagePath;
		if (!ClassifyPackageIdentity(PhysicalPath, PackagePath))
			return Error(EAssetError::InvalidPath,
				"DAST v10 inspection requires a mounted package identity.");
		return InspectAssetPackage(PhysicalPath, PackagePath, OutInspection);
	}

	auto InspectAssetPackage(std::string_view PhysicalPath,
		const FPackagePath& PackagePath,
		FAssetPackageInspection& OutInspection) -> FAssetResult
	{
		OutInspection = {};
		FByteBuffer Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError, std::format("Failed to open asset package {}.", PhysicalPath));
		return InspectAssetPackageBytes(PhysicalPath, Bytes, PackagePath, OutInspection);
	}


	auto CanonicalizeAssetPackageForCook(
		FByteView Bytes,
		FByteView BulkBytes,
		const FPackagePath& PackagePath,
		FByteBuffer& OutBytes,
		FByteBuffer& OutBulkBytes) -> FAssetResult
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
		FByteBuffer& OutBulkBytes) -> FAssetResult
	{
		OutBytes.clear();
		OutBulkBytes.clear();
		const AssetPrivate::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = AssetPrivate::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		if (!Codec->bCanMutate)
			return Error(EAssetError::UnsupportedVersion,
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
			FAssetResult RelocateResult = Codec->Relocate(
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
		FAssetResult Result = Codec->ReadHeader(Context, Header);
		if (!Result) return Result;
		if (Header.EntryKind != EAssetRegistryEntryKind::Asset)
			return Error(EAssetError::InvalidPackageType,
				"CookCanonicalizationRedirectorPackage: redirector packages are uncooked-only.");
		std::vector<FAssetReferenceEdge> References;
		Result = Codec->ExtractReferences(Context, References);
		if (!Result) return Result;
		const FAssetPublicationCoordinator& Registry = GetAssetPublicationCoordinator();
		std::vector<FAssetRedirectorFixupMapping> Mappings;
		auto ResolveReference = [&](const FPackagePath& Path,
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
			const FAssetPathResolveResult Resolution = Durin::ResolveAssetPathForOperation(
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
		Result = AssetPrivate::RewritePackageReferencesForMutation(
			Bytes, BulkBytes, OutputPackagePath, Mappings,
			std::numeric_limits<uint64>::max(), OutBytes);
		if (Result) OutBulkBytes.assign(BulkBytes.begin(), BulkBytes.end());
		return Result;

	}

	auto FAssetMutationCoordinator::SavePackage(DPackage* Package, EAssetPackageSaveMode Mode) -> FAssetResult
	{
		if (auto Guard = AssetPrivate::FAssetLiveLoadGuard::Check("mutation", ""); !Guard) return Guard;
		const std::array<DPackage*, 1> Packages{Package};
		return SavePackagesAtomically(Packages, {.RootPackage = Package, .Mode = Mode});
	}

}
