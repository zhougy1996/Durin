#include "AssetPackageArchive.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageLinker.h"
#include "Asset/EditorBulkData.h"
#include "Asset/EditorBulkDataStorage.h"
#include "DObject/PackageValueCodec.h"

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
				&& Durin::PackagePrivate::GetSerializedTypeSignature(Current) == StoredSignature) return nullptr;
			FProperty* Match = nullptr;
			bool bAmbiguous = false;
			Owner->ForEachProperty([&](FProperty* Property) {
				if (bAmbiguous) return;
				const FPropertyDeprecation* Deprecation = Property ? Property->GetDeprecation() : nullptr;
				if (!Deprecation || Deprecation->HistoricalName.ToString() != StoredName
					|| Property->GetKind() != StoredKind
					|| Durin::PackagePrivate::GetSerializedTypeSignature(Property) != StoredSignature) return;
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
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile, .Actual = Baseline, .Expected = 2});
				return static_cast<EArchiveStructBaseline>(Baseline);
			}

			auto GetResult() const -> FPackageObjectLoadResult
			{
				if (const auto* Failure = GetFailure())
				{
					auto Error = LoadError;
					if (Error.Code == EAssetError::None)
					{
						if (Failure->Code == EArchiveFailureCode::UnsupportedVersion)
							Error.Code = EAssetError::UnsupportedVersion;
						else if (Failure->Code == EArchiveFailureCode::UnsupportedType
							|| Failure->Code == EArchiveFailureCode::UnsupportedOperation
							|| Failure->Code == EArchiveFailureCode::MalformedSerializer
							|| Failure->Code == EArchiveFailureCode::MissingBaseReflectedFields)
							Error.Code = EAssetError::UnsupportedProperty;
						else Error.Code = EAssetError::CorruptFile;
					}
					Error.ObjectPath = Object.GetObjectPath();
					Error.ArchiveCode = Failure->Code;
					Error.ArchivePath = Failure->Path;
					if (std::holds_alternative<std::monostate>(Error.Cause))
						Error.Cause = GetValueFailureCause();
					return {std::move(Error)};
				}
				for (size_t Index = 0; Index < Fields.size(); ++Index)
					if (!Consumed[Index]) return {{.Code = EAssetError::UnsupportedProperty,
						.ObjectPath = Object.GetObjectPath(), .Subject = Fields[Index].Name,
						.DeclaringType = Fields[Index].DeclaringClass,
						.ActualType = Fields[Index].TypeSignature}};
				return {};
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
					FailLoad(EArchiveFailureCode::TruncatedPayload,
						{.Code = EAssetError::CorruptFile,
							.Actual = Bytes.size(), .Expected = Scope.Record->Payload.size() - Scope.Offset,
							.Offset = Scope.Offset, .Total = Scope.Record->Payload.size()});
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
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::UnsupportedVersion,
							.Actual = DastVersion ? DastVersion->Version : 0});
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
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile});
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
						FailLoad(EArchiveFailureCode::InvalidData,
							{.Code = EAssetError::CorruptFile});
						return;
					}
					Value.Buffer = FSharedByteBuffer::Take(std::move(Bytes));
				}
				else
				{
					Value.PackageResource = Bindings.BulkResource;
					if (!Value.PackageResource)
						FailLoad(EArchiveFailureCode::InvalidData,
							{.Code = EAssetError::CorruptFile});
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
						FailLoad(EArchiveFailureCode::InvalidObjectReference,
							{.Code = EAssetError::InvalidObjectGraph, .Actual = Id, .Expected = Objects.size()});
						return;
					}
					Value = Objects[static_cast<size_t>(Id - 1)];
				}
				else if (Kind == 2)
				{
					std::string PathString;
					if (!ReadString(PathString)) return;
					FObjectPath Path;
					if (const auto Validation = FObjectPath::TryCreate(PathString, Path); !Validation)
					{
						FailLoad(EArchiveFailureCode::InvalidPath,
							{.Code = EAssetError::InvalidPath,
								.Subject = PathString, .Cause = Validation.Error});
						return;
					}
					if (!Bindings.ResolveExternalObject)
					{
						FailLoad(EArchiveFailureCode::InvalidObjectReference,
							{.Code = EAssetError::MissingDependency, .Subject = PathString});
						return;
					}
					FAssetResult Result = Bindings.ResolveExternalObject(Path, Value);
					if (!Result || !Value)
					{
						FailLoad(EArchiveFailureCode::InvalidObjectReference,
							{.Code = Result ? EAssetError::MissingDependency : Result.Error,
								.Subject = PathString,
								.AssetCause = Result ? nullptr : std::make_shared<FAssetResult>(std::move(Result))});
						return;
					}
				}
				else if (Kind != 0)
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile, .Actual = Kind, .Expected = 2});
					return;
				}

				const FArchiveLogicalTypeDescriptor& Type = Durin::PackagePrivate::UnwrapFixed(GetCurrentLogicalType());
				if (Value && !Type.QualifiedType.IsNone())
				{
					DClass* Expected = FindClassByQualifiedName(Type.QualifiedType.ToString());
					if (Expected && !Value->IsA(Expected))
						FailLoad(EArchiveFailureCode::InvalidData,
							{.Code = EAssetError::TypeMismatch,
								.Subject = Value->GetObjectPath(), .ExpectedType = Expected->GetQualifiedName().ToString(),
								.ActualType = Value->GetClass()->GetQualifiedName().ToString()});
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
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile, .Actual = Kind, .Expected = 1});
					return;
				}
				std::string PathString;
				if (!ReadString(PathString)) return;
				if (PathString.empty())
				{
					FailLoad(EArchiveFailureCode::InvalidPath,
						{.Code = EAssetError::CorruptFile});
					return;
				}
				FObjectPath Loaded;
				if (const auto PathValidation = FObjectPath::TryCreate(PathString, Loaded); !PathValidation)
				{
					FailLoad(EArchiveFailureCode::InvalidPath,
						{.Code = EAssetError::InvalidPath,
							.Subject = PathString, .Cause = PathValidation.Error});
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
					FailLoad(EArchiveFailureCode::InvalidObjectReference,
						{.Code = EAssetError::InvalidObjectGraph, .Subject = EnteredObject.GetObjectPath()});
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
					: Durin::PackagePrivate::GetNativeKind(Descriptor.LogicalType);
				const std::string ExpectedSignature = Property && !bByteStorageProperty
					? Durin::PackagePrivate::GetSerializedTypeSignature(Property)
					: Durin::PackagePrivate::GetNativeTypeSignature(Descriptor.LogicalType);
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
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::TypeMismatch, .Subject = Name,
							.DeclaringType = DeclaringType, .ExpectedType = ExpectedSignature,
							.ActualType = FoundSignature, .Actual = static_cast<uint32>(FoundKind),
							.Expected = static_cast<uint32>(ExpectedKind)});
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
						FailLoad(EArchiveFailureCode::TrailingData,
							{.Code = EAssetError::CorruptFile,
								.Actual = Scope.Record->Payload.size() - Scope.Offset,
								.Offset = Scope.Offset, .Total = Scope.Record->Payload.size()});
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

			auto FailLoad(EArchiveFailureCode Code, FPackageObjectLoadError Error) -> void
			{
				if (HasError()) return;
				LoadError = std::move(Error);
				Fail(Code, {});
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
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile});
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
					FailLoad(EArchiveFailureCode::TruncatedPayload,
						{.Code = EAssetError::CorruptFile, .Actual = sizeof(T),
							.Expected = Scope.Record->Payload.size() - Scope.Offset,
							.Offset = Scope.Offset, .Total = Scope.Record->Payload.size()});
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
				if (Size > Durin::PackagePrivate::MaximumPackageStringBytes || Size > GetRemainingPayloadBytes())
				{
					FailLoad(EArchiveFailureCode::TruncatedPayload,
						{.Code = EAssetError::CorruptFile, .Actual = Size,
							.Expected = std::min<uint64>(Durin::PackagePrivate::MaximumPackageStringBytes, GetRemainingPayloadBytes())});
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
				const auto& Type = Durin::PackagePrivate::UnwrapFixed(InputType);
				if (Type.Kind != FArchiveLogicalTypeDescriptor::EKind::Struct)
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile});
					return false;
				}

				std::string StructName;
				uint64 FieldCount = 0;
				if (!ReadString(StructName) || !Read(FieldCount) || FieldCount > 100000)
				{
					if (!HasError()) FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile, .Actual = FieldCount, .Expected = 100000});
					return false;
				}
				if (!Type.QualifiedType.IsNone() && StructName != Type.QualifiedType.ToString())
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{.Code = EAssetError::CorruptFile,
							.ExpectedType = Type.QualifiedType.ToString(), .ActualType = StructName});
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
						if (!HasError()) FailLoad(EArchiveFailureCode::TruncatedPayload,
							{.Code = EAssetError::CorruptFile, .Subject = Field.Name,
								.Actual = PayloadSize, .Expected = GetRemainingPayloadBytes()});
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
			FPackageObjectLoadError LoadError;
		};

	}

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FPackageLoadBindings& Bindings,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions,
		const FArchiveState& Context) -> FPackageObjectLoadResult
	{
		FAuthoredLoadArchive Archive(
			Object, Fields, Objects, Bindings, SourceVersion, CustomVersions, Context);
		{
			auto Scope = Archive.EnterObject(Object);
			if (Context.bCooking) Object.SerializeCooked(Archive);
			else Object.Serialize(Archive);
		}
		return Archive.GetResult();
	}
}

namespace Durin
{
	auto FormatPackageObjectLoadError(const FPackageObjectLoadError& Error) -> std::string
	{
		const auto Cause = std::visit([](const auto& Value) -> std::string {
			using T = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<T, FObjectError>) return FormatObjectError(Value);
			else if constexpr (std::is_same_v<T, FPropertyValueError>) return FormatPropertyValueError(Value);
			else if constexpr (std::is_same_v<T, FReflectedMapKeyError>) return FormatReflectedMapKeyError(Value);
			else if constexpr (std::is_same_v<T, FObjectValidationError>) return FormatObjectValidationError(Value);
			else return {};
		}, Error.Cause);
		if (!Cause.empty()) return Cause;
		if (Error.Code == EAssetError::None) return {};
		if (Error.AssetCause && !Error.AssetCause->Message.empty()) return Error.AssetCause->Message;
		return std::format("Package field load failed for '{}' at '{}' (asset code={}, archive code={}).",
			Error.ObjectPath, Error.ArchivePath, static_cast<uint32>(Error.Code),
			Error.ArchiveCode ? std::to_string(static_cast<uint32>(*Error.ArchiveCode)) : "none");
	}
}
