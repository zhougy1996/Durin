#include "AssetPackageArchive.h"
#include "DObject/PackageCapture.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageLinker.h"
#include "Asset/EditorBulkData.h"
#include "Asset/EditorBulkDataStorage.h"
#include "AssetPackageValueCodec.h"

#include "Asset/Redirector.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"
#include "Misc/Paths.h"

namespace Durin::AssetPrivate
{
	namespace
	{
		auto FindReflectedProperty(const FArchiveFieldDescriptor& Descriptor) -> FProperty*
		{
			if (Descriptor.DeclaringType.IsNone() || Descriptor.Name.IsNone()) return nullptr;
			DStruct* Struct = FindStructByQualifiedName(Descriptor.DeclaringType);
			return Struct ? Struct->FindPropertyByName(Descriptor.Name, false) : nullptr;
		}

		auto FindObjectProperty(DObject* Object, FName Name) -> FProperty*
		{
			if (!Object || !Object->GetClass() || Name.IsNone()) return nullptr;
			FProperty* Result = nullptr;
			Object->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (!Result && Property && Property->NamePrivate == Name) Result = Property;
			}, true);
			return Result;
		}

		auto FindDeclaringStruct(FName QualifiedName) -> DStructBase*
		{
			if (DClass* Class = FindClassByQualifiedName(QualifiedName)) return Class;
			return FindStructByQualifiedName(QualifiedName);
		}

		auto FindDeprecatedRoute(
			DStructBase* Owner,
			std::string_view StoredName,
			DurinCodeGen::EPropertyGenFlags StoredKind,
			std::string_view StoredSignature
		) -> FProperty*
		{
			if (!Owner) return nullptr;
			if (FProperty* Current = Owner->FindPropertyByName(FName(StoredName), false);
				Current && !Current->IsDeprecated() && Current->GetKind() == StoredKind
				&& GetSerializedTypeSignature(Current) == StoredSignature) return nullptr;
			FProperty* Match = nullptr;
			bool bAmbiguous = false;
			Owner->ForEachProperty([&](FProperty* Property) {
				if (bAmbiguous) return;
				const FPropertyDeprecation* Deprecation = Property ? Property->GetDeprecation() : nullptr;
				if (!Deprecation || Deprecation->HistoricalName.ToString() != StoredName
					|| Property->GetKind() != StoredKind
					|| GetSerializedTypeSignature(Property) != StoredSignature) return;
				if (Match)
				{
					bAmbiguous = true;
					return;
				}
				Match = Property;
			}, false);
			return bAmbiguous ? nullptr : Match;
		}

		class FAuthoredLoadArchive final : public FObjectArchive
		{
		public:
			FAuthoredLoadArchive(
				DObject& InObject,
				std::span<const FAuthoredPackageFieldRecord> InFields,
				std::span<DObject* const> InObjects,
				const FPackageLoadBindings& InBindings,
				uint32 SourceVersion,
				std::span<const FArchiveCustomVersion> CustomVersions,
				const FArchiveState& Context)
				: FObjectArchive({EArchiveDirection::Load,
					Context.bCooking ? EArchivePurpose::CookedPackage : EArchivePurpose::AuthoredPackage,
					EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
					| EArchiveCapability::ObjectReferences
					| EArchiveCapability::SoftObjectReferences
					| EArchiveCapability::RemainingPayload | EArchiveCapability::CustomVersions,
					true, Context.bCooking, Context.bFilterEditorOnly,
					EArchiveBulkDataPolicy::External, Context.Target},
					FArchiveVersionContext{
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), SourceVersion}},
						std::vector<FArchiveCustomVersion>(CustomVersions.begin(), CustomVersions.end())})
				, Object(InObject), Fields(InFields), Objects(InObjects), Bindings(InBindings),
				  Consumed(InFields.size(), 0)
			{
			}

			auto GetStructBaseline() -> EArchiveStructBaseline override
			{
				uint8 Baseline = 0;
				if (!Read(Baseline) || Baseline > 2)
					SetError("Invalid Struct baseline mode.");
				return static_cast<EArchiveStructBaseline>(Baseline);
			}

			auto GetAssetError() const -> EAssetError
			{
				if (bAssetErrorSet) return AssetError;
				const FArchiveFailure* Failure = GetFailure();
				if (Failure && Failure->Code == EArchiveFailureCode::UnsupportedVersion)
					return EAssetError::UnsupportedVersion;
				if (Failure && (Failure->Code == EArchiveFailureCode::UnsupportedType
					|| Failure->Code == EArchiveFailureCode::UnsupportedOperation
					|| Failure->Code == EArchiveFailureCode::MalformedSerializer
					|| Failure->Code == EArchiveFailureCode::MissingBaseReflectedFields))
					return EAssetError::UnsupportedProperty;
				return EAssetError::CorruptFile;
			}

			auto HasUnconsumedFields() const -> bool
			{
				return std::ranges::find(Consumed, uint8{0}) != Consumed.end();
			}

			auto GetLoadedDeprecatedProperties(FName DeclaringType) const
				-> std::span<const FName> override
			{
				const auto It = LoadedDeprecatedProperties.find(DeclaringType.ToString());
				return It == LoadedDeprecatedProperties.end()
					? std::span<const FName>{} : std::span<const FName>(It->second);
			}

			auto SerializeRawBytes(FMutableByteView Bytes) -> void override
			{
				if (HasError() || !IsCurrentFieldAvailable()) return;
				FLoadScope& Scope = Stack.back();
				if (Bytes.size() > Scope.Record->Payload.size() - Scope.Offset)
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::TruncatedPayload,
						std::format("Truncated property payload (requested {}, remaining {}, total {}, offset {}).",
							Bytes.size(), Scope.Record->Payload.size() - Scope.Offset,
							Scope.Record->Payload.size(), Scope.Offset));
					return;
				}
				if (!Bytes.empty())
					std::memcpy(Bytes.data(), Scope.Record->Payload.data() + Scope.Offset, Bytes.size());
				Scope.Offset += Bytes.size();
			}

				auto SerializeBulkData(
				FArchiveBulkDataValue& Value,
				const FArchiveBulkDataParameters& Parameters) -> void override
			{
				(void)Parameters;
				if (HasError() || !IsCurrentFieldAvailable()) return;
				const FArchiveFormatVersion* DastVersion =
					GetVersionContext().FindFormat(FName("DAST"));
				if (!DastVersion || !ObjectPackage::IsSupportedPackageReaderVersion(DastVersion->Version))
				{
					FailLoad(EAssetError::UnsupportedVersion,
						EArchiveFailureCode::InvalidData,
						"Authored bulk fields require a supported DAST package version.");
					return;
				}
				uint64 FieldIndex = 0;
				uint8 Placement = 0;
				uint8 StorageFlags = 0;
				uint16 Alignment = 0;
				uint32 ContentIdVersion = 0;
				FGuid InstanceId;
				uint64 HashLow = 0, HashHigh = 0;
				uint64 LogicalSize = 0, StoredSize = 0, SegmentOffset = 0;
				*this << FieldIndex << Placement << StorageFlags << Alignment
					<< ContentIdVersion << InstanceId << HashLow << HashHigh
					<< LogicalSize << StoredSize << SegmentOffset;
				if (HasError()) return;
				if (FieldIndex == 0 || Placement > 1 || StorageFlags != 0
					|| ContentIdVersion != EditorBulkDataContentIdVersion
					|| !InstanceId.IsValid() || FXxHash128{HashLow, HashHigh}.IsZero()
					|| LogicalSize != StoredSize || LogicalSize > MaximumAuthoredBulkBytes
					|| (Placement == 0 && (Alignment != 1 || SegmentOffset != 0))
					|| (Placement == 1 && (Alignment != EditorBulkDataExternalAlignment
						|| SegmentOffset % Alignment != 0)))
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
						"DAST authored bulk field metadata is invalid.");
					return;
				}
				Value = {.PayloadId = InstanceId,
					.LogicalSize = LogicalSize,
					.StoredSize = StoredSize,
					.ContentHash = {HashLow, HashHigh},
					.StorageKind = Placement == 0 ? EArchiveBulkDataStorageKind::Inline
						: EArchiveBulkDataStorageKind::External,
					.SegmentOffset = SegmentOffset,
					.Alignment = Alignment};
				if (Placement == 0)
				{
					FByteBuffer Bytes(static_cast<size_t>(StoredSize));
					ReadBytes(Bytes);
					if (HasError()) return;
					if (FXxHash128::HashBuffer(Bytes) != Value.ContentHash)
					{
						FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
							"DAST inline authored bulk content identity is invalid.");
						return;
					}
					Value.Buffer = FSharedByteBuffer::Take(std::move(Bytes));
				}
				else
				{
					Value.PackageResource = Bindings.BulkResource;
					if (!Value.PackageResource)
						FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
							"DAST external bulk field requires an explicit package resource binding.");
				}
			}

			auto SerializeObjectReference(DObject*& Value) -> void override
			{
				if (HasError() || !IsCurrentFieldAvailable()) return;
				uint8 Kind = 0;
				if (!Read(Kind)) return;
				Value = nullptr;
				if (Kind == 1)
				{
					uint64 Id = 0;
					if (!Read(Id)) return;
					if (Id == 0 || Id > Objects.size())
					{
						FailLoad(EAssetError::InvalidObjectGraph,
							EArchiveFailureCode::InvalidObjectReference,
							"Invalid internal object reference.");
						return;
					}
					Value = Objects[static_cast<size_t>(Id - 1)];
				}
				else if (Kind == 2)
				{
					std::string PathString;
					if (!ReadString(PathString)) return;
					FObjectPath Path;
					if (!FObjectPath::TryCreate(PathString, Path))
					{
						FailLoad(EAssetError::InvalidPath, EArchiveFailureCode::InvalidPath,
							std::format("Invalid external object reference '{}'.", PathString));
						return;
					}
					if (!Bindings.ResolveExternalObject)
					{
						FailLoad(EAssetError::MissingDependency,
							EArchiveFailureCode::InvalidObjectReference,
							"External object reference requires an explicit resolver binding.");
						return;
					}
					FAssetResult Result = Bindings.ResolveExternalObject(Path, Value);
					if (!Result || !Value)
					{
						FailLoad(Result ? EAssetError::MissingDependency : Result.Error,
							EArchiveFailureCode::InvalidObjectReference,
							Result.Message.empty() ? "External object resolver returned no object."
								: Result.Message);
						return;
					}
				}
				else if (Kind != 0)
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
						"Unknown object reference kind.");
					return;
				}

				const FArchiveLogicalTypeDescriptor& Type = UnwrapFixed(GetCurrentLogicalType());
				if (Value && !Type.QualifiedType.IsNone())
				{
					DClass* Expected = FindClassByQualifiedName(Type.QualifiedType.ToString());
					if (Expected && !Value->IsA(Expected))
						FailLoad(EAssetError::TypeMismatch, EArchiveFailureCode::InvalidData,
							"Object reference class mismatch.");
				}
			}

			auto SerializeSoftObjectValue(FObjectPath& Value) -> void override
			{
				if (HasError() || !IsCurrentFieldAvailable()) return;
				uint8 Kind = 0;
				if (!Read(Kind)) return;
				if (Kind == 0) { Value = {}; return; }
				if (Kind != 1)
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
						"Unknown soft object reference tag.");
					return;
				}
				std::string PathString;
				if (!ReadString(PathString)) return;
				if (PathString.empty())
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidPath,
						"Soft object path payload is empty.");
					return;
				}
				std::string Error;
				FObjectPath Loaded;
				if (!FObjectPath::TryCreate(PathString, Loaded, &Error))
				{
					FailLoad(EAssetError::InvalidPath, EArchiveFailureCode::InvalidPath,
						Error.empty() ? "Invalid soft object path." : Error);
					return;
				}
				Value = std::move(Loaded);
			}

			auto GetRemainingPayloadBytes() const -> uint64 override
			{
				if (!IsCurrentFieldAvailable()) return std::numeric_limits<uint64>::max();
				const FLoadScope& Scope = Stack.back();
				return static_cast<uint64>(Scope.Record->Payload.size() - Scope.Offset);
			}

			auto IsCurrentFieldAvailable() const -> bool override
			{
				return Stack.empty() || Stack.back().Record != nullptr;
			}

		protected:
			auto OnEnterObject(DObject& EnteredObject) -> void override
			{
				if (&EnteredObject != &Object)
					FailLoad(EAssetError::InvalidObjectGraph,
						EArchiveFailureCode::InvalidObjectReference,
						"Authored load entered an unexpected object.");
			}

			auto OnEnterField(const FArchiveFieldDescriptor& Descriptor) -> void override
			{
				if (HasError()) { Stack.emplace_back(); return; }
				std::span<const FAuthoredPackageFieldRecord> Candidates;
				std::span<uint8> CandidateConsumed;
				const bool bTopLevel = Stack.empty();
				if (bTopLevel)
				{
					Candidates = Fields;
					CandidateConsumed = Consumed;
				}
				else
				{
					FLoadScope& Parent = Stack.back();
					if (!Parent.Record) { Stack.emplace_back(); return; }
					FStructState* StructState = &Parent.Struct;
					if (!PathTypes.empty() && PathTypes.back().FieldDepth == Stack.size())
						StructState = &PathTypes.back().Struct;
					if (!PrepareStruct(*StructState, GetCurrentLogicalType())) { Stack.emplace_back(); return; }
					Candidates = StructState->Fields;
					CandidateConsumed = StructState->Consumed;
				}

				FProperty* Property = bTopLevel
					? FindObjectProperty(&Object, Descriptor.Name)
					: FindReflectedProperty(Descriptor);
				const bool bByteStorageProperty = Property
					&& (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Byte
						|| Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Blob);
				const auto ExpectedKind = Property && !bByteStorageProperty ? Property->GetKind()
					: GetNativeKind(Descriptor.LogicalType);
				const std::string ExpectedSignature = Property && !bByteStorageProperty
					? GetSerializedTypeSignature(Property)
					: GetNativeTypeSignature(Descriptor.LogicalType);
				const std::string DeclaringType = Descriptor.DeclaringType.ToString();
				const FPropertyDeprecation* Deprecation = Property ? Property->GetDeprecation() : nullptr;
				const std::string Name = Deprecation
					? Deprecation->HistoricalName.ToString() : Descriptor.Name.ToString();
				DStructBase* DeclaringStruct = FindDeclaringStruct(Descriptor.DeclaringType);
				bool bFoundIdentity = false;
				bool bReservedForDeprecatedRoute = false;
				DurinCodeGen::EPropertyGenFlags FoundKind = DurinCodeGen::EPropertyGenFlags::None;
				std::string FoundSignature;
				for (size_t Index = 0; Index < Candidates.size(); ++Index)
				{
					const auto& Candidate = Candidates[Index];
					if (CandidateConsumed[Index]
						|| Candidate.DeclaringClass != DeclaringType || Candidate.Name != Name) continue;
					bFoundIdentity = true;
					FoundKind = Candidate.Kind;
					FoundSignature = Candidate.TypeSignature;
					if (!Deprecation && FindDeprecatedRoute(DeclaringStruct, Candidate.Name,
						Candidate.Kind, Candidate.TypeSignature))
					{
						bReservedForDeprecatedRoute = true;
						continue;
					}
					if (Candidate.Kind != ExpectedKind
						|| Candidate.TypeSignature != ExpectedSignature) continue;
					CandidateConsumed[Index] = 1;
					if (Deprecation)
						LoadedDeprecatedProperties[DeclaringType].push_back(Property->NamePrivate);
					FLoadScope Scope;
					Scope.Record = &Candidate;
					Scope.Type = Descriptor.LogicalType;
					Stack.push_back(std::move(Scope));
					return;
				}
				if (!bTopLevel && bFoundIdentity && !bReservedForDeprecatedRoute)
				{
					FailLoad(EAssetError::TypeMismatch, EArchiveFailureCode::InvalidData,
						std::format("Serialized struct field {}::{} is incompatible with the current schema "
							"(stored kind={}, signature='{}'; expected kind={}, signature='{}').",
							DeclaringType, Name, static_cast<uint32>(FoundKind),
							FoundSignature, static_cast<uint32>(ExpectedKind),
							ExpectedSignature));
				}
				Stack.emplace_back();
			}

			auto OnEnterFixedArrayElement(uint64) -> void override
			{
				if (!IsCurrentFieldAvailable()) { PushUnavailablePath(); return; }
				const auto& Type = GetCurrentLogicalType();
				PushPathType(Type.ElementType);
			}

			auto OnEnterArrayElement(uint64) -> void override
			{
				if (!IsCurrentFieldAvailable()) { PushUnavailablePath(); return; }
				const auto& Type = GetCurrentLogicalType();
				PushPathType(Type.ElementType);
			}

			auto OnEnterMapKey(uint64) -> void override
			{
				if (!IsCurrentFieldAvailable()) { PushUnavailablePath(); return; }
				const auto& Type = GetCurrentLogicalType();
				PushPathType(Type.KeyType);
			}

			auto OnEnterMapValue(uint64) -> void override
			{
				if (!IsCurrentFieldAvailable()) { PushUnavailablePath(); return; }
				const auto& Type = GetCurrentLogicalType();
				PushPathType(Type.ValueType);
			}

			auto OnLeavePath() -> void override
			{
				if (PathTypes.empty()) return;
				FPathType& Path = PathTypes.back();
				if (!HasError() && !Stack.empty() && Stack.back().Record
					&& Path.Type.Kind == FArchiveLogicalTypeDescriptor::EKind::Struct)
					PrepareStruct(Path.Struct, Path.Type);
				PathTypes.pop_back();
			}

			auto OnLeaveField() -> void override
			{
				if (Stack.empty()) return;
				FLoadScope& Scope = Stack.back();
				if (!HasError() && Scope.Record)
				{
					if (Scope.Type.Kind == FArchiveLogicalTypeDescriptor::EKind::Struct)
						PrepareStruct(Scope.Struct, Scope.Type);
					if (!HasError() && Scope.Offset != Scope.Record->Payload.size())
						FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
							"Property payload has trailing bytes.");
				}
				Stack.pop_back();
			}

		private:
			struct FStructState
			{
				bool bPrepared = false;
				std::vector<FAuthoredPackageFieldRecord> Fields;
				std::vector<uint8> Consumed;
			};

			struct FLoadScope
			{
				FLoadScope() = default;
				FLoadScope(const FLoadScope&) = delete;
				auto operator=(const FLoadScope&) -> FLoadScope& = delete;
				FLoadScope(FLoadScope&&) noexcept = default;
				auto operator=(FLoadScope&&) noexcept -> FLoadScope& = default;
				const FAuthoredPackageFieldRecord* Record = nullptr;
				FArchiveLogicalTypeDescriptor Type;
				size_t Offset = 0;
				FStructState Struct;
			};

			struct FPathType
			{
				FPathType() = default;
				FPathType(const FPathType&) = delete;
				auto operator=(const FPathType&) -> FPathType& = delete;
				FPathType(FPathType&&) noexcept = default;
				auto operator=(FPathType&&) noexcept -> FPathType& = default;
				size_t FieldDepth = 0;
				FArchiveLogicalTypeDescriptor Type;
				FStructState Struct;
			};

			auto FailLoad(EAssetError Error, EArchiveFailureCode Code,
				std::string_view Message) -> void
			{
				if (!HasError()) AssetError = Error;
				if (!HasError()) bAssetErrorSet = true;
				Fail(Code, Message);
			}

			auto GetCurrentLogicalType() const -> const FArchiveLogicalTypeDescriptor&
			{
				if (!PathTypes.empty() && PathTypes.back().FieldDepth == Stack.size())
					return PathTypes.back().Type;
				return Stack.back().Type;
			}

			auto PushPathType(const std::shared_ptr<FArchiveLogicalTypeDescriptor>& Type) -> void
			{
				if (!Type)
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
						"Authored container path has no logical element type.");
					return;
				}
				FPathType Path;
				Path.FieldDepth = Stack.size();
				Path.Type = *Type;
				PathTypes.push_back(std::move(Path));
			}

			auto PushUnavailablePath() -> void
			{
				FPathType Path;
				Path.FieldDepth = Stack.size();
				PathTypes.push_back(std::move(Path));
			}

			template<typename T> auto Read(T& Value) -> bool
			{
				if (Stack.empty() || !Stack.back().Record) return true;
				FLoadScope& Scope = Stack.back();
				if (sizeof(T) > Scope.Record->Payload.size() - Scope.Offset)
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::TruncatedPayload,
						"Truncated property payload.");
					return false;
				}
				std::memcpy(&Value, Scope.Record->Payload.data() + Scope.Offset, sizeof(T));
				Scope.Offset += sizeof(T);
				return true;
			}

			auto ReadString(std::string& Value) -> bool
			{
				uint64 Size = 0;
				if (!Read(Size)) return false;
				if (Size > MaximumPackageStringBytes || Size > GetRemainingPayloadBytes())
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::TruncatedPayload,
						"Truncated or overlong string payload.");
					return false;
				}
				FLoadScope& Scope = Stack.back();
				Value.assign(reinterpret_cast<const char*>(Scope.Record->Payload.data() + Scope.Offset),
					static_cast<size_t>(Size));
				Scope.Offset += static_cast<size_t>(Size);
				return true;
			}

			auto PrepareStruct(FStructState& State,
				const FArchiveLogicalTypeDescriptor& InputType) -> bool
			{
				if (State.bPrepared) return !HasError();
				State.bPrepared = true;
				const auto& Type = UnwrapFixed(InputType);
				if (Type.Kind != FArchiveLogicalTypeDescriptor::EKind::Struct)
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
						"A nested authored field was entered outside a struct value.");
					return false;
				}

				std::string StructName;
				uint64 FieldCount = 0;
				if (!ReadString(StructName) || !Read(FieldCount) || FieldCount > 100000)
				{
					if (!HasError()) FailLoad(EAssetError::CorruptFile,
						EArchiveFailureCode::InvalidData, "Invalid struct payload header.");
					return false;
				}
				if (!Type.QualifiedType.IsNone() && StructName != Type.QualifiedType.ToString())
				{
					FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
						"Invalid struct payload type.");
					return false;
				}
				LoadedDeprecatedProperties[StructName].clear();
				State.Fields.reserve(static_cast<size_t>(FieldCount));
				for (uint64 Index = 0; Index < FieldCount; ++Index)
				{
					FAuthoredPackageFieldRecord Field;
					uint8 Kind = 0;
					uint64 PayloadSize = 0;
					if (!ReadString(Field.DeclaringClass) || !ReadString(Field.Name)
						|| !Read(Kind) || !ReadString(Field.TypeSignature)
						|| !Read(PayloadSize) || PayloadSize > GetRemainingPayloadBytes())
					{
						if (!HasError()) FailLoad(EAssetError::CorruptFile,
							EArchiveFailureCode::TruncatedPayload, "Invalid struct field record.");
						return false;
					}
					Field.Kind = static_cast<DurinCodeGen::EPropertyGenFlags>(Kind);
					Field.Payload.resize(static_cast<size_t>(PayloadSize));
					if (PayloadSize != 0)
					{
						SerializeRawBytes(Field.Payload);
						if (HasError()) return false;
					}
					State.Fields.push_back(std::move(Field));
				}
				State.Consumed.assign(State.Fields.size(), 0);
				return true;
			}

			DObject& Object;
			std::span<const FAuthoredPackageFieldRecord> Fields;
			std::span<DObject* const> Objects;
			const FPackageLoadBindings& Bindings;
			std::vector<uint8> Consumed;
			std::vector<FLoadScope> Stack;
			std::vector<FPathType> PathTypes;
			std::unordered_map<std::string, std::vector<FName>> LoadedDeprecatedProperties;
			EAssetError AssetError = EAssetError::CorruptFile;
			bool bAssetErrorSet = false;
		};

	}

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FPackageLoadBindings& Bindings,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions,
		const FArchiveState& Context) -> FAssetResult
	{
		FArchiveVersionContext VersionContext{
			{FArchiveFormatVersion{FName("DAST"), SourceVersion}},
			std::vector<FArchiveCustomVersion>(CustomVersions.begin(), CustomVersions.end())};
		FAuthoredLoadArchive Archive(
			Object, Fields, Objects, Bindings, SourceVersion, CustomVersions, Context);
		{
			auto Scope = Archive.EnterObject(Object);
			if (Context.bCooking) Object.SerializeCooked(Archive);
			else Object.Serialize(Archive);
		}
		if (Archive.HasError())
			return {Archive.GetAssetError(), std::string(Archive.GetError())};
		if (Archive.HasUnconsumedFields())
			return {EAssetError::UnsupportedProperty,
				"Serialized fields are not present in the live schema."};
		return {};
	}
}

