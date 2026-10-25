#include "AssetPackageArchive.h"
#include "AssetPropertyKindTraits.h"
#include "AssetRuntimeStateInternal.h"
#include "Asset/PackageVersionPolicy.h"
#include "AssetPackageLinker.h"
#include "AssetPackageLinkerSchema.h"
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
		auto Error(EAssetReadError Code, std::string Message) -> FAssetReadResult
		{ return {Code, std::move(Message)}; }

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
						{EAssetReadError::CorruptFile, std::format("Invalid struct baseline {}; expected 0..2.", Baseline)});
				return static_cast<EArchiveStructBaseline>(Baseline);
			}

			auto GetResult() const -> FAssetReadResult
			{
				if (const auto* Failure = GetFailure())
				{
					auto Code = LoadError;
					if (Code == EAssetReadError::None)
					{
						if (Failure->Code == EArchiveFailureCode::UnsupportedVersion)
							Code = EAssetReadError::UnsupportedVersion;
						else if (Failure->Code == EArchiveFailureCode::UnsupportedType
							|| Failure->Code == EArchiveFailureCode::UnsupportedOperation
							|| Failure->Code == EArchiveFailureCode::MalformedSerializer
							|| Failure->Code == EArchiveFailureCode::MissingBaseReflectedFields)
							Code = EAssetReadError::UnsupportedProperty;
						else Code = EAssetReadError::CorruptFile;
					}
					std::string Reason = Failure->Message;
					const auto Cause = std::visit([](const auto& Value) -> std::string {
						using T = std::decay_t<decltype(Value)>;
						if constexpr (std::is_same_v<T, FObjectPathError>) return ToString(Value);
						else if constexpr (std::is_same_v<T, FPropertyValueError>) return ToString(Value);
						else if constexpr (std::is_same_v<T, FReflectedMapKeyError>) return ToString(Value);
						else if constexpr (std::is_same_v<T, FObjectValidationError>) return ToString(Value);
						else return {};
					}, GetValueFailureCause());
					if (!Cause.empty()) Reason += (Reason.empty() ? "" : " ") + Cause;
					return {Code, std::format("Package field load failed for '{}' at '{}' (archive code={}): {}",
						Object.GetObjectPath(), Failure->Path, static_cast<uint32>(Failure->Code), Reason)};
				}
				for (size_t Index = 0; Index < Fields.size(); ++Index)
					if (!Consumed[Index]) return {EAssetReadError::UnsupportedProperty,
						std::format("Object '{}' has an unconsumed field '{}::{}' of type '{}'.",
							Object.GetObjectPath(), Fields[Index].DeclaringClass, Fields[Index].Name,
							Fields[Index].TypeSignature)};
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
				if (IsError() || !IsCurrentFieldAvailable()) return;
				FLoadScope& Scope = Stack.back();
				if (Bytes.size() > Scope.Record->Payload.size() - Scope.Offset)
				{
					FailLoad(EArchiveFailureCode::TruncatedPayload,
						{EAssetReadError::CorruptFile, std::format("Truncated payload: requested {} bytes, remaining {}, offset {}, total {}.", Bytes.size(), Scope.Record->Payload.size() - Scope.Offset, Scope.Offset, Scope.Record->Payload.size())});
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
				if (IsError() || !IsCurrentFieldAvailable()) return;
				const FArchiveFormatVersion* DastVersion =
					GetVersionContext().FindFormat(FName("DAST"));
				if (!DastVersion || !ObjectPackage::IsSupportedPackageReaderVersion(DastVersion->Version))
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::UnsupportedVersion, std::format("Unsupported DAST bulk version {}.", DastVersion ? DastVersion->Version : 0)});
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
				if (IsError()) return;
				if (FieldIndex == 0 || Placement > 1 || StorageFlags != 0
					|| ContentIdVersion != EditorBulkDataContentIdVersion
					|| !InstanceId.IsValid() || FXxHash128{HashLow, HashHigh}.IsZero()
					|| LogicalSize != StoredSize || LogicalSize > MaximumAuthoredBulkBytes
					|| (Placement == 0 && (Alignment != 1 || SegmentOffset != 0))
					|| (Placement == 1 && (Alignment != EditorBulkDataExternalAlignment
						|| SegmentOffset % Alignment != 0)))
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::CorruptFile, "Invalid bulk descriptor metadata."});
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
					if (IsError()) return;
					if (FXxHash128::HashBuffer(Bytes) != Value.ContentHash)
					{
						FailLoad(EArchiveFailureCode::InvalidData,
							{EAssetReadError::CorruptFile, "Inline bulk payload content hash mismatch."});
						return;
					}
					Value.Buffer = FSharedByteBuffer::Take(std::move(Bytes));
				}
				else
				{
					Value.PackageResource = Bindings.BulkResource;
					if (!Value.PackageResource)
						FailLoad(EArchiveFailureCode::InvalidData,
							{EAssetReadError::CorruptFile, "External bulk payload requires a resource binding."});
				}
			}

			auto SerializeObjectReference(DObject*& Value) -> void override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return;
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
							{EAssetReadError::InvalidObjectGraph, std::format("Invalid object export index {}; export count {}.", Id, Objects.size())});
						return;
					}
					Value = Objects[static_cast<size_t>(Id - 1)];
				}
				else if (Kind == 2)
				{
					std::string PathString;
					if (!ReadString(PathString)) return;
					FObjectPath Path;
					if (const auto Validation = FObjectPath::TryCreateWithDiagnostic(PathString, Path); !Validation)
					{
						FailLoad(EArchiveFailureCode::InvalidPath,
							{EAssetReadError::InvalidPath, std::format("Invalid dependency path '{}': {}", PathString, ToString(Validation.error()))});
						return;
					}
					if (!Bindings.ResolveExternalObject)
					{
						FailLoad(EArchiveFailureCode::InvalidObjectReference,
							{EAssetReadError::MissingDependency, std::format("No resolver for dependency '{}'.", PathString)});
						return;
					}
					auto Result = Bindings.ResolveExternalObject(Path, Value);
					if (!Result || !Value)
					{
						FailLoad(EArchiveFailureCode::InvalidObjectReference,
							{Result ? EAssetReadError::MissingDependency : Result.Error, std::format("Dependency '{}': {}", PathString, Result ? "Resolver returned no object." : Result.Message)});
						return;
					}
				}
				else if (Kind != 0)
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::CorruptFile, std::format("Invalid hard reference kind {}; expected 0..2.", Kind)});
					return;
				}

				const FArchiveLogicalTypeDescriptor& Type = Durin::PackagePrivate::UnwrapFixed(GetCurrentLogicalType());
				if (Value && !Type.QualifiedType.IsNone())
				{
					DClass* Expected = FindClassByQualifiedName(Type.QualifiedType.ToString());
					if (Expected && !Value->IsA(Expected))
						FailLoad(EArchiveFailureCode::InvalidData,
							{EAssetReadError::TypeMismatch, std::format("Object '{}': expected type '{}', actual '{}'.", Value->GetObjectPath(), Expected->GetQualifiedName().ToString(), Value->GetClass()->GetQualifiedName().ToString())});
				}
			}

			auto SerializeSoftObjectValue(FObjectPath& Value) -> void override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return;
				uint8 Kind = 0;
				if (!Read(Kind)) return;
				if (Kind == 0) { Value = {}; return; }
				if (Kind != 1)
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::CorruptFile, std::format("Invalid soft reference kind {}; expected 0..1.", Kind)});
					return;
				}
				std::string PathString;
				if (!ReadString(PathString)) return;
				if (PathString.empty())
				{
					FailLoad(EArchiveFailureCode::InvalidPath,
						{EAssetReadError::CorruptFile, "Soft reference path is empty."});
					return;
				}
				FObjectPath Loaded;
				if (const auto PathValidation = FObjectPath::TryCreateWithDiagnostic(PathString, Loaded); !PathValidation)
				{
					FailLoad(EArchiveFailureCode::InvalidPath,
						{EAssetReadError::InvalidPath, std::format("Invalid soft reference '{}': {}", PathString, ToString(PathValidation.error()))});
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
						{EAssetReadError::InvalidObjectGraph, std::format("Serializer entered another object '{}'.", EnteredObject.GetObjectPath())});
			}

			auto OnEnterField(const FArchiveFieldDescriptor& Descriptor) -> void override
			{
				if (IsError()) { Stack.emplace_back(); return; }
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
					if (!Deprecation && LinkerSchema::ResolvePropertyForLoad(DeclaringStruct, Candidate.Name,
						Candidate.Kind, Candidate.TypeSignature).Route == LinkerSchema::EPropertyLoadRoute::Deprecated)
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
						{EAssetReadError::TypeMismatch, std::format("Field '{}::{}': expected '{}' (kind {}), actual '{}' (kind {}).", DeclaringType, Name, ExpectedSignature, static_cast<uint32>(ExpectedKind), FoundSignature, static_cast<uint32>(FoundKind))});
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
				if (!IsError() && !Stack.empty() && Stack.back().Record
					&& Path.Type.Kind == FArchiveLogicalTypeDescriptor::EKind::Struct)
					PrepareStruct(Path.Struct, Path.Type);
				PathTypes.pop_back();
			}

			auto OnLeaveField() -> void override
			{
				if (Stack.empty()) return;
				FLoadScope& Scope = Stack.back();
				if (!IsError() && Scope.Record)
				{
					if (Scope.Type.Kind == FArchiveLogicalTypeDescriptor::EKind::Struct)
						PrepareStruct(Scope.Struct, Scope.Type);
					if (!IsError() && Scope.Offset != Scope.Record->Payload.size())
						FailLoad(EArchiveFailureCode::TrailingData,
							{EAssetReadError::CorruptFile, std::format("Trailing payload: {} bytes at offset {} of {}.", Scope.Record->Payload.size() - Scope.Offset, Scope.Offset, Scope.Record->Payload.size())});
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

			auto FailLoad(EArchiveFailureCode Code, FAssetReadResult Error) -> void
			{
				if (IsError()) return;
				LoadError = Error.Error;
				Fail(Code, std::move(Error.Message));
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
						{EAssetReadError::CorruptFile, "Missing nested logical type descriptor."});
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
						{EAssetReadError::CorruptFile, std::format("Truncated payload: requested {} bytes, remaining {}, offset {}, total {}.", sizeof(T), Scope.Record->Payload.size() - Scope.Offset, Scope.Offset, Scope.Record->Payload.size())});
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
						{EAssetReadError::CorruptFile, std::format("Truncated string: requested {} bytes, available limit {}.", Size, std::min<uint64>(Durin::PackagePrivate::MaximumPackageStringBytes, GetRemainingPayloadBytes()))});
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
				if (State.bPrepared) return !IsError();
				State.bPrepared = true;
				const auto& Type = Durin::PackagePrivate::UnwrapFixed(InputType);
				if (Type.Kind != FArchiveLogicalTypeDescriptor::EKind::Struct)
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::CorruptFile, "Expected a struct logical type."});
					return false;
				}

				std::string StructName;
				uint64 FieldCount = 0;
				if (!ReadString(StructName) || !Read(FieldCount) || FieldCount > 100000)
				{
					if (!IsError()) FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::CorruptFile, std::format("Struct field count {} exceeds limit 100000.", FieldCount)});
					return false;
				}
				if (!Type.QualifiedType.IsNone() && StructName != Type.QualifiedType.ToString())
				{
					FailLoad(EArchiveFailureCode::InvalidData,
						{EAssetReadError::CorruptFile, std::format("Expected struct '{}', actual '{}'.", Type.QualifiedType.ToString(), StructName)});
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
						if (!IsError()) FailLoad(EArchiveFailureCode::TruncatedPayload,
							{EAssetReadError::CorruptFile, std::format("Truncated field '{}': requested {} bytes, remaining {}.", Field.Name, PayloadSize, GetRemainingPayloadBytes())});
						return false;
					}
					Field.Kind = static_cast<DurinCodeGen::EPropertyGenFlags>(Kind);
					Field.Payload.resize(static_cast<size_t>(PayloadSize));
					if (PayloadSize != 0)
					{
						SerializeRawBytes(Field.Payload);
						if (IsError()) return false;
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
			EAssetReadError LoadError = EAssetReadError::None;
		};

	}

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FPackageLoadBindings& Bindings,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions,
		const FArchiveState& Context) -> FAssetReadResult
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
	auto DecodeReferenceByteToolValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		Durin::PackagePrivate::FByteReader& Reader,
		const std::vector<DObject*>& Objects,
		uint32 SourceVersion) -> FAssetReadResult
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
					Path, ObjectProperty->GetReferencedClass());
				if (!Result) return Error(EAssetReadError::MissingDependency, Result.error().Message);
				Value = *Result;
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
				return Error(EAssetReadError::InvalidPath, ToString(PathValidation.error()));
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
				if (!ValueResult) StorageError = Durin::ToString(ValueResult.error());
				return ValueResult.has_value();
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
					auto Result = DecodeReferenceByteToolValue(
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
					Validation.error().StructName = Struct->GetQualifiedName().ToString();
					Validation.error().SourceVersion = SourceVersion;
					auto Result = Error(EAssetReadError::CorruptFile, ToString(Validation.error()));
					return Result;
				}
			}
			if (!([&] {
				const auto ValueResult = Property->CopyAssignValue(Property->GetValuePtr(Container, ArrayIndex), StructValue);
				if (!ValueResult) StorageError = Durin::ToString(ValueResult.error());
				return ValueResult.has_value();
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
				auto Result = DecodeReferenceByteToolValue(
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
					if (!ValueResult) StorageError = Durin::ToString(ValueResult.error());
					return ValueResult.has_value();
				}())
					|| !([&] {
						const auto ValueResult = ValueStorage.DefaultConstruct(Map->GetValueProp(), 0);
						if (!ValueResult) StorageError = Durin::ToString(ValueResult.error());
						return ValueResult.has_value();
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
						if (!ValueResult) StorageError = Durin::ToString(ValueResult.error());
						return ValueResult.has_value();
					}())
						|| !([&] {
							const auto ValueResult = ValueStorage.DefaultConstruct(Map->GetValueProp(), 0);
							if (!ValueResult) StorageError = Durin::ToString(ValueResult.error());
							return ValueResult.has_value();
						}()))
						return Error(EAssetReadError::UnsupportedProperty, std::move(StorageError));
				}
				auto Result = DecodeReferenceByteToolValue(
					Map->GetKeyProp(), KeyStorage.GetContainer(), 0, Reader, Objects, SourceVersion);
				if (!Result)
				{
					Result.Message = std::format("MapEntry[{}].Key: {}", Index, Result.Message);
					return Result;
				}
				Result = DecodeReferenceByteToolValue(
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

}
