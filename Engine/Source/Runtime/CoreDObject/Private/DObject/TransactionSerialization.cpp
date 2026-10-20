#include "DObject/TransactionSerialization.h"
#include "DObject/TransactionDiff.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/NativeEnum.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	namespace
	{
		constexpr auto MutableFlags = EObjectFlags::Transient | EObjectFlags::Public
			| EObjectFlags::Standalone | EObjectFlags::Transactional;

		auto AppendType(FArchive& Ar, const FArchiveLogicalTypeDescriptor& Type, uint32 Depth = 0) -> void
		{
			if (Depth > 64) { Ar.SetError("Transaction field type exceeds the schema depth limit."); return; }
			auto Kind = Type.Kind;
			auto Signed = Type.bSigned;
			auto Floating = Type.bFloating;
			auto Width = Type.BitWidth;
			auto Name = Type.QualifiedType;
			auto Version = Type.NativeFieldVersion;
			auto Dimension = Type.FixedArrayDimension;
			Ar << Kind << Signed << Floating << Width << Name << Version << Dimension;
			for (const auto& Child : {Type.ElementType, Type.KeyType, Type.ValueType})
			{
				bool Present = Child != nullptr;
				Ar << Present;
				if (Child) AppendType(Ar, *Child, Depth + 1);
			}
		}

		auto AppendProperty(FArchive& Ar, const FProperty& Property,
			std::unordered_set<const DStruct*>& Active, uint32 Depth = 0) -> void
		{
			if (Depth > 64) { Ar.SetError("Transaction property schema exceeds the depth limit."); return; }
			auto Name = Property.NamePrivate;
			auto Kind = Property.GetKind();
			auto Dimension = Property.GetArrayDim();
			auto Size = static_cast<uint64>(Property.GetElementSize());
			auto Flags = Property.GetPropertyFlags();
			Ar << Name << Kind << Dimension << Size << Flags;
			if (const auto* Referenced = Property.GetReferencedClass())
			{
				auto TypeName = Referenced->GetQualifiedName(); Ar << TypeName;
			}
			switch (Kind)
			{
			case DurinCodeGen::EPropertyGenFlags::Enum:
			{
				const auto& Enum = static_cast<const FEnumProperty&>(Property);
				auto TypeName = Enum.GetEnum() ? Enum.GetEnum()->GetQualifiedName() : FName();
				auto Underlying = Enum.GetUnderlyingType();
				Ar << TypeName << Underlying;
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Struct:
			{
				const auto* Struct = static_cast<const FStructProperty&>(Property).GetStruct();
				if (!Struct) { Ar.SetError("Transaction struct metadata is unavailable."); return; }
				auto TypeName = Struct->GetQualifiedName(); Ar << TypeName;
				bool Repeated = Active.contains(Struct); Ar << Repeated;
				if (!Repeated)
				{
					Active.insert(Struct);
					Struct->ForEachProperty([&](FProperty* Field) {
						if (Field && !Field->HasAnyPropertyFlags(EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated))
							AppendProperty(Ar, *Field, Active, Depth + 1);
					}, false);
					Active.erase(Struct);
				}
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Array:
			{
				const auto& Array = static_cast<const FArrayProperty&>(Property);
				if (!Array.HasCapability(EArrayOpsFlags::Count | EArrayOpsFlags::ConstTraversal
					| EArrayOpsFlags::DetachedStorage | EArrayOpsFlags::TransactionalCommit
					| EArrayOpsFlags::RandomAccess | EArrayOpsFlags::DefaultGrow | EArrayOpsFlags::CopyAssign))
				{ Ar.SetError("Transaction array requires reversible operations and parent baseline copying."); return; }
				if (auto* Inner = Array.GetInner()) AppendProperty(Ar, *Inner, Active, Depth + 1);
				else Ar.SetError("Transaction array element metadata is unavailable.");
				break;
			}
			case DurinCodeGen::EPropertyGenFlags::Map:
			{
				const auto& Map = static_cast<const FMapProperty&>(Property);
				if (!Map.HasCapability(EMapOpsFlags::Count | EMapOpsFlags::ConstTraversal
					| EMapOpsFlags::DetachedStorage | EMapOpsFlags::TransactionalCommit
					| EMapOpsFlags::Insert | EMapOpsFlags::Lookup))
				{ Ar.SetError("Transaction map requires reversible operations and parent baseline lookup."); return; }
				if (!Map.GetKeyProp() || !Map.GetValueProp()) Ar.SetError("Transaction map metadata is unavailable.");
				else { AppendProperty(Ar, *Map.GetKeyProp(), Active, Depth + 1); AppendProperty(Ar, *Map.GetValueProp(), Active, Depth + 1); }
				break;
			}
			default: break;
			}
		}

		// Shared field-selection and manifest bookkeeping for both archive directions.
		class FTransactionArchive : public FObjectArchive
		{
		public:
			FTransactionArchive(DObject& InObject, bool Loading, std::vector<FName>& InExcluded,
				FArchiveVersionContext Versions = {})
				: FObjectArchive({Loading ? EArchiveDirection::Load : EArchiveDirection::Save,
					EArchivePurpose::Transaction, EArchiveCapability::RawBytes | EArchiveCapability::Position
					| EArchiveCapability::RemainingPayload | EArchiveCapability::CanonicalMapOrder
					| EArchiveCapability::ObjectReferences | EArchiveCapability::CustomVersions}, std::move(Versions))
				, Object(InObject), Excluded(InExcluded), SchemaWriter(Schema, EArchivePurpose::Transaction)
			{
				std::unordered_set<const DStruct*> Active;
				Object.GetClass()->ForEachProperty([&](FProperty* Property) {
					if (Property && !Property->HasAnyPropertyFlags(EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated))
						AppendProperty(SchemaWriter, *Property, Active);
				});
				if (SchemaWriter.IsError()) SetError(SchemaWriter.GetError());
			}
			auto IsCurrentFieldAvailable() const -> bool override
			{ return Available.empty() || Available.back(); }
			auto GetStructBaseline() -> EArchiveStructBaseline override { return EArchiveStructBaseline::Parent; }
			auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
			{
				if (!IsCurrentFieldAvailable()) return;
				auto Key = Value.GetKey(); Key.SerializeForSnapshot(*this);
				if (IsLoading() && !IsError()) Value.SetKey(Key);
			}
			FByteBuffer Schema;
			std::vector<FSerializedTaggedData> Fields;
		protected:
			auto OnEnterField(const FArchiveFieldDescriptor& Field) -> void override
			{
				if (Available.empty()) bOmittedRoot = std::ranges::contains(Excluded, Field.Name);
				const bool Include = IsCurrentFieldAvailable()
					&& !bOmittedRoot && !EnumHasAnyFlags(Field.PropertyFlags, EPropertyFlags::NonTransactional | EPropertyFlags::Deprecated);
				if (Available.empty() && !bOmittedRoot)
				{
					auto Declaring = Field.DeclaringType;
					auto Name = Field.Name;
					auto Dimension = Field.ArrayDimension;
					auto Flags = Field.PropertyFlags;
					SchemaWriter << Declaring << Name << Dimension << Flags;
					AppendType(SchemaWriter, Field.LogicalType);
					Fields.push_back({Declaring, Name, Tell(), Tell(), false});
				}
				Available.push_back(Include);
			}
			auto OnLeaveField() -> void override
			{
				if (Available.size() == 1 && !bOmittedRoot && !Fields.empty()) Fields.back().End = Tell();
				if (!Available.empty()) Available.pop_back();
			}
			auto OnReflectedPropertyValue(FProperty& Property, const void*, uint32) -> void override
			{
				if (Available.size() == 1 && !Fields.empty() && Fields.back().Name == Property.NamePrivate
					&& Object.GetClass()->FindPropertyByName(Property.NamePrivate) == &Property) Fields.back().bReflected = true;
			}
			auto OnResolvePropertySaveValue(FProperty& Property, const void* Container, uint32 Index,
				FArchivePropertySaveValue& Out) -> EArchivePropertySaveDisposition override
			{
				Out = {Container, Index};
				return Container == &Object && std::ranges::contains(Excluded, Property.NamePrivate)
					? EArchivePropertySaveDisposition::Omit : EArchivePropertySaveDisposition::LiveValue;
			}
			DObject& Object;
			std::vector<FName>& Excluded;
		private:
			FObjectMemoryWriter SchemaWriter;
			std::vector<bool> Available;
			bool bOmittedRoot = false;
		};
	}

	class FSerializedObjectDataWriter final : public FTransactionArchive
	{
	public:
		FSerializedObjectDataWriter(DObject& Object, FSerializedObject& InData, FTransactionSerializationContext& Context, bool FrozenPolicy)
			: FTransactionArchive(Object, false, Context.ExcludedMembers), Data(InData), References(Context.ReferenceSlots), bFrozenPolicy(FrozenPolicy) {}

		auto Tell() const -> uint64 override { return Data.Bytes.size(); }
		auto SerializeRawBytes(FMutableByteView Bytes) -> void override
		{
			if (!IsError() && IsCurrentFieldAvailable()) Data.Bytes.insert(Data.Bytes.end(), Bytes.begin(), Bytes.end());
		}
		auto SerializeWeakObjectReference(FWeakObjectPtr& Value) -> void override
		{
			if (!IsCurrentFieldAvailable()) return;
			References.push_back({Tell(), true});
			FTransactionArchive::SerializeWeakObjectReference(Value);
		}
		auto SerializeObjectReference(DObject*& Value) -> void override
		{
			if (!IsCurrentFieldAvailable()) return;
			References.push_back({Tell(), false});
			FObjectKey Key(Value); Key.SerializeForSnapshot(*this);
			if (!Key.IsNull() && !std::ranges::contains(Data.HardReferences, Key)) Data.HardReferences.push_back(Key);
		}
	protected:
		auto OnResolvePropertySaveValue(FProperty& Property, const void* Container, uint32 Index,
			FArchivePropertySaveValue& Out) -> EArchivePropertySaveDisposition override
		{
			const auto Policy = FTransactionArchive::OnResolvePropertySaveValue(Property, Container, Index, Out);
			if (Policy == EArchivePropertySaveDisposition::Omit || Container != &Object) return Policy;
			if (Property.GetKind() == DurinCodeGen::EPropertyGenFlags::Object)
				for (uint32 Element = 0; Element < Property.GetArrayDim(); ++Element)
					if (auto* Reference = static_cast<const FObjectProperty&>(Property).GetObjectPropertyValue(Container, Element);
						Reference && Reference->GetConstructionPurpose() == EObjectConstructionPurpose::Generated)
					{
						if (bFrozenPolicy) SetError("A captured transaction field acquired a construction-owned generated reference.");
						else Excluded.push_back(Property.NamePrivate);
						return EArchivePropertySaveDisposition::Omit;
					}

			return Policy;
		}
	private:
		FSerializedObject& Data;
		std::vector<FSerializedObjectReferenceSlot>& References;
		bool bFrozenPolicy;
	};

	class FSerializedObjectDataReader final : public FTransactionArchive
	{
	public:
		FSerializedObjectDataReader(DObject& Object, const FSerializedObjectCapture& InState, std::vector<FName>& Excluded)
			: FTransactionArchive(Object, true, Excluded, InState.Context->Versions), State(InState) {}
		auto RestoreMetadata() -> void
		{ Object.RestoreTransactionMetadata(State.Info.Identity.OriginalName, State.Info.Outer.Resolve(), State.Info.Flags); }
		auto Tell() const -> uint64 override { return Offset; }
		auto GetRemainingPayloadBytes() const -> uint64 override { return State.Data->Bytes.size() - Offset; }
		auto SerializeRawBytes(FMutableByteView Bytes) -> void override
		{
			if (IsError() || !IsCurrentFieldAvailable()) return;
			if (Bytes.size() > GetRemainingPayloadBytes()) { SetError("Truncated transaction object state."); return; }
			if (!Bytes.empty()) std::memcpy(Bytes.data(), State.Data->Bytes.data() + Offset, Bytes.size());
			Offset += Bytes.size();
		}
		auto SerializeObjectReference(DObject*& Value) -> void override
		{
			if (!IsCurrentFieldAvailable()) return;
			FObjectKey Key; Key.SerializeForSnapshot(*this);
			if (IsError()) return;
			Value = Key.IsNull() ? nullptr : FPersistentObjectRef::FromKey(Key).Resolve();
			if (!Key.IsNull() && !Value) SetError("A transaction hard reference no longer resolves.");
		}
	private:
		const FSerializedObjectCapture& State;
		uint64 Offset = 0;
	};

	auto CaptureSerializedObject(DObject& Object, const FSerializedObjectCapture* CapturePolicy, FDiffableObject* Observation) -> std::expected<FSerializedObjectCapture, std::string>
	{
		FGarbageCollectionDeferralScope DeferCollection;
		if (!IsValid(&Object) || !Object.GetClass() || Object.IsTemplateObject())
			return std::unexpected("Transaction object capture requires a live ordinary object.");
		if (CapturePolicy && (!CapturePolicy->Data || !CapturePolicy->Context || CapturePolicy->Info.Target.Resolve() != &Object))
			return std::unexpected("Transaction capture policy belongs to a different object.");
		// Lazily declared intrinsic classes can be pending after object construction.
		// Complete their registration before recording a generation-checked identity.
		DObjectForceRegistration(Object.GetClass());
		if (!IsValid(Object.GetClass())) return std::unexpected("Transaction class metadata is unavailable.");
		FSerializedObjectCapture State;
		auto Data = std::make_shared<FSerializedObject>();
		auto Context = std::make_shared<FTransactionSerializationContext>();
		State.Info.Target = FPersistentObjectRef(&Object);
		State.Info.Class = FPersistentObjectRef(Object.GetClass());
		State.Info.Outer = FPersistentObjectRef(Object.GetOuter());
		State.Info.Package = FPersistentObjectRef(Object.GetPackage());
		State.Info.Identity = CaptureTransactionObjectIdentity(Object); State.Info.Flags = Object.GetObjectFlags();
		if (CapturePolicy) Context->ExcludedMembers = CapturePolicy->Context->ExcludedMembers;
		try
		{
			FSerializedObjectDataWriter Writer(Object, *Data, *Context, CapturePolicy != nullptr);
			{ auto Scope = Writer.EnterObject(Object); if (!Writer.IsError()) Object.Serialize(Writer); }
			if (Writer.IsError()) return std::unexpected(std::string(Writer.GetError()));
			Context->Schema = std::move(Writer.Schema);
			Context->Versions = Writer.GetVersionContext();
			State.Data = Data; State.Context = Context;
			if (Observation) *Observation = {State.Info, State.Data, State.Context, std::move(Writer.Fields)};
		}
		catch (const std::exception& Error) { return std::unexpected(std::format("Transaction serialization capture threw: {}", Error.what())); }
		catch (...) { return std::unexpected("Transaction serialization capture threw an unknown exception."); }
		return State;
	}

	static auto ValidateSerializedObjectIdentity(DObject& Object, const FSerializedObjectCapture& Capture) -> FTransactionCaptureResult
	{
		if (!Capture.Data || !Capture.Context) return std::unexpected("Transaction capture is incomplete.");
		if (!IsValid(&Object) || Capture.Info.Target.Resolve() != &Object)
			return std::unexpected("Transaction object identity is no longer available.");
		if (Capture.Info.Class.Resolve() != Object.GetClass()) return std::unexpected("Transaction object class is no longer compatible.");
		if (Object.IsTemplateObject()) return std::unexpected("Transaction object became a template.");
		if (Capture.Info.Identity.OriginalName.IsNone() || Capture.Info.Outer.IsStale() || Capture.Info.Package.IsStale())
			return std::unexpected("Transaction object metadata no longer resolves.");
		if ((Capture.Info.Flags & ~MutableFlags) != (Object.GetObjectFlags() & ~MutableFlags))
			return std::unexpected("Transaction replay cannot restore object construction flags.");
		for (auto* Parent = Capture.Info.Outer.Resolve(); Parent; Parent = Parent->GetOuter())
			if (Parent == &Object) return std::unexpected("Transaction object Outer would create a cycle.");
		if (Capture.Info.Identity.OriginalName != Object.GetFName() || Capture.Info.Outer.Resolve() != Object.GetOuter())
			for (auto* Sibling : GDObjectArray.GetObjectsWithOuter(Capture.Info.Outer.Resolve(), EObjectQueryScope::LiveOnly))
				if (Sibling != &Object && Sibling->GetFName() == Capture.Info.Identity.OriginalName)
					return std::unexpected("Transaction object Name is reserved by another live object.");
		if (auto* ParentPackage = Cast<DPackage>(Capture.Info.Outer.Resolve()); ParentPackage && ParentPackage->IsAssetPackage()
			&& !EnumHasAnyFlags(Capture.Info.Flags, EObjectFlags::Transient)
			&& std::ranges::any_of(ParentPackage->GetTopLevelAssets(), [&](const DObject* Asset) {
				return Asset != &Object && !Asset->IsGarbage() && Asset->GetFName() == Capture.Info.Identity.OriginalName;
			}))
			return std::unexpected("Transaction object metadata conflicts with a top-level asset name.");
		for (auto Key : Capture.Data->HardReferences)
			if (FPersistentObjectRef::FromKey(Key).IsStale()) return std::unexpected("A transaction object hard reference expired.");
		return {};
	}

	auto ValidateSerializedObject(DObject& Object, const FSerializedObjectCapture& Capture) -> FTransactionCaptureResult
	{
		if (auto Result = ValidateSerializedObjectIdentity(Object, Capture); !Result) return Result;
		auto Current = CaptureSerializedObject(Object, &Capture);
		if (!Current) return std::unexpected(Current.error());
		if (!Capture.HasSameSchema(*Current)) return std::unexpected("Transaction serialized field schema changed.");
		return {};
	}

	auto LoadSerializedObject(DObject& Object, const FSerializedObjectCapture& Capture) -> FCommandChangeResult
	{
		FGarbageCollectionDeferralScope DeferCollection;
		FScopedPackageDirtySuppression SuppressDirty;
		if (auto Result = ValidateSerializedObjectIdentity(Object, Capture); !Result) return std::unexpected(FCommandChangeError{Result.error()});
		try
		{
			auto Excluded = Capture.Context->ExcludedMembers;
			FSerializedObjectDataReader Reader(Object, Capture, Excluded);
			Reader.RestoreMetadata();
			{ auto Scope = Reader.EnterObject(Object); if (!Reader.IsError()) Object.Serialize(Reader); }
			if (!Reader.IsError() && (Reader.GetRemainingPayloadBytes() != 0 || Reader.Schema != Capture.Context->Schema))
				Reader.SetError("Transaction object state did not consume its complete matching schema.");
			if (Reader.IsError()) return std::unexpected(FCommandChangeError{std::string(Reader.GetError())});
			return {};
		}
		catch (const std::exception& Error) { return std::unexpected(FCommandChangeError{std::format("Transaction serialization load threw: {}", Error.what()), true}); }
		catch (...) { return std::unexpected(FCommandChangeError{"Transaction serialization load threw an unknown exception.", true}); }
	}

	auto FSerializedObject::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		for (auto Key : HardReferences) FPersistentObjectRef::FromKey(Key).AddReferencedObjects(Collector);
	}

	auto FSerializedObjectInfo::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{
		Target.AddReferencedObjects(Collector); Class.AddReferencedObjects(Collector);
		Outer.AddReferencedObjects(Collector); Package.AddReferencedObjects(Collector);
	}

	auto FTransactionSerializationContext::IsCompatibleWith(const FTransactionSerializationContext& Other) const -> bool
	{
		return Schema == Other.Schema && Versions.CustomVersions == Other.Versions.CustomVersions
			&& std::ranges::equal(Versions.Formats, Other.Versions.Formats, [](const auto& Left, const auto& Right) {
				return Left.Format == Right.Format && Left.Version == Right.Version;
			});
	}

	auto FSerializedObjectCapture::AddReferencedObjects(FReferenceCollector& Collector) const -> void
	{ Info.AddReferencedObjects(Collector); if (Data) Data->AddReferencedObjects(Collector); }

	auto FSerializedObjectCapture::TryGetAllocatedSize(size_t& OutBytes) const -> bool
	{
		if (!Data || !Context) return false;
		size_t Total = 0;
		auto Add = [&](size_t Count, size_t Size) {
			if (Count > std::numeric_limits<size_t>::max() / Size) return false;
			const auto Bytes = Count * Size;
			if (Total > std::numeric_limits<size_t>::max() - Bytes) return false;
			Total += Bytes; return true;
		};
		if (!Add(Info.Identity.OriginalOuterPath.capacity(), 1)
			|| !Add(Info.Identity.OriginalClassPath.capacity(), 1)
			|| !Add(Info.Identity.OriginalPackagePath.capacity(), 1)) return false;
		if (!Add(Data->Bytes.capacity(), 1) || !Add(Context->Schema.capacity(), 1)
			|| !Add(Data->HardReferences.capacity(), sizeof(FObjectKey))
			|| !Add(Context->ExcludedMembers.capacity(), sizeof(FName))
			|| !Add(Context->ReferenceSlots.capacity(), sizeof(FSerializedObjectReferenceSlot))
			|| !Add(Context->Versions.Formats.capacity(), sizeof(FArchiveFormatVersion))
			|| !Add(Context->Versions.CustomVersions.capacity(), sizeof(FArchiveCustomVersion))) return false;
		OutBytes = Total; return true;
	}
}
