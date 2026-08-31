#include "AssetRuntimeStateInternal.h"
#include "AssetDeletionInternal.h"
#include "AssetMutationJournalInternal.h"
#include "AssetMutationReferenceInternal.h"
#include "AssetRelocationExtensionsInternal.h"
#include "Asset/PackageSerialization.h"
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
#include "Misc/FileTime.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Threading/RunnableThread.h"

namespace Durin::Asset
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

		auto FailSaveOverride(std::string_view Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = Message;
			return false;
		}

		auto ObjectOwnsProperty(const DObject& Object, const FProperty& Property) -> bool
		{
			if (!Object.GetClass()) return false;
			bool bFound = false;
			Object.GetClass()->ForEachProperty([&](FProperty* Candidate) {
				bFound = bFound || Candidate == &Property;
			}, true);
			return bFound;
		}
	}

	auto FObjectSaveOverrides::FindMutableObject(const DObject& Object) -> FObjectSaveOverride*
	{
		auto It = std::ranges::find(Objects, &Object, &FObjectSaveOverride::Object);
		return It == Objects.end() ? nullptr : &*It;
	}

	auto FObjectSaveOverrides::FindObject(const DObject& Object) const -> const FObjectSaveOverride*
	{
		auto It = std::ranges::find(Objects, &Object, &FObjectSaveOverride::Object);
		return It == Objects.end() ? nullptr : &*It;
	}

	auto FObjectSaveOverrides::AddObjectOmission(
		const DObject& Object, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (FObjectSaveOverride* Existing = FindMutableObject(Object))
		{
			if (Existing->bOmitObject || !Existing->Properties.empty())
				return FailSaveOverride("A conflicting save override already exists for the object.", OutError);
			Existing->bOmitObject = true;
			return true;
		}
		Objects.push_back({.Object = &Object, .bOmitObject = true});
		return true;
	}

	auto FObjectSaveOverrides::AddPropertyOmission(
		const DObject& Object, const FProperty& Property, std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!ObjectOwnsProperty(Object, Property))
			return FailSaveOverride("The omitted property does not belong to the target object's reflected schema.", OutError);
		FObjectSaveOverride* ObjectOverride = FindMutableObject(Object);
		if (!ObjectOverride)
		{
			Objects.push_back({.Object = &Object});
			ObjectOverride = &Objects.back();
		}
		if (ObjectOverride->bOmitObject
			|| std::ranges::find(ObjectOverride->Properties, &Property,
				&FPropertySaveOverride::Property) != ObjectOverride->Properties.end())
			return FailSaveOverride("A conflicting save override already exists for the property.", OutError);
		ObjectOverride->Properties.push_back({.Property = &Property});
		return true;
	}

	auto FObjectSaveOverrides::AddPropertyValueRaw(
		const DObject& Object, const FProperty& Property, const void* Replacement,
		size_t ReplacementSize, size_t ReplacementAlignment,
		DurinCodeGen::EPropertyGenFlags ReplacementKind,
		const DStruct* ReplacementStruct, const DClass* ReplacementClass,
		std::string* OutError) -> bool
	{
		if (OutError) OutError->clear();
		if (!ObjectOwnsProperty(Object, Property))
			return FailSaveOverride("The replacement property does not belong to the target object's reflected schema.", OutError);
		if (!Replacement || Property.GetArrayDim() != 1
			|| Property.GetValueSize() != ReplacementSize
			|| Property.GetValueAlignment() != ReplacementAlignment
			|| Property.GetKind() != ReplacementKind)
			return FailSaveOverride("The replacement value does not exactly match the reflected property storage type.", OutError);
		if (ReplacementKind == DurinCodeGen::EPropertyGenFlags::Struct
			&& static_cast<const FStructProperty&>(Property).GetStruct() != ReplacementStruct)
			return FailSaveOverride("The replacement Struct type does not match the reflected property type.", OutError);
		if ((ReplacementKind == DurinCodeGen::EPropertyGenFlags::Object
				|| ReplacementKind == DurinCodeGen::EPropertyGenFlags::SoftObject)
			&& (Property.GetReferencedClass() != ReplacementClass
				|| (ReplacementKind == DurinCodeGen::EPropertyGenFlags::Object
					&& !Property.IsObjectPtrWrapper())))
			return FailSaveOverride("The replacement object wrapper type does not match the reflected property type.", OutError);
		FObjectSaveOverride* ObjectOverride = FindMutableObject(Object);
		if (!ObjectOverride)
		{
			Objects.push_back({.Object = &Object});
			ObjectOverride = &Objects.back();
		}
		if (ObjectOverride->bOmitObject
			|| std::ranges::find(ObjectOverride->Properties, &Property,
				&FPropertySaveOverride::Property) != ObjectOverride->Properties.end())
			return FailSaveOverride("A conflicting save override already exists for the property.", OutError);

		FReflectedValueStorage Storage;
		std::string CaptureError;
		if (!Storage.CopyConstruct(&Property, Replacement, 0, &CaptureError))
			return FailSaveOverride(CaptureError, OutError);
		FPropertyValueSnapshot Snapshot;
		if (!CapturePropertyValue(&Property, Storage.GetContainer(), 0, Snapshot, &CaptureError))
			return FailSaveOverride(CaptureError, OutError);
		ObjectOverride->Properties.push_back({
			.Property = &Property,
			.Kind = EPropertySaveOverrideKind::Replace,
			.Replacement = std::move(Snapshot)});
		return true;
	}

	using Private::FAssetReferenceStoreRegistry;
	using Private::GetAssetReferenceStoreRegistry;
	using Private::EAssetMutationState;
	using Private::FAssetMutationJournal;
	using Private::FAssetMutationJournalEntry;
	using Private::FingerprintRelocationFile;
	using Private::LoadRelocationBytes;
	using Private::MakePackageFingerprint;
	using Private::NormalizePhysicalPath;
	using Private::PublishRelocationFile;
	using Private::RebuildReferenceProjectionForPublishedEntries;
	using Private::SaveRelocationBytes;
	using Private::WriteMutationJournalState;
	using Private::AssetReferenceLess;

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			std::span<const std::byte> Bytes,
			const FPackagePath& PackagePath,
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
			FByteArray BulkBytes;
		};

		auto Error(EAssetError Code, std::string Message) -> FAssetResult { return {Code, std::move(Message)}; }

		auto LoadPackageBulkBytes(std::string_view PhysicalPath,
			FByteArray& OutBytes) -> FAssetResult
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

		auto ValidateAssetPackageClosure(std::span<const std::byte> Bytes,
			std::span<const std::byte> BulkBytes, const FPackagePath& PackagePath)
			-> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
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
			std::span<const std::byte> Bytes,
			FEditorBulkDataCompanionTransaction& OutTransaction,
			std::string& OutError) -> bool
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

			std::error_code ErrorCode;
			OutTransaction.bHadFinal =
				std::filesystem::is_regular_file(OutTransaction.FinalPath, ErrorCode);
			if (ErrorCode && !IsMissingPathError(ErrorCode))
			{
				OutError = std::format("Failed to inspect authored bulk companion {}: {}",
					OutTransaction.FinalPath.generic_string(), ErrorCode.message());
				return false;
			}
			if (OutTransaction.bHadFinal)
			{
				FByteArray PriorBytes;
				if (!FFileHelper::LoadFileToArray(PriorBytes, OutTransaction.FinalPath))
				{
					OutError = "Prior authored bulk companion is unreadable.";
					return false;
				}
				FFileHelper::FAtomicFileError BackupError;
				if (!FFileHelper::SaveArrayToFileAtomically(
						PriorBytes, OutTransaction.BackupPath, &BackupError))
				{
					OutError = BackupError.ToString();
					return false;
				}
			}
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
					Bytes, OutTransaction.FinalPath, &PublicationError))
			{
				std::filesystem::remove(OutTransaction.BackupPath, ErrorCode);
				OutError = PublicationError.ToString();
				return false;
			}
			OutTransaction.bPublished = true;
			OutError.clear();
			return true;
		}

		auto RollbackEditorBulkDataCompanion(
			FEditorBulkDataCompanionTransaction& Transaction,
			std::string& OutError) -> bool
		{
			if (!Transaction.bPublished) return true;
			std::error_code ErrorCode;
			if (!Transaction.bHadFinal)
			{
				std::filesystem::remove(Transaction.FinalPath, ErrorCode);
				if (!ErrorCode) std::filesystem::remove(Transaction.BackupPath, ErrorCode);
			}
			else
			{
				FByteArray PriorBytes;
				if (!FFileHelper::LoadFileToArray(PriorBytes, Transaction.BackupPath))
				{
					OutError = "Authored bulk rollback backup is missing or unreadable.";
					return false;
				}
				FFileHelper::FAtomicFileError PublicationError;
				if (!FFileHelper::SaveArrayToFileAtomically(
						PriorBytes, Transaction.FinalPath, &PublicationError))
				{
					OutError = PublicationError.ToString();
					return false;
				}
				std::filesystem::remove(Transaction.BackupPath, ErrorCode);
			}
			if (ErrorCode)
			{
				OutError = std::format("Authored bulk rollback cleanup failed: {}",
					ErrorCode.message());
				return false;
			}
			Transaction.bPublished = false;
			OutError.clear();
			return true;
		}

		auto VerifyEditorBulkDataCompanion(
			const FEditorBulkDataCompanionTransaction& Transaction,
			std::string& OutError) -> bool
		{
			if (!Transaction.bPublished) return true;
			FByteArray Bytes;
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
			std::error_code ErrorCode;
			std::filesystem::remove(Transaction.BackupPath, ErrorCode);
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
			uint32 SourceVersion = AssetPackageV9FormatVersion) -> FAssetResult
		{
			const DurinCodeGen::EPropertyGenFlags Kind = Property->GetKind();
			if (Private::IsByteToolRawScalarKind(Kind))
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
					std::span<const std::byte> Payload;
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
				FObjectPath Path;
				if (!Reader.ReadString(PathString) || !FObjectPath::TryCreate(PathString, Path))
					return Error(EAssetError::InvalidPath, "Invalid external object reference.");
				return FAssetRuntimeState::Get().GetLoadService().LoadObject(
					Path, nullptr, OutObject);
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
			FByteArray& OutBytes,
			FPackageFile* OutFile = nullptr,
			const FAssetPackageSerializationOptions& Options = {}) -> FAssetResult
		{
			const Private::FAssetPackageCodec* Codec =
				Private::FindAssetPackageWriter(OrdinaryAssetPackageWriterVersion);
			if (!Codec)
				return Error(EAssetError::UnsupportedVersion,
					"The ordinary asset package writer is unavailable.");
			FAssetPackageSerializationOptions EffectiveOptions = Options;
			std::vector<FEditorBulkDataStoragePayload> BulkPayloads;
			if (OutFile) EffectiveOptions.EditorBulkDataStoragePayloads = &BulkPayloads;
			Private::FAssetPackageEncodedClosure Closure;
			FAssetResult Result = Codec->Write(
				Package, Closure, EDefaultDeltaMode::NoDelta, EffectiveOptions);
			if (!Result) return Result;
			FPackagePath PackagePath;
			if (!Package || !FPackagePath::TryCreate(Package->GetPackagePath(), PackagePath))
				return Error(EAssetError::InvalidPath, "Package path is invalid.");
			if (OutFile)
			{
				OutFile->BulkBytes = Closure.BulkBytes;
				FAssetPackageHeader Header;
				const Private::FAssetPackageReadContext Context{
					.PackageBytes = Closure.PackageBytes,
					.BulkBytes = Closure.BulkBytes,
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
			const FAssetCatalogEntry Existing = Durin::Asset::FindAssetExact(Path);
			if (!Existing || IsSupportedAssetPackageReaderVersion(Existing->FormatVersion))
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
			const FPackagePath* SourcePath) -> FAssetResult
		{
			const FPackageFile File{
				.FormatVersion = Metadata.FormatVersion,
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

	auto ValidateAssetPackageBytes(std::span<const std::byte> Bytes,
		const FPackagePath& PackagePath, std::span<const std::byte> BulkBytes) -> FAssetResult
	{
		return ValidateAssetPackageClosure(Bytes, BulkBytes, PackagePath);
	}

	auto SerializeAssetPackageBytes(
		DPackage* Package,
		FByteArray& OutBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetResult
	{
		return BuildPackageBytes(Package, OutBytes, nullptr, Options);
	}

	auto SerializeAssetPackageClosure(
		DPackage* Package,
		FByteArray& OutBytes,
		FByteArray& OutBulkBytes,
		const FAssetPackageSerializationOptions& Options) -> FAssetResult
	{
		FPackageFile File;
		FAssetResult Result = BuildPackageBytes(Package, OutBytes, &File, Options);
		if (!Result) return Result;
		OutBulkBytes = std::move(File.BulkBytes);
		return {};
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
			FPackagePath Path;
			FPackageFile File;
			FByteArray Bytes;
			std::filesystem::path Destination;
			std::filesystem::path Staged;
			std::filesystem::path Backup;
			std::filesystem::path PublishedCompanion;
			FEditorBulkDataCompanionTransaction CompanionTransaction;
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
		std::unordered_set<FPackagePath> Paths;
		for (DPackage* Package : Packages)
		{
			FPackagePath Path;
			if (!Package || !Package->IsAssetPackage()
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
			Result = BuildPackageBytes(
				Package, Staged.Bytes, &Staged.File, Serialization);
			if (!Result) return Result;
			Result = ValidateAssetPackageBytes(
				Staged.Bytes, Path, Staged.File.BulkBytes);
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
		auto RollbackCompanions = [&] {
			std::string IgnoredError;
			for (auto It = StagedPackages.rbegin(); It != StagedPackages.rend(); ++It)
				RollbackEditorBulkDataCompanion(It->CompanionTransaction, IgnoredError);
		};
		auto AbortStaging = [&] {
			CleanupStaging();
			RollbackCompanions();
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
			RollbackCompanions();
		};

		for (size_t Index = 0; Index < StagedPackages.size(); ++Index)
		{
			FStagedPackage& Staged = StagedPackages[Index];
			if (Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::CreateDirectories, Index))
			{
				AbortStaging();
				return Error(EAssetError::IoError, "Injected asset-bundle directory creation failure.");
			}
			std::error_code Ec;
			std::filesystem::create_directories(Staged.Destination.parent_path(), Ec);
			if (Ec)
			{
				AbortStaging();
				return Error(EAssetError::IoError, std::format(
					"Failed to create package directory {}: {}",
					Staged.Destination.parent_path().generic_string(), Ec.message()));
			}
			if (!Staged.File.BulkBytes.empty())
			{
				if (Options.ShouldFail
					&& Options.ShouldFail(EAssetBundleSavePhase::PublishCompanion, Index))
				{
					AbortStaging();
					return Error(EAssetError::IoError,
						"Injected asset-bundle companion publication failure.");
				}
				FByteArray CompanionBytes;
				FPackageBulkSegmentSummary SegmentSummary;
				std::string CompanionError;
				CompanionBytes = Staged.File.BulkBytes;
				SegmentSummary = {CompanionBytes.size(),
					FXxHash128::HashBuffer(CompanionBytes)};
				Staged.PublishedCompanion = Staged.Destination;
				Staged.PublishedCompanion.replace_extension(".dbulk");
				if (!PublishEditorBulkDataCompanion(
						Staged.Destination, SegmentSummary.Digest, SegmentSummary.Extent, CompanionBytes,
						Staged.CompanionTransaction, CompanionError))
				{
					AbortStaging();
					return Error(EAssetError::IoError, std::move(CompanionError));
				}
			}
			else
			{
				std::string CompanionError;
				if (!PrepareEditorBulkDataCompanionState(
						Staged.Destination, CompanionError))
				{
					AbortStaging();
					return Error(EAssetError::IoError, std::move(CompanionError));
				}
			}
			if (Options.ShouldFail && Options.ShouldFail(EAssetBundleSavePhase::StagePackage, Index))
			{
				AbortStaging();
				return Error(EAssetError::IoError, "Injected asset-bundle package staging failure.");
			}
			FFileHelper::FAtomicFileError PublicationError;
			if (!FFileHelper::SaveArrayToFileAtomically(
				std::span{reinterpret_cast<const std::byte*>(Staged.Bytes.data()), Staged.Bytes.size()},
				Staged.Staged,
				&PublicationError))
			{
				AbortStaging();
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
		for (FStagedPackage& Staged : StagedPackages)
		{
			std::string CompanionError;
			if (!VerifyEditorBulkDataCompanion(
					Staged.CompanionTransaction, CompanionError))
			{
				RollbackPublication();
				return Error(EAssetError::CorruptFile,
					CompanionError.empty()
						? "Published authored bulk companion failed verification."
						: std::move(CompanionError));
			}
		}
		if (Options.ShouldFail
			&& Options.ShouldFail(EAssetBundleSavePhase::PublishRegistry, StagedPackages.size()))
		{
			RollbackPublication();
			return Error(EAssetError::IoError, "Injected asset-bundle registry publication failure.");
		}

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
		if (FAssetResult RegistryResult = Registry.PublishAssetMetadataBatch(
			std::move(PublishedMetadata)); !RegistryResult)
		{
			RollbackPublication();
			return RegistryResult;
		}

		for (FStagedPackage& Staged : StagedPackages)
		{
			if (FindResidentPackage(Staged.Path) == Staged.Package)
				Staged.Package->MarkAsPublished();
			Staged.Package->ClearDirty();
			std::error_code Ec;
			std::filesystem::remove(Staged.Backup, Ec);
			CommitEditorBulkDataCompanion(Staged.CompanionTransaction);
			CleanupStaleEditorBulkDataCompanions(
				Staged.Destination, Staged.PublishedCompanion);
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
		if (!Path.IsValid())
			return Error(EAssetError::InvalidPath, "The asset admission path is invalid.");
		if (Durin::Asset::FindAssetExact(Path) || FindResidentPackage(Path))
			return Error(EAssetError::AlreadyExists,
				"The asset admission path is already occupied.");
		const std::string PhysicalPath = GetPhysicalPath(Path);
		if (PhysicalPath.empty())
			return Error(EAssetError::InvalidPath,
				"The asset admission path is outside mounted package content.");
		FAssetPackageHeader Header;
		if (FAssetResult Result = ReadAssetPackageHeader(PhysicalPath, Path, Header); !Result)
			return Result;
		FByteArray Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError,
				"The asset package could not be read for admission validation.");
		FByteArray BulkBytes;
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
		FEditorBulkDataStorageDescriptor& OutValue) const -> bool
	{
		OutValue = {};
		if (Kind != DurinCodeGen::EPropertyGenFlags::BulkData
			|| SourceFormatVersion != AssetPackageV9FormatVersion) return false;
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
		return FXxHash128::HashBuffer(
			std::span<const std::byte>(Payload).subspan(Reader.Offset))
			== OutValue.ContentHash;
	}

	auto FAssetPackageField::TryReadEditorBulkDataStorageDescriptor(
		FEditorBulkDataStorageDescriptor& OutValue) const -> bool
	{
		return TryReadBulkDataStorageDescriptor(OutValue);
	}

	auto FAssetPackageField::TryInspectStructFields(
		std::vector<FAssetPackageField>& OutFields) const -> bool
	{
		OutFields.clear();
		if (Kind != DurinCodeGen::EPropertyGenFlags::Struct) return false;
		FByteReader Reader{Payload};
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
			SourceFormatVersion == 0 ? AssetPackageV9FormatVersion : SourceFormatVersion)
			&& Reader.Offset == Payload.size();
	}

	namespace
	{
		auto InspectAssetPackageBytes(
			std::string_view PhysicalPath,
			std::span<const std::byte> Bytes,
			const FPackagePath& PackagePath,
			FAssetPackageInspection& OutInspection) -> FAssetResult
		{
			OutInspection = {};
			FAssetResult Result = MakePackageFingerprint(PhysicalPath, Bytes, OutInspection.Fingerprint);
			if (!Result) return Result;
			const Private::FAssetPackageCodec* Codec = nullptr;
			if (Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
				return Result;
			if (!PackagePath.IsValid())
				return Error(EAssetError::InvalidPath,
					"DAST v9 inspection requires a mounted package identity.");
			FByteArray BulkBytes;
			if (Result = LoadPackageBulkBytes(PhysicalPath, BulkBytes); !Result)
				return Result;
			const Private::FAssetPackageReadContext Context{
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
				"DAST v9 inspection requires a mounted package identity.");
		return InspectAssetPackage(PhysicalPath, PackagePath, OutInspection);
	}

	auto InspectAssetPackage(std::string_view PhysicalPath,
		const FPackagePath& PackagePath,
		FAssetPackageInspection& OutInspection) -> FAssetResult
	{
		OutInspection = {};
		FByteArray Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, PhysicalPath))
			return Error(EAssetError::IoError, std::format("Failed to open asset package {}.", PhysicalPath));
		return InspectAssetPackageBytes(PhysicalPath, Bytes, PackagePath, OutInspection);
	}


	auto CanonicalizeAssetPackageForCook(
		std::span<const std::byte> Bytes,
		std::span<const std::byte> BulkBytes,
		const FPackagePath& PackagePath,
		FByteArray& OutBytes,
		FByteArray& OutBulkBytes) -> FAssetResult
	{
		return CanonicalizeAssetPackageForCook(
			Bytes, BulkBytes, PackagePath, PackagePath, OutBytes, OutBulkBytes);
	}

	auto CanonicalizeAssetPackageForCook(
		std::span<const std::byte> Bytes,
		std::span<const std::byte> BulkBytes,
		const FPackagePath& SourcePackagePath,
		const FPackagePath& OutputPackagePath,
		FByteArray& OutBytes,
		FByteArray& OutBulkBytes) -> FAssetResult
	{
		OutBytes.clear();
		OutBulkBytes.clear();
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		if (!Codec->bCanMutate)
			return Error(EAssetError::UnsupportedVersion,
				"Cook canonicalization requires package mutation capability.");
		FByteArray RelocatedBytes;
		FByteArray RelocatedBulkBytes;
		if (SourcePackagePath != OutputPackagePath)
		{
			const Private::FAssetPackageReadContext SourceContext{
				.PackageBytes = Bytes, .BulkBytes = BulkBytes,
				.PackagePath = SourcePackagePath,
				.PhysicalPackageBytes = Bytes.size()};
			Private::FAssetPackageEncodedClosure Relocated;
			FAssetResult RelocateResult = Codec->Relocate(
				SourceContext, OutputPackagePath, Relocated);
			if (!RelocateResult) return RelocateResult;
			RelocatedBytes = std::move(Relocated.PackageBytes);
			RelocatedBulkBytes = std::move(Relocated.BulkBytes);
			Bytes = RelocatedBytes;
			BulkBytes = RelocatedBulkBytes;
		}
		const Private::FAssetPackageReadContext Context{
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
			const FAssetPathResolveResult Resolution = Durin::Asset::ResolveAssetPath(
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
		Result = Private::RewritePackageReferencesForMutation(
			Bytes, BulkBytes, OutputPackagePath, Mappings,
			std::numeric_limits<uint64>::max(), OutBytes);
		if (Result) OutBulkBytes.assign(BulkBytes.begin(), BulkBytes.end());
		return Result;

	}

	auto FAssetMutationCoordinator::SavePackage(DPackage* Package) -> FAssetResult
	{
		if (RuntimeConfiguration.IsCooked())
			return Error(EAssetError::ReadOnlyMode, "Cooked runtime package mode does not permit package saves.");
		if (!Package || !Package->IsAssetPackage())
			return Error(EAssetError::InvalidPackageType,
				"Only asset packages can be saved as asset files.");
		FPackagePath Path;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), Path))
			return Error(EAssetError::InvalidPath, "Package path is invalid.");
		FAssetResult WriteAdmission = ValidatePackageWriteAdmission(Path);
		if (!WriteAdmission) return WriteAdmission;
		FAssetResult VersionResult = ValidateSaveVersion(Registry, Path);
		if (!VersionResult) return VersionResult;
		const std::filesystem::path Destination(GetPhysicalPath(Path));
		FPackageFile File;
		FByteArray Bytes;
		FAssetPackageSerializationOptions Serialization;
		FAssetResult SerializationResult = BuildPackageBytes(
			Package, Bytes, &File, Serialization);
		if (!SerializationResult) return SerializationResult;
		std::string CompanionStateError;
		if (!PrepareEditorBulkDataCompanionState(Destination, CompanionStateError))
			return Error(EAssetError::IoError, std::move(CompanionStateError));
		FByteArray PriorPackageBytes;
		std::error_code PackageErrorCode;
		const bool bHadPriorPackage =
			std::filesystem::is_regular_file(Destination, PackageErrorCode);
		if (PackageErrorCode && !IsMissingPathError(PackageErrorCode))
			return Error(EAssetError::IoError, std::format(
				"Failed to inspect package destination {}: {}",
				Destination.generic_string(), PackageErrorCode.message()));
		if (bHadPriorPackage
			&& !FFileHelper::LoadFileToArray(PriorPackageBytes, Destination))
			return Error(EAssetError::IoError, "Prior package is unreadable.");
		FFileHelper::FAtomicFileError PublicationError;
		std::filesystem::path PublishedCompanion;
		FEditorBulkDataCompanionTransaction CompanionTransaction;
		if (!File.BulkBytes.empty())
		{
			FByteArray CompanionBytes;
			FPackageBulkSegmentSummary SegmentSummary;
			std::string CompanionError;
			CompanionBytes = File.BulkBytes;
			SegmentSummary = {CompanionBytes.size(),
				FXxHash128::HashBuffer(CompanionBytes)};
			PublishedCompanion = Destination;
			PublishedCompanion.replace_extension(".dbulk");
			if (!PublishEditorBulkDataCompanion(
					Destination, SegmentSummary.Digest, SegmentSummary.Extent, CompanionBytes,
					CompanionTransaction, CompanionError))
				return Error(EAssetError::IoError, std::move(CompanionError));
		}
		if (!FFileHelper::SaveArrayToFileAtomically(
			std::span{reinterpret_cast<const std::byte*>(Bytes.data()), Bytes.size()},
			Destination,
			&PublicationError
		))
		{
			std::string RollbackError;
			RollbackEditorBulkDataCompanion(CompanionTransaction, RollbackError);
			return Error(EAssetError::IoError, PublicationError.ToString());
		}
		std::string CompanionVerificationError;
		if (!VerifyEditorBulkDataCompanion(
				CompanionTransaction, CompanionVerificationError))
		{
			if (bHadPriorPackage)
				FFileHelper::SaveArrayToFileAtomically(
					PriorPackageBytes, Destination, nullptr);
			else
				std::filesystem::remove(Destination, PackageErrorCode);
			std::string RollbackError;
			RollbackEditorBulkDataCompanion(CompanionTransaction, RollbackError);
			return Error(EAssetError::CorruptFile,
				CompanionVerificationError.empty()
					? "Published authored bulk companion failed verification."
					: std::move(CompanionVerificationError));
		}
		const auto LastWriteTime = std::filesystem::last_write_time(Destination);
		FAssetResult RegistryResult = Registry.PublishAssetMetadata(FAssetData{
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
			.FileSize = std::filesystem::file_size(Destination),
			.LastWriteTime = LastWriteTime,
			.LastWriteTimeTicks = FileTime::ToStableTicks(LastWriteTime)});
		if (!RegistryResult)
		{
			if (bHadPriorPackage)
				FFileHelper::SaveArrayToFileAtomically(
					PriorPackageBytes, Destination, nullptr);
			else
				std::filesystem::remove(Destination, PackageErrorCode);
			std::string RollbackError;
			RollbackEditorBulkDataCompanion(CompanionTransaction, RollbackError);
			return RegistryResult;
		}
		Package->ClearDirty();
		if (FindResidentPackage(Path) == Package)
			Package->MarkAsPublished();
		CommitEditorBulkDataCompanion(CompanionTransaction);
		CleanupStaleEditorBulkDataCompanions(Destination, PublishedCompanion);
		return {};
	}

	namespace
	{
		auto RebuildReferenceProjectionForPublishedEntriesImpl(
			std::span<const FAssetMutationJournalEntry> Entries,
			const std::unordered_map<FPackagePath, FAssetData>& Assets,
			std::vector<FAssetReferenceEdge>& Edges,
			std::unordered_map<FPackagePath, FAssetPackageFingerprint>& Fingerprints)
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
			const std::unordered_map<FPackagePath, FAssetData>& Assets,
			std::vector<FAssetReferenceEdge>& Edges,
			std::unordered_map<FPackagePath, FAssetPackageFingerprint>& Fingerprints)
			-> FAssetResult
		{
			return RebuildReferenceProjectionForPublishedEntriesImpl(
				Entries, Assets, Edges, Fingerprints);
		}
	}



}
