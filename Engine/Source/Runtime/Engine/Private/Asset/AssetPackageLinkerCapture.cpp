#include "AssetPackageArchive.h"
#include "AssetRuntimeStateInternal.h"
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
		constexpr std::string_view RedirectorClassName = "Durin::DAssetRedirector";

		enum class ENodeKind : uint8 { Field, Fixed, Array, MapKey, MapValue };

		struct FCapturedNode
		{
			ENodeKind Kind = ENodeKind::Field;
			uint64 Index = 0;
			FArchiveFieldDescriptor Field;
			FProperty* ReflectedProperty = nullptr;
			FByteArray Raw;
			std::vector<FCapturedNode> Children;
			FByteArray BulkBytes;
			uint64 BulkElementSize = 1;
			uint32 BulkAlignment = 1;
			uint8 BulkStorage = 0;
			bool bDetachedBulk = false;
		};

		struct FCapturedObject
		{
			uint64 Id = 0;
			uint64 OuterId = 0;
			std::string ClassName;
			std::string ObjectName;
			std::vector<FCapturedNode> Fields;
		};

		// Owns every value and dependency needed after live package capture ends.
		struct FCapturedPackage
		{
			std::vector<FCapturedObject> Objects;
			std::vector<FPackagePath> Dependencies;
			std::vector<FObjectPath> HardReferenceTargets;
			std::vector<FEditorBulkDataStoragePayload> BulkPayloads;
			std::vector<std::pair<uint64, uint64>> InternalReferences;
		};

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

		auto ReflectedStructIdentity(const DStructBase* Struct) -> std::string
		{
			if (const auto* Class = Cast<DClass>(Struct)) return Class->GetQualifiedName().ToString();
			if (const auto* ValueStruct = Cast<DStruct>(Struct)) return ValueStruct->GetQualifiedName().ToString();
			return "<unknown>";
		}

		template<typename FVisitor>
		auto VisitReflectedPropertySchema(DStructBase* Root,
			std::unordered_set<const DStructBase*>& Visited, FVisitor& Visitor) -> bool
		{
			if (!Root || !Visited.emplace(Root).second) return true;
			bool bValid = true;
			std::function<void(FProperty*)> VisitProperty = [&](FProperty* Property) {
				if (!bValid || !Property) return;
				bValid = Visitor(Root, Property);
				if (!bValid) return;
				switch (Property->GetKind())
				{
				case DurinCodeGen::EPropertyGenFlags::Struct:
					bValid = VisitReflectedPropertySchema(
						static_cast<FStructProperty*>(Property)->GetStruct(), Visited, Visitor);
					break;
				case DurinCodeGen::EPropertyGenFlags::Array:
					VisitProperty(static_cast<FArrayProperty*>(Property)->GetInner());
					break;
				case DurinCodeGen::EPropertyGenFlags::Map:
				{
					auto* Map = static_cast<FMapProperty*>(Property);
					VisitProperty(Map->GetKeyProp());
					VisitProperty(Map->GetValueProp());
					break;
				}
				default:
					break;
				}
			};
			Root->ForEachProperty(VisitProperty, true);
			return bValid;
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

		auto EqualType(const FArchiveLogicalTypeDescriptor& A,
			const FArchiveLogicalTypeDescriptor& B) -> bool
		{
			auto EqualPtr = [](const auto& Left, const auto& Right) {
				return (!Left && !Right) || (Left && Right && EqualType(*Left, *Right));
			};
			return A.Kind == B.Kind && A.bSigned == B.bSigned && A.bFloating == B.bFloating
				&& A.BitWidth == B.BitWidth && A.QualifiedType == B.QualifiedType
				&& A.NativeFieldVersion == B.NativeFieldVersion
				&& A.FixedArrayDimension == B.FixedArrayDimension
				&& EqualPtr(A.ElementType, B.ElementType) && EqualPtr(A.KeyType, B.KeyType)
				&& EqualPtr(A.ValueType, B.ValueType);
		}

		auto EqualNode(const FCapturedNode& A, const FCapturedNode& B) -> bool
		{
			if (A.Kind != B.Kind || A.Index != B.Index || A.Children.size() != B.Children.size()) return false;
			if (A.Kind == ENodeKind::Field
				&& (A.Field.DeclaringType != B.Field.DeclaringType || A.Field.Name != B.Field.Name
					|| A.Field.ArrayDimension != B.Field.ArrayDimension
					|| A.Field.PropertyFlags != B.Field.PropertyFlags
					|| !EqualType(A.Field.LogicalType, B.Field.LogicalType))) return false;
			for (size_t Index = 0; Index < A.Children.size(); ++Index)
				if (!EqualNode(A.Children[Index], B.Children[Index])) return false;
			return true;
		}

		auto EqualManifest(const FCapturedPackage& A, const FCapturedPackage& B) -> bool
		{
			if (A.Dependencies != B.Dependencies
				|| A.InternalReferences != B.InternalReferences
				|| A.Objects.size() != B.Objects.size()
				|| A.BulkPayloads.size() != B.BulkPayloads.size()) return false;
			for (size_t Index = 0; Index < A.BulkPayloads.size(); ++Index)
			{
				const auto& Left = A.BulkPayloads[Index].Descriptor;
				const auto& Right = B.BulkPayloads[Index].Descriptor;
				if (Left.PayloadId != Right.PayloadId
					|| Left.LogicalByteCount != Right.LogicalByteCount
					|| Left.StoredByteCount != Right.StoredByteCount
					|| Left.ContentHash != Right.ContentHash
					|| Left.SegmentOffset != Right.SegmentOffset
					|| Left.Alignment != Right.Alignment) return false;
			}
			for (size_t ObjectIndex = 0; ObjectIndex < A.Objects.size(); ++ObjectIndex)
			{
				const auto& Left = A.Objects[ObjectIndex];
				const auto& Right = B.Objects[ObjectIndex];
				if (Left.Id != Right.Id || Left.OuterId != Right.OuterId
					|| Left.ClassName != Right.ClassName || Left.ObjectName != Right.ObjectName
					|| Left.Fields.size() != Right.Fields.size()) return false;
				for (size_t FieldIndex = 0; FieldIndex < Left.Fields.size(); ++FieldIndex)
					if (!EqualNode(Left.Fields[FieldIndex], Right.Fields[FieldIndex])) return false;
			}
			return true;
		}

		class FAuthoredLoadArchive final : public FObjectArchive
		{
		public:
			FAuthoredLoadArchive(
				DObject& InObject,
				std::span<const FAuthoredPackageFieldRecord> InFields,
				std::span<DObject* const> InObjects,
				const FPackagePath& InPackagePath,
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
				, Object(InObject), Fields(InFields), Objects(InObjects), PackagePath(InPackagePath),
				  Consumed(InFields.size(), 0)
			{
			}

			auto GetAssetError() const -> EAssetError
			{
				if (bAssetErrorSet) return AssetError;
				const FArchiveFailure* Failure = GetFailure();
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

			auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override
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
				if (!DastVersion || DastVersion->Version != ObjectPackage::DastV9FormatVersion)
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
					FByteArray Bytes(static_cast<size_t>(StoredSize));
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
					Value.PackageResource = GetPackageResourceManager().FindPackage(
						PackagePath.ToString());
					if (!Value.PackageResource)
						FailLoad(EAssetError::CorruptFile, EArchiveFailureCode::InvalidData,
							"DAST package bulk resource was not registered before graph load.");
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
					FAssetResult Result = FAssetRuntimeState::Get().GetLoadService().LoadObject(
						Path, nullptr, Value);
					if (!Result)
					{
						FailLoad(EAssetError::MissingDependency,
							EArchiveFailureCode::InvalidObjectReference, Result.Message);
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
					&& UnwrapFixed(Path.Type).Kind == FArchiveLogicalTypeDescriptor::EKind::Struct)
					PrepareStruct(Path.Struct, Path.Type);
				PathTypes.pop_back();
			}

			auto OnLeaveField() -> void override
			{
				if (Stack.empty()) return;
				FLoadScope& Scope = Stack.back();
				if (!HasError() && Scope.Record)
				{
					if (UnwrapFixed(Scope.Type).Kind == FArchiveLogicalTypeDescriptor::EKind::Struct)
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
			FPackagePath PackagePath;
			std::vector<uint8> Consumed;
			std::vector<FLoadScope> Stack;
			std::vector<FPathType> PathTypes;
			std::unordered_map<std::string, std::vector<FName>> LoadedDeprecatedProperties;
			EAssetError AssetError = EAssetError::CorruptFile;
			bool bAssetErrorSet = false;
		};

		class FAuthoredCaptureArchive final : public FObjectArchive
		{
		public:
			FAuthoredCaptureArchive(
				const std::unordered_map<DObject*, uint64>& InObjectIds,
				const FAssetPackageSerializationOptions& InOptions,
				bool bInCapturePayload,
				uint32 TargetFormatVersion,
				FXxHash128 InContainerHash)
				: FObjectArchive(MakeArchiveState(InOptions, bInCapturePayload), FArchiveVersionContext{
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), TargetFormatVersion}}, {}})
				, ObjectIds(InObjectIds), Options(InOptions), bCapturePayload(bInCapturePayload)
				, ContainerHash(InContainerHash)
			{
				EnableCapabilities(EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
					| EArchiveCapability::CanonicalMapOrder | EArchiveCapability::ObjectReferences
					| EArchiveCapability::SoftObjectReferences | EArchiveCapability::MultiPassDiscovery);
			}

			auto TakePackage() -> FCapturedPackage
			{
				Package.Dependencies.assign(Dependencies.begin(), Dependencies.end());
				std::ranges::sort(Package.Dependencies, {}, [](const FPackagePath& Path) {
					return Path.GetView();
				});
				Package.HardReferenceTargets.assign(
					HardReferenceTargets.begin(), HardReferenceTargets.end());
				std::ranges::sort(Package.HardReferenceTargets);
				return std::move(Package);
			}

			auto SerializeRawBytes(std::span<std::byte> Bytes) -> void override
			{
				if (NodeStack.empty() || !NodeStack.back())
				{
					if (SuppressedDepth == 0)
						Fail(EArchiveFailureCode::MalformedSerializer,
							"Authored raw bytes require an active named field.");
					return;
				}
				if (!bCapturePayload) return;
				NodeStack.back()->Raw.insert(
					NodeStack.back()->Raw.end(), Bytes.begin(), Bytes.end());
			}

			auto SerializeBulkData(
				FArchiveBulkDataValue& Value,
				const FArchiveBulkDataParameters& Parameters) -> void override
			{
				if (HasError() || SuppressedDepth != 0) return;
				const FArchiveFormatVersion* DastVersion =
					GetVersionContext().FindFormat(FName("DAST"));
				if (!DastVersion || DastVersion->Version != ObjectPackage::DastV9FormatVersion)
				{
					Fail(EArchiveFailureCode::InvalidData,
						"Package bulk fields require a supported DAST package version.");
					return;
				}
				const bool bCooked = IsCooking();
				uint64 FieldIndex = Package.BulkPayloads.size() + 1;
				if (bCooked && !Value.PayloadId.IsValid())
					Value.PayloadId = {0x434f4f4bu, static_cast<uint32>(FieldIndex >> 32),
						static_cast<uint32>(FieldIndex), 0x4649454cu};
				if (bCooked && Value.ContentHash.IsZero()
					&& Value.Buffer.GetSize() == Value.LogicalSize)
					Value.ContentHash = FXxHash128::HashBuffer(Value.Buffer.GetBytes());
				if (!Value.PayloadId.IsValid() || Value.LogicalSize != Value.StoredSize
					|| (bCapturePayload && (Value.Buffer.GetSize() != Value.LogicalSize
						|| FXxHash128::HashBuffer(Value.Buffer.GetBytes()) != Value.ContentHash)))
				{
					Fail(EArchiveFailureCode::InvalidData,
						"Package bulk capture requires valid metadata and verified resident bytes.");
					return;
				}
				const bool bExternal = Value.LogicalSize > EditorBulkDataExternalThreshold
					&& Parameters.StoragePolicy != EArchiveBulkDataStoragePolicy::ForceInline;
				const uint32 RequestedAlignment = bCooked
					? Parameters.Alignment : EditorBulkDataExternalAlignment;
				const uint32 Alignment = bExternal ? RequestedAlignment : 1;
				if (bExternal && (Alignment == 0 || Alignment > 4096
					|| (Alignment & (Alignment - 1)) != 0))
				{
					Fail(EArchiveFailureCode::InvalidAlignment,
						"Package bulk field alignment is invalid.");
					return;
				}
				const uint64 SegmentOffset = bExternal
					? (NextExternalOffset + Alignment - 1) & ~static_cast<uint64>(Alignment - 1)
					: 0;
				if (bExternal)
				{
					if (SegmentOffset > PackageBulkDataMaximumSegmentBytes
						|| Value.StoredSize > PackageBulkDataMaximumSegmentBytes - SegmentOffset)
					{
						Fail(EArchiveFailureCode::LimitExceeded,
							"Package bulk segment exceeds the 1 GiB limit.");
						return;
					}
					NextExternalOffset = SegmentOffset + Value.StoredSize;
				}
				Value.StorageKind = bExternal
					? EArchiveBulkDataStorageKind::External
					: EArchiveBulkDataStorageKind::Inline;
				Value.ContainerHash = bExternal ? ContainerHash : FXxHash128{};
				FEditorBulkDataStorageDescriptor Descriptor{
					.PayloadId = Value.PayloadId,
					.LogicalByteCount = Value.LogicalSize,
					.StoredByteCount = Value.StoredSize,
					.ContentHash = Value.ContentHash,
					.ContainerHash = Value.ContainerHash,
					.StorageKind = bExternal ? EEditorBulkDataStorageKind::External
						: EEditorBulkDataStorageKind::Inline,
					.SegmentOffset = SegmentOffset,
					.Alignment = Alignment};
				Package.BulkPayloads.push_back({Descriptor, Value.Buffer});
				if (NodeStack.empty() || !NodeStack.back())
				{
					Fail(EArchiveFailureCode::MalformedSerializer,
						"Package bulk capture requires an active value node.");
					return;
				}
				FCapturedNode& Node = *NodeStack.back();
				Node.BulkBytes.assign(Value.Buffer.GetBytes().begin(),
					Value.Buffer.GetBytes().end());
				Node.BulkElementSize = Parameters.ElementSize;
				Node.BulkAlignment = Alignment;
				Node.BulkStorage = bExternal ? 1 : 0;
				Node.bDetachedBulk = true;

				uint8 Placement = bExternal ? 1 : 0;
				uint8 StorageFlags = 0;
				uint16 WireAlignment = static_cast<uint16>(Alignment);
				uint32 ContentIdVersion = EditorBulkDataContentIdVersion;
				uint64 HashLow = Value.ContentHash.HashLow;
				uint64 HashHigh = Value.ContentHash.HashHigh;
				FGuid PayloadId = Value.PayloadId;
				uint64 LogicalSize = Value.LogicalSize;
				uint64 StoredSize = Value.StoredSize;
				uint64 WireSegmentOffset = SegmentOffset;
				*this << FieldIndex << Placement << StorageFlags << WireAlignment
					<< ContentIdVersion << PayloadId << HashLow << HashHigh
					<< LogicalSize << StoredSize << WireSegmentOffset;
				if (!bExternal) WriteBytes(Value.Buffer.GetBytes());
			}

			auto SerializeObjectReference(DObject*& Value) -> void override
			{
				if (HasError() || SuppressedDepth != 0) return;
				uint8 Kind = 0;
				uint64 Id = 0;
				std::string_view ExternalPath;
				if (Value)
				{
					if (auto It = ObjectIds.find(Value); It != ObjectIds.end())
					{
						Kind = 1;
						Id = It->second;
						if (!CurrentObject)
						{
							Fail(EArchiveFailureCode::MalformedSerializer,
								"An internal reference was serialized outside an object scope.");
							return;
						}
						Package.InternalReferences.emplace_back(CurrentObject->Id, Id);
					}
					else
					{
						DPackage* ExternalPackage = Value->GetPackage();
						FObjectPath TargetPath;
						if (!ExternalPackage
							|| !FObjectPath::TryCreate(Value->GetObjectPath(), TargetPath))
						{
							Fail(EArchiveFailureCode::InvalidObjectReference,
								"Cross-package hard references must target an exact persistent object.");
							return;
						}
						Kind = 2;
						Dependencies.insert(TargetPath.GetPackagePath());
						HardReferenceTargets.insert(TargetPath);
						ExternalPaths.push_back(TargetPath.ToString());
						ExternalPath = ExternalPaths.back();
					}
				}
				Append(Kind);
				if (Kind == 1) Append(Id);
				else if (Kind == 2) AppendString(ExternalPath);
			}

			auto SerializeSoftObjectValue(FObjectPath& Value) -> void override
			{
				if (HasError() || SuppressedDepth != 0) return;
				const uint8 Kind = Value.IsValid() ? 1 : 0;
				Append(Kind);
				if (Kind == 0) return;
				const std::string Path = Value.ToString();
				if (Path.empty() || Path.size() > MaximumPackageStringBytes)
				{
					Fail(EArchiveFailureCode::InvalidPath,
						"Soft object path exceeds the authored package bound.");
					return;
				}
				AppendString(Path);
			}

		protected:
			auto OnResolvePropertySaveValue(
				FProperty& Property,
				const void* Container,
				uint32 ArrayIndex,
				FArchivePropertySaveValue& OutValue) -> EArchivePropertySaveDisposition override
			{
				OutValue = {Container, ArrayIndex};
				if (!CurrentDObject || !Options.SaveOverrides)
					return EArchivePropertySaveDisposition::LiveValue;
				const FObjectSaveOverride* ObjectOverride =
					Options.SaveOverrides->FindObject(*CurrentDObject);
				if (!ObjectOverride) return EArchivePropertySaveDisposition::LiveValue;
				auto It = std::ranges::find(
					ObjectOverride->Properties, &Property, &FPropertySaveOverride::Property);
				if (It == ObjectOverride->Properties.end())
					return EArchivePropertySaveDisposition::LiveValue;
				if (It->Kind == EPropertySaveOverrideKind::Omit)
					return EArchivePropertySaveDisposition::Omit;

				auto StorageIt = std::ranges::find_if(ReplacementValues,
					[&](const FResolvedReplacement& Value) {
						return Value.Object == CurrentDObject && Value.Property == &Property;
					});
				if (StorageIt == ReplacementValues.end())
				{
					FResolvedReplacement& Value = ReplacementValues.emplace_back();
					Value.Object = CurrentDObject;
					Value.Property = &Property;
					std::string Error;
					if (!Value.Storage.DefaultConstruct(&Property, ArrayIndex, &Error)
						|| !RestorePropertyValue(
							&Property, Value.Storage.GetContainer(), ArrayIndex,
							It->Replacement, &Error))
					{
						SetError(Error.empty() ? "The save override replacement could not be materialized." : Error);
						return EArchivePropertySaveDisposition::Omit;
					}
					StorageIt = std::prev(ReplacementValues.end());
				}
				OutValue = {StorageIt->Storage.GetContainer(), StorageIt->Storage.GetArrayIndex()};
				return EArchivePropertySaveDisposition::ReplacementValue;
			}

			auto OnEnterObject(DObject& Object) -> void override
			{
				auto It = ObjectIds.find(&Object);
				if (It == ObjectIds.end())
				{
					Fail(EArchiveFailureCode::InvalidObjectReference,
						"The serializer entered an object outside the frozen package graph.");
					return;
				}
				FCapturedObject& Record = Package.Objects.emplace_back();
				Record.Id = It->second;
				Record.ClassName = Object.GetClass()->GetQualifiedName().ToString();
				Record.ObjectName = Object.GetName();
				if (Object.GetOuter() != Object.GetPackage())
				{
					auto OuterIt = ObjectIds.find(Object.GetOuter());
					if (OuterIt == ObjectIds.end())
					{
						Fail(EArchiveFailureCode::InvalidObjectReference,
							"Package inner object has an outer outside the frozen graph.");
						return;
					}
					Record.OuterId = OuterIt->second;
				}
				CurrentObject = &Record;
			}

			auto OnLeaveObject() -> void override
			{
				CurrentObject = nullptr;
				NodeStack.clear();
			}

			auto OnEnterField(const FArchiveFieldDescriptor& Field) -> void override
			{
				const bool bTopLevel = NodeStack.empty();
				FProperty* Property = bTopLevel
					? FindObjectProperty(CurrentDObject, Field.Name)
					: FindReflectedProperty(Field);
				const bool bSuppress = SuppressedDepth != 0 || (bTopLevel && Property
					&& Options.PropertyFilter && !Options.PropertyFilter(CurrentDObject, Property));
				if (bSuppress)
				{
					++SuppressedDepth;
					NodeStack.push_back(nullptr);
					return;
				}
				if (!CurrentObject)
				{
					Fail(EArchiveFailureCode::MalformedSerializer,
						"An authored field was entered outside an object scope.");
					NodeStack.push_back(nullptr);
					return;
				}
				auto& Children = NodeStack.empty() ? CurrentObject->Fields : NodeStack.back()->Children;
				FCapturedNode& Node = Children.emplace_back();
				Node.Kind = ENodeKind::Field;
				Node.Field = Field;
				Node.ReflectedProperty = Property;
				NodeStack.push_back(&Node);
			}

			auto OnLeaveField() -> void override
			{
				if (NodeStack.empty()) return;
				if (!NodeStack.back() && SuppressedDepth != 0) --SuppressedDepth;
				NodeStack.pop_back();
			}

			auto OnEnterFixedArrayElement(uint64 Index) -> void override { PushPath(ENodeKind::Fixed, Index); }
			auto OnEnterArrayElement(uint64 Index) -> void override { PushPath(ENodeKind::Array, Index); }
			auto OnEnterMapKey(uint64 Index) -> void override { PushPath(ENodeKind::MapKey, Index); }
			auto OnEnterMapValue(uint64 Index) -> void override { PushPath(ENodeKind::MapValue, Index); }
			auto OnLeavePath() -> void override
			{
				if (!NodeStack.empty()) NodeStack.pop_back();
			}

		private:
			static auto MakeArchiveState(
				const FAssetPackageSerializationOptions& InOptions,
				bool bInCapturePayload) -> FArchiveState
			{
				const bool bCooked = InOptions.Domain == EAssetPackageSaveDomain::Cooked;
				auto PlatformName = [](ECookTargetPlatform Platform) -> std::string {
					switch (Platform)
					{
					case ECookTargetPlatform::Win64: return "Win64";
					default: return {};
					}
				};
				auto ProfileName = [](ECookTargetProfile Profile) -> std::string {
					switch (Profile)
					{
					case ECookTargetProfile::Game: return "Game";
					case ECookTargetProfile::EditorValidation: return "EditorValidation";
					default: return {};
					}
				};
				return {
					.Direction = EArchiveDirection::Save,
					.Purpose = bCooked ? EArchivePurpose::CookedPackage
						: (bInCapturePayload ? EArchivePurpose::AuthoredPackage
							: EArchivePurpose::Discovery),
					.Capabilities = EArchiveCapability::None,
					.bPersistent = bCooked,
					.bCooking = bCooked,
					.bFilterEditorOnly = bCooked && !InOptions.bRetainEditorOnlyData,
					.Target = {PlatformName(InOptions.TargetPlatform),
						ProfileName(InOptions.TargetProfile)}};
			}

			struct FResolvedReplacement
			{
				const DObject* Object = nullptr;
				const FProperty* Property = nullptr;
				FReflectedValueStorage Storage;
			};

			template<typename T> auto Append(const T& Value) -> void
			{
				if (!bCapturePayload || SuppressedDepth != 0) return;
				if (NodeStack.empty() || !NodeStack.back())
				{
					Fail(EArchiveFailureCode::MalformedSerializer,
						"Authored values require an active named field.");
					return;
				}
				const auto Bytes = std::as_bytes(std::span{&Value, 1});
				NodeStack.back()->Raw.insert(
					NodeStack.back()->Raw.end(), Bytes.begin(), Bytes.end());
			}

			auto AppendString(std::string_view Value) -> void
			{
				Append(uint64(Value.size()));
				if (!bCapturePayload || SuppressedDepth != 0 || NodeStack.empty() || !NodeStack.back()) return;
				const auto Bytes = std::as_bytes(std::span(Value));
				NodeStack.back()->Raw.insert(
					NodeStack.back()->Raw.end(), Bytes.begin(), Bytes.end());
			}

			auto PushPath(ENodeKind Kind, uint64 Index) -> void
			{
				if (NodeStack.empty() || !NodeStack.back())
				{
					NodeStack.push_back(nullptr);
					return;
				}
				FCapturedNode& Node = NodeStack.back()->Children.emplace_back();
				Node.Kind = Kind;
				Node.Index = Index;
				NodeStack.push_back(&Node);
			}

			const std::unordered_map<DObject*, uint64>& ObjectIds;
			const FAssetPackageSerializationOptions& Options;
			bool bCapturePayload = false;
			FCapturedPackage Package;
			FCapturedObject* CurrentObject = nullptr;
			DObject* CurrentDObject = nullptr;
			std::vector<FCapturedNode*> NodeStack;
			uint32 SuppressedDepth = 0;
			std::unordered_set<FPackagePath> Dependencies;
			std::unordered_set<FObjectPath> HardReferenceTargets;
			std::vector<std::string> ExternalPaths;
			std::vector<FResolvedReplacement> ReplacementValues;
			FXxHash128 ContainerHash;
			uint64 NextExternalOffset = 0;

		public:
			auto SetCurrentObject(DObject* Object) -> void { CurrentDObject = Object; }
		};

		auto TranslateArchiveFailure(const FArchive& Archive) -> FAssetResult
		{
			const FArchiveFailure* Failure = Archive.GetFailure();
			if (!Failure) return {};
			EAssetError Error = EAssetError::UnsupportedProperty;
			switch (Failure->Code)
			{
			case EArchiveFailureCode::UnsupportedVersion: Error = EAssetError::UnsupportedVersion; break;
			case EArchiveFailureCode::InvalidObjectReference: Error = EAssetError::InvalidObjectGraph; break;
			case EArchiveFailureCode::InvalidPath: Error = EAssetError::InvalidPath; break;
			default: break;
			}
			return {Error, std::string(Archive.GetError())};
		}

		auto EncodeValue(const FCapturedNode& Node,
			const FArchiveLogicalTypeDescriptor& Type, FByteWriter& Writer) -> bool;

		auto GatherObjects(DObject* Object, std::vector<DObject*>& OutObjects) -> void
		{
			if (!Object) return;
			OutObjects.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(
				Object, EObjectQueryScope::LiveOnly))
				GatherObjects(Inner, OutObjects);
		}

		auto HasFrozenObjectGraph(DObject* Root, std::span<DObject* const> Frozen) -> bool
		{
			std::vector<DObject*> Current;
			GatherObjects(Root, Current);
			return std::ranges::equal(Current, Frozen);
		}

		auto HasFrozenPackageGraph(DPackage* Package,
			std::span<DObject* const> Frozen) -> bool
		{
			std::vector<DObject*> Current;
			if (Package) for (DObject* Asset : Package->GetTopLevelAssets())
				GatherObjects(Asset, Current);
			return std::ranges::equal(Current, Frozen);
		}

		auto IsObjectOmitted(
			const DObject* Object,
			const FObjectSaveOverrides* Overrides) -> bool
		{
			for (const DObject* Candidate = Object; Candidate; Candidate = Candidate->GetOuter())
			{
				const FObjectSaveOverride* Override = Overrides
					? Overrides->FindObject(*Candidate) : nullptr;
				if (Override && Override->bOmitObject) return true;
			}
			return false;
		}

		auto PruneUnreachableCookedObjects(
			const FCapturedPackage& Discovery,
			std::vector<DObject*>& Objects) -> bool
		{
			if (Discovery.Objects.size() != Objects.size() || Objects.empty()) return false;
			std::vector<uint8> Reachable(Objects.size(), 0);
			Reachable[0] = 1;
			bool bChanged = true;
			while (bChanged)
			{
				bChanged = false;
				for (const auto& [SourceId, TargetId] : Discovery.InternalReferences)
				{
					if (SourceId == 0 || SourceId > Reachable.size()
						|| TargetId == 0 || TargetId > Reachable.size()) return false;
					if (!Reachable[SourceId - 1] || Reachable[TargetId - 1]) continue;
					uint64 CurrentId = TargetId;
					while (CurrentId != 0 && !Reachable[CurrentId - 1])
					{
						Reachable[CurrentId - 1] = 1;
						bChanged = true;
						CurrentId = Discovery.Objects[CurrentId - 1].OuterId;
					}
				}
			}
			std::vector<DObject*> Filtered;
			Filtered.reserve(Objects.size());
			for (size_t Index = 0; Index < Objects.size(); ++Index)
				if (Reachable[Index]) Filtered.push_back(Objects[Index]);
			Objects = std::move(Filtered);
			return true;
		}

		auto CapturePackage(
			std::span<DObject* const> Objects,
			const std::unordered_map<DObject*, uint64>& ObjectIds,
			const FAssetPackageSerializationOptions& Options,
			bool bCapturePayload,
			uint32 TargetFormatVersion,
			FXxHash128 ContainerHash,
			FCapturedPackage& OutPackage) -> FAssetResult
		{
			FAuthoredCaptureArchive Archive(
				ObjectIds, Options, bCapturePayload, TargetFormatVersion, ContainerHash);
			for (DObject* Object : Objects)
			{
				Archive.SetCurrentObject(Object);
				{
					auto Scope = Archive.EnterObject(*Object);
					if (Options.Domain == EAssetPackageSaveDomain::Cooked)
						Object->SerializeCooked(Archive);
					else Object->Serialize(Archive);
				}
				if (Archive.HasError()) return TranslateArchiveFailure(Archive);
			}
			OutPackage = Archive.TakePackage();
			return {};
		}

		auto ComputeContainerHash(std::span<const FEditorBulkDataStoragePayload> Payloads) -> FXxHash128
		{
			std::vector<const FEditorBulkDataStoragePayload*> Sorted;
			for (const auto& Payload : Payloads)
				if (Payload.Descriptor.LogicalByteCount >= EditorBulkDataExternalThreshold)
					Sorted.push_back(&Payload);
			std::ranges::sort(Sorted, {}, [](const FEditorBulkDataStoragePayload* Payload) {
				return Payload->Descriptor.PayloadId;
			});
			if (Sorted.empty()) return {};
			FByteArray Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::BulkData);
			uint64 Count = Sorted.size();
			Writer << Count;
			for (const FEditorBulkDataStoragePayload* Payload : Sorted)
			{
				FGuid PayloadId = Payload->Descriptor.PayloadId;
				FGuid ReservedIdentity;
				uint32 ReservedVersion = 0;
				uint64 LogicalBytes = Payload->Descriptor.LogicalByteCount;
				uint64 StoredBytes = Payload->Descriptor.StoredByteCount;
				uint64 HashLow = Payload->Descriptor.ContentHash.HashLow;
				uint64 HashHigh = Payload->Descriptor.ContentHash.HashHigh;
				Writer << PayloadId << ReservedIdentity << ReservedVersion << LogicalBytes
					<< StoredBytes << HashLow << HashHigh;
			}
			return Writer.HasError() ? FXxHash128{} : FXxHash128::HashBuffer(Bytes);
		}

		auto FindLinkerSchema(std::span<const ObjectPackage::FSerializedSchema> Schemas,
			std::string_view Name) -> const ObjectPackage::FSerializedSchema*
		{
			const auto It = std::ranges::find(Schemas, Name,
				&ObjectPackage::FSerializedSchema::QualifiedName);
			return It == Schemas.end() ? nullptr : &*It;
		}

		auto AdaptLinkerType(const FArchiveLogicalTypeDescriptor& Input,
			ObjectPackage::FSerializedType& OutType, std::string& OutError) -> bool
		{
			using K = FArchiveLogicalTypeDescriptor::EKind;
			using O = ObjectPackage::EValueKind;
			ObjectPackage::FSerializedType Type;
			switch (Input.Kind)
			{
			case K::Scalar:
				if (Input.bFloating) Type.Kind = Input.BitWidth == 32 ? O::F32 : O::F64;
				else if (Input.bSigned) Type.Kind = Input.BitWidth == 8 ? O::I8
					: Input.BitWidth == 16 ? O::I16 : Input.BitWidth == 32 ? O::I32 : O::I64;
				else Type.Kind = Input.BitWidth == 8 ? O::U8
					: Input.BitWidth == 16 ? O::U16 : Input.BitWidth == 32 ? O::U32 : O::U64;
				break;
			case K::Enum:
				Type.Kind = O::Enum;
				Type.QualifiedName = Input.QualifiedType.ToString();
				Type.Parameter = static_cast<uint64>(Input.BitWidth == 8 ? O::U8
					: Input.BitWidth == 16 ? O::U16 : Input.BitWidth == 32 ? O::U32 : O::U64);
				break;
			case K::String: Type.Kind = O::String; break;
			case K::Name: Type.Kind = O::Name; break;
			case K::Guid: Type.Kind = O::Guid; break;
			case K::Bytes: Type.Kind = O::Bytes; break;
			case K::BulkData: Type.Kind = O::BulkData; break;
			case K::Object:
				Type.Kind = O::HardReference; Type.QualifiedName = Input.QualifiedType.ToString(); break;
			case K::SoftObject:
				Type.Kind = O::SoftReference; Type.QualifiedName = Input.QualifiedType.ToString(); break;
			case K::WeakObject:
				OutError = "Archive weak-object values cannot be represented by the package linker.";
				return false;
			case K::Struct:
				Type.Kind = O::Struct;
				Type.QualifiedName = Input.QualifiedType.ToString();
				break;
			case K::Array: case K::FixedArray:
			{
				ObjectPackage::FSerializedType Element;
				if (!Input.ElementType || !AdaptLinkerType(*Input.ElementType, Element, OutError)) break;
				Type.Kind = Input.Kind == K::Array ? O::Array : O::FixedArray;
				Type.Parameter = Input.Kind == K::FixedArray ? Input.FixedArrayDimension : 0;
				Type.Children.push_back(std::move(Element));
				OutType = std::move(Type);
				return true;
			}
			case K::Map:
			{
				ObjectPackage::FSerializedType Key, Value;
				if (!Input.KeyType || !Input.ValueType
					|| !AdaptLinkerType(*Input.KeyType, Key, OutError)
					|| !AdaptLinkerType(*Input.ValueType, Value, OutError)) break;
				Type.Kind = O::Map;
				Type.Children = {std::move(Key), std::move(Value)};
				OutType = std::move(Type);
				return true;
			}
			}
			OutType = std::move(Type);
			return true;
		}

		auto DiscoverLinkerField(const FCapturedNode& Node,
			ObjectPackage::FLinkerTables& Linker, std::string& OutError) -> bool
		{
			if (Node.Kind != ENodeKind::Field)
			{
				OutError = "A discovered field node has the wrong event kind.";
				return false;
			}
			std::function<bool(const FCapturedNode&)> DiscoverChildren = [&](const FCapturedNode& Child) {
				if (Child.Kind == ENodeKind::Field) return DiscoverLinkerField(Child, Linker, OutError);
				for (const FCapturedNode& Nested : Child.Children)
					if (!DiscoverChildren(Nested)) return false;
				return true;
			};
			for (const FCapturedNode& Child : Node.Children)
				if (!DiscoverChildren(Child)) return false;

			ObjectPackage::FSerializedType Type;
			if (!AdaptLinkerType(Node.Field.LogicalType, Type, OutError)) return false;
			if (Node.ReflectedProperty
				&& Node.ReflectedProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Bool)
				Type = {.Kind = ObjectPackage::EValueKind::Bool};
			const std::string SchemaName = Node.Field.DeclaringType.ToString();
			const std::string FieldName = Node.Field.Name.ToString();
			auto Schema = std::ranges::find(Linker.Schemas, SchemaName,
				&ObjectPackage::FSerializedSchema::QualifiedName);
			if (Schema == Linker.Schemas.end())
			{
				Linker.Schemas.push_back({SchemaName, {}});
				Schema = std::prev(Linker.Schemas.end());
			}
			auto Existing = std::ranges::find(Schema->Fields, FieldName,
				&ObjectPackage::FSerializedField::Name);
			if (Existing == Schema->Fields.end()) Schema->Fields.push_back({FieldName, Type, 0});
			else if (Existing->Type != Type)
			{
				OutError = std::format("Repeated field discovery changed the logical type of {}::{}.",
					SchemaName, FieldName);
				return false;
			}
			Linker.Types.push_back(std::move(Type));
			return true;
		}

		auto ExpandLinkerType(const ObjectPackage::FSerializedType& Input,
			std::span<const ObjectPackage::FSerializedSchema> Schemas,
			ObjectPackage::FSerializedType& OutType, std::string& OutError,
			uint32 Depth = 0) -> bool
		{
			if (Depth > ObjectPackage::DastV8MaximumValueDepth)
			{
				OutError = "Live reflected type exceeds the package nesting limit.";
				return false;
			}
			ObjectPackage::FSerializedType Type{
				.Kind = Input.Kind,
				.QualifiedName = Input.QualifiedName,
				.Parameter = Input.Parameter};
			if (Input.Kind == ObjectPackage::EValueKind::Struct)
			{
				if (const auto* Schema = FindLinkerSchema(Schemas, Input.QualifiedName))
					for (const auto& Field : Schema->Fields)
					{
						ObjectPackage::FSerializedType Child;
						if (!ExpandLinkerType(Field.Type, Schemas, Child, OutError, Depth + 1))
							return false;
						Type.Children.push_back(std::move(Child));
					}
			}
			else for (const auto& InputChild : Input.Children)
			{
				ObjectPackage::FSerializedType Child;
				if (!ExpandLinkerType(InputChild, Schemas, Child, OutError, Depth + 1))
					return false;
				Type.Children.push_back(std::move(Child));
			}
			OutType = std::move(Type);
			return true;
		}

		auto FinalizeLinkerTypes(ObjectPackage::FLinkerTables& Linker,
			std::string& OutError) -> bool
		{
			const std::vector<ObjectPackage::FSerializedSchema> ShallowSchemas = Linker.Schemas;
			std::vector<ObjectPackage::FSerializedSchema> Schemas;
			Schemas.reserve(ShallowSchemas.size());
			for (const auto& InputSchema : ShallowSchemas)
			{
				ObjectPackage::FSerializedSchema Schema{.QualifiedName = InputSchema.QualifiedName};
				Schema.Fields.reserve(InputSchema.Fields.size());
				for (const auto& InputField : InputSchema.Fields)
				{
					ObjectPackage::FSerializedType Type;
					if (!ExpandLinkerType(InputField.Type, ShallowSchemas, Type, OutError)) return false;
					Schema.Fields.push_back({InputField.Name, std::move(Type), InputField.AuthoredFlags});
				}
				Schemas.push_back(std::move(Schema));
			}
			std::vector<ObjectPackage::FSerializedType> Types;
			Types.reserve(Linker.Types.size());
			for (const auto& InputType : Linker.Types)
			{
				ObjectPackage::FSerializedType Type;
				if (!ExpandLinkerType(InputType, ShallowSchemas, Type, OutError)) return false;
				Types.push_back(std::move(Type));
			}
			Linker.Schemas = std::move(Schemas);
			Linker.Types = std::move(Types);
			return true;
		}

		template<typename T>
		auto ReadCaptured(std::span<const std::byte> Bytes, size_t& Offset, T& Out) -> bool
		{
			if (sizeof(T) > Bytes.size() - Offset) return false;
			std::memcpy(&Out, Bytes.data() + Offset, sizeof(T)); Offset += sizeof(T); return true;
		}

		auto ReadCapturedString(std::span<const std::byte> Bytes, size_t& Offset, std::string& Out) -> bool
		{
			uint64 Size = 0;
			if (!ReadCaptured(Bytes, Offset, Size) || Size > Bytes.size() - Offset) return false;
			Out.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), size_t(Size)); Offset += size_t(Size); return true;
		}

		auto FindDeltaField(const std::vector<FDefaultDeltaFieldPlan>* Fields,
			const FArchiveFieldDescriptor& Descriptor) -> const FDefaultDeltaFieldPlan*
		{
			if (!Fields) return nullptr;
			const auto It = std::ranges::find_if(*Fields, [&](const FDefaultDeltaFieldPlan& Candidate) {
				return Candidate.Descriptor.DeclaringType == Descriptor.DeclaringType
					&& Candidate.Descriptor.Name == Descriptor.Name;
			});
			return It == Fields->end() ? nullptr : &*It;
		}

		auto MaterializeLinkerValue(const FCapturedNode& Node, const FArchiveLogicalTypeDescriptor& Type,
			const FCapturedPackage& Package, std::span<const uint64> InternalReferenceIds,
			ObjectPackage::FLinkerTables& Linker, ObjectPackage::FSerializedValue& Out,
			std::string& OutError, const FDefaultDeltaNode* DeltaNode = nullptr) -> bool
		{
			using K = FArchiveLogicalTypeDescriptor::EKind;
			auto Invalid = [&]() {
				OutError = "Captured Archive events do not match their frozen logical type."; return false;
			};
			if (Type.Kind == K::FixedArray || Type.Kind == K::Array || Type.Kind == K::Map)
			{
				uint64 Count = Type.Kind == K::FixedArray ? Type.FixedArrayDimension : 0;
				if (Type.Kind != K::FixedArray)
				{
					size_t Offset = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Count) || Offset != Node.Raw.size()) return Invalid();
				}
				const uint64 ExpectedChildren = Type.Kind == K::Map ? Count * 2 : Count;
				if (Node.Children.size() != ExpectedChildren) return Invalid();
				for (size_t Index = 0; Index < Node.Children.size(); ++Index)
				{
					const auto* ChildType = Type.Kind == K::Map ? (Index % 2 == 0 ? Type.KeyType.get() : Type.ValueType.get()) : Type.ElementType.get();
					if (!ChildType) return Invalid();
					ObjectPackage::FSerializedValue Child;
					const FDefaultDeltaNode* ChildDelta = DeltaNode && Index < DeltaNode->Elements.size() ? DeltaNode->Elements[Index].get() : nullptr;
					if (!MaterializeLinkerValue(Node.Children[Index], *ChildType, Package, InternalReferenceIds,
						Linker, Child, OutError, ChildDelta)) return false;
					Out.Elements.push_back(std::move(Child));
				}
				return true;
			}
			if (Type.Kind == K::Struct)
			{
				if (!Node.Raw.empty()) return Invalid();
				for (const FCapturedNode& ChildNode : Node.Children)
				{
					const FDefaultDeltaFieldPlan* DeltaField = FindDeltaField(DeltaNode ? &DeltaNode->Fields : nullptr, ChildNode.Field);
					if (DeltaNode && !DeltaField) return Invalid();
					if (DeltaField && DeltaField->Disposition == EDefaultDeltaDisposition::Omitted) continue;
					ObjectPackage::FSerializedValue Child;
					if (!MaterializeLinkerValue(ChildNode, ChildNode.Field.LogicalType, Package, InternalReferenceIds,
						Linker, Child, OutError,
						DeltaField && DeltaField->Value ? DeltaField->Value.get() : nullptr)) return false;
					Out.FieldNames.push_back(ChildNode.Field.Name.ToString());
					Out.Provenances.push_back(DeltaField && DeltaField->Provenance == EDefaultDeltaProvenance::Forced
						? ObjectPackage::EPropertyProvenance::Forced
						: ObjectPackage::EPropertyProvenance::Explicit);
					Out.Elements.push_back(std::move(Child));
				}
				return true;
			}

			size_t Offset = 0;
			switch (Type.Kind)
			{
			case K::Scalar:
				if (Node.Kind == ENodeKind::Field && Node.ReflectedProperty
					&& Node.ReflectedProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Bool)
				{
					uint8 Encoded = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Encoded) || Encoded > 1) return Invalid();
					Out.Bool = Encoded != 0; break;
				}
				if (Type.bFloating)
				{
					if (Type.BitWidth == 32) { uint32 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); Out.FloatingBits = Bits; }
					else if (!ReadCaptured(std::span(Node.Raw), Offset, Out.FloatingBits)) return Invalid();
				}
				else if (Type.BitWidth == 8) { uint8 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); if (Type.bSigned) Out.Signed = int8(Bits); else Out.Unsigned = Bits; }
				else if (Type.BitWidth == 16) { uint16 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); if (Type.bSigned) Out.Signed = int16(Bits); else Out.Unsigned = Bits; }
				else if (Type.BitWidth == 32) { uint32 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); if (Type.bSigned) Out.Signed = int32(Bits); else Out.Unsigned = Bits; }
				else { uint64 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); if (Type.bSigned) Out.Signed = int64(Bits); else Out.Unsigned = Bits; }
				break;
			case K::Enum:
				if (Type.BitWidth == 8) { uint8 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); Out.Unsigned = Bits; }
				else if (Type.BitWidth == 16) { uint16 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); Out.Unsigned = Bits; }
				else if (Type.BitWidth == 32) { uint32 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); Out.Unsigned = Bits; }
				else { uint64 Bits = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Bits)) return Invalid(); Out.Unsigned = Bits; }
				break;
			case K::String: case K::Name:
				if (!ReadCapturedString(Node.Raw, Offset, Out.Text)) return Invalid();
				if (Type.Kind == K::Name && !Out.Text.empty()) Linker.Names.push_back(Out.Text);
				break;
			case K::Guid:
				if (!ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.A) || !ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.B)
					|| !ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.C) || !ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.D)) return Invalid();
				break;
			case K::Bytes:
				Out.Bytes = Node.Raw; Offset = Node.Raw.size(); break;
			case K::BulkData:
				if (!Node.bDetachedBulk || Node.BulkElementSize == 0
					|| Node.BulkAlignment == 0 || Node.BulkStorage > 1) return Invalid();
				Out.Bytes = Node.BulkBytes;
				Out.BulkElementSize = static_cast<uint32>(Node.BulkElementSize);
				Out.BulkAlignment = Node.BulkAlignment;
				Out.BulkStorage = Node.BulkStorage == 0
					? ObjectPackage::EBulkStorageKind::Inline
					: ObjectPackage::EBulkStorageKind::External;
				Offset = Node.Raw.size();
				break;
			case K::Object:
			{
				uint8 ReferenceTag = 0;
				if (!ReadCaptured(std::span(Node.Raw), Offset, ReferenceTag) || ReferenceTag > 2) return Invalid();
				if (ReferenceTag == 1)
				{
					uint64 CapturedId = 0;
					if (!ReadCaptured(std::span(Node.Raw), Offset, CapturedId) || CapturedId == 0
						|| CapturedId > InternalReferenceIds.size()) return Invalid();
					if (!ObjectPackage::FPackageIndex::TryExport(
						InternalReferenceIds[CapturedId - 1] - 1, Out.Reference)) return Invalid();
				}
				if (ReferenceTag == 2)
				{
					std::string Path; if (!ReadCapturedString(Node.Raw, Offset, Path)) return Invalid();
					const auto It = std::ranges::find_if(Package.HardReferenceTargets,
						[&](const FObjectPath& Value) { return Value.ToString() == Path; });
					if (It == Package.HardReferenceTargets.end()) return Invalid();
					if (!ObjectPackage::FPackageIndex::TryImport(
						std::distance(Package.HardReferenceTargets.begin(), It), Out.Reference)) return Invalid();
				}
				break;
			}
			case K::SoftObject:
			{
				uint8 ReferenceTag = 0;
				if (!ReadCaptured(std::span(Node.Raw), Offset, ReferenceTag) || ReferenceTag > 1) return Invalid();
				if (ReferenceTag == 1)
				{
					if (!ReadCapturedString(Node.Raw, Offset, Out.Text)) return Invalid();
					FObjectPath Path;
					if (!FObjectPath::TryCreate(Out.Text, Path)) return Invalid();
					Linker.Names.push_back(Out.Text);
					Linker.Summary.SoftPackageDependencies.push_back(Path.GetPackagePath());
				}
				break;
			}
			default: return Invalid();
			}
			return Offset == Node.Raw.size() || Invalid();
		}

		auto BuildLinkerTables(const FCapturedPackage& Captured, const FAuthoredPackageSummary& Summary,
			const FPackagePath& PackagePath,
			std::span<DObject* const> Objects, const FDefaultDeltaPlan& DeltaPlan,
			std::span<const ObjectPackage::FCustomVersion> CustomVersions,
			std::span<DObject* const> TopLevelAssets,
			ObjectPackage::FLinkerTables& Out, std::string& OutError) -> bool
		{
			ObjectPackage::FLinkerTables Linker;
			Linker.Summary.PackagePath = PackagePath;
			Linker.Summary.HardPackageDependencies = Summary.Dependencies;
			for (const FObjectPath& Target : Captured.HardReferenceTargets)
				Linker.Imports.push_back({.ObjectPath = Target});
			Linker.CustomVersions.assign(CustomVersions.begin(), CustomVersions.end());
			std::vector<std::string> Paths(Captured.Objects.size());
			for (const auto& Object : Captured.Objects)
			{
				if (Object.Id == 0 || Object.Id > Captured.Objects.size() || Object.OuterId >= Object.Id)
				{
					OutError = "Captured object ids are not topological."; return false;
				}
				const std::string OuterPath = Object.OuterId == 0 ? std::string{} : Paths[Object.OuterId - 1];
				const std::string Path = OuterPath.empty() ? Object.ObjectName : OuterPath + "/" + Object.ObjectName;
				Paths[Object.Id - 1] = Path;
				for (const auto& Field : Object.Fields)
					if (!DiscoverLinkerField(Field, Linker, OutError)) return false;
			}
			if (!FinalizeLinkerTypes(Linker, OutError)) return false;
			if (Objects.size() != Captured.Objects.size() || DeltaPlan.Objects.size() != Captured.Objects.size())
			{
				OutError = "Delta plan object graph differs from Archive discovery."; return false;
			}
			std::unordered_map<const DObject*, const FDefaultDeltaObjectPlan*> DeltaObjects;
			for (const FDefaultDeltaObjectPlan& DeltaObject : DeltaPlan.Objects)
			{
				if (!DeltaObject.Object || !DeltaObjects.emplace(DeltaObject.Object, &DeltaObject).second)
				{
					OutError = "Delta plan contains an invalid or duplicate object."; return false;
				}
			}
			std::vector<size_t> CanonicalOrder(Captured.Objects.size());
			for (size_t Index = 0; Index < CanonicalOrder.size(); ++Index) CanonicalOrder[Index] = Index;
			std::ranges::sort(CanonicalOrder, [&](size_t LeftIndex, size_t RightIndex) {
				const auto& Left = Captured.Objects[LeftIndex];
				const auto& Right = Captured.Objects[RightIndex];
				const std::string_view LeftOuter = Left.OuterId == 0 ? std::string_view{} : Paths[Left.OuterId - 1];
				const std::string_view RightOuter = Right.OuterId == 0 ? std::string_view{} : Paths[Right.OuterId - 1];
				if (LeftOuter.empty() != RightOuter.empty()) return LeftOuter.empty();
				if (LeftOuter != RightOuter) return LeftOuter < RightOuter;
				if (Left.ClassName != Right.ClassName) return Left.ClassName < Right.ClassName;
				return Left.ObjectName < Right.ObjectName;
			});
			std::vector<uint64> InternalReferenceIds(Captured.Objects.size());
			for (size_t CanonicalIndex = 0; CanonicalIndex < CanonicalOrder.size(); ++CanonicalIndex)
				InternalReferenceIds[CanonicalOrder[CanonicalIndex]] = CanonicalIndex + 1;
			for (size_t CanonicalIndex = 0; CanonicalIndex < CanonicalOrder.size(); ++CanonicalIndex)
			{
				const size_t SourceIndex = CanonicalOrder[CanonicalIndex];
				const auto& Object = Captured.Objects[SourceIndex];
				ObjectPackage::FPackageIndex Outer;
				if (Object.OuterId != 0 && !ObjectPackage::FPackageIndex::TryExport(
					InternalReferenceIds[Object.OuterId - 1] - 1, Outer))
				{
					OutError = "Captured object has invalid Outer topology."; return false;
				}
				Linker.Exports.push_back({.ObjectName = Object.ObjectName,
					.ClassName = Object.ClassName, .Outer = Outer});
			}

			for (DObject* Asset : TopLevelAssets)
			{
				const auto Source = std::ranges::find(Objects, Asset);
				if (!Asset || Source == Objects.end())
				{
					OutError = "A top-level asset is absent from the captured export topology.";
					return false;
				}
				const size_t SourceIndex = static_cast<size_t>(std::distance(Objects.begin(), Source));
				ObjectPackage::FPackageIndex Export;
				FTopLevelAssetPath AssetPath;
				FObjectPath RedirectDestination;
				if (!ObjectPackage::FPackageIndex::TryExport(
						InternalReferenceIds[SourceIndex] - 1, Export)
					|| !FTopLevelAssetPath::TryCreate(PackagePath, Asset->GetName(), AssetPath))
				{
					OutError = "A top-level asset has invalid linker identity."; return false;
				}
				if (auto* Redirector = Cast<DAssetRedirector>(Asset))
				{
					DObject* Destination = Redirector->GetDestinationObject();
					if (!Destination || Destination == Asset
						|| !FObjectPath::TryCreate(Destination->GetObjectPath(), RedirectDestination))
					{
						OutError = "A top-level redirector destination is invalid."; return false;
					}
				}
				Linker.Summary.TopLevelAssets.push_back({.Export = Export,
					.AssetPath = std::move(AssetPath),
					.ClassName = Asset->GetClass()->GetQualifiedName().ToString(),
					.RedirectDestination = std::move(RedirectDestination)});
			}
			std::ranges::sort(Linker.Summary.TopLevelAssets,
				[](const auto& Left, const auto& Right) { return Left.AssetPath < Right.AssetPath; });

			for (size_t CanonicalIndex = 0; CanonicalIndex < CanonicalOrder.size(); ++CanonicalIndex)
			{
				const size_t SourceIndex = CanonicalOrder[CanonicalIndex];
				const auto& Object = Captured.Objects[SourceIndex];
				const auto DeltaIt = DeltaObjects.find(Objects[SourceIndex]);
				if (DeltaIt == DeltaObjects.end())
				{
					OutError = std::format("Delta plan object graph differs at {}.", Paths[Object.Id - 1]);
					return false;
				}
				const FDefaultDeltaObjectPlan& DeltaObject = *DeltaIt->second;
				for (const auto& Field : Object.Fields)
				{
					const FDefaultDeltaFieldPlan* DeltaField = FindDeltaField(&DeltaObject.Fields, Field.Field);
					if (!DeltaField) { OutError = "Delta plan is missing an Archive field."; return false; }
					if (DeltaField->Disposition == EDefaultDeltaDisposition::Omitted) continue;
					const std::string SchemaName = Field.Field.DeclaringType.ToString();
					const std::string FieldName = Field.Field.Name.ToString();
					const auto* Schema = FindLinkerSchema(Linker.Schemas, SchemaName);
					const auto SchemaField = Schema ? std::ranges::find(
						Schema->Fields, FieldName, &ObjectPackage::FSerializedField::Name)
						: std::vector<ObjectPackage::FSerializedField>::const_iterator{};
					if (!Schema || SchemaField == Schema->Fields.end())
					{
						OutError = "A captured field is absent from its linker schema."; return false;
					}
					ObjectPackage::FSerializedValue Value;
					if (!MaterializeLinkerValue(Field, Field.Field.LogicalType, Captured, InternalReferenceIds,
						Linker, Value, OutError,
						DeltaField->Value ? DeltaField->Value.get() : nullptr)) return false;
					Linker.Exports[CanonicalIndex].Properties.push_back({
						.DeclaringType = SchemaName,
						.FieldName = FieldName,
						.Type = SchemaField->Type,
						.Provenance = DeltaField->Provenance == EDefaultDeltaProvenance::Forced
							? ObjectPackage::EPropertyProvenance::Forced
							: ObjectPackage::EPropertyProvenance::Explicit,
						.Value = std::move(Value)});
				}
			}
			std::ranges::sort(Linker.Summary.SoftPackageDependencies);
			Linker.Summary.SoftPackageDependencies.erase(std::ranges::unique(
				Linker.Summary.SoftPackageDependencies).begin(),
				Linker.Summary.SoftPackageDependencies.end());
			Out = std::move(Linker); return true;
		}

	}

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		const FPackagePath& PackagePath,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions,
		const FArchiveState& Context) -> FAssetResult
	{
		FArchiveVersionContext VersionContext{
			{FArchiveFormatVersion{FName("DAST"), SourceVersion}},
			std::vector<FArchiveCustomVersion>(CustomVersions.begin(), CustomVersions.end())};
		FAuthoredLoadArchive Archive(
			Object, Fields, Objects, PackagePath, SourceVersion, CustomVersions, Context);
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
		const FAssetPackageSerializationOptions& InputOptions,
		ObjectPackage::FLinkerTables& OutLinker, std::string* OutError) -> FAssetResult
	{
		std::vector<FEditorBulkDataStoragePayload> Payloads;
		FAssetPackageSerializationOptions Options = InputOptions;
		Options.EditorBulkDataStoragePayloads = &Payloads;
		struct FCaptureDiagnostic
		{
			std::string LogicalPath;
			std::string Message;
			auto Reset() -> void { *this = {}; }
		} Diagnostic;
		auto Finish = [&](FAssetResult Result) {
			if (OutError)
				*OutError = Result ? std::string{} : Diagnostic.Message;
			return Result;
		};
		if (Package && !Package->IsAssetPackage())
		{
			Diagnostic = {{}, "Only asset packages can be serialized."};
			return Finish({EAssetError::InvalidPackageType, Diagnostic.Message});
		}
		if (!Package || Package->GetTopLevelAssets().empty())
		{
			Diagnostic = {{}, "Package has no top-level assets."};
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		if (Options.Domain == EAssetPackageSaveDomain::Cooked
			&& (Options.TargetPlatform == ECookTargetPlatform::Invalid
				|| Options.TargetProfile == ECookTargetProfile::Invalid))
		{
			Diagnostic = {{},
				"Cooked package serialization requires an explicit target platform and profile."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		FPackagePath PackagePath;
		if (!FPackagePath::TryCreate(Package->GetPackagePath(), PackagePath))
		{
			Diagnostic = {{}, "Package has an invalid asset path."};
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}

		std::vector<DObject*> FrozenObjects;
		for (DObject* Asset : Package->GetTopLevelAssets())
			AssetPrivate::GatherObjects(Asset, FrozenObjects);
		if (Options.SaveOverrides)
		{
			for (const FObjectSaveOverride& Override : Options.SaveOverrides->GetObjects())
			{
				if (!Override.Object
					|| std::ranges::find(FrozenObjects, Override.Object) == FrozenObjects.end())
				{
					Diagnostic = {{},
						"A save override targets an object outside the frozen package graph."};
					return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
				}
			}
			for (DObject* Asset : Package->GetTopLevelAssets())
				if (const FObjectSaveOverride* RootOverride =
					Options.SaveOverrides->FindObject(*Asset);
					RootOverride && RootOverride->bOmitObject)
				{
					Diagnostic = {{},
						"A top-level asset cannot be omitted from its own package save."};
					return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
				}
		}
		std::vector<DObject*> Objects;
		for (DObject* Object : FrozenObjects)
			if (!AssetPrivate::IsObjectOmitted(Object, Options.SaveOverrides.get()))
				Objects.push_back(Object);
		std::unordered_map<DObject*, uint64> ObjectIds;
		for (size_t Index = 0; Index < Objects.size(); ++Index) ObjectIds.emplace(Objects[Index], Index + 1);
		AssetPrivate::FCapturedPackage Discovery;
		FAssetResult Result = AssetPrivate::CapturePackage(
			Objects, ObjectIds, Options, false,
			OrdinaryAssetPackageWriterVersion, {}, Discovery);
		if (!Result)
		{
			Diagnostic = {{}, Result.Message}; return Finish(Result);
		}
		if (!AssetPrivate::HasFrozenPackageGraph(Package, FrozenObjects))
		{
			Diagnostic = {{}, "Archive discovery mutated the frozen package object graph."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		if (Options.Domain == EAssetPackageSaveDomain::Cooked
			&& !Options.bRetainEditorOnlyData)
		{
			if (!AssetPrivate::PruneUnreachableCookedObjects(Discovery, Objects))
			{
				Diagnostic = {{},
					"Cooked object reachability discovery produced an invalid graph."};
				return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
			}
			ObjectIds.clear();
			for (size_t Index = 0; Index < Objects.size(); ++Index)
				ObjectIds.emplace(Objects[Index], Index + 1);
			Result = AssetPrivate::CapturePackage(
				Objects, ObjectIds, Options, false,
				OrdinaryAssetPackageWriterVersion, {}, Discovery);
			if (!Result)
			{
				Diagnostic = {{}, Result.Message};
				return Finish(Result);
			}
		}
		const FXxHash128 ContainerHash = AssetPrivate::ComputeContainerHash(Discovery.BulkPayloads);
		if (!Discovery.BulkPayloads.empty()
			&& std::ranges::any_of(Discovery.BulkPayloads, [](const FEditorBulkDataStoragePayload& Payload) {
				return Payload.Descriptor.LogicalByteCount > EditorBulkDataExternalThreshold;
			}) && ContainerHash.IsZero())
		{
			Diagnostic = {{},
				"Authored bulk container identity could not be computed."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		AssetPrivate::FCapturedPackage Captured;
		Result = AssetPrivate::CapturePackage(
			Objects, ObjectIds, Options, true,
			OrdinaryAssetPackageWriterVersion, ContainerHash, Captured);
		if (!Result)
		{
			Diagnostic = {{}, Result.Message}; return Finish(Result);
		}
		if (!Options.EditorBulkDataStoragePayloads
			&& std::ranges::any_of(Captured.BulkPayloads, [](const FEditorBulkDataStoragePayload& Payload) {
				return Payload.Descriptor.StorageKind == EEditorBulkDataStorageKind::External;
			}))
		{
			Diagnostic = {{},
				"External authored bulk data requires a package publication payload collector."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		if (Options.EditorBulkDataStoragePayloads)
		{
			Options.EditorBulkDataStoragePayloads->clear();
			for (const FEditorBulkDataStoragePayload& Payload : Captured.BulkPayloads)
				Options.EditorBulkDataStoragePayloads->push_back(Payload);
		}
		if (!AssetPrivate::HasFrozenPackageGraph(Package, FrozenObjects)
			|| !AssetPrivate::EqualManifest(Discovery, Captured))
		{
			Diagnostic = {{}, "Archive emission changed the frozen object, field, type, dependency, or version manifest."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}

		AssetPrivate::FAuthoredPackageSummary Summary;
		DObject* FirstAsset = Package->GetTopLevelAssets().front();
		Summary.AssetClassName = FirstAsset->GetClass()->GetQualifiedName().ToString();
		Summary.Dependencies = Captured.Dependencies;
		if (auto* Redirector = Cast<DAssetRedirector>(FirstAsset))
		{
			Summary.EntryKind = EAssetRegistryEntryKind::Redirector;
			DObject* Destination = Redirector->GetDestinationObject();
			DPackage* DestinationPackage = Destination ? Destination->GetPackage() : nullptr;
			if (!DestinationPackage || !FPackagePath::TryCreate(
					DestinationPackage->GetPackagePath(), Summary.RedirectDestination)
				|| Destination == FirstAsset)
			{
				Diagnostic = {{}, "Redirector destination is invalid."};
				return Finish({EAssetError::CorruptFile, Diagnostic.Message});
			}
		}

		FDefaultDeltaPlan DeltaPlan;
		FDefaultDeltaDiagnostic DeltaDiagnostic;
		const EDefaultDeltaMode EffectiveDeltaMode =
			Options.Domain == EAssetPackageSaveDomain::Cooked
				|| (Options.SaveOverrides && !Options.SaveOverrides->IsEmpty())
			? EDefaultDeltaMode::NoDelta : DeltaMode;
		FArchiveState DeltaContext;
		if (Options.Domain == EAssetPackageSaveDomain::Cooked)
		{
			DeltaContext.bPersistent = true;
			DeltaContext.bCooking = true;
			DeltaContext.bFilterEditorOnly = !Options.bRetainEditorOnlyData;
			DeltaContext.Target.Platform = "Win64";
			DeltaContext.Target.Profile = Options.TargetProfile == ECookTargetProfile::Game
				? "Game" : "EditorValidation";
		}
		bool bDeltaBuilt = true;
		for (DObject* Asset : Package->GetTopLevelAssets())
		{
			FDefaultDeltaPlan AssetPlan;
			if (!BuildDefaultDeltaPlan(
				Asset, EffectiveDeltaMode, AssetPlan, &DeltaDiagnostic, DeltaContext))
			{
				bDeltaBuilt = false;
				break;
			}
			DeltaPlan.Objects.insert(DeltaPlan.Objects.end(),
				std::make_move_iterator(AssetPlan.Objects.begin()),
				std::make_move_iterator(AssetPlan.Objects.end()));
		}
		if (!bDeltaBuilt)
		{
			auto ReasonName = [](EDefaultDeltaFailureReason Reason) -> std::string_view {
				switch (Reason)
				{
				case EDefaultDeltaFailureReason::InvalidInput: return "InvalidInput";
				case EDefaultDeltaFailureReason::MissingClassDefault: return "MissingClassDefault";
				case EDefaultDeltaFailureReason::DefaultObjectGraphFailure: return "DefaultObjectGraphFailure";
				case EDefaultDeltaFailureReason::ArchiveFailure: return "ArchiveFailure";
				case EDefaultDeltaFailureReason::ManifestMismatch: return "ManifestMismatch";
				case EDefaultDeltaFailureReason::DuplicateField: return "DuplicateField";
				case EDefaultDeltaFailureReason::UnsupportedLogicalType: return "UnsupportedLogicalType";
				case EDefaultDeltaFailureReason::UnsupportedIdentity: return "UnsupportedIdentity";
				case EDefaultDeltaFailureReason::MissingStructDefault: return "MissingStructDefault";
				case EDefaultDeltaFailureReason::DepthLimit: return "DepthLimit";
				case EDefaultDeltaFailureReason::FieldLimit: return "FieldLimit";
				case EDefaultDeltaFailureReason::PathLimit: return "PathLimit";
				case EDefaultDeltaFailureReason::AuthoredOverrideFailure: return "AuthoredOverrideFailure";
				default: return "Unknown";
				}
			};
			std::string Message = std::format(
				"Default-relative logical planning failed: reason={}, path='{}'",
				ReasonName(DeltaDiagnostic.Reason), DeltaDiagnostic.LogicalPath);
			if (DeltaDiagnostic.ApplicableLimit != 0)
				Message += std::format(", observed={}, limit={}",
					DeltaDiagnostic.ObservedValue, DeltaDiagnostic.ApplicableLimit);
			Message += ".";
			Diagnostic = {DeltaDiagnostic.LogicalPath,
				std::move(Message)};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		std::erase_if(DeltaPlan.Objects, [&](const FDefaultDeltaObjectPlan& ObjectPlan) {
			return std::ranges::find(Objects, ObjectPlan.Object) == Objects.end();
		});
		std::vector<ObjectPackage::FCustomVersion> CustomVersions;
		std::string LinkerError;
		if (!AssetPrivate::BuildLinkerTables(Captured, Summary, PackagePath, Objects,
				DeltaPlan, CustomVersions, Package->GetTopLevelAssets(), OutLinker, LinkerError))
		{
			Diagnostic.Message = std::move(LinkerError);
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		Diagnostic.Reset();
		return Finish({});
	}

}
