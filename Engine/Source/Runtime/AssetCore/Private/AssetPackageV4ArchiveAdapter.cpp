#include "AssetPackageArchive.h"
#include "AssetSystemInternal.h"
#include "AssetPackageVersionPolicy.h"
#include "AssetPackageV4Writer.h"
#include "AssetPackageValueCodec.h"

#include "AssetRedirector.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"

namespace Durin::Asset::Private
{
	namespace
	{
		constexpr std::string_view RedirectorClassName = "Durin::Asset::DAssetRedirector";

		enum class ENodeKind : uint8 { Field, Fixed, Array, MapKey, MapValue };

		struct FCapturedNode
		{
			ENodeKind Kind = ENodeKind::Field;
			uint64 Index = 0;
			FArchiveFieldDescriptor Field;
			FProperty* ReflectedProperty = nullptr;
			std::vector<uint8> Raw;
			std::vector<FCapturedNode> Children;
		};

		struct FCapturedObject
		{
			uint64 Id = 0;
			uint64 OuterId = 0;
			std::string ClassName;
			std::string ObjectName;
			std::vector<FCapturedNode> Fields;
		};

		struct FCapturedPackage
		{
			std::vector<FCapturedObject> Objects;
			std::vector<FAssetPath> Dependencies;
		};

		auto FindReflectedProperty(const FArchiveFieldDescriptor& Descriptor) -> FProperty*
		{
			if (Descriptor.DeclaringType.IsNone() || Descriptor.Name.IsNone()) return nullptr;
			for (DObject* Object : GDObjectArray.GetAll(EObjectQueryScope::IncludeTemplates))
			{
				auto* Struct = Cast<DStruct>(Object);
				if (!Struct || Struct->GetQualifiedName() != Descriptor.DeclaringType) continue;
				FProperty* Result = nullptr;
				Struct->ForEachProperty([&](FProperty* Property) {
					if (Property && Property->NamePrivate == Descriptor.Name) Result = Property;
				}, false);
				return Result;
			}
			return nullptr;
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
			if (A.Dependencies != B.Dependencies || A.Objects.size() != B.Objects.size()) return false;
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
				uint32 SourceVersion,
				std::span<const FArchiveCustomVersion> CustomVersions)
				: FObjectArchive({EArchiveDirection::Load, EArchivePurpose::AuthoredPackage,
					EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
					| EArchiveCapability::ObjectReferences
					| EArchiveCapability::SoftObjectReferences
					| EArchiveCapability::RemainingPayload | EArchiveCapability::CustomVersions},
					FArchiveVersionContext{
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), SourceVersion}},
						std::vector<FArchiveCustomVersion>(CustomVersions.begin(), CustomVersions.end())})
				, Object(InObject), Fields(InFields), Objects(InObjects), Consumed(InFields.size(), 0)
			{
			}

			auto GetAssetError() const -> EAssetError
			{
				if (bAssetErrorSet) return AssetError;
				const FArchiveFailure* Failure = GetFailure();
				if (Failure && (Failure->Code == EArchiveFailureCode::UnsupportedType
					|| Failure->Code == EArchiveFailureCode::MalformedSerializer
					|| Failure->Code == EArchiveFailureCode::MissingBaseReflectedFields
					|| Failure->Message.find("OperationUnavailable") != std::string::npos))
					return EAssetError::UnsupportedProperty;
				return EAssetError::CorruptFile;
			}

			auto HasUnconsumedFields() const -> bool
			{
				return std::ranges::find(Consumed, uint8{0}) != Consumed.end();
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
					FAssetPath Path;
					if (!FAssetPath::TryCreate(PathString, Path))
					{
						FailLoad(EAssetError::InvalidPath, EArchiveFailureCode::InvalidPath,
							"Invalid external object reference.");
						return;
					}
					FAssetResult Result = FAssetRuntimeState::Get().LoadAsset(Path, Value);
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

			auto SerializeSoftObjectPath(FSoftObjectPath& Value) -> void override
			{
				if (HasError() || !IsCurrentFieldAvailable()) return;
				uint8 Kind = 0;
				if (!Read(Kind)) return;
				if (Kind == 0) { Value.Reset(); return; }
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
				FSoftObjectPath Loaded;
				if (!FSoftObjectPath::TryCreate(PathString, Loaded, &Error))
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
				const auto ExpectedKind = Property ? Property->GetKind()
					: GetNativeKind(Descriptor.LogicalType);
				const std::string ExpectedSignature = Property
					? GetSerializedTypeSignature(Property)
					: GetNativeTypeSignature(Descriptor.LogicalType);
				const std::string DeclaringType = Descriptor.DeclaringType.ToString();
				const std::string Name = Descriptor.Name.ToString();
				bool bFoundIdentity = false;
				for (size_t Index = 0; Index < Candidates.size(); ++Index)
				{
					const auto& Candidate = Candidates[Index];
					if (CandidateConsumed[Index]
						|| Candidate.DeclaringClass != DeclaringType || Candidate.Name != Name) continue;
					bFoundIdentity = true;
					if (Candidate.Kind != ExpectedKind
						|| Candidate.TypeSignature != ExpectedSignature) continue;
					CandidateConsumed[Index] = 1;
					FLoadScope Scope;
					Scope.Record = &Candidate;
					Scope.Type = Descriptor.LogicalType;
					Stack.push_back(std::move(Scope));
					return;
				}
				if (!bTopLevel && bFoundIdentity)
				{
					FailLoad(EAssetError::TypeMismatch, EArchiveFailureCode::InvalidData,
						std::format("Serialized struct field {}::{} is incompatible with the current schema.",
							DeclaringType, Name));
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
						auto Bytes = std::as_writable_bytes(std::span<uint8>(Field.Payload));
						SerializeRawBytes(Bytes);
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
			std::vector<uint8> Consumed;
			std::vector<FLoadScope> Stack;
			std::vector<FPathType> PathTypes;
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
				uint32 TargetFormatVersion)
				: FObjectArchive({EArchiveDirection::Save,
					bInCapturePayload ? EArchivePurpose::AuthoredPackage : EArchivePurpose::Discovery,
					EArchiveCapability::None}, FArchiveVersionContext{
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), TargetFormatVersion}}, {}})
				, ObjectIds(InObjectIds), Options(InOptions), bCapturePayload(bInCapturePayload)
			{
				EnableCapabilities(EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
					| EArchiveCapability::CanonicalMapOrder | EArchiveCapability::ObjectReferences
					| EArchiveCapability::SoftObjectReferences | EArchiveCapability::MultiPassDiscovery);
			}

			auto TakePackage() -> FCapturedPackage
			{
				Package.Dependencies.assign(Dependencies.begin(), Dependencies.end());
				std::ranges::sort(Package.Dependencies, {}, [](const FAssetPath& Path) {
					return Path.GetView();
				});
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
				const auto* Data = reinterpret_cast<const uint8*>(Bytes.data());
				NodeStack.back()->Raw.insert(NodeStack.back()->Raw.end(), Data, Data + Bytes.size());
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
					}
					else
					{
						DPackage* ExternalPackage = Value->GetPackage();
						FAssetPath Path;
						if (!ExternalPackage || ExternalPackage->GetAsset() != Value)
						{
							Fail(EArchiveFailureCode::InvalidObjectReference,
								"Cross-package references may only target a package main asset.");
							return;
						}
						if (!FAssetPath::TryCreate(ExternalPackage->GetPackagePath(), Path))
						{
							Fail(EArchiveFailureCode::InvalidPath,
								"Referenced package has an invalid asset path.");
							return;
						}
						Kind = 2;
						Dependencies.insert(Path);
						ExternalPaths.push_back(std::move(Path));
						ExternalPath = ExternalPaths.back().GetView();
					}
				}
				Append(Kind);
				if (Kind == 1) Append(Id);
				else if (Kind == 2) AppendString(ExternalPath);
			}

			auto SerializeSoftObjectPath(FSoftObjectPath& Value) -> void override
			{
				if (HasError() || SuppressedDepth != 0) return;
				const uint8 Kind = Value.IsNull() ? 0 : 1;
				Append(Kind);
				if (Kind == 0) return;
				const std::string_view Path = Value.GetAssetPath().GetView();
				if (Path.empty() || Path.size() > MaximumPackageStringBytes)
				{
					Fail(EArchiveFailureCode::InvalidPath,
						"Soft object path exceeds the authored package bound.");
					return;
				}
				AppendString(Path);
			}

		protected:
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
				if (Record.Id != 1)
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
			template<typename T> auto Append(const T& Value) -> void
			{
				if (!bCapturePayload || SuppressedDepth != 0) return;
				if (NodeStack.empty() || !NodeStack.back())
				{
					Fail(EArchiveFailureCode::MalformedSerializer,
						"Authored values require an active named field.");
					return;
				}
				const auto* Data = reinterpret_cast<const uint8*>(&Value);
				NodeStack.back()->Raw.insert(NodeStack.back()->Raw.end(), Data, Data + sizeof(T));
			}

			auto AppendString(std::string_view Value) -> void
			{
				Append(uint64(Value.size()));
				if (!bCapturePayload || SuppressedDepth != 0 || NodeStack.empty() || !NodeStack.back()) return;
				NodeStack.back()->Raw.insert(NodeStack.back()->Raw.end(), Value.begin(), Value.end());
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
			std::unordered_set<FAssetPath> Dependencies;
			std::vector<FAssetPath> ExternalPaths;

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

			auto CapturePackage(
			std::span<DObject* const> Objects,
			const std::unordered_map<DObject*, uint64>& ObjectIds,
			const FAssetPackageSerializationOptions& Options,
			bool bCapturePayload,
			uint32 TargetFormatVersion,
			FCapturedPackage& OutPackage) -> FAssetResult
		{
			FAuthoredCaptureArchive Archive(
				ObjectIds, Options, bCapturePayload, TargetFormatVersion);
			for (DObject* Object : Objects)
			{
				Archive.SetCurrentObject(Object);
				{
					auto Scope = Archive.EnterObject(*Object);
					Object->Serialize(Archive);
				}
				if (Archive.HasError()) return TranslateArchiveFailure(Archive);
			}
			OutPackage = Archive.TakePackage();
			return {};
		}

		auto AdaptV4Type(const FArchiveLogicalTypeDescriptor& Input,
			DastV4::FTypePtr& OutType, DastV4::FWriterDiagnostic& Diagnostic) -> bool
		{
			using K = FArchiveLogicalTypeDescriptor::EKind;
			using O = DastV4::ETypeOpcode;
			switch (Input.Kind)
			{
			case K::Scalar:
				if (Input.bFloating) OutType = DastV4::MakeType(Input.BitWidth == 32 ? O::F32 : O::F64);
				else if (Input.bSigned) OutType = DastV4::MakeType(Input.BitWidth == 8 ? O::I8 : Input.BitWidth == 16 ? O::I16 : Input.BitWidth == 32 ? O::I32 : O::I64);
				else OutType = DastV4::MakeType(Input.BitWidth == 8 ? O::U8 : Input.BitWidth == 16 ? O::U16 : Input.BitWidth == 32 ? O::U32 : O::U64);
				return true;
			case K::Enum:
				// Authored Archive enum signatures freeze storage width but not signedness;
				// the v4 bridge therefore uses the unsigned opcode.
				OutType = DastV4::MakeType(O::Enum, Input.QualifiedType.ToString(), uint8(
					Input.BitWidth == 8 ? O::U8 : Input.BitWidth == 16 ? O::U16
						: Input.BitWidth == 32 ? O::U32 : O::U64));
				return true;
			case K::String: OutType = DastV4::MakeType(O::String); return true;
			case K::Name: OutType = DastV4::MakeType(O::Name); return true;
			case K::Guid: OutType = DastV4::MakeType(O::Guid); return true;
			case K::Bytes: OutType = DastV4::MakeType(O::Bytes); return true;
			case K::Object: OutType = DastV4::MakeType(O::HardRef, Input.QualifiedType.ToString()); return true;
			case K::SoftObject: OutType = DastV4::MakeType(O::SoftRef, Input.QualifiedType.ToString()); return true;
			case K::Struct: OutType = DastV4::MakeType(O::Struct, Input.QualifiedType.ToString()); return true;
			case K::Array: case K::FixedArray:
			{
				DastV4::FTypePtr Element;
				if (!Input.ElementType || !AdaptV4Type(*Input.ElementType, Element, Diagnostic)) break;
				OutType = DastV4::MakeType(Input.Kind == K::Array ? O::Array : O::FixedArray,
					{}, Input.Kind == K::FixedArray ? Input.FixedArrayDimension : 0, {Element});
				return true;
			}
			case K::Map:
			{
				DastV4::FTypePtr Key, Value;
				if (!Input.KeyType || !Input.ValueType || !AdaptV4Type(*Input.KeyType, Key, Diagnostic)
					|| !AdaptV4Type(*Input.ValueType, Value, Diagnostic)) break;
				OutType = DastV4::MakeType(O::Map, {}, 0, {Key, Value}); return true;
			}
			}
			Diagnostic = {DastV4::EWriterFailure::UnsupportedType, {}, "Archive logical type cannot be represented by DAST v4."};
			return false;
		}

		auto AreV4TypesEquivalent(const DastV4::FTypeDescriptor& Left,
			const DastV4::FTypeDescriptor& Right) -> bool
		{
			if (Left.Opcode != Right.Opcode || Left.QualifiedName != Right.QualifiedName
				|| Left.Parameter != Right.Parameter || Left.Children.size() != Right.Children.size()
				|| Left.bHasDeterministicStructOperations != Right.bHasDeterministicStructOperations
				|| Left.bHasCustomSerializer != Right.bHasCustomSerializer) return false;
			for (size_t Index = 0; Index < Left.Children.size(); ++Index)
				if (!Left.Children[Index] || !Right.Children[Index]
					|| !AreV4TypesEquivalent(*Left.Children[Index], *Right.Children[Index])) return false;
			return true;
		}

		auto DiscoverV4Field(const FCapturedNode& Node, DastV4::FPackageInput& Input,
			DastV4::FWriterDiagnostic& Diagnostic) -> bool
		{
			if (Node.Kind != ENodeKind::Field)
			{
				Diagnostic = {DastV4::EWriterFailure::ManifestMismatch, {}, "A discovered field node has the wrong event kind."};
				return false;
			}
			DastV4::FTypePtr Type;
			if (!AdaptV4Type(Node.Field.LogicalType, Type, Diagnostic)) return false;
			if (Node.ReflectedProperty
				&& Node.ReflectedProperty->GetKind() == DurinCodeGen::EPropertyGenFlags::Bool)
				Type = DastV4::MakeType(DastV4::ETypeOpcode::Bool);
			const std::string SchemaName = Node.Field.DeclaringType.ToString();
			const std::string FieldName = Node.Field.Name.ToString();
			auto Schema = std::ranges::find(Input.Schemas, SchemaName, &DastV4::FSchemaDescriptor::QualifiedName);
			if (Schema == Input.Schemas.end())
			{
				Input.Schemas.push_back({SchemaName, {}});
				Schema = std::prev(Input.Schemas.end());
			}
			auto Existing = std::ranges::find(Schema->Fields, FieldName, &DastV4::FFieldDescriptor::Name);
			if (Existing == Schema->Fields.end()) Schema->Fields.push_back({FieldName, Type, 0});
			else if (!Existing->Type || !AreV4TypesEquivalent(*Existing->Type, *Type))
			{
				Diagnostic = {DastV4::EWriterFailure::ManifestMismatch,
					SchemaName + "::" + FieldName,
					"Repeated field discovery changed its logical type."};
				return false;
			}
			Input.Types.push_back(std::move(Type));
			std::function<bool(const FCapturedNode&)> DiscoverChildren = [&](const FCapturedNode& Child) {
				if (Child.Kind == ENodeKind::Field) return DiscoverV4Field(Child, Input, Diagnostic);
				for (const FCapturedNode& Nested : Child.Children)
					if (!DiscoverChildren(Nested)) return false;
				return true;
			};
			for (const FCapturedNode& Child : Node.Children)
				if (!DiscoverChildren(Child)) return false;
			return true;
		}

		template<typename T>
		auto ReadCaptured(std::span<const uint8> Bytes, size_t& Offset, T& Out) -> bool
		{
			if (sizeof(T) > Bytes.size() - Offset) return false;
			std::memcpy(&Out, Bytes.data() + Offset, sizeof(T)); Offset += sizeof(T); return true;
		}

		auto ReadCapturedString(std::span<const uint8> Bytes, size_t& Offset, std::string& Out) -> bool
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

		auto MaterializeV4Value(const FCapturedNode& Node, const FArchiveLogicalTypeDescriptor& Type,
			const FCapturedPackage& Package, std::span<const uint64> InternalReferenceIds,
			DastV4::FPackageInput& Input, DastV4::FValue& Out,
			DastV4::FWriterDiagnostic& Diagnostic, const FDefaultDeltaNode* DeltaNode = nullptr) -> bool
		{
			using K = FArchiveLogicalTypeDescriptor::EKind;
			auto Invalid = [&]() {
				Diagnostic = {DastV4::EWriterFailure::ManifestMismatch, {}, "Captured Archive events do not match their frozen logical type."}; return false;
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
					DastV4::FValue Child;
					const FDefaultDeltaNode* ChildDelta = DeltaNode && Index < DeltaNode->Elements.size() ? DeltaNode->Elements[Index].get() : nullptr;
					if (!MaterializeV4Value(Node.Children[Index], *ChildType, Package, InternalReferenceIds,
						Input, Child, Diagnostic, ChildDelta)) return false;
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
					DastV4::FValue Child;
					if (!MaterializeV4Value(ChildNode, ChildNode.Field.LogicalType, Package, InternalReferenceIds,
						Input, Child, Diagnostic,
						DeltaField && DeltaField->Value ? DeltaField->Value.get() : nullptr)) return false;
					Out.FieldNames.push_back(ChildNode.Field.Name.ToString());
					Out.Provenances.push_back(DeltaField ? DeltaField->Provenance : EDefaultDeltaProvenance::Explicit);
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
				if (Type.Kind == K::Name && !Out.Text.empty()) Input.AdditionalNames.push_back(Out.Text);
				break;
			case K::Guid:
				if (!ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.A) || !ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.B)
					|| !ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.C) || !ReadCaptured(std::span(Node.Raw), Offset, Out.Guid.D)) return Invalid();
				break;
			case K::Bytes: Out.Bytes = Node.Raw; Offset = Node.Raw.size(); break;
			case K::Object:
				if (!ReadCaptured(std::span(Node.Raw), Offset, Out.ReferenceTag) || Out.ReferenceTag > 2) return Invalid();
				if (Out.ReferenceTag == 1)
				{
					uint64 CapturedId = 0;
					if (!ReadCaptured(std::span(Node.Raw), Offset, CapturedId) || CapturedId == 0
						|| CapturedId > InternalReferenceIds.size()) return Invalid();
					Out.ReferenceId = InternalReferenceIds[CapturedId - 1];
				}
				if (Out.ReferenceTag == 2)
				{
					std::string Path; if (!ReadCapturedString(Node.Raw, Offset, Path)) return Invalid();
					const auto It = std::ranges::find(Package.Dependencies, Path, [](const FAssetPath& Value) { return Value.GetView(); });
					if (It == Package.Dependencies.end()) return Invalid(); Out.ReferenceId = uint64(std::distance(Package.Dependencies.begin(), It) + 1);
				}
				break;
			case K::SoftObject:
				if (!ReadCaptured(std::span(Node.Raw), Offset, Out.ReferenceTag) || Out.ReferenceTag > 1) return Invalid();
				if (Out.ReferenceTag == 1) { if (!ReadCapturedString(Node.Raw, Offset, Out.Text)) return Invalid(); Input.AdditionalNames.push_back(Out.Text); }
				break;
			default: return Invalid();
			}
			return Offset == Node.Raw.size() || Invalid();
		}

		auto BuildV4Input(const FCapturedPackage& Captured, const FAuthoredPackageSummary& Summary,
			std::span<DObject* const> Objects, const FDefaultDeltaPlan& DeltaPlan,
			DastV4::FPackageInput& Out, DastV4::FWriterDiagnostic& Diagnostic) -> bool
		{
			DastV4::FPackageInput Input;
			Input.AssetClass = Summary.AssetClassName;
			Input.EntryKind = Summary.EntryKind;
			Input.RedirectDestination = Summary.RedirectDestination.GetView();
			for (const auto& Dependency : Summary.Dependencies) Input.Dependencies.emplace_back(Dependency.GetView());
			std::vector<std::string> Paths(Captured.Objects.size());
			for (const auto& Object : Captured.Objects)
			{
				if (Object.Id == 0 || Object.Id > Captured.Objects.size() || Object.OuterId >= Object.Id)
				{
					Diagnostic = {DastV4::EWriterFailure::InvalidTopology, {}, "Captured object ids are not topological."}; return false;
				}
				const std::string OuterPath = Object.OuterId == 0 ? std::string{} : Paths[Object.OuterId - 1];
				const std::string Path = OuterPath.empty() ? Object.ObjectName : OuterPath + "/" + Object.ObjectName;
				Paths[Object.Id - 1] = Path;
				Input.Objects.push_back({Path, OuterPath, Object.ClassName, Object.ObjectName});
				for (const auto& Field : Object.Fields) if (!DiscoverV4Field(Field, Input, Diagnostic)) return false;
			}
			if (Objects.size() != Captured.Objects.size() || DeltaPlan.Objects.size() != Captured.Objects.size())
			{
				Diagnostic = {DastV4::EWriterFailure::ManifestMismatch, {}, "Delta plan object graph differs from Archive discovery."}; return false;
			}
			std::unordered_map<const DObject*, const FDefaultDeltaObjectPlan*> DeltaObjects;
			for (const FDefaultDeltaObjectPlan& DeltaObject : DeltaPlan.Objects)
			{
				if (!DeltaObject.Object || !DeltaObjects.emplace(DeltaObject.Object, &DeltaObject).second)
				{
					Diagnostic = {DastV4::EWriterFailure::ManifestMismatch, {}, "Delta plan contains an invalid or duplicate object."}; return false;
				}
			}
			std::vector<size_t> CanonicalOrder(Input.Objects.size());
			for (size_t Index = 0; Index < CanonicalOrder.size(); ++Index) CanonicalOrder[Index] = Index;
			std::ranges::sort(CanonicalOrder, [&](size_t LeftIndex, size_t RightIndex) {
				const auto& Left = Input.Objects[LeftIndex];
				const auto& Right = Input.Objects[RightIndex];
				if (Left.OuterPath.empty() != Right.OuterPath.empty()) return Left.OuterPath.empty();
				if (Left.OuterPath != Right.OuterPath) return Left.OuterPath < Right.OuterPath;
				if (Left.ClassName != Right.ClassName) return Left.ClassName < Right.ClassName;
				return Left.ObjectName < Right.ObjectName;
			});
			std::vector<uint64> InternalReferenceIds(Input.Objects.size());
			for (size_t CanonicalIndex = 0; CanonicalIndex < CanonicalOrder.size(); ++CanonicalIndex)
				InternalReferenceIds[CanonicalOrder[CanonicalIndex]] = CanonicalIndex + 1;
			for (size_t ObjectIndex = 0; ObjectIndex < Captured.Objects.size(); ++ObjectIndex)
			{
				const auto& Object = Captured.Objects[ObjectIndex];
				const auto DeltaIt = DeltaObjects.find(Objects[ObjectIndex]);
				if (DeltaIt == DeltaObjects.end())
				{
					Diagnostic = {DastV4::EWriterFailure::ManifestMismatch, Paths[Object.Id - 1], "Delta plan object graph differs from Archive discovery."}; return false;
				}
				const FDefaultDeltaObjectPlan& DeltaObject = *DeltaIt->second;
				DastV4::FObjectValueInput Values{.ObjectPath = Paths[Object.Id - 1]};
				for (const auto& Field : Object.Fields)
				{
					const FDefaultDeltaFieldPlan* DeltaField = FindDeltaField(&DeltaObject.Fields, Field.Field);
					if (!DeltaField) { Diagnostic = {DastV4::EWriterFailure::ManifestMismatch, {}, "Delta plan is missing an Archive field."}; return false; }
					if (DeltaField->Disposition == EDefaultDeltaDisposition::Omitted) continue;
					DastV4::FValue Value;
					if (!MaterializeV4Value(Field, Field.Field.LogicalType, Captured, InternalReferenceIds,
						Input, Value, Diagnostic,
						DeltaField->Value ? DeltaField->Value.get() : nullptr)) return false;
					Values.KnownOverrides.push_back({Field.Field.DeclaringType.ToString(), Field.Field.Name.ToString(), DeltaField->Provenance, std::move(Value)});
				}
				Input.ObjectValues.push_back(std::move(Values));
			}
			Out = std::move(Input); return true;
		}
	}

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		uint32 SourceVersion,
		std::span<const FArchiveCustomVersion> CustomVersions) -> FAssetResult
	{
		FAuthoredLoadArchive Archive(Object, Fields, Objects, SourceVersion, CustomVersions);
		{
			auto Scope = Archive.EnterObject(Object);
			Object.Serialize(Archive);
		}
		if (Archive.HasError())
			return {Archive.GetAssetError(), std::string(Archive.GetError())};
		if (Archive.HasUnconsumedFields())
			return {EAssetError::UnsupportedProperty,
				"Serialized fields are not present in the live schema."};
		return {};
	}
}

