#include "AssetPackageArchive.h"
#include "AssetObjectLoadTiming.h"
#include "AssetPropertyKindTraits.h"
#include "AssetRuntimeStateInternal.h"
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

		class FPackageLoadArchive final : public FObjectArchive
		{
		public:
			FPackageLoadArchive(
				DObject& InObject,
				std::span<const ObjectPackage::FPropertyTag> InFields,
				const ObjectPackage::FLinkerTables& InLinker,
				std::span<DObject* const> InObjects,
				const FPackageLoadBindings& InBindings,
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
						std::vector<FArchiveFormatVersion>{FArchiveFormatVersion{FName("DAST"), InLinker.FormatVersion}},
						InLinker.CustomVersions})
				, Object(InObject), Fields(InFields), Linker(InLinker), Objects(InObjects), Bindings(InBindings),
				  Consumed(InFields.size(), 0)
			{
			}

			auto GetStructBaseline() -> EArchiveStructBaseline override
			{
				if (!IsCurrentFieldAvailable()) return EArchiveStructBaseline::Complete;
				return Scopes.back().Value->Baseline;
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
				{
					if (Consumed[Index]) continue;
					const auto& Tag = Fields[Index];
					const auto* Schema = LinkerSchema::FindSchema(Linker, Tag.DeclaringType);
					const auto Field = Schema ? LinkerSchema::FindPropertyByName(*Schema, Tag.FieldName) : std::nullopt;
					if (!IsCooking() && Field && Field->Match.Route == LinkerSchema::EPropertyLoadRoute::Removed) continue;
					return {EAssetReadError::UnsupportedProperty,
						std::format("Object '{}' has an unconsumed field '{}::{}' of kind '{}'.",
							Object.GetObjectPath(), Tag.DeclaringType, Tag.FieldName, static_cast<uint32>(Tag.Type.Kind))};
				}
				return {};
			}

			auto GetLoadedDeprecatedProperties(FName DeclaringType) const
				-> std::span<const FName> override
			{
				if (!IsCurrentFieldAvailable()) return {};
				const auto& Scope = Scopes.back();
				return Scope.Type->Kind == K::Struct && Scope.Type->QualifiedName == DeclaringType.ToString()
					? std::span<const FName>(Scope.DeprecatedProperties) : std::span<const FName>{};
			}

			auto SerializeRawBytes(FMutableByteView Bytes) -> void override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return;
				auto& Scope = Scopes.back();
				if (Scope.Type->Kind == K::Byte)
				{
					if (Bytes.size() != 1 || !TakeAtomic(Scope)) { Invalid("Byte fields require one byte transfer."); return; }
					Bytes[0] = static_cast<std::byte>(Scope.Value->Unsigned);
					return;
				}
				if (Scope.Type->Kind != K::Bytes)
				{ Invalid("Raw byte reads require an opaque Bytes field."); return; }
				if (Bytes.size() > Scope.Value->Bytes.size() - Scope.Cursor)
				{
					FailLoad(EArchiveFailureCode::TruncatedPayload, {EAssetReadError::CorruptFile, "Opaque byte field is truncated."});
					return;
				}
				if (!Bytes.empty()) std::memcpy(Bytes.data(), Scope.Value->Bytes.data() + Scope.Cursor, Bytes.size());
				Scope.Cursor += Bytes.size();
			}

			auto SerializeBulkData(FArchiveBulkDataValue& Value, const FArchiveBulkDataParameters& Parameters) -> void override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return;
				auto& Scope = Scopes.back();
				if (Scope.Type->Kind != K::BulkData || !TakeAtomic(Scope)) { Invalid("Expected one BulkData value."); return; }
				const auto& Source = *Scope.Value;
				const bool bExternal = Source.BulkStorage == ObjectPackage::EBulkStorageKind::External;
				const uint64 Size = Source.bBulkPayloadAvailable ? Source.Bytes.size() : Source.BulkStoredSize;
				const auto Hash = Source.bBulkPayloadAvailable ? FXxHash128::HashBuffer(Source.Bytes) : Source.BulkContentHash;
				if (Parameters.ElementSize == 0 || Source.BulkElementSize != Parameters.ElementSize
					|| Size % Parameters.ElementSize != 0 || Size > MaximumAuthoredBulkBytes || Hash.IsZero()
					|| (Source.BulkStorage != ObjectPackage::EBulkStorageKind::Inline && !bExternal)
					|| (!bExternal && (!Source.bBulkPayloadAvailable || Source.BulkAlignment != 1))
					|| (bExternal && (Source.BulkAlignment != EditorBulkDataExternalAlignment
						|| Source.BulkOffset % Source.BulkAlignment != 0)))
				{ Invalid("Invalid bulk descriptor metadata or element size."); return; }
				FGuid Id{static_cast<uint32>(Hash.HashLow), static_cast<uint32>(Hash.HashLow >> 32),
					static_cast<uint32>(Hash.HashHigh), static_cast<uint32>(Hash.HashHigh >> 32)};
				if (!Id.IsValid()) Id.A = 1;
				FArchiveBulkDataValue Loaded{.PayloadId = Id, .LogicalSize = Size, .StoredSize = Size,
					.ContentHash = Hash, .StorageKind = bExternal ? EArchiveBulkDataStorageKind::External : EArchiveBulkDataStorageKind::Inline,
					.SegmentOffset = bExternal ? Source.BulkOffset : 0, .Alignment = Source.BulkAlignment};
				if (bExternal)
				{
					if (!Bindings.BulkResource) { Invalid("External bulk payload requires a resource binding."); return; }
					Loaded.PackageResource = Bindings.BulkResource;
				}
				else Loaded.Buffer = FSharedByteBuffer::Copy(Source.Bytes);
				Value = std::move(Loaded);
			}

			auto SerializeObjectReference(DObject*& Value) -> void override
			{
				FScopedObjectLoadPhase Timing(Linker.Summary.PackagePath, EObjectLoadPhase::ResolveReferences, 0, 1);
				if (IsError() || !IsCurrentFieldAvailable()) return;
				auto& Scope = Scopes.back();
				if (Scope.Type->Kind != K::HardReference || !TakeAtomic(Scope)) { Invalid("Expected one hard reference."); return; }
				const auto Reference = Scope.Value->Reference;
				DObject* Loaded = nullptr;
				if (Reference.IsExport())
				{
					if (Reference.GetTableIndex() >= Objects.size())
					{ FailLoad(EArchiveFailureCode::InvalidObjectReference, {EAssetReadError::InvalidObjectGraph, "Invalid object export index."}); return; }
					Loaded = Objects[Reference.GetTableIndex()];
				}
				else if (Reference.IsImport())
				{
					const ObjectPackage::FPackageImport* Import = nullptr;
					if (!Linker.TryGetImport(Reference, Import) || !Import) { Invalid("Invalid hard-reference import."); return; }
					if (!Bindings.ResolveExternalObject)
					{ FailLoad(EArchiveFailureCode::InvalidObjectReference, {EAssetReadError::MissingDependency, "No resolver for package dependency."}); return; }
					const auto Result = Bindings.ResolveExternalObject(Import->ObjectPath, Loaded);
					if (!Result || !Loaded)
					{ FailLoad(EArchiveFailureCode::InvalidObjectReference, {Result ? EAssetReadError::MissingDependency : Result.Error,
						std::format("Dependency '{}': {}", Import->ObjectPath.ToString(), Result ? "Resolver returned no object." : Result.Message)}); return; }
				}
				if (Loaded && !Scope.Type->QualifiedName.empty())
				{
					DClass* Expected = FindClassByQualifiedName(FName(Scope.Type->QualifiedName));
					if (Expected && !Loaded->IsA(Expected))
					{ FailLoad(EArchiveFailureCode::InvalidData, {EAssetReadError::TypeMismatch, "Resolved object has an incompatible class."}); return; }
				}
				Value = Loaded;
			}

			auto SerializeSoftObjectValue(FObjectPath& Value) -> void override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return;
				auto& Scope = Scopes.back();
				if (Scope.Type->Kind != K::SoftReference || !TakeAtomic(Scope)) { Invalid("Expected one soft reference."); return; }
				if (Scope.Value->Text.empty()) { Value = {}; return; }
				FObjectPath Loaded;
				if (auto Result = FObjectPath::TryCreateWithDiagnostic(Scope.Value->Text, Loaded); !Result)
				{ FailLoad(EArchiveFailureCode::InvalidPath, {EAssetReadError::InvalidPath, ToString(Result.error())}); return; }
				Value = std::move(Loaded);
			}

			auto GetRemainingPayloadBytes() const -> uint64 override
			{
				if (IsCurrentFieldAvailable() && Scopes.back().Type->Kind == K::Bytes)
					return Scopes.back().Value->Bytes.size() - Scopes.back().Cursor;
				return std::numeric_limits<uint64>::max();
			}

			auto IsCurrentFieldAvailable() const -> bool override
			{ return !Scopes.empty() && Scopes.back().Value != nullptr; }

		protected:
			auto TryLoadLogicalPrimitive(EArchiveLogicalPrimitiveKind Kind, void* Out) -> bool override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return true;
				auto& Scope = Scopes.back();
				if (Scope.Type->Kind == K::Bytes) return false; // Opaque native data retains its authored byte representation.
				using P = EArchiveLogicalPrimitiveKind;
				K StoredKind = Scope.Type->Kind;
				if (StoredKind == K::Array || StoredKind == K::Map)
				{
					if (Kind != P::UInt64 || Scope.Cursor != 0) { Invalid("Containers require one unsigned 64-bit count read."); return true; }
					*static_cast<uint64*>(Out) = Scope.Value->Elements.size() / (StoredKind == K::Map ? 2 : 1);
					++Scope.Cursor;
					return true;
				}
				if (StoredKind == K::Enum) StoredKind = static_cast<K>(Scope.Type->Parameter);
				if (Scope.IsIntrinsicComponent()) StoredKind = Scope.Type->Parameter == 6 ? K::F32 : K::F64;
				if (StoredKind == K::Byte) StoredKind = K::U8;
				const auto& V = *Scope.Value;
				// Canonical enum storage preserves its unsigned bit pattern even when
				// the reflected C++ underlying type is signed. Only equal widths match.
				const bool bUnsignedEnum = Scope.Type->Kind == K::Enum
					&& StoredKind >= K::U8 && StoredKind <= K::U64;
				if (bUnsignedEnum)
				{
					if (Kind == P::Int8 && StoredKind == K::U8) StoredKind = K::I8;
					if (Kind == P::Int16 && StoredKind == K::U16) StoredKind = K::I16;
					if (Kind == P::Int32 && StoredKind == K::U32) StoredKind = K::I32;
					if (Kind == P::Int64 && StoredKind == K::U64) StoredKind = K::I64;
				}
				// Native boolean fields use the scalar U8 descriptor. Reflected
				// booleans instead have their own Bool kind; both encode only 0 or 1.
				if (Kind == P::Bool && StoredKind == K::U8)
				{
					if (V.Unsigned > 1 || !TakeAtomic(Scope)) Invalid("Native boolean storage requires one zero-or-one byte.");
					else *static_cast<bool*>(Out) = V.Unsigned != 0;
					return true;
				}
				auto Load = [&]<typename T>(K Expected, T Value) {
					if (StoredKind != Expected || !TakeAtomic(Scope))
						Invalid(std::format("Primitive read does not match the stored logical value (stored kind={}, requested kind={}).",
							static_cast<uint32>(Scope.Type->Kind), static_cast<uint32>(Expected)));
					else
					{
						if constexpr (std::is_integral_v<T> && std::is_signed_v<T>)
							if (bUnsignedEnum) Value = std::bit_cast<T>(static_cast<std::make_unsigned_t<T>>(V.Unsigned));
						*static_cast<T*>(Out) = Value;
					}
				};
				const uint64 Bits = Scope.IsIntrinsicComponent() ? V.ComponentBits[Scope.ComponentOffset] : V.FloatingBits;
				switch (Kind)
				{
				case P::Bool: Load(K::Bool, V.Bool); break;
				case P::Int8: Load(K::I8, static_cast<int8>(V.Signed)); break;
				case P::Int16: Load(K::I16, static_cast<int16>(V.Signed)); break;
				case P::Int32: Load(K::I32, static_cast<int32>(V.Signed)); break;
				case P::Int64: Load(K::I64, V.Signed); break;
				case P::UInt8: Load(K::U8, static_cast<uint8>(V.Unsigned)); break;
				case P::UInt16: Load(K::U16, static_cast<uint16>(V.Unsigned)); break;
				case P::UInt32: Load(K::U32, static_cast<uint32>(V.Unsigned)); break;
				case P::UInt64: Load(K::U64, V.Unsigned); break;
				case P::Float32: Load(K::F32, std::bit_cast<float>(static_cast<uint32>(Bits))); break;
				case P::Float64: Load(K::F64, std::bit_cast<double>(Bits)); break;
				case P::Guid: Load(K::Guid, V.Guid); break;
				}
				return true;
			}

			auto TryLoadLogicalText(EArchiveLogicalTextKind Kind, std::string& Out, uint64 MaximumBytes) -> bool override
			{
				if (IsError() || !IsCurrentFieldAvailable()) return true;
				auto& Scope = Scopes.back();
				if (Scope.Type->Kind == K::Bytes) return false;
				const auto Expected = Kind == EArchiveLogicalTextKind::Name ? K::Name : K::String;
				if (Scope.Type->Kind != Expected || !TakeAtomic(Scope)) { Invalid("Text read does not match the stored logical value."); return true; }
				if (Scope.Value->Text.size() > MaximumBytes)
				{ FailLoad(EArchiveFailureCode::LimitExceeded, {EAssetReadError::CorruptFile, "String exceeds its serialization limit."}); return true; }
				Out = Scope.Value->Text;
				return true;
			}

			auto OnEnterObject(DObject& EnteredObject) -> void override
			{
				if (&EnteredObject != &Object)
					FailLoad(EArchiveFailureCode::InvalidObjectReference, {EAssetReadError::InvalidObjectGraph, "Serializer entered another object."});
			}

			auto OnEnterField(const FArchiveFieldDescriptor& Descriptor) -> void override
			{
				if (IsError() || (!Scopes.empty() && !IsCurrentFieldAvailable())) { Scopes.emplace_back(); return; }
				if (!Scopes.empty() && Scopes.back().Layout != 0)
				{ EnterIntrinsicField(Descriptor); return; }
				const bool bTopLevel = Scopes.empty();
				FLoadScope* Parent = bTopLevel ? nullptr : &Scopes.back();
				if (Parent && Parent->Type->Kind != K::Struct) { Invalid("Nested fields require a struct value."); Scopes.emplace_back(); return; }
				DStructBase* DeclaringStruct = FindClassByQualifiedName(Descriptor.DeclaringType);
				if (!DeclaringStruct) DeclaringStruct = FindStructByQualifiedName(Descriptor.DeclaringType);
				FProperty* Property = DeclaringStruct ? DeclaringStruct->FindPropertyByName(Descriptor.Name, false) : nullptr;
				const bool bByteStorage = Property && (Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Byte || Property->GetKind() == DurinCodeGen::EPropertyGenFlags::Blob);
				const auto ExpectedKind = Property && !bByteStorage ? Property->GetKind() : PackagePrivate::GetNativeKind(Descriptor.LogicalType);
				const auto ExpectedSignature = Property && !bByteStorage ? PackagePrivate::GetSerializedTypeSignature(Property) : PackagePrivate::GetNativeTypeSignature(Descriptor.LogicalType);
				const auto* Deprecation = Property ? Property->GetDeprecation() : nullptr;
				const std::string Name = Deprecation ? Deprecation->HistoricalName.ToString() : Descriptor.Name.ToString();
				const std::string Owner = Descriptor.DeclaringType.ToString();
				const auto* Schema = LinkerSchema::FindSchema(Linker, Owner);
				if (Parent && (Parent->Type->QualifiedName != Owner || Parent->Value->FieldNames.size() != Parent->Value->Elements.size()
					|| ObjectPackage::StructFieldTypes(*Parent->Type, *Parent->Value).size() != Parent->Value->Elements.size()))
				{ Invalid("Struct value does not match its field descriptors."); Scopes.emplace_back(); return; }
				auto& Used = bTopLevel ? Consumed : Parent->Consumed;
				const size_t Count = bTopLevel ? Fields.size() : Parent->Value->Elements.size();
				if (Used.empty()) Used.resize(Count, 0);
				for (size_t Index = 0; Index < Count; ++Index)
				{
					const std::string& StoredName = bTopLevel ? Fields[Index].FieldName : Parent->Value->FieldNames[Index];
					if (Used[Index] || StoredName != Name || (bTopLevel && Fields[Index].DeclaringType != Owner)) continue;
					const auto Field = Schema ? LinkerSchema::FindPropertyByName(*Schema, StoredName) : std::nullopt;
					if (!Field) { Invalid("Stored field is missing from its schema."); break; }
					const auto& Type = bTopLevel ? Fields[Index].Type : ObjectPackage::StructFieldTypes(*Parent->Type, *Parent->Value)[Index];
					if (Type != Field->Descriptor->Type) { Invalid("Stored field type differs from its schema."); break; }
					if (!IsCooking() && Field->Match.Route == LinkerSchema::EPropertyLoadRoute::Removed) continue;
					if (!Deprecation && Field->Match.Route == LinkerSchema::EPropertyLoadRoute::Deprecated) continue;
					if (Field->Kind != ExpectedKind || Field->Signature != ExpectedSignature) continue;
					Used[Index] = 1;
					if (Deprecation && Parent) Parent->DeprecatedProperties.push_back(Property->NamePrivate);
					PushValue(Type, bTopLevel ? Fields[Index].Value : Parent->Value->Elements[Index]);
					return;
				}
				Scopes.emplace_back();
			}

			auto OnEnterFixedArrayElement(uint64 Index) -> void override { EnterElement(K::FixedArray, Index, 0); }
			auto OnEnterArrayElement(uint64 Index) -> void override { EnterElement(K::Array, Index, 0); }
			auto OnEnterMapKey(uint64 Index) -> void override { EnterElement(K::Map, Index, 0); }
			auto OnEnterMapValue(uint64 Index) -> void override { EnterElement(K::Map, Index, 1); }
			auto OnLeavePath() -> void override { LeaveValue(); }
			auto OnLeaveField() -> void override { LeaveValue(); }

		private:
			using K = ObjectPackage::EValueKind;
			// Only traversal state is owned here. Types, values and component storage belong to Linker.
			struct FLoadScope
			{
				const ObjectPackage::FSerializedType* Type = nullptr;
				const ObjectPackage::FSerializedValue* Value = nullptr;
				size_t Cursor = 0;
				std::vector<uint8> Consumed;
				uint64 Layout = 0;
				size_t ComponentOffset = 0;
				std::vector<FName> DeprecatedProperties;

				auto IsIntrinsicComponent() const -> bool
				{ return Type && Type->Kind == K::Intrinsic && Layout == 0; }
			};

			auto FailLoad(EArchiveFailureCode Code, FAssetReadResult Error) -> void
			{ if (!IsError()) { LoadError = Error.Error; Fail(Code, std::move(Error.Message)); } }
			auto Invalid(std::string_view Message) -> void
			{ FailLoad(EArchiveFailureCode::InvalidData, {EAssetReadError::CorruptFile, std::string(Message)}); }
			auto TakeAtomic(FLoadScope& Scope) -> bool
			{ if (Scope.Cursor != 0) return false; ++Scope.Cursor; return true; }
			auto PushValue(const ObjectPackage::FSerializedType& Type, const ObjectPackage::FSerializedValue& Value) -> void
			{
				FLoadScope Scope{.Type = &Type, .Value = &Value};
				if (Type.Kind == K::Intrinsic) Scope.Layout = Type.Parameter;
				Scopes.push_back(std::move(Scope));
			}

			auto EnterElement(K Kind, uint64 Index, size_t Side) -> void
			{
				if (IsError() || !IsCurrentFieldAvailable()) { Scopes.emplace_back(); return; }
				auto& Parent = Scopes.back();
				const size_t Arity = Kind == K::Map ? 2 : 1;
				if (Parent.Type->Kind != Kind || Parent.Type->Children.size() != Arity
					|| Index >= Parent.Value->Elements.size() / Arity || (Kind != K::FixedArray && Parent.Cursor != 1))
				{ Invalid("Invalid container element scope or missing count read."); Scopes.emplace_back(); return; }
				if (Parent.Consumed.empty()) Parent.Consumed.resize(Parent.Value->Elements.size(), 0);
				const size_t Position = static_cast<size_t>(Index) * Arity + Side;
				if (Parent.Consumed[Position]) { Invalid("Container element was consumed twice."); Scopes.emplace_back(); return; }
				Parent.Consumed[Position] = 1;
				PushValue(Parent.Type->Children[Side], Parent.Value->Elements[Position]);
			}

			auto EnterIntrinsicField(const FArchiveFieldDescriptor& Descriptor) -> void
			{
				auto& Parent = Scopes.back();
				if (Descriptor.DeclaringType.ToString() != LinkerSchema::IntrinsicName(Parent.Layout))
				{ Invalid("Intrinsic field has an incompatible owner."); Scopes.emplace_back(); return; }
				const std::array<std::string_view, 4> Lower{"x", "y", "z", "w"};
				const std::array<std::string_view, 4> Color{"R", "G", "B", "A"};
				const std::array<std::string_view, 3> Transform{"Rotation", "Translation", "Scale3D"};
				const size_t Count = Parent.Layout == 5 ? 3 : Parent.Layout == 1 ? 2 : Parent.Layout == 2 ? 3 : 4;
				if (Parent.Consumed.empty()) Parent.Consumed.resize(Count, 0);
				const auto Name = Descriptor.Name.ToString();
				for (size_t Index = 0; Index < Count; ++Index)
				{
					const auto Expected = Parent.Layout == 5 ? Transform[Index] : Parent.Layout == 6 ? Color[Index]
						: Parent.Layout == 4 ? Lower[(Index + 3) % 4] : Lower[Index];
					if (Name != Expected) continue;
					if (Parent.Consumed[Index]) { Invalid("Intrinsic field was consumed twice."); break; }
					Parent.Consumed[Index] = 1;
					FLoadScope Child{.Type = Parent.Type, .Value = Parent.Value,
						.ComponentOffset = Parent.ComponentOffset};
					if (Parent.Layout == 5)
					{
						Child.Layout = Index == 0 ? 4 : 2;
						Child.ComponentOffset += Index == 0 ? 0 : Index == 1 ? 4 : 7;
					}
					else Child.ComponentOffset += Index;
					if (Child.ComponentOffset >= Child.Value->ComponentBits.size()) { Invalid("Intrinsic component storage is truncated."); break; }
					Scopes.push_back(std::move(Child));
					return;
				}
				Scopes.emplace_back();
			}

			auto LeaveValue() -> void
			{
				if (Scopes.empty()) return;
				auto& Scope = Scopes.back();
				if (!IsError() && Scope.Value)
				{
					const auto Kind = Scope.Type->Kind;
					if (Scope.IsIntrinsicComponent() || (Kind != K::Struct && Kind != K::Intrinsic && Kind != K::FixedArray
						&& Kind != K::Array && Kind != K::Map && Kind != K::Bytes))
					{
						if (Scope.Cursor != 1) Invalid("Stored value was not consumed exactly once.");
					}
					else if (Kind == K::Bytes)
					{
						if (Scope.Cursor != Scope.Value->Bytes.size()) Invalid("Opaque byte field has trailing payload.");
					}
					else
					{
						const size_t Count = Scope.Layout != 0 ? (Scope.Layout == 5 ? 3 : Scope.Layout == 1 ? 2 : Scope.Layout == 2 ? 3 : 4)
							: Scope.Value->Elements.size();
						if ((Kind == K::Array || Kind == K::Map) && Scope.Cursor != 1) Invalid("Container count was not consumed.");
						for (size_t Index = 0; Index < Count && !IsError(); ++Index)
						{
							if (Index < Scope.Consumed.size() && Scope.Consumed[Index]) continue;
							if (Kind == K::Struct && !IsCooking())
							{
								const auto* Schema = LinkerSchema::FindSchema(Linker, Scope.Type->QualifiedName);
								const auto Field = Schema && Index < Scope.Value->FieldNames.size()
									? LinkerSchema::FindPropertyByName(*Schema, Scope.Value->FieldNames[Index]) : std::nullopt;
								if (Field && Field->Match.Route == LinkerSchema::EPropertyLoadRoute::Removed) continue;
							}
							FailLoad(EArchiveFailureCode::MalformedSerializer, {EAssetReadError::UnsupportedProperty, "Stored field or container element was not consumed."});
						}
					}
				}
				Scopes.pop_back();
			}

			DObject& Object;
			std::span<const ObjectPackage::FPropertyTag> Fields;
			const ObjectPackage::FLinkerTables& Linker;
			std::span<DObject* const> Objects;
			const FPackageLoadBindings& Bindings;
			std::vector<uint8> Consumed;
			std::vector<FLoadScope> Scopes;
			EAssetReadError LoadError = EAssetReadError::None;
		};

	}

	auto LoadPackageObject(
		DObject& Object,
		std::span<const ObjectPackage::FPropertyTag> Fields,
		const ObjectPackage::FLinkerTables& Linker,
		std::span<DObject* const> Objects,
		const FPackageLoadBindings& Bindings,
		const FArchiveState& Context) -> FAssetReadResult
	{
		FScopedObjectLoadPhase Timing(Linker.Summary.PackagePath, EObjectLoadPhase::SerializeExports, 1, Fields.size());
		FPackageLoadArchive Archive(
			Object, Fields, Linker, Objects, Bindings, Context);
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