namespace Durin::AssetPrivate
{
 auto CaptureLivePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
  const FAssetPackageSerializationOptions& Options, ObjectPackage::FLinkerTables& OutLinker,
  std::string* OutError, uint32 FormatVersion) -> FAssetResult
 {
  FPackageCaptureOptions Capture;
  Capture.bCooking = Options.Domain == EAssetPackageSaveDomain::Cooked;
  Capture.bRetainEditorOnlyData = Options.bRetainEditorOnlyData;
  Capture.Target.Platform = Options.TargetPlatform == ECookTargetPlatform::Win64 ? "Win64" : "";
  Capture.Target.Profile = Options.TargetProfile == ECookTargetProfile::Game ? "Game"
   : Options.TargetProfile == ECookTargetProfile::EditorValidation ? "EditorValidation" : "";
  Capture.SaveOverrides = Options.SaveOverrides;
  Capture.PropertyFilter = Options.PropertyFilter;
  if (Package) for (DObject* Asset : Package->GetTopLevelAssets())
   if (auto* Redirector = Cast<DAssetRedirector>(Asset))
   {
    DObject* Destination = Redirector->GetDestinationObject();
    FObjectPath Path;
    if (!Destination || Destination == Asset || !Destination->GetPackage()
     || !FObjectPath::TryCreate(Destination->GetObjectPath(), Path))
     return {EAssetError::CorruptFile, "Redirector destination is invalid."};
    Capture.RedirectDestinations.emplace(Asset, std::move(Path));
   }
  auto Result = CapturePackageLinker(Package, DeltaMode, Capture, OutLinker, OutError, FormatVersion);
  switch (Result.Error)
  {
   case EPackageSaveError::None: return {};
   case EPackageSaveError::InvalidPath: return {EAssetError::InvalidPath, Result.Message};
   case EPackageSaveError::InvalidPackageType: return {EAssetError::InvalidPackageType, Result.Message};
   case EPackageSaveError::InvalidObjectGraph: return {EAssetError::InvalidObjectGraph, Result.Message};
   case EPackageSaveError::UnsupportedVersion: return {EAssetError::UnsupportedVersion, Result.Message};
   default: return {EAssetError::UnsupportedProperty, Result.Message};
  }
 }
}