namespace Durin::Asset::DastV4
{
	auto WriteRedirectorPackage(
		const FAssetPath& SourcePath,
		const FAssetPath& DestinationPath,
		std::vector<uint8>& OutBytes) -> FAssetResult
	{
		constexpr std::string_view RedirectorClass = "Durin::Asset::DAssetRedirector";
		auto DestinationType = MakeType(ETypeOpcode::HardRef, "Durin::DObject");
		FPackageInput Input{
			.AssetClass = std::string(RedirectorClass),
			.EntryKind = EAssetRegistryEntryKind::Redirector,
			.RedirectDestination = DestinationPath.ToString(),
			.Dependencies = {DestinationPath.ToString()},
			.Types = {DestinationType},
			.Schemas = {{std::string(RedirectorClass), {{"DestinationObject", DestinationType, 0}}}},
			.Objects = {{std::string(SourcePath.GetAssetName()), {},
				std::string(RedirectorClass), std::string(SourcePath.GetAssetName())}},
			.ObjectValues = {{std::string(SourcePath.GetAssetName()), {{
				.SchemaName = std::string(RedirectorClass),
				.FieldName = "DestinationObject",
				.Value = {.ReferenceTag = 2, .ReferenceId = 1}}}}}};
		FWriterDiagnostic Diagnostic;
		if (!WritePackage(Input, OutBytes, &Diagnostic))
			return {EAssetError::CorruptFile, Diagnostic.Message};
		return {};
	}

