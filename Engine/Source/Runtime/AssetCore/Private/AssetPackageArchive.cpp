#include "AssetPackageArchive.h"
#include "AssetPackageValueCodec.h"

#include "AssetRedirector.h"
#include "DObject/Archive.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Object.h"
#include "DObject/Package.h"
#include "DObject/SoftObjectPtr.h"

#include <cstring>
#include <ranges>
#include <unordered_map>
#include <unordered_set>

namespace Durin::Asset::Private
{
	namespace
	{
		constexpr uint32 AssetMagic = 0x54534144; // DAST
		constexpr uint32 AssetVersion = 3;
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
			for (DObject* Object : GDObjectArray.GetAll())
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

		class FAuthoredLoadArchive final : public FArchive
		{
		public:
			FAuthoredLoadArchive(
				DObject& InObject,
				std::span<const FAuthoredPackageFieldRecord> InFields,
				std::span<DObject* const> InObjects,
				uint32 SourceVersion)
				: FArchive({EArchiveDirection::Load, EArchivePurpose::AuthoredPackage,
					EArchiveCapability::StructuredFields | EArchiveCapability::RawBytes
					| EArchiveCapability::ObjectReferences
					| EArchiveCapability::SoftObjectReferences
					| EArchiveCapability::RemainingPayload},
					FArchiveVersionContext{
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), SourceVersion}}, {}})
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

			auto TakeLegacyFields() -> std::vector<FAssetLegacyField>
			{
				std::vector<FAssetLegacyField> Result;
				for (size_t Index = 0; Index < Fields.size(); ++Index)
				{
					if (Consumed[Index]) continue;
					const auto& Field = Fields[Index];
					Result.push_back({
						.DeclaringClass = Field.DeclaringClass,
						.Name = Field.Name,
						.Kind = Field.Kind,
						.TypeSignature = Field.TypeSignature,
						.Payload = Field.Payload});
				}
				return Result;
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
					FAssetResult Result = FAssetManager::Get().LoadAsset(Path, Value);
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

		class FAuthoredCaptureArchive final : public FArchive
		{
		public:
			FAuthoredCaptureArchive(
				const std::unordered_map<DObject*, uint64>& InObjectIds,
				const FAssetPackageSerializationOptions& InOptions,
				bool bInCapturePayload)
				: FArchive({EArchiveDirection::Save,
					bInCapturePayload ? EArchivePurpose::AuthoredPackage : EArchivePurpose::Discovery,
					EArchiveCapability::None}, FArchiveVersionContext{
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), AssetVersion}}, {}})
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

		auto WriteField(const FCapturedNode& Node, FByteWriter& Writer) -> bool
		{
			if (Node.Kind != ENodeKind::Field) return false;
			const auto Kind = Node.ReflectedProperty ? Node.ReflectedProperty->GetKind()
				: GetNativeKind(Node.Field.LogicalType);
			const std::string Signature = Node.ReflectedProperty
				? GetSerializedTypeSignature(Node.ReflectedProperty)
				: GetNativeTypeSignature(Node.Field.LogicalType);
			FByteWriter Payload;
			if (!EncodeValue(Node, Node.Field.LogicalType, Payload)) return false;
			Writer.WriteString(Node.Field.DeclaringType.ToString());
			Writer.WriteString(Node.Field.Name.ToString());
			Writer.Write(uint8(Kind));
			Writer.WriteString(Signature);
			Writer.Write(uint64(Payload.Bytes.size()));
			Writer.WriteBytes(Payload.Bytes);
			return true;
		}

		auto EncodeValue(const FCapturedNode& Node,
			const FArchiveLogicalTypeDescriptor& Type, FByteWriter& Writer) -> bool
		{
			using EKind = FArchiveLogicalTypeDescriptor::EKind;
			if (Type.Kind == EKind::FixedArray)
			{
				if (!Type.ElementType || Node.Children.size() != Type.FixedArrayDimension) return false;
				for (size_t Index = 0; Index < Node.Children.size(); ++Index)
					if (Node.Children[Index].Kind != ENodeKind::Fixed
						|| Node.Children[Index].Index != Index
						|| !EncodeValue(Node.Children[Index], *Type.ElementType, Writer)) return false;
				return Node.Raw.empty();
			}
			if (Type.Kind == EKind::Struct)
			{
				if (!Node.Raw.empty()) return false;
				Writer.WriteString(Type.QualifiedType.ToString());
				Writer.Write(uint64(Node.Children.size()));
				for (const FCapturedNode& Child : Node.Children)
					if (!WriteField(Child, Writer)) return false;
				return true;
			}
			if (Type.Kind == EKind::Array)
			{
				if (!Type.ElementType || Node.Raw.size() != sizeof(uint64)) return false;
				uint64 Count = 0;
				std::memcpy(&Count, Node.Raw.data(), sizeof(Count));
				if (Node.Children.size() != Count) return false;
				Writer.WriteBytes(Node.Raw);
				for (size_t Index = 0; Index < Node.Children.size(); ++Index)
					if (Node.Children[Index].Kind != ENodeKind::Array
						|| Node.Children[Index].Index != Index
						|| !EncodeValue(Node.Children[Index], *Type.ElementType, Writer)) return false;
				return true;
			}
			if (Type.Kind == EKind::Map)
			{
				if (!Type.KeyType || !Type.ValueType || Node.Raw.size() != sizeof(uint64)) return false;
				uint64 Count = 0;
				std::memcpy(&Count, Node.Raw.data(), sizeof(Count));
				if (Node.Children.size() != Count * 2) return false;
				Writer.WriteBytes(Node.Raw);
				for (uint64 Index = 0; Index < Count; ++Index)
				{
					const auto& Key = Node.Children[static_cast<size_t>(Index * 2)];
					const auto& Value = Node.Children[static_cast<size_t>(Index * 2 + 1)];
					if (Key.Kind != ENodeKind::MapKey || Value.Kind != ENodeKind::MapValue
						|| Key.Index != Index || Value.Index != Index
						|| !EncodeValue(Key, *Type.KeyType, Writer)
						|| !EncodeValue(Value, *Type.ValueType, Writer)) return false;
				}
				return true;
			}
			if (!Node.Children.empty()) return false;
			Writer.WriteBytes(Node.Raw);
			return true;
		}

		auto GatherObjects(DObject* Object, std::vector<DObject*>& OutObjects) -> void
		{
			if (!Object) return;
			OutObjects.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(Object)) GatherObjects(Inner, OutObjects);
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
			FCapturedPackage& OutPackage) -> FAssetResult
		{
			FAuthoredCaptureArchive Archive(ObjectIds, Options, bCapturePayload);
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

		auto WritePackage(const FCapturedPackage& Package,
			const FAuthoredPackageSummary& Summary, std::vector<uint8>& OutBytes) -> FAssetResult
		{
			FByteWriter Writer;
			Writer.Write(AssetMagic);
			Writer.Write(AssetVersion);
			Writer.WriteString(Summary.AssetClassName);
			Writer.Write(uint8(Summary.EntryKind));
			Writer.WriteString(Summary.RedirectDestination.GetView());
			Writer.Write(uint64(Summary.Dependencies.size()));
			for (const FAssetPath& Dependency : Summary.Dependencies) Writer.WriteString(Dependency.GetView());
			Writer.Write(uint64(Package.Objects.size()));
			for (const FCapturedObject& Object : Package.Objects)
			{
				Writer.Write(Object.Id);
				Writer.Write(Object.OuterId);
				Writer.WriteString(Object.ClassName);
				Writer.WriteString(Object.ObjectName);
				Writer.Write(uint64(Object.Fields.size()));
				for (const FCapturedNode& Field : Object.Fields)
					if (!WriteField(Field, Writer))
						return {EAssetError::UnsupportedProperty,
							"ArchiveFailure:MalformedSerializer:: Authored field events do not match their frozen logical type."};
			}
			OutBytes = std::move(Writer.Bytes);
			return {};
		}
	}

	auto BuildAuthoredPackageBytes(
		DPackage* Package,
		std::vector<uint8>& OutBytes,
		FAuthoredPackageSummary& OutSummary,
		const FAssetPackageSerializationOptions& Options) -> FAssetResult
	{
		if (Package && !Package->IsAssetPackage())
			return {EAssetError::InvalidPackageType, "Only asset packages can be serialized."};
		if (!Package || !Package->GetAsset())
			return {EAssetError::InvalidObjectGraph, "Package has no main asset."};
		FAssetPath PackagePath;
		if (!FAssetPath::TryCreate(Package->GetPackagePath(), PackagePath))
			return {EAssetError::InvalidPath, "Package has an invalid asset path."};

		std::vector<DObject*> Objects;
		GatherObjects(Package->GetAsset(), Objects);
		std::unordered_map<DObject*, uint64> ObjectIds;
		for (size_t Index = 0; Index < Objects.size(); ++Index) ObjectIds.emplace(Objects[Index], Index + 1);

		FCapturedPackage Discovery;
		FAssetResult Result = CapturePackage(Objects, ObjectIds, Options, false, Discovery);
		if (!Result) return Result;
		if (!HasFrozenObjectGraph(Package->GetAsset(), Objects))
			return {EAssetError::UnsupportedProperty,
				"ArchiveFailure:MalformedSerializer:: Authored discovery mutated the frozen package object graph."};
		FCapturedPackage Saved;
		Result = CapturePackage(Objects, ObjectIds, Options, true, Saved);
		if (!Result) return Result;
		if (!HasFrozenObjectGraph(Package->GetAsset(), Objects))
			return {EAssetError::UnsupportedProperty,
				"ArchiveFailure:MalformedSerializer:: Authored emission mutated the frozen package object graph."};
		if (!EqualManifest(Discovery, Saved))
			return {EAssetError::UnsupportedProperty,
				"ArchiveFailure:MalformedSerializer:: Authored serializer grew the frozen field, type, dependency, object, or version manifest during emission."};

		FAuthoredPackageSummary Summary;
		Summary.AssetClassName = Package->GetAsset()->GetClass()->GetQualifiedName().ToString();
		Summary.Dependencies = Saved.Dependencies;
		if (auto* Redirector = Cast<DAssetRedirector>(Package->GetAsset()))
		{
			Summary.EntryKind = EAssetRegistryEntryKind::Redirector;
			DObject* Destination = Redirector->GetDestinationObject();
			DPackage* DestinationPackage = Destination ? Destination->GetPackage() : nullptr;
			if (!DestinationPackage || DestinationPackage->GetAsset() != Destination
				|| !FAssetPath::TryCreate(DestinationPackage->GetPackagePath(), Summary.RedirectDestination))
				return {EAssetError::CorruptFile,
					"CorruptRedirector: DestinationObject must be a non-null package main asset."};
			if (Summary.RedirectDestination == PackagePath || Saved.Objects.size() != 1
				|| Summary.Dependencies.size() != 1
				|| Summary.Dependencies.front() != Summary.RedirectDestination
				|| Saved.Objects.front().Fields.size() != 1)
				return {EAssetError::CorruptFile,
					"CorruptRedirector: the redirect summary and serialized body are inconsistent."};
			const FCapturedObject& Object = Saved.Objects.front();
			const FCapturedNode& Field = Object.Fields.front();
			FByteWriter ExpectedPayload;
			ExpectedPayload.Write(uint8(2));
			ExpectedPayload.WriteString(Summary.RedirectDestination.GetView());
			if (Object.Id != 1 || Object.OuterId != 0
				|| Object.ClassName != RedirectorClassName
				|| Field.Field.DeclaringType.ToString() != RedirectorClassName
				|| Field.Field.Name.ToString() != "DestinationObject"
				|| !Field.ReflectedProperty
				|| Field.ReflectedProperty->GetKind() != DurinCodeGen::EPropertyGenFlags::Object
				|| GetSerializedTypeSignature(Field.ReflectedProperty)
					!= "Object:Durin::DObject:true"
				|| !Field.Children.empty() || Field.Raw != ExpectedPayload.Bytes)
				return {EAssetError::CorruptFile,
					"CorruptRedirector: DestinationObject metadata or payload is invalid."};
		}

		std::vector<uint8> Bytes;
		Result = WritePackage(Saved, Summary, Bytes);
		if (!Result) return Result;
		OutBytes = std::move(Bytes);
		OutSummary = std::move(Summary);
		return {};
	}

	auto LoadAuthoredObject(
		DObject& Object,
		std::span<const FAuthoredPackageFieldRecord> Fields,
		std::span<DObject* const> Objects,
		uint32 SourceVersion,
		std::vector<FAssetLegacyField>& OutLegacyFields) -> FAssetResult
	{
		FAuthoredLoadArchive Archive(Object, Fields, Objects, SourceVersion);
		{
			auto Scope = Archive.EnterObject(Object);
			Object.Serialize(Archive);
		}
		if (Archive.HasError())
			return {Archive.GetAssetError(), std::string(Archive.GetError())};
		std::vector<FAssetLegacyField> Legacy = Archive.TakeLegacyFields();
		OutLegacyFields.insert(OutLegacyFields.end(),
			std::make_move_iterator(Legacy.begin()), std::make_move_iterator(Legacy.end()));
		return {};
	}
}
