#include "DObject/PackageCapture.h"
#include "DObject/PackageValueCodec.h"
#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "CoreGlobals.h"
#include "Threading/RunnableThread.h"

namespace Durin::PackagePrivate
{
	namespace
	{
		constexpr uint64 PackageBulkDataMaximumSegmentBytes = ObjectPackage::DastMaximumBulkBytes;
		constexpr uint64 PackageBulkExternalThreshold = 256ull * 1024ull;
		constexpr uint32 PackageBulkExternalAlignment = 16;
		constexpr uint32 PackageBulkContentIdVersion = 1;

		auto IsTransientObjectGraph(const DObject* Object) -> bool
		{
			for (const DObject* Outer = Object; Outer; Outer = Outer->GetOuter())
				if (Outer->HasAnyObjectFlags(EObjectFlags::Transient)) return true;
			return false;
		}

		enum class ENodeKind : uint8 { Field, Fixed, Array, MapKey, MapValue };

		struct FCapturedNode
		{
			ENodeKind Kind = ENodeKind::Field;
			uint64 Index = 0;
			FArchiveFieldDescriptor Field;
			FProperty* ReflectedProperty = nullptr;
			FByteBuffer Raw;
			std::vector<FCapturedNode> Children;
			FByteBuffer BulkBytes;
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
			std::vector<FCustomVersion> CustomVersions;
			std::vector<FCapturedObject> Objects;
			std::vector<FPackagePath> Dependencies;
			std::vector<FObjectPath> HardReferenceTargets;
			std::vector<FPackageBulkStoragePayload> BulkPayloads;
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
			if (A.CustomVersions != B.CustomVersions || A.Dependencies != B.Dependencies
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

		class FAuthoredCaptureArchive final : public FObjectArchive
		{
		public:
			FAuthoredCaptureArchive(
				const std::unordered_map<DObject*, uint64>& InObjectIds,
				const FPackageCaptureOptions& InOptions,
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

			FPackageCaptureError CaptureFailure;
			auto FailCapture(EPackageCaptureReason Reason, EArchiveFailureCode Code) -> void
			{
				if (HasError()) return;
				CaptureFailure.Reason = Reason;
				CaptureFailure.ArchiveCode = Code;
				if (CurrentDObject && CaptureFailure.ObjectPath.empty()) CaptureFailure.ObjectPath = CurrentDObject->GetObjectPath();
				Fail(Code, FormatPackageCaptureError(CaptureFailure));
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

			auto SerializeRawBytes(FMutableByteView Bytes) -> void override
			{
				if (NodeStack.empty() || !NodeStack.back())
				{
					if (SuppressedDepth == 0)
						FailCapture(EPackageCaptureReason::RawOutsideField, EArchiveFailureCode::MalformedSerializer);
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
				if (!DastVersion || !ObjectPackage::IsSupportedPackageReaderVersion(DastVersion->Version))
				{
					CaptureFailure.Actual = DastVersion ? DastVersion->Version : 0;
					FailCapture(EPackageCaptureReason::BulkVersion, EArchiveFailureCode::InvalidData);
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
					CaptureFailure.Actual = Value.Buffer.GetSize();
					CaptureFailure.Expected = Value.LogicalSize;
					FailCapture(EPackageCaptureReason::BulkMetadata, EArchiveFailureCode::InvalidData);
					return;
				}
				const bool bExternal = Value.LogicalSize > PackageBulkExternalThreshold
					&& Parameters.StoragePolicy != EArchiveBulkDataStoragePolicy::ForceInline;
				const uint32 RequestedAlignment = bCooked
					? Parameters.Alignment : PackageBulkExternalAlignment;
				const uint32 Alignment = bExternal ? RequestedAlignment : 1;
				if (bExternal && (Alignment == 0 || Alignment > 4096
					|| (Alignment & (Alignment - 1)) != 0))
				{
					CaptureFailure.Actual = Alignment;
					CaptureFailure.Expected = 4096;
					FailCapture(EPackageCaptureReason::BulkAlignment, EArchiveFailureCode::InvalidAlignment);
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
						CaptureFailure.Actual = Value.StoredSize;
						CaptureFailure.Expected = PackageBulkDataMaximumSegmentBytes;
						FailCapture(EPackageCaptureReason::BulkLimit, EArchiveFailureCode::LimitExceeded);
						return;
					}
					NextExternalOffset = SegmentOffset + Value.StoredSize;
				}
				Value.StorageKind = bExternal
					? EArchiveBulkDataStorageKind::External
					: EArchiveBulkDataStorageKind::Inline;
				Value.ContainerHash = bExternal ? ContainerHash : FXxHash128{};
				FPackageBulkStorageDescriptor Descriptor{
					.PayloadId = Value.PayloadId,
					.LogicalByteCount = Value.LogicalSize,
					.StoredByteCount = Value.StoredSize,
					.ContentHash = Value.ContentHash,
					.ContainerHash = Value.ContainerHash,
					.StorageKind = bExternal ? EPackageBulkStorageKind::External
						: EPackageBulkStorageKind::Inline,
					.SegmentOffset = SegmentOffset,
					.Alignment = Alignment};
				Package.BulkPayloads.push_back({Descriptor, Value.Buffer});
				if (NodeStack.empty() || !NodeStack.back())
				{
					FailCapture(EPackageCaptureReason::BulkOutsideValue, EArchiveFailureCode::MalformedSerializer);
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
				uint32 ContentIdVersion = PackageBulkContentIdVersion;
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
				// Runtime-only targets have no persistent identity, including children
				// whose own flags do not repeat their transient Outer's flags.
				if (Value && !IsTransientObjectGraph(Value))
				{
					if (auto It = ObjectIds.find(Value); It != ObjectIds.end())
					{
						Kind = 1;
						Id = It->second;
						if (!CurrentObject)
						{
							FailCapture(EPackageCaptureReason::ReferenceOutsideObject, EArchiveFailureCode::MalformedSerializer);
							return;
						}
						Package.InternalReferences.emplace_back(CurrentObject->Id, Id);
					}
					else
					{
						DPackage* ExternalPackage = Value->GetPackage();
						FObjectPath TargetPath;
						const auto PathResult = FObjectPath::TryCreateWithDiagnostic(Value->GetObjectPath(), TargetPath);
						if (!ExternalPackage || !PathResult)
						{
							CaptureFailure.ObjectPath = Value->GetObjectPath();
							if (!PathResult) CaptureFailure.Message = FormatObjectError(PathResult.Error);
							FailCapture(EPackageCaptureReason::InvalidHardReference, EArchiveFailureCode::InvalidObjectReference);
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
					CaptureFailure.ObjectPath = Path;
					CaptureFailure.Actual = Path.size();
					CaptureFailure.Expected = MaximumPackageStringBytes;
					FailCapture(EPackageCaptureReason::SoftPathLimit, EArchiveFailureCode::InvalidPath);
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
					const auto ValueResult = Value.Storage.DefaultConstruct(&Property, ArrayIndex);
					if (!ValueResult)
					{
						CaptureFailure.Message = FormatPropertyValueError(ValueResult.Error);
						FailCapture(EPackageCaptureReason::ReplacementValue, EArchiveFailureCode::InvalidData);
						return EArchivePropertySaveDisposition::Omit;
					}
					const auto SnapshotResult = RestorePropertyValue(&Property, Value.Storage.GetContainer(), ArrayIndex, It->Replacement);
					if (!SnapshotResult)
					{
						CaptureFailure.Message = FormatPropertySnapshotError(SnapshotResult.Error);
						FailCapture(EPackageCaptureReason::ReplacementValue, EArchiveFailureCode::InvalidData);
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
					FailCapture(EPackageCaptureReason::ObjectOutsideGraph, EArchiveFailureCode::InvalidObjectReference);
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
						FailCapture(EPackageCaptureReason::OuterOutsideGraph, EArchiveFailureCode::InvalidObjectReference);
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
					FailCapture(EPackageCaptureReason::FieldOutsideObject, EArchiveFailureCode::MalformedSerializer);
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
				const FPackageCaptureOptions& InOptions,
				bool bInCapturePayload) -> FArchiveState
			{
				const bool bCooked = InOptions.bCooking;
				return {
					.Direction = EArchiveDirection::Save,
					.Purpose = bCooked ? EArchivePurpose::CookedPackage
						: (bInCapturePayload ? EArchivePurpose::AuthoredPackage
							: EArchivePurpose::Discovery),
					.Capabilities = EArchiveCapability::None,
					.bPersistent = bCooked,
					.bCooking = bCooked,
					.bFilterEditorOnly = bCooked && !InOptions.bRetainEditorOnlyData,
					.Target = InOptions.Target};
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
					FailCapture(EPackageCaptureReason::ValueOutsideField, EArchiveFailureCode::MalformedSerializer);
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
			const FPackageCaptureOptions& Options;
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
			auto SetCurrentObject(DObject* Object) -> void
			{
				CurrentDObject = Object;
				CaptureFailure.ObjectPath = Object ? Object->GetObjectPath() : std::string{};
			}
		};

		auto TranslateArchiveFailure(const FAuthoredCaptureArchive& Archive) -> FPackageCaptureResult
		{
			const FArchiveFailure* Failure = Archive.GetFailure();
			if (!Failure) return {};
			FPackageCaptureError Error = Archive.CaptureFailure;
			if (Error.Reason == EPackageCaptureReason::None) Error.Reason = EPackageCaptureReason::ArchiveFailure;
			Error.ArchiveCode = Failure->Code;
			Error.ArchivePath = Failure->Path;
			// Capture the first Archive diagnostic while its callback context is alive.
			if (Error.Message.empty()) Error.Message = Failure->Message;
			Error.Message = std::format("{} (object '{}', archive '{}')",
				Error.Message, Error.ObjectPath, Error.ArchivePath);
			return {std::move(Error)};
		}

		auto GatherObjects(DObject* Object, std::vector<DObject*>& OutObjects) -> void
		{
			if (!Object) return;
			OutObjects.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(
				Object, Object->GetPackage() && Object->GetPackage()->IsGraphPrivate()
					? EObjectQueryScope::IncludeUnpublished : EObjectQueryScope::LiveOnly))
				if (!Inner->IsTemplateObject()) GatherObjects(Inner, OutObjects);
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
				if (Candidate->HasAnyObjectFlags(EObjectFlags::Transient)) return true;
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
			// Every top-level export is a package root. Seeding only the first
			// asset discards independent material variants in multi-asset packages.
			for (size_t Index = 0; Index < Discovery.Objects.size(); ++Index)
				if (Discovery.Objects[Index].OuterId == 0) Reachable[Index] = 1;
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
			const FPackageCaptureOptions& Options,
			bool bCapturePayload,
			uint32 TargetFormatVersion,
			FXxHash128 ContainerHash,
			FCapturedPackage& OutPackage) -> FPackageCaptureResult
		{
			FAuthoredCaptureArchive Archive(
				ObjectIds, Options, bCapturePayload, TargetFormatVersion, ContainerHash);
			for (DObject* Object : Objects)
			{
				Archive.SetCurrentObject(Object);
				{
					auto Scope = Archive.EnterObject(*Object);
					if (Options.bCooking)
						Object->SerializeCooked(Archive);
					else Object->Serialize(Archive);
				}
				if (Archive.HasError()) return TranslateArchiveFailure(Archive);
			}
			OutPackage = Archive.TakePackage();
			OutPackage.CustomVersions = Archive.GetVersionContext().CustomVersions;
			std::ranges::sort(OutPackage.CustomVersions);
			return {};
		}

		auto ComputeContainerHash(std::span<const FPackageBulkStoragePayload> Payloads) -> FXxHash128
		{
			std::vector<const FPackageBulkStoragePayload*> Sorted;
			for (const auto& Payload : Payloads)
				if (Payload.Descriptor.LogicalByteCount >= PackageBulkExternalThreshold)
					Sorted.push_back(&Payload);
			std::ranges::sort(Sorted, {}, [](const FPackageBulkStoragePayload* Payload) {
				return Payload->Descriptor.PayloadId;
			});
			if (Sorted.empty()) return {};
			FByteBuffer Bytes;
			FCanonicalMemoryWriter Writer(Bytes, EArchivePurpose::BulkData);
			uint64 Count = Sorted.size();
			Writer << Count;
			for (const FPackageBulkStoragePayload* Payload : Sorted)
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
			ObjectPackage::FSerializedType& OutType, FPackageCaptureError& OutError) -> bool
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
				OutError = {.Reason = EPackageCaptureReason::WeakObject};
				return false;
			case K::Struct:
				Type.Kind = O::Struct;
				Type.QualifiedName = Input.QualifiedType.ToString();
				break;
			case K::Array: case K::FixedArray:
			{
				ObjectPackage::FSerializedType Element;
				if (!Input.ElementType) { OutError = {.Reason = EPackageCaptureReason::MissingChildType}; return false; }
				if (!AdaptLinkerType(*Input.ElementType, Element, OutError)) return false;
				Type.Kind = Input.Kind == K::Array ? O::Array : O::FixedArray;
				Type.Parameter = Input.Kind == K::FixedArray ? Input.FixedArrayDimension : 0;
				Type.Children.push_back(std::move(Element));
				OutType = std::move(Type);
				return true;
			}
			case K::Map:
			{
				ObjectPackage::FSerializedType Key, Value;
				if (!Input.KeyType || !Input.ValueType) { OutError = {.Reason = EPackageCaptureReason::MissingChildType}; return false; }
				if (!AdaptLinkerType(*Input.KeyType, Key, OutError)
					|| !AdaptLinkerType(*Input.ValueType, Value, OutError)) return false;
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
			ObjectPackage::FLinkerTables& Linker, FPackageCaptureError& OutError) -> bool
		{
			if (Node.Kind != ENodeKind::Field)
			{
				OutError = {.Reason = EPackageCaptureReason::FieldEventKind};
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
			if (!AdaptLinkerType(Node.Field.LogicalType, Type, OutError))
			{
				OutError.SchemaName = Node.Field.DeclaringType.ToString();
				OutError.FieldName = Node.Field.Name.ToString();
				return false;
			}
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
				OutError = {.Reason = EPackageCaptureReason::FieldTypeChanged, .SchemaName = SchemaName, .FieldName = FieldName};
				return false;
			}
			Linker.Types.push_back(std::move(Type));
			return true;
		}

		auto ExpandLinkerType(const ObjectPackage::FSerializedType& Input,
			std::span<const ObjectPackage::FSerializedSchema> Schemas,
			ObjectPackage::FSerializedType& OutType, FPackageCaptureError& OutError,
			uint32 Depth = 0) -> bool
		{
			if (Depth > ObjectPackage::DastMaximumValueDepth)
			{
				OutError = {.Reason = EPackageCaptureReason::TypeDepth, .SchemaName = Input.QualifiedName, .Actual = Depth, .Expected = ObjectPackage::DastMaximumValueDepth};
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
			FPackageCaptureError& OutError) -> bool
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
		auto ReadCaptured(FByteView Bytes, size_t& Offset, T& Out) -> bool
		{
			if (sizeof(T) > Bytes.size() - Offset) return false;
			std::memcpy(&Out, Bytes.data() + Offset, sizeof(T)); Offset += sizeof(T); return true;
		}

		auto ReadCapturedString(FByteView Bytes, size_t& Offset, std::string& Out) -> bool
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
			FPackageCaptureError& OutError, const FDefaultDeltaNode* DeltaNode = nullptr) -> bool
		{
			using K = FArchiveLogicalTypeDescriptor::EKind;
			auto Invalid = [&]() {
				OutError = {.Reason = EPackageCaptureReason::ValueManifest, .SchemaName = Type.QualifiedType.ToString(), .FieldName = Node.Field.Name.ToString(), .Actual = Node.Raw.size()}; return false;
			};
			if (Type.Kind == K::FixedArray || Type.Kind == K::Array || Type.Kind == K::Map)
			{
				uint64 Count = Type.Kind == K::FixedArray ? Type.FixedArrayDimension : 0;
				if (Type.Kind != K::FixedArray)
				{
					size_t Offset = 0; if (!ReadCaptured(std::span(Node.Raw), Offset, Count) || Offset != Node.Raw.size()) return Invalid();
				}
				const uint64 ExpectedChildren = Type.Kind == K::Map ? Count * 2 : Count;
				if (Node.Children.size() != ExpectedChildren)
				{
					Invalid(); OutError.Actual = Node.Children.size(); OutError.Expected = ExpectedChildren; return false;
				}
				for (size_t Index = 0; Index < Node.Children.size(); ++Index)
				{
					const auto* ChildType = Type.Kind == K::Map ? (Index % 2 == 0 ? Type.KeyType.get() : Type.ValueType.get()) : Type.ElementType.get();
					if (!ChildType) return Invalid();
					ObjectPackage::FSerializedValue Child;
					const FDefaultDeltaNode* ChildDelta = DeltaNode && Index < DeltaNode->Elements.size() ? DeltaNode->Elements[Index].get() : nullptr;
					if (!MaterializeLinkerValue(Node.Children[Index], *ChildType, Package, InternalReferenceIds,
						Linker, Child, OutError, ChildDelta))
					{
						OutError.Route.insert(OutError.Route.begin(), std::to_string(Index)); return false;
					}
					Out.Elements.push_back(std::move(Child));
				}
				return true;
			}
			if (Type.Kind == K::Struct)
			{
				if (!Node.Raw.empty()) return Invalid();
				Out.FieldTypes.emplace();
				Out.Baseline = !DeltaNode || DeltaNode->Baseline == EDefaultDeltaBaselineKind::None
					? EArchiveStructBaseline::Complete
					: DeltaNode->Baseline == EDefaultDeltaBaselineKind::StructTypeDefault
						? EArchiveStructBaseline::TypeDefault : EArchiveStructBaseline::Parent;
				for (const FCapturedNode& ChildNode : Node.Children)
				{
					const FDefaultDeltaFieldPlan* DeltaField = FindDeltaField(DeltaNode ? &DeltaNode->Fields : nullptr, ChildNode.Field);
					if (DeltaNode && !DeltaField) return Invalid();
					if (DeltaField && DeltaField->Disposition == EDefaultDeltaDisposition::Omitted) continue;
					const auto Schema = std::ranges::find(Linker.Schemas, Type.QualifiedType.ToString(), &ObjectPackage::FSerializedSchema::QualifiedName);
					if (Schema == Linker.Schemas.end()) return Invalid();
					const auto Field = std::ranges::find(Schema->Fields, ChildNode.Field.Name.ToString(), &ObjectPackage::FSerializedField::Name);
					if (Field == Schema->Fields.end()) return Invalid();
					Out.FieldTypes->push_back(Field->Type);
					ObjectPackage::FSerializedValue Child;
					if (!MaterializeLinkerValue(ChildNode, ChildNode.Field.LogicalType, Package, InternalReferenceIds,
						Linker, Child, OutError,
						DeltaField && DeltaField->Value ? DeltaField->Value.get() : nullptr))
					{
						OutError.Route.insert(OutError.Route.begin(), ChildNode.Field.Name.ToString()); return false;
					}
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
					if (const auto PathResult = FObjectPath::TryCreateWithDiagnostic(Out.Text, Path); !PathResult)
					{
						Invalid(); OutError.Message = FormatObjectError(PathResult.Error); return false;
					}
					Linker.Names.push_back(Out.Text);
					Linker.Summary.SoftPackageDependencies.push_back(Path.GetPackagePath());
				}
				break;
			}
			default: return Invalid();
			}
			return Offset == Node.Raw.size() || Invalid();
		}

		auto BuildLinkerTables(const FCapturedPackage& Captured, const FPackageCaptureOptions& Options,
			const FPackagePath& PackagePath,
			std::span<DObject* const> Objects, const FDefaultDeltaPlan& DeltaPlan,
			std::span<const ObjectPackage::FCustomVersion> CustomVersions,
			std::span<DObject* const> TopLevelAssets,
			ObjectPackage::FLinkerTables& Out, FPackageCaptureError& OutError, uint32 FormatVersion) -> bool
		{
			ObjectPackage::FLinkerTables Linker;
			Linker.FormatVersion = FormatVersion;
			Linker.Summary.PackagePath = PackagePath;
			Linker.Summary.HardPackageDependencies = Captured.Dependencies;
			for (const FObjectPath& Target : Captured.HardReferenceTargets)
				Linker.Imports.push_back({.ObjectPath = Target});
			Linker.CustomVersions.assign(CustomVersions.begin(), CustomVersions.end());
			std::vector<std::string> Paths(Captured.Objects.size());
			for (const auto& Object : Captured.Objects)
			{
				if (Object.Id == 0 || Object.Id > Captured.Objects.size() || Object.OuterId >= Object.Id)
				{
					OutError = {.Reason = EPackageCaptureReason::ObjectTopology, .ObjectPath = Object.ObjectName, .Actual = Object.Id, .Expected = Captured.Objects.size()}; return false;
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
				OutError = {.Reason = EPackageCaptureReason::DeltaGraph, .Actual = DeltaPlan.Objects.size(), .Expected = Captured.Objects.size()}; return false;
			}
			std::unordered_map<const DObject*, const FDefaultDeltaObjectPlan*> DeltaObjects;
			for (const FDefaultDeltaObjectPlan& DeltaObject : DeltaPlan.Objects)
			{
				if (!DeltaObject.Object || !DeltaObjects.emplace(DeltaObject.Object, &DeltaObject).second)
				{
					OutError = {.Reason = EPackageCaptureReason::DeltaObject}; return false;
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
					OutError = {.Reason = EPackageCaptureReason::OuterTopology, .ObjectPath = Object.ObjectName, .Actual = Object.OuterId}; return false;
				}
				Linker.Exports.push_back({.ObjectName = Object.ObjectName,
					.ClassName = Object.ClassName, .Outer = Outer});
			}

			for (DObject* Asset : TopLevelAssets)
			{
				const auto Source = std::ranges::find(Objects, Asset);
				if (!Asset || Source == Objects.end())
				{
					OutError = {.Reason = EPackageCaptureReason::MissingAsset, .ObjectPath = Asset ? Asset->GetObjectPath() : "<null>"};
					return false;
				}
				const size_t SourceIndex = static_cast<size_t>(std::distance(Objects.begin(), Source));
				ObjectPackage::FPackageIndex Export;
				FTopLevelAssetPath AssetPath;
				FObjectPath RedirectDestination;
				const auto PathResult = FTopLevelAssetPath::TryCreateWithDiagnostic(PackagePath, Asset->GetName(), AssetPath);
				if (!ObjectPackage::FPackageIndex::TryExport(
						InternalReferenceIds[SourceIndex] - 1, Export) || !PathResult)
				{
					OutError = {.Reason = EPackageCaptureReason::AssetIdentity, .ObjectPath = Asset->GetObjectPath()};
					if (!PathResult) OutError.Message = FormatObjectError(PathResult.Error);
					return false;
				}
				if (const auto It = Options.RedirectDestinations.find(Asset);
					It != Options.RedirectDestinations.end()) RedirectDestination = It->second;
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
					OutError = {.Reason = EPackageCaptureReason::MissingDeltaObject, .ObjectPath = Paths[Object.Id - 1]};
					return false;
				}
				const FDefaultDeltaObjectPlan& DeltaObject = *DeltaIt->second;
				Linker.Exports[CanonicalIndex].bUseClassDefaults = DeltaObject.ClassDefaultObject != nullptr;
				for (const auto& Field : Object.Fields)
				{
					const FDefaultDeltaFieldPlan* DeltaField = FindDeltaField(&DeltaObject.Fields, Field.Field);
					if (!DeltaField) { OutError = {.Reason = EPackageCaptureReason::MissingDeltaField, .ObjectPath = Paths[Object.Id - 1], .SchemaName = Field.Field.DeclaringType.ToString(), .FieldName = Field.Field.Name.ToString()}; return false; }
					if (DeltaField->Disposition == EDefaultDeltaDisposition::Omitted) continue;
					const std::string SchemaName = Field.Field.DeclaringType.ToString();
					const std::string FieldName = Field.Field.Name.ToString();
					const auto* Schema = FindLinkerSchema(Linker.Schemas, SchemaName);
					const auto SchemaField = Schema ? std::ranges::find(
						Schema->Fields, FieldName, &ObjectPackage::FSerializedField::Name)
						: std::vector<ObjectPackage::FSerializedField>::const_iterator{};
					if (!Schema || SchemaField == Schema->Fields.end())
					{
						OutError = {.Reason = EPackageCaptureReason::MissingSchemaField, .ObjectPath = Paths[Object.Id - 1], .SchemaName = SchemaName, .FieldName = FieldName}; return false;
					}
					ObjectPackage::FSerializedValue Value;
					if (!MaterializeLinkerValue(Field, Field.Field.LogicalType, Captured, InternalReferenceIds,
						Linker, Value, OutError,
						DeltaField->Value ? DeltaField->Value.get() : nullptr))
					{
						OutError.ObjectPath = Paths[Object.Id - 1];
						OutError.Route.insert(OutError.Route.begin(), FieldName);
						return false;
					}
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

}

namespace Durin
{
	static auto FormatCaptureDeltaFailure(const FDefaultDeltaDiagnostic& DeltaDiagnostic) -> std::string
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
		return Message;
	}

	auto FormatPackageCaptureError(const FPackageCaptureError& Error) -> std::string
	{
		if (!Error.Message.empty()) return Error.Message;
		switch (Error.Reason)
		{
		case EPackageCaptureReason::None: return {};
		case EPackageCaptureReason::RawOutsideField: return "Authored raw bytes require an active named field.";
		case EPackageCaptureReason::BulkVersion: return "Package bulk fields require a supported DAST package version.";
		case EPackageCaptureReason::BulkMetadata: return "Package bulk capture requires valid metadata and verified resident bytes.";
		case EPackageCaptureReason::BulkAlignment: return "Package bulk field alignment is invalid.";
		case EPackageCaptureReason::BulkLimit: return "Package bulk segment exceeds the 1 GiB limit.";
		case EPackageCaptureReason::BulkOutsideValue: return "Package bulk capture requires an active value node.";
		case EPackageCaptureReason::ReferenceOutsideObject: return "An internal reference was serialized outside an object scope.";
		case EPackageCaptureReason::InvalidHardReference: return "Cross-package hard references must target an exact persistent object.";
		case EPackageCaptureReason::SoftPathLimit: return "Soft object path exceeds the authored package bound.";
		case EPackageCaptureReason::ObjectOutsideGraph: return "The serializer entered an object outside the frozen package graph.";
		case EPackageCaptureReason::OuterOutsideGraph: return "Package inner object has an outer outside the frozen graph.";
		case EPackageCaptureReason::FieldOutsideObject: return "An authored field was entered outside an object scope.";
		case EPackageCaptureReason::ValueOutsideField: return "Authored values require an active named field.";
		case EPackageCaptureReason::WeakObject: return "Archive weak-object values cannot be represented by the package linker.";
		case EPackageCaptureReason::FieldEventKind: return "A discovered field node has the wrong event kind.";
		case EPackageCaptureReason::TypeDepth: return "Live reflected type exceeds the package nesting limit.";
		case EPackageCaptureReason::ValueManifest: return "Captured Archive events do not match their frozen logical type.";
		case EPackageCaptureReason::ObjectTopology: return "Captured object ids are not topological.";
		case EPackageCaptureReason::DeltaGraph: return "Delta plan object graph differs from Archive discovery.";
		case EPackageCaptureReason::DeltaObject: return "Delta plan contains an invalid or duplicate object.";
		case EPackageCaptureReason::OuterTopology: return "Captured object has invalid Outer topology.";
		case EPackageCaptureReason::AssetIdentity: return "A top-level asset has invalid linker identity.";
		case EPackageCaptureReason::MissingDeltaField: return "Delta plan is missing an Archive field.";
		case EPackageCaptureReason::MissingSchemaField: return "A captured field is absent from its linker schema.";
		case EPackageCaptureReason::PackageType: return "Only asset packages can be serialized.";
		case EPackageCaptureReason::MissingAssets: return "Package has no top-level assets.";
		case EPackageCaptureReason::CookTarget: return "Cooked package serialization requires an explicit target platform and profile.";
		case EPackageCaptureReason::PackagePath: return "Package has an invalid asset path.";
		case EPackageCaptureReason::ClassDefaults: return "Class default construction failed before delta capture.";
		case EPackageCaptureReason::OverrideOutsideGraph: return "A save override targets an object outside the frozen package graph.";
		case EPackageCaptureReason::OmittedAsset: return "A top-level asset cannot be omitted from its own package save.";
		case EPackageCaptureReason::DiscoveryMutation: return "Archive discovery mutated the frozen package object graph.";
		case EPackageCaptureReason::CookGraph: return "Cooked object reachability discovery produced an invalid graph.";
		case EPackageCaptureReason::BulkIdentity: return "Authored bulk container identity could not be computed.";
		case EPackageCaptureReason::EmissionMutation: return "Archive emission changed the frozen object, field, type, dependency, or version manifest.";
		case EPackageCaptureReason::FieldTypeChanged: return std::format("Repeated field discovery changed the logical type of {}::{}.", Error.SchemaName, Error.FieldName);
		case EPackageCaptureReason::MissingAsset: return std::format("A top-level asset is absent from the captured export topology: {}.", Error.ObjectPath);
		case EPackageCaptureReason::MissingDeltaObject: return std::format("Delta plan object graph differs at {}.", Error.ObjectPath);
		case EPackageCaptureReason::MissingChildType: return "An Archive container type has no child descriptor.";
		case EPackageCaptureReason::ReplacementValue:
			return "The save override replacement could not be materialized.";
		case EPackageCaptureReason::ArchiveFailure:
			return std::format("Package Archive failed at '{}', code={}." , Error.ArchivePath, static_cast<uint32>(Error.ArchiveCode.value_or(EArchiveFailureCode::InvalidData)));
		case EPackageCaptureReason::DefaultDelta: return "Default-relative logical planning failed.";
		}
		return {};
	}

	auto GetPackageCaptureSaveError(const FPackageCaptureError& Error) -> EPackageSaveError
	{
		switch (Error.Reason)
		{
		case EPackageCaptureReason::None: return EPackageSaveError::None;
		case EPackageCaptureReason::PackageType: return EPackageSaveError::InvalidPackageType;
		case EPackageCaptureReason::PackagePath: case EPackageCaptureReason::SoftPathLimit: return EPackageSaveError::InvalidPath;
		case EPackageCaptureReason::MissingAssets: case EPackageCaptureReason::ClassDefaults:
		case EPackageCaptureReason::OverrideOutsideGraph: case EPackageCaptureReason::OmittedAsset:
		case EPackageCaptureReason::CookGraph: return EPackageSaveError::InvalidObjectGraph;
		default: break;
		}
		if (Error.ArchiveCode == EArchiveFailureCode::UnsupportedVersion) return EPackageSaveError::UnsupportedVersion;
		if (Error.ArchiveCode == EArchiveFailureCode::InvalidObjectReference) return EPackageSaveError::InvalidObjectGraph;
		if (Error.ArchiveCode == EArchiveFailureCode::InvalidPath) return EPackageSaveError::InvalidPath;
		return EPackageSaveError::UnsupportedProperty;
	}

	auto CapturePackageLinker(DPackage* Package, EDefaultDeltaMode DeltaMode,
		const FPackageCaptureOptions& InputOptions,
		ObjectPackage::FLinkerTables& OutLinker, uint32 FormatVersion) -> FPackageCaptureResult
	{
		check(IsInGameThread());
		const auto& Options = InputOptions;
		if (Package && !Package->IsAssetPackage())
		{
			return {{.Reason = EPackageCaptureReason::PackageType}};
		}
		if (!Package || Package->GetTopLevelAssets().empty())
		{
			return {{.Reason = EPackageCaptureReason::MissingAssets}};
		}
		if (Options.bCooking
			&& (Options.Target.Platform.empty() || Options.Target.Profile.empty()))
		{
			return {{.Reason = EPackageCaptureReason::CookTarget}};
		}
		FPackagePath PackagePath;
		if (const auto PathResult = FPackagePath::TryCreateWithDiagnostic(Package->GetPackagePath(), PackagePath); !PathResult)
		{
			return {{.Reason = EPackageCaptureReason::PackagePath, .ObjectPath = Package->GetPackagePath(), .Message = FormatObjectError(PathResult.Error)}};
		}

		std::vector<DObject*> FrozenObjects;
		for (DObject* Asset : Package->GetTopLevelAssets())
			PackagePrivate::GatherObjects(Asset, FrozenObjects);
		if (DeltaMode == EDefaultDeltaMode::Enabled
			&& !Options.bCooking)
		{
			std::vector<DClass*> Classes;
			for (DObject* Object : FrozenObjects) Classes.push_back(Object->GetClass());
			if (!Private::CreateClassDefaultObjectsForBatch(Classes))
				return {{.Reason = EPackageCaptureReason::ClassDefaults}};
		}
		if (Options.SaveOverrides)
		{
			for (const FObjectSaveOverride& Override : Options.SaveOverrides->GetObjects())
			{
				if (!Override.Object
					|| std::ranges::find(FrozenObjects, Override.Object) == FrozenObjects.end())
				{
					return {{.Reason = EPackageCaptureReason::OverrideOutsideGraph, .ObjectPath = Override.Object ? Override.Object->GetObjectPath() : std::string{}}};
				}
			}
			for (DObject* Asset : Package->GetTopLevelAssets())
				if (const FObjectSaveOverride* RootOverride =
					Options.SaveOverrides->FindObject(*Asset);
					RootOverride && RootOverride->bOmitObject)
				{
					return {{.Reason = EPackageCaptureReason::OmittedAsset, .ObjectPath = Asset->GetObjectPath()}};
				}
		}
		std::vector<DObject*> Objects;
		for (DObject* Object : FrozenObjects)
			if (!PackagePrivate::IsObjectOmitted(Object, Options.SaveOverrides.get()))
				Objects.push_back(Object);
		std::unordered_map<DObject*, uint64> ObjectIds;
		for (size_t Index = 0; Index < Objects.size(); ++Index) ObjectIds.emplace(Objects[Index], Index + 1);
		PackagePrivate::FCapturedPackage Discovery;
		FPackageCaptureResult Result = PackagePrivate::CapturePackage(
			Objects, ObjectIds, Options, false,
			FormatVersion, {}, Discovery);
		if (!Result)
		{
			return Result;
		}
		if (!PackagePrivate::HasFrozenPackageGraph(Package, FrozenObjects))
		{
			return {{.Reason = EPackageCaptureReason::DiscoveryMutation}};
		}
		if (Options.bCooking
			&& !Options.bRetainEditorOnlyData)
		{
			if (!PackagePrivate::PruneUnreachableCookedObjects(Discovery, Objects))
			{
				return {{.Reason = EPackageCaptureReason::CookGraph}};
			}
			ObjectIds.clear();
			for (size_t Index = 0; Index < Objects.size(); ++Index)
				ObjectIds.emplace(Objects[Index], Index + 1);
			Result = PackagePrivate::CapturePackage(
				Objects, ObjectIds, Options, false,
				FormatVersion, {}, Discovery);
			if (!Result)
			{
				return Result;
			}
		}
		const FXxHash128 ContainerHash = PackagePrivate::ComputeContainerHash(Discovery.BulkPayloads);
		if (!Discovery.BulkPayloads.empty()
			&& std::ranges::any_of(Discovery.BulkPayloads, [](const FPackageBulkStoragePayload& Payload) {
				return Payload.Descriptor.LogicalByteCount > PackagePrivate::PackageBulkExternalThreshold;
			}) && ContainerHash.IsZero())
		{
			return {{.Reason = EPackageCaptureReason::BulkIdentity}};
		}
		PackagePrivate::FCapturedPackage Captured;
		Result = PackagePrivate::CapturePackage(
			Objects, ObjectIds, Options, true,
			FormatVersion, ContainerHash, Captured);
		if (!Result)
		{
			return Result;
		}
		if (!PackagePrivate::HasFrozenPackageGraph(Package, FrozenObjects)
			|| !PackagePrivate::EqualManifest(Discovery, Captured))
		{
			return {{.Reason = EPackageCaptureReason::EmissionMutation}};
		}

		FDefaultDeltaPlan DeltaPlan;
		FDefaultDeltaDiagnostic DeltaDiagnostic;
		const EDefaultDeltaMode EffectiveDeltaMode =
			Options.bCooking
				|| (Options.SaveOverrides && !Options.SaveOverrides->IsEmpty())
			? EDefaultDeltaMode::NoDelta : DeltaMode;
		FArchiveState DeltaContext;
		if (Options.bCooking)
		{
			DeltaContext.bPersistent = true;
			DeltaContext.bCooking = true;
			DeltaContext.bFilterEditorOnly = !Options.bRetainEditorOnlyData;
			DeltaContext.Target = Options.Target;
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
			return {{.Reason = EPackageCaptureReason::DefaultDelta, .Message = FormatCaptureDeltaFailure(DeltaDiagnostic)}};
		}
		std::erase_if(DeltaPlan.Objects, [&](const FDefaultDeltaObjectPlan& ObjectPlan) {
			return std::ranges::find(Objects, ObjectPlan.Object) == Objects.end();
		});
		const auto& CustomVersions = Captured.CustomVersions;
		FPackageCaptureError LinkerError;
		if (!PackagePrivate::BuildLinkerTables(Captured, Options, PackagePath, Objects,
				DeltaPlan, CustomVersions, Package->GetTopLevelAssets(), OutLinker, LinkerError, FormatVersion))
		{
			return {std::move(LinkerError)};
		}
		return {};
	}

}