	auto WriteAssetPackage(DPackage* Package, std::vector<uint8>& OutBytes,
		const FAssetPackageWriteOptions& Options, FWriterDiagnostic* OutDiagnostic) -> FAssetResult
	{
		FWriterDiagnostic Diagnostic;
		auto Finish = [&](FAssetResult Result) {
			if (OutDiagnostic) *OutDiagnostic = Diagnostic;
			return Result;
		};
		if (Package && !Package->IsAssetPackage())
		{
			Diagnostic = {EWriterFailure::InvalidInput, {}, "Only asset packages can be serialized."};
			return Finish({EAssetError::InvalidPackageType, Diagnostic.Message});
		}
		if (!Package || !Package->GetAsset())
		{
			Diagnostic = {EWriterFailure::InvalidTopology, {}, "Package has no main asset."};
			return Finish({EAssetError::InvalidObjectGraph, Diagnostic.Message});
		}
		FAssetPath PackagePath;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), PackagePath))
		{
			Diagnostic = {EWriterFailure::InvalidInput, {}, "Package has an invalid asset path."};
			return Finish({EAssetError::InvalidPath, Diagnostic.Message});
		}

		std::vector<DObject*> Objects;
		Private::GatherObjects(Package->GetAsset(), Objects);
		std::unordered_map<DObject*, uint64> ObjectIds;
		for (size_t Index = 0; Index < Objects.size(); ++Index) ObjectIds.emplace(Objects[Index], Index + 1);
		Private::FCapturedPackage Discovery;
		FAssetResult Result = Private::CapturePackage(
			Objects, ObjectIds, Options.Serialization, false, Version, Discovery);
		if (!Result)
		{
			Diagnostic = {EWriterFailure::ArchiveFailure, {}, Result.Message}; return Finish(Result);
		}
		if (!Private::HasFrozenObjectGraph(Package->GetAsset(), Objects))
		{
			Diagnostic = {EWriterFailure::ManifestMismatch, {}, "Archive discovery mutated the frozen package object graph."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		Private::FCapturedPackage Captured;
		Result = Private::CapturePackage(
			Objects, ObjectIds, Options.Serialization, true, Version, Captured);
		if (!Result)
		{
			Diagnostic = {EWriterFailure::ArchiveFailure, {}, Result.Message}; return Finish(Result);
		}
		if (!Private::HasFrozenObjectGraph(Package->GetAsset(), Objects)
			|| !Private::EqualManifest(Discovery, Captured))
		{
			Diagnostic = {EWriterFailure::ManifestMismatch, {}, "Archive emission changed the frozen object, field, type, dependency, or version manifest."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}

		Private::FAuthoredPackageSummary Summary;
		Summary.AssetClassName = Package->GetAsset()->GetClass()->GetQualifiedName().ToString();
		Summary.Dependencies = Captured.Dependencies;
		if (auto* Redirector = Cast<DAssetRedirector>(Package->GetAsset()))
		{
			Summary.EntryKind = EAssetRegistryEntryKind::Redirector;
			DObject* Destination = Redirector->GetDestinationObject();
			DPackage* DestinationPackage = Destination ? Destination->GetPackage() : nullptr;
			if (!DestinationPackage || DestinationPackage->GetAsset() != Destination
				|| !FAssetPath::TryCreate(DestinationPackage->GetPackagePath(), Summary.RedirectDestination)
				|| Summary.RedirectDestination == PackagePath)
			{
				Diagnostic = {EWriterFailure::InvalidInput, {}, "Redirector destination is invalid."};
				return Finish({EAssetError::CorruptFile, Diagnostic.Message});
			}
		}

		FDefaultDeltaPlan DeltaPlan;
		FDefaultDeltaDiagnostic DeltaDiagnostic;
		if (!BuildDefaultDeltaPlan(Package->GetAsset(), Options.DeltaMode, DeltaPlan, &DeltaDiagnostic))
		{
			Diagnostic = {EWriterFailure::DeltaPlanFailure, DeltaDiagnostic.LogicalPath,
				"Default-relative logical planning failed."};
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		}
		FPackageInput Input;
		if (!Private::BuildV4Input(Captured, Summary, Objects, DeltaPlan, Input, Diagnostic))
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		std::vector<uint8> Bytes;
		if (!WritePackage(Input, Bytes, &Diagnostic))
			return Finish({EAssetError::UnsupportedProperty, Diagnostic.Message});
		OutBytes = std::move(Bytes);
		Diagnostic.Reset();
		return Finish({});
	}
}
