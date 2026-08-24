#include "AssetMutationReferenceInternal.h"
#include "AssetPackageCodec.h"
#include "AssetPackageValueCodec.h"
#include "Asset/PackageVersionPolicy.h"

#include "DObject/Class.h"
#include "DObject/DObjectArray.h"
#include "DObject/DurinPropertyTypes.h"
#include "DObject/Package.h"

namespace Durin::Asset
{
	using Private::AssetReferenceLess;
	using Private::DecodeReferenceByteToolValue;
	using Private::FByteReader;
	using Private::FByteWriter;
	using Private::GetSerializedTypeSignature;
	using Private::IsSerializedTypeSignatureCompatible;
	using Private::MaximumPackageStringBytes;

	namespace
	{
		auto Error(EAssetError Code, std::string Message) -> FAssetResult
		{
			return {Code, std::move(Message)};
		}

		struct FPackageFile
		{
			uint32 FormatVersion = 0;
			std::string AssetClassName;
			EAssetRegistryEntryKind EntryKind =
				EAssetRegistryEntryKind::Asset;
			FAssetPath RedirectDestination;
			std::vector<FAssetPath> Dependencies;
		};

		auto GatherObjects(
			DObject* Object,
			std::vector<DObject*>& OutObjects) -> void
		{
			if (!Object) return;
			OutObjects.push_back(Object);
			for (DObject* Inner : GDObjectArray.GetObjectsWithOuter(
				Object, EObjectQueryScope::LiveOnly))
				GatherObjects(Inner, OutObjects);
		}

	constexpr uint32 MaximumReferenceContainerDepth = 4;
	constexpr uint64 MaximumReferencesPerPackage = 100000;
	constexpr uint64 MaximumReferencesPerSnapshot = 1000000;
	constexpr uint64 MaximumReferenceDisplayRouteBytes = 4 * 1024;
	constexpr uint64 MaximumReferenceRouteTokenBytes = 1024 * 1024;

	template<typename Predicate>
	auto ContainsPropertyMatchingImpl(
		const FProperty* Property,
		const Predicate& Matches,
		std::unordered_set<const DStruct*>& VisitingStructs) -> bool
	{
		if (!Property) return false;
		if (Matches(Property)) return true;
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Array:
			return ContainsPropertyMatchingImpl(
				static_cast<const FArrayProperty*>(Property)->GetInner(), Matches, VisitingStructs);
		case DurinCodeGen::EPropertyGenFlags::Map:
			return ContainsPropertyMatchingImpl(
				static_cast<const FMapProperty*>(Property)->GetKeyProp(), Matches, VisitingStructs)
				|| ContainsPropertyMatchingImpl(
					static_cast<const FMapProperty*>(Property)->GetValueProp(), Matches, VisitingStructs);
		case DurinCodeGen::EPropertyGenFlags::Struct:
		{
			DStruct* Struct = static_cast<const FStructProperty*>(Property)->GetStruct();
			if (!Struct || !VisitingStructs.insert(Struct).second) return false;
			bool bContainsMatch = false;
			Struct->ForEachProperty([&](FProperty* Field) {
				if (!bContainsMatch && Field && !Field->HasAnyPropertyFlags(EPropertyFlags::Transient))
					bContainsMatch = ContainsPropertyMatchingImpl(Field, Matches, VisitingStructs);
			}, false);
			VisitingStructs.erase(Struct);
			return bContainsMatch;
		}
		default:
			return false;
		}
	}

	template<typename Predicate>
	auto ContainsPropertyMatching(const FProperty* Property, const Predicate& Matches) -> bool
	{
		std::unordered_set<const DStruct*> VisitingStructs;
		return ContainsPropertyMatchingImpl(Property, Matches, VisitingStructs);
	}

	auto ContainsAssetReferenceProperty(const FProperty* Property) -> bool
	{
		return ContainsPropertyMatching(Property, [](const FProperty* Candidate) {
			return Candidate->GetKind() == DurinCodeGen::EPropertyGenFlags::Object
				|| Candidate->GetKind() == DurinCodeGen::EPropertyGenFlags::SoftObject;
		});
	}

	auto ContainsSoftObjectProperty(const FProperty* Property) -> bool
	{
		return ContainsPropertyMatching(Property, [](const FProperty* Candidate) {
			return Candidate->GetKind() == DurinCodeGen::EPropertyGenFlags::SoftObject;
		});
	}

	auto CompareReferenceRoute(
		std::span<const FAssetReferenceRouteSegment> Left,
		std::span<const FAssetReferenceRouteSegment> Right) -> int
	{
		const size_t Count = std::min(Left.size(), Right.size());
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (Left[Index].Kind != Right[Index].Kind)
				return static_cast<uint8>(Left[Index].Kind) < static_cast<uint8>(Right[Index].Kind) ? -1 : 1;
			if (Left[Index].Kind == EAssetReferenceRouteKind::MapValue)
			{
				if (Left[Index].MapKeyToken != Right[Index].MapKeyToken)
					return std::ranges::lexicographical_compare(
						Left[Index].MapKeyToken, Right[Index].MapKeyToken) ? -1 : 1;
			}
			else if (Left[Index].Kind == EAssetReferenceRouteKind::StructField)
			{
				const auto LeftField = std::tie(
					Left[Index].DeclaringType, Left[Index].FieldName);
				const auto RightField = std::tie(
					Right[Index].DeclaringType, Right[Index].FieldName);
				if (LeftField != RightField) return LeftField < RightField ? -1 : 1;
			}
			else if (Left[Index].Index != Right[Index].Index)
				return Left[Index].Index < Right[Index].Index ? -1 : 1;
		}
		if (Left.size() == Right.size()) return 0;
		return Left.size() < Right.size() ? -1 : 1;
	}

	auto AssetReferenceLessImpl(
		const FAssetReferenceEdge& Left,
		const FAssetReferenceEdge& Right) -> bool
	{
		const auto LeftKey = std::tuple(
			Left.TargetPath.GetView(), Left.SourcePackage.GetView(), Left.SourceObjectId,
			std::string_view(Left.DeclaringType), std::string_view(Left.FieldName), Left.Kind);
		const auto RightKey = std::tuple(
			Right.TargetPath.GetView(), Right.SourcePackage.GetView(), Right.SourceObjectId,
			std::string_view(Right.DeclaringType), std::string_view(Right.FieldName), Right.Kind);
		if (LeftKey != RightKey) return LeftKey < RightKey;
		return CompareReferenceRoute(Left.Route, Right.Route) < 0;
	}

	auto AppendMapTokenDisplay(std::string& Path, std::span<const std::byte> Token) -> void
	{
		Path.append("[key:");
		for (const std::byte Byte : Token)
			Path.append(std::format("{:02x}", std::to_integer<uint32>(Byte)));
		Path.push_back(']');
	}

	struct FReferenceExtractionContext
	{
		const FAssetPath& SourcePackage;
		const FAssetPackageFingerprint& Fingerprint;
		const FAssetPackageObjectInspection& Object;
		std::string_view DeclaringType;
		std::string_view FieldName;
		EAssetReferenceKind ObjectKind = EAssetReferenceKind::HardObject;
		std::vector<FAssetReferenceEdge>& References;
	};

	auto ExtractReferenceValue(
		FProperty* Property,
		FByteReader& Reader,
		const FReferenceExtractionContext& Context,
		std::vector<FAssetReferenceRouteSegment>& Route,
		const std::string& PropertyPath,
		uint32 ContainerDepth) -> FAssetResult;

	auto ExtractReferencePropertyValues(
		FProperty* Property,
		std::span<const std::byte> Payload,
		const FReferenceExtractionContext& Context,
		std::vector<FAssetReferenceRouteSegment>& Route,
		const std::string& PropertyPath,
		uint32 ContainerDepth) -> FAssetResult
	{
		FByteReader Reader{Payload};
		for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
		{
			const bool bFixedArray = Property->GetArrayDim() > 1;
			std::string ElementPath = PropertyPath;
			if (bFixedArray)
			{
				if (ContainerDepth >= MaximumReferenceContainerDepth)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexDepthExceeded: fixed-array route exceeds four levels.");
				Route.push_back({
					.Kind = EAssetReferenceRouteKind::FixedArray,
					.Index = ArrayIndex});
				ElementPath.append(std::format("[fixed:{}]", ArrayIndex));
			}
			FAssetResult Result = ExtractReferenceValue(
				Property, Reader, Context, Route, ElementPath,
				ContainerDepth + (bFixedArray ? 1 : 0));
			if (bFixedArray) Route.pop_back();
			if (!Result) return Result;
		}
		if (Reader.Offset != Payload.size())
			return Error(EAssetError::CorruptFile,
				std::format("SoftReferencePayloadTrailingBytes: {} has trailing bytes.", PropertyPath));
		return {};
	}

	auto ExtractReferenceValue(
		FProperty* Property,
		FByteReader& Reader,
		const FReferenceExtractionContext& Context,
		std::vector<FAssetReferenceRouteSegment>& Route,
		const std::string& PropertyPath,
		uint32 ContainerDepth) -> FAssetResult
	{
		if (!Property)
			return Error(EAssetError::TypeMismatch,
				"SoftReferenceSchemaMismatch: reflected property metadata is missing.");
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Object:
		{
			auto* ObjectProperty = static_cast<FObjectProperty*>(Property);
			if (!ObjectProperty->IsObjectPtrWrapper())
				return Error(EAssetError::UnsupportedProperty,
					"AssetReferenceSchemaMismatch: raw object pointers are unsupported.");
			uint8 ReferenceKind = 0;
			if (!Reader.Read(ReferenceKind))
				return Error(EAssetError::CorruptFile,
					std::format("AssetReferencePayloadTruncated: {} has no reference tag.", PropertyPath));
			if (ReferenceKind == 0) return {};
			if (ReferenceKind == 1)
			{
				uint64 ObjectId = 0;
				if (!Reader.Read(ObjectId) || ObjectId == 0)
					return Error(EAssetError::InvalidObjectGraph,
						std::format("AssetReferenceInternalObject: {} has an invalid object id.", PropertyPath));
				return {};
			}
			if (ReferenceKind != 2)
				return Error(EAssetError::CorruptFile,
					std::format("AssetReferencePayloadTag: {} has unknown tag {}.", PropertyPath, ReferenceKind));
			std::string PathString;
			FAssetPath TargetPath;
			if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
				|| !FAssetPath::TryCreate(PathString, TargetPath))
				return Error(EAssetError::InvalidPath,
					std::format("AssetReferenceInvalidPath: {} has an invalid external path.", PropertyPath));
			DClass* ExpectedClass = ObjectProperty->GetReferencedClass();
			if (!ExpectedClass)
				return Error(EAssetError::TypeMismatch,
					std::format("AssetReferenceSchemaMismatch: {} has no referenced class.", PropertyPath));
			if (PropertyPath.size() > MaximumReferenceDisplayRouteBytes)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexDisplayRouteExceeded: display route exceeds 4 KiB.");
			if (Context.References.size() >= MaximumReferencesPerPackage)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexOccurrenceExceeded: package exceeds 100,000 occurrences.");
			Context.References.push_back({
				.SourcePackage = Context.SourcePackage,
				.SourceFingerprint = Context.Fingerprint,
				.SourceObjectId = Context.Object.Id,
				.SourceClass = Context.Object.ClassName,
				.DeclaringType = std::string(Context.DeclaringType),
				.FieldName = std::string(Context.FieldName),
				.Kind = Context.ObjectKind,
				.ExpectedClass = ExpectedClass->GetQualifiedName().ToString(),
				.TargetPath = std::move(TargetPath),
				.Route = Route,
				.DisplayRoute = PropertyPath});
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::SoftObject:
		{
			uint8 ReferenceKind = 0;
			if (!Reader.Read(ReferenceKind))
				return Error(EAssetError::CorruptFile,
					std::format("SoftReferencePayloadTruncated: {} has no reference tag.", PropertyPath));
			if (ReferenceKind == 0) return {};
			if (ReferenceKind != 1)
				return Error(EAssetError::CorruptFile,
					std::format("SoftReferencePayloadTag: {} has unknown tag {}.", PropertyPath, ReferenceKind));
			std::string PathString;
			if (!Reader.ReadString(PathString, MaximumPackageStringBytes) || PathString.empty())
				return Error(EAssetError::CorruptFile,
					std::format("SoftReferencePayloadPath: {} is truncated or overlong.", PropertyPath));
			FSoftObjectPath SoftPath;
			std::string PathError;
			if (!FSoftObjectPath::TryCreate(PathString, SoftPath, &PathError))
				return Error(EAssetError::InvalidPath, std::format(
					"SoftReferenceInvalidPath: {} contains '{}': {}",
					PropertyPath, PathString, PathError));
			auto* SoftProperty = static_cast<FSoftObjectProperty*>(Property);
			DClass* ExpectedClass = SoftProperty->GetExpectedClass();
			if (!ExpectedClass)
				return Error(EAssetError::TypeMismatch,
					std::format("SoftReferenceSchemaMismatch: {} has no expected class.", PropertyPath));
			if (PropertyPath.size() > MaximumReferenceDisplayRouteBytes)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexDisplayRouteExceeded: display route exceeds 4 KiB.");
			if (Context.References.size() >= MaximumReferencesPerPackage)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexOccurrenceExceeded: package exceeds 100,000 occurrences.");
			Context.References.push_back({
				.SourcePackage = Context.SourcePackage,
				.SourceFingerprint = Context.Fingerprint,
				.SourceObjectId = Context.Object.Id,
				.SourceClass = Context.Object.ClassName,
				.DeclaringType = std::string(Context.DeclaringType),
				.FieldName = std::string(Context.FieldName),
				.Kind = EAssetReferenceKind::SoftObject,
				.ExpectedClass = ExpectedClass->GetQualifiedName().ToString(),
				.TargetPath = SoftPath.GetAssetPath(),
				.Route = Route,
				.DisplayRoute = PropertyPath});
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Array:
		{
			if (ContainerDepth >= MaximumReferenceContainerDepth)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexDepthExceeded: Array route exceeds four levels.");
			auto* Array = static_cast<FArrayProperty*>(Property);
			uint64 Count = 0;
			if (!Array->GetInner() || !Reader.Read(Count) || Count > 10000000)
				return Error(EAssetError::CorruptFile,
					std::format("SoftReferenceArrayPayload: {} has an invalid count.", PropertyPath));
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				Route.push_back({
					.Kind = EAssetReferenceRouteKind::ArrayElement,
					.Index = Index});
				FAssetResult Result = ExtractReferenceValue(
					Array->GetInner(), Reader, Context, Route,
					std::format("{}[{}]", PropertyPath, Index), ContainerDepth + 1);
				Route.pop_back();
				if (!Result) return Result;
			}
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Map:
		{
			if (ContainerDepth >= MaximumReferenceContainerDepth)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceIndexDepthExceeded: Map route exceeds four levels.");
			auto* Map = static_cast<FMapProperty*>(Property);
			uint64 Count = 0;
			if (!Map->GetKeyProp() || !Map->GetValueProp() || !Reader.Read(Count) || Count > 10000000)
				return Error(EAssetError::CorruptFile,
					std::format("SoftReferenceMapPayload: {} has an invalid count.", PropertyPath));
			if (ContainsAssetReferenceProperty(Map->GetKeyProp()))
				return Error(EAssetError::TypeMismatch,
					"AssetReferenceSchemaMismatch: reference Map keys are unsupported.");
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				FReflectedValueStorage KeyStorage;
				std::string StorageError;
				if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError))
					return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
				FAssetResult KeyResult = DecodeReferenceByteToolValue(
					Map->GetKeyProp(),
					KeyStorage.GetContainer(),
					0,
					Reader,
					{},
					AssetPackageObjectStreamVersion);
				if (!KeyResult)
				{
					KeyResult.Message = std::format("SoftReferenceMapKey[{}]: {}", Index, KeyResult.Message);
					return KeyResult;
				}
				std::vector<std::byte> KeyToken;
				if (!BuildCanonicalMapKeyToken(
					Map->GetKeyProp(), KeyStorage.GetContainer(), 0, KeyToken, &StorageError))
					return Error(EAssetError::TypeMismatch, std::move(StorageError));
				if (KeyToken.size() > MaximumReferenceRouteTokenBytes)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexRouteTokenExceeded: Map key token exceeds 1 MiB.");
				std::string ValuePath = PropertyPath;
				AppendMapTokenDisplay(ValuePath, KeyToken);
				if (ValuePath.size() > MaximumReferenceDisplayRouteBytes)
					return Error(EAssetError::CorruptFile,
						"AssetReferenceIndexDisplayPathExceeded: display path exceeds 4 KiB.");
				Route.push_back({
					.Kind = EAssetReferenceRouteKind::MapValue,
					.MapKeyToken = std::move(KeyToken)});
				FAssetResult Result = ExtractReferenceValue(
					Map->GetValueProp(), Reader, Context, Route, ValuePath, ContainerDepth + 1);
				Route.pop_back();
				if (!Result) return Result;
			}
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Struct:
		{
			auto* StructProperty = static_cast<FStructProperty*>(Property);
			DStruct* Struct = StructProperty->GetStruct();
			std::string StructName;
			uint64 FieldCount = 0;
			if (!Struct || !Reader.ReadString(StructName, MaximumPackageStringBytes)
				|| StructName != Struct->GetQualifiedName().ToString()
				|| !Reader.Read(FieldCount) || FieldCount > 100000)
				return Error(EAssetError::TypeMismatch,
					std::format("SoftReferenceStructPayload: {} has an incompatible header.", PropertyPath));
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				std::string DeclaringStruct;
				std::string FieldName;
				std::string Signature;
				uint8 Kind = 0;
				uint64 PayloadSize = 0;
				std::span<const std::byte> FieldPayload;
				if (!Reader.ReadString(DeclaringStruct, MaximumPackageStringBytes)
					|| !Reader.ReadString(FieldName, MaximumPackageStringBytes)
					|| !Reader.Read(Kind)
					|| !Reader.ReadString(Signature, MaximumPackageStringBytes)
					|| !Reader.Read(PayloadSize) || PayloadSize > Reader.Bytes.size()
					|| !Reader.ReadSpan(static_cast<size_t>(PayloadSize), FieldPayload))
					return Error(EAssetError::CorruptFile,
						std::format("SoftReferenceStructPayload: {} has a malformed field.", PropertyPath));
				if (DeclaringStruct != StructName) continue;
				FProperty* Field = Struct->FindPropertyBySerializedName(FName(FieldName), false);
				if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
					|| !ContainsAssetReferenceProperty(Field)) continue;
				if (static_cast<uint8>(Field->GetKind()) != Kind
					|| !IsSerializedTypeSignatureCompatible(Field, Signature))
					return Error(EAssetError::TypeMismatch, std::format(
						"SoftReferenceSchemaMismatch: {}.{} has an incompatible signature.",
						PropertyPath, FieldName));
				Route.push_back({
					.Kind = EAssetReferenceRouteKind::StructField,
					.DeclaringType = DeclaringStruct,
					.FieldName = FieldName});
				FAssetResult Result = ExtractReferencePropertyValues(
					Field, FieldPayload, Context, Route,
					std::format("{}.{}", PropertyPath, FieldName), ContainerDepth);
				Route.pop_back();
				if (!Result) return Result;
			}
			return {};
		}
		default:
			return Error(EAssetError::TypeMismatch, std::format(
				"SoftReferenceSchemaMismatch: {} does not contain a supported soft value.", PropertyPath));
		}
	}

	struct FLoadedSoftReferenceCollector
	{
		const FAssetPath& TargetPath;
		std::vector<FSoftObjectPtr*>& Values;
	};

	struct FLoadedSoftContainerVisitContext
	{
		FLoadedSoftReferenceCollector& Collector;
		FProperty* Inner = nullptr;
		uint32 ContainerDepth = 0;
		FAssetResult Result;
	};

	auto CollectLoadedSoftValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		FLoadedSoftReferenceCollector& Collector,
		uint32 ContainerDepth) -> FAssetResult;

	auto CollectLoadedSoftArrayElement(
		void* RawContext,
		uint64,
		void* Element) -> bool
	{
		auto& Context = *static_cast<FLoadedSoftContainerVisitContext*>(RawContext);
		Context.Result = CollectLoadedSoftValue(
			Context.Inner, Element, 0, Context.Collector,
			Context.ContainerDepth + 1);
		return Context.Result.Succeeded();
	}

	auto CollectLoadedSoftMapValue(
		void* RawContext,
		const void*,
		void* Value) -> bool
	{
		auto& Context = *static_cast<FLoadedSoftContainerVisitContext*>(RawContext);
		Context.Result = CollectLoadedSoftValue(
			Context.Inner, Value, 0, Context.Collector,
			Context.ContainerDepth + 1);
		return Context.Result.Succeeded();
	}

	auto CollectLoadedSoftValue(
		FProperty* Property,
		void* Container,
		uint32 ArrayIndex,
		FLoadedSoftReferenceCollector& Collector,
		uint32 ContainerDepth) -> FAssetResult
	{
		if (!Property || !Container)
			return Error(EAssetError::TypeMismatch,
				"SoftReferenceMoveSchemaMismatch: live property metadata is unavailable.");
		if (ContainerDepth > MaximumReferenceContainerDepth)
			return Error(EAssetError::CorruptFile,
				"SoftReferenceMoveDepthExceeded: live value exceeds four container levels.");
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::SoftObject:
		{
			auto* Value = static_cast<FSoftObjectProperty*>(Property)
				->GetSoftObjectPtr(Container, ArrayIndex);
			if (!Value)
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceMoveSchemaMismatch: live soft value accessor is unavailable.");
			if (!Value->IsNull()
				&& Value->GetSoftObjectPath().GetAssetPath() == Collector.TargetPath)
				Collector.Values.push_back(Value);
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Struct:
		{
			auto* StructProperty = static_cast<FStructProperty*>(Property);
			DStruct* Struct = StructProperty->GetStruct();
			if (!Struct || !Struct->HasCompleteAuthoredFields())
				return Error(EAssetError::TypeMismatch,
					"SoftReferenceMoveSchemaMismatch: live struct metadata is incomplete.");
			void* StructValue = Property->GetValuePtr(Container, ArrayIndex);
			FAssetResult Result;
			Struct->ForEachProperty([&](FProperty* Field) {
				if (!Result || !Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
					|| !ContainsSoftObjectProperty(Field)) return;
				for (uint32 FieldIndex = 0; FieldIndex < Field->GetArrayDim(); ++FieldIndex)
				{
					Result = CollectLoadedSoftValue(
						Field, StructValue, FieldIndex, Collector,
						ContainerDepth + (Field->GetArrayDim() > 1 ? 1 : 0));
					if (!Result) return;
				}
			}, false);
			return Result;
		}
		case DurinCodeGen::EPropertyGenFlags::Array:
		{
			auto* Array = static_cast<FArrayProperty*>(Property);
			if (!Array->GetInner()
				|| !Array->HasCapability(EArrayOpsFlags::MutableTraversal))
				return Error(EAssetError::UnsupportedProperty,
					"SoftReferenceMoveArrayUnavailable: mutable traversal is required.");
			FLoadedSoftContainerVisitContext Context{
				Collector, Array->GetInner(), ContainerDepth};
			const EContainerOpResult VisitResult = Array->VisitMutableElements(
				Container, &CollectLoadedSoftArrayElement, &Context, ArrayIndex);
			if (!Context.Result) return Context.Result;
			if (VisitResult != EContainerOpResult::Success)
				return Error(EAssetError::UnsupportedProperty,
					"SoftReferenceMoveArrayFailed: mutable traversal failed.");
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Map:
		{
			auto* Map = static_cast<FMapProperty*>(Property);
			if (!Map->GetValueProp()
				|| !Map->HasCapability(EMapOpsFlags::MutableMappedTraversal))
				return Error(EAssetError::UnsupportedProperty,
					"SoftReferenceMoveMapUnavailable: mutable value traversal is required.");
			FLoadedSoftContainerVisitContext Context{
				Collector, Map->GetValueProp(), ContainerDepth};
			const EContainerOpResult VisitResult = Map->VisitMutableEntries(
				Container, &CollectLoadedSoftMapValue, &Context, ArrayIndex);
			if (!Context.Result) return Context.Result;
			if (VisitResult != EContainerOpResult::Success)
				return Error(EAssetError::UnsupportedProperty,
					"SoftReferenceMoveMapFailed: mutable value traversal failed.");
			return {};
		}
		default:
			return Error(EAssetError::TypeMismatch,
				"SoftReferenceMoveSchemaMismatch: unsupported live soft container.");
		}
	}

	auto CollectLoadedPackageSoftReferences(
		DPackage* Package,
		const FAssetPath& TargetPath,
		std::vector<FSoftObjectPtr*>& OutValues) -> FAssetResult
	{
		if (!Package || !Package->GetAsset())
			return Error(EAssetError::InvalidObjectGraph,
				"SoftReferenceMoveInvalidPackage: loaded package has no asset.");
		std::vector<DObject*> Objects;
		GatherObjects(Package->GetAsset(), Objects);
		FLoadedSoftReferenceCollector Collector{TargetPath, OutValues};
		FAssetResult Result;
		for (DObject* Object : Objects)
		{
			Object->GetClass()->ForEachProperty([&](FProperty* Property) {
				if (!Result || !Property
					|| Property->HasAnyPropertyFlags(EPropertyFlags::Transient)
					|| !ContainsSoftObjectProperty(Property)) return;
				for (uint32 ArrayIndex = 0;
					ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
				{
					Result = CollectLoadedSoftValue(
						Property, Object, ArrayIndex, Collector,
						Property->GetArrayDim() > 1 ? 1 : 0);
					if (!Result) return;
				}
			}, true);
			if (!Result) return Result;
		}
		return {};
	}

	auto FindFixupDestination(
		const FAssetPath& Source,
		std::span<const FAssetRedirectorFixupMapping> Mappings) -> const FAssetPath*
	{
		const auto It = std::ranges::find(Mappings, Source,
			&FAssetRedirectorFixupMapping::RedirectorPath);
		return It == Mappings.end() ? nullptr : &It->FinalPath;
	}

	auto RewriteSerializedReferenceValue(
		FProperty* Property,
		FByteReader& Reader,
		FByteWriter& Writer,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64& RewriteCount,
		uint32 ContainerDepth) -> FAssetResult;

	auto RewriteSerializedReferenceProperty(
		FProperty* Property,
		std::span<const std::byte> Payload,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		std::vector<std::byte>& OutPayload,
		uint64& RewriteCount,
		uint32 ContainerDepth = 0) -> FAssetResult
	{
		FByteReader Reader{Payload};
		FByteWriter Writer;
		for (uint32 ArrayIndex = 0; ArrayIndex < Property->GetArrayDim(); ++ArrayIndex)
		{
			FAssetResult Result = RewriteSerializedReferenceValue(
				Property, Reader, Writer, Mappings, RewriteCount,
				ContainerDepth + (Property->GetArrayDim() > 1 ? 1 : 0));
			if (!Result) return Result;
		}
		if (Reader.Offset != Payload.size())
			return Error(EAssetError::CorruptFile,
				"AssetReferenceFixupTrailingBytes: field payload has trailing bytes.");
		OutPayload = std::move(Writer.Bytes);
		return {};
	}

	auto RewriteSerializedReferenceValue(
		FProperty* Property,
		FByteReader& Reader,
		FByteWriter& Writer,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64& RewriteCount,
		uint32 ContainerDepth) -> FAssetResult
	{
		if (!Property || ContainerDepth > MaximumReferenceContainerDepth)
			return Error(EAssetError::TypeMismatch,
				"AssetReferenceFixupSchemaMismatch: serialized container metadata is invalid.");
		switch (Property->GetKind())
		{
		case DurinCodeGen::EPropertyGenFlags::Object:
		{
			uint8 Kind = 0;
			if (!Reader.Read(Kind))
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupTruncated: missing object reference tag.");
			Writer.Write(Kind);
			if (Kind == 0) return {};
			if (Kind == 1)
			{
				uint64 ObjectId = 0;
				if (!Reader.Read(ObjectId) || ObjectId == 0)
					return Error(EAssetError::InvalidObjectGraph,
						"AssetReferenceFixupInternalObject: invalid object id.");
				Writer.Write(ObjectId);
				return {};
			}
			if (Kind != 2)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupTag: unknown object reference tag.");
			std::string PathString;
			FAssetPath Path;
			if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
				|| !FAssetPath::TryCreate(PathString, Path))
				return Error(EAssetError::InvalidPath,
					"AssetReferenceFixupPath: invalid external object path.");
			if (const FAssetPath* Destination = FindFixupDestination(Path, Mappings))
			{
				Writer.WriteString(Destination->GetView());
				++RewriteCount;
			}
			else Writer.WriteString(PathString);
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::SoftObject:
		{
			uint8 Kind = 0;
			if (!Reader.Read(Kind))
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupTruncated: missing soft reference tag.");
			Writer.Write(Kind);
			if (Kind == 0) return {};
			if (Kind != 1)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupTag: unknown soft reference tag.");
			std::string PathString;
			FSoftObjectPath Path;
			std::string PathError;
			if (!Reader.ReadString(PathString, MaximumPackageStringBytes)
				|| PathString.empty())
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupPath: soft path is truncated or overlong.");
			if (!FSoftObjectPath::TryCreate(PathString, Path, &PathError))
				return Error(EAssetError::InvalidPath, std::move(PathError));
			if (const FAssetPath* Destination = FindFixupDestination(
				Path.GetAssetPath(), Mappings))
			{
				Writer.WriteString(Destination->GetView());
				++RewriteCount;
			}
			else Writer.WriteString(PathString);
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Array:
		{
			auto* Array = static_cast<FArrayProperty*>(Property);
			uint64 Count = 0;
			if (!Array->GetInner() || !Reader.Read(Count) || Count > 10000000)
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupArrayPayload: invalid count.");
			Writer.Write(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				FAssetResult Result = RewriteSerializedReferenceValue(
					Array->GetInner(), Reader, Writer, Mappings,
					RewriteCount, ContainerDepth + 1);
				if (!Result) return Result;
			}
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Map:
		{
			auto* Map = static_cast<FMapProperty*>(Property);
			uint64 Count = 0;
			if (!Map->GetKeyProp() || !Map->GetValueProp()
				|| !Reader.Read(Count) || Count > 10000000
				|| ContainsAssetReferenceProperty(Map->GetKeyProp()))
				return Error(EAssetError::CorruptFile,
					"AssetReferenceFixupMapPayload: invalid map schema or count.");
			Writer.Write(Count);
			for (uint64 Index = 0; Index < Count; ++Index)
			{
				const size_t KeyOffset = Reader.Offset;
				FReflectedValueStorage KeyStorage;
				std::string StorageError;
				if (!KeyStorage.DefaultConstruct(Map->GetKeyProp(), 0, &StorageError))
					return Error(EAssetError::UnsupportedProperty, std::move(StorageError));
				FAssetResult Result = DecodeReferenceByteToolValue(
					Map->GetKeyProp(),
					KeyStorage.GetContainer(),
					0,
					Reader,
					{},
					AssetPackageObjectStreamVersion);
				if (!Result) return Result;
				Writer.WriteBytes(Reader.Bytes.subspan(KeyOffset, Reader.Offset - KeyOffset));
				Result = RewriteSerializedReferenceValue(
					Map->GetValueProp(), Reader, Writer, Mappings,
					RewriteCount, ContainerDepth + 1);
				if (!Result) return Result;
			}
			return {};
		}
		case DurinCodeGen::EPropertyGenFlags::Struct:
		{
			auto* StructProperty = static_cast<FStructProperty*>(Property);
			DStruct* Struct = StructProperty->GetStruct();
			std::string StructName;
			uint64 FieldCount = 0;
			if (!Struct || !Reader.ReadString(StructName, MaximumPackageStringBytes)
				|| StructName != Struct->GetQualifiedName().ToString()
				|| !Reader.Read(FieldCount) || FieldCount > 100000)
				return Error(EAssetError::TypeMismatch,
					"AssetReferenceFixupStructPayload: incompatible header.");
			Writer.WriteString(StructName);
			Writer.Write(FieldCount);
			for (uint64 Index = 0; Index < FieldCount; ++Index)
			{
				std::string DeclaringStruct;
				std::string FieldName;
				std::string Signature;
				uint8 Kind = 0;
				uint64 PayloadSize = 0;
				std::span<const std::byte> FieldPayload;
				if (!Reader.ReadString(DeclaringStruct, MaximumPackageStringBytes)
					|| !Reader.ReadString(FieldName, MaximumPackageStringBytes)
					|| !Reader.Read(Kind)
					|| !Reader.ReadString(Signature, MaximumPackageStringBytes)
					|| !Reader.Read(PayloadSize) || PayloadSize > Reader.Bytes.size()
					|| !Reader.ReadSpan(static_cast<size_t>(PayloadSize), FieldPayload))
					return Error(EAssetError::CorruptFile,
						"AssetReferenceFixupStructPayload: malformed field.");
				Writer.WriteString(DeclaringStruct);
				Writer.WriteString(FieldName);
				Writer.Write(Kind);
				Writer.WriteString(Signature);
				FProperty* Field = DeclaringStruct == StructName
					? Struct->FindPropertyBySerializedName(FName(FieldName), false) : nullptr;
				if (!Field || Field->HasAnyPropertyFlags(EPropertyFlags::Transient)
					|| !ContainsAssetReferenceProperty(Field))
				{
					Writer.Write(PayloadSize);
					Writer.WriteBytes(FieldPayload.data(), FieldPayload.size());
					continue;
				}
				if (static_cast<uint8>(Field->GetKind()) != Kind
					|| !IsSerializedTypeSignatureCompatible(Field, Signature))
					return Error(EAssetError::TypeMismatch,
						"AssetReferenceFixupSchemaMismatch: struct field signature changed.");
				std::vector<std::byte> RewrittenPayload;
				FAssetResult Result = RewriteSerializedReferenceProperty(
					Field, FieldPayload, Mappings, RewrittenPayload,
					RewriteCount, ContainerDepth);
				if (!Result) return Result;
				Writer.Write(uint64(RewrittenPayload.size()));
				Writer.WriteBytes(RewrittenPayload.data(), RewrittenPayload.size());
			}
			return {};
		}
		default:
			return Error(EAssetError::TypeMismatch,
				"AssetReferenceFixupSchemaMismatch: unsupported serialized reference container.");
		}
	}

	auto RewritePackageReferences(
		std::span<const std::byte> Bytes,
		std::span<const FAssetRedirectorFixupMapping> Mappings,
		uint64 ExpectedRewriteCount,
		std::vector<std::byte>& OutBytes) -> FAssetResult
	{
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		if (!Codec->bCanMutate)
			return Error(EAssetError::UnsupportedVersion,
				"Reference rewrite requires package mutation capability.");
		return Codec->RewriteReferences(Bytes, Mappings, ExpectedRewriteCount, OutBytes);
	}

	auto ReadPackageMetadata(
		std::span<const std::byte> Bytes, FPackageFile& OutFile) -> FAssetResult
	{
		const Private::FAssetPackageCodec* Codec = nullptr;
		if (FAssetResult Result = Private::ResolveAssetPackageReader(Bytes, Codec); !Result)
			return Result;
		FAssetPackageHeader Header;
		if (FAssetResult Result = Codec->ReadHeader(
			Bytes, Bytes.size(), Header); !Result)
			return Result;
		FPackageFile File{
			.FormatVersion = Header.FormatVersion,
			.AssetClassName = std::move(Header.AssetClassName),
			.EntryKind = Header.EntryKind};
		File.RedirectDestination = std::move(Header.RedirectDestination);
		File.Dependencies = std::move(Header.Dependencies);
		OutFile = std::move(File);
		return {};
	}

	}

	namespace Private
	{
		auto RewritePackageReferencesForMutation(
			std::span<const std::byte> Bytes,
			std::span<const FAssetRedirectorFixupMapping> Mappings,
			uint64 ExpectedRewriteCount,
			std::vector<std::byte>& OutBytes) -> FAssetResult
		{
			return RewritePackageReferences(
				Bytes, Mappings, ExpectedRewriteCount, OutBytes);
		}

		auto ReadMutationPackageMetadata(
			std::span<const std::byte> Bytes,
			FMutationPackageMetadata& OutMetadata) -> FAssetResult
		{
			FPackageFile File;
			FAssetResult Result = ReadPackageMetadata(Bytes, File);
			if (!Result) return Result;
			OutMetadata = {
				.FormatVersion = File.FormatVersion,
				.AssetClassName = std::move(File.AssetClassName),
				.EntryKind = File.EntryKind,
				.RedirectDestination = std::move(File.RedirectDestination),
				.Dependencies = std::move(File.Dependencies)};
			return {};
		}

		auto CollectLoadedPackageSoftReferencesForMutation(
			DPackage* Package,
			const FAssetPath& TargetPath,
			std::vector<FSoftObjectPtr*>& OutValues) -> FAssetResult
		{
			return CollectLoadedPackageSoftReferences(
				Package, TargetPath, OutValues);
		}

		auto AssetReferenceLess(
			const FAssetReferenceEdge& Left,
			const FAssetReferenceEdge& Right) -> bool
		{
			return AssetReferenceLessImpl(Left, Right);
		}

		auto ExtractAssetReferencesForCook(
			const FAssetPackageInspection& Inspection,
			std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult;
	}

	namespace
	{
		auto ExtractAssetReferencesInternal(
			const FAssetPath& SourcePackage,
			const FAssetPackageInspection& Inspection,
			std::vector<FAssetReferenceEdge>& OutReferences,
			bool bRequireValidSource) -> FAssetResult
		{
			OutReferences.clear();
			if (bRequireValidSource && !SourcePackage.IsValid())
				return Error(EAssetError::InvalidPath,
					"AssetReferenceIndexInvalidSource: source package path is invalid.");
			if (Inspection.Objects.empty())
				return Error(EAssetError::InvalidObjectGraph,
					"AssetReferenceIndexInvalidPackage: package has no main object.");
			if (Inspection.Header.AssetClassName != Inspection.Objects.front().ClassName)
				return Error(EAssetError::TypeMismatch,
					"AssetReferenceIndexRuntimeTypeMismatch: header and main-object classes differ.");

			std::vector<FAssetReferenceEdge> References;
			for (const FAssetPackageObjectInspection& Object : Inspection.Objects)
			{
				DClass* ObjectClass = FindClassByQualifiedName(FName(Object.ClassName));
				if (!ObjectClass)
					return Error(EAssetError::UnknownClass, std::format(
						"AssetReferenceIndexUnknownClass: {} is unavailable.", Object.ClassName));
				for (const FAssetPackageField& Field : Object.Fields)
				{
					DClass* DeclaringClass = FindClassByQualifiedName(FName(Field.DeclaringClass));
					FProperty* Property = DeclaringClass && ObjectClass->IsChildOf(DeclaringClass)
						? DeclaringClass->FindPropertyBySerializedName(FName(Field.Name), false)
						: nullptr;
					if (!Property)
					{
						if (Field.TypeSignature.find("SoftObject:") != std::string::npos
							|| Field.TypeSignature.find("Object:") != std::string::npos)
							return Error(EAssetError::TypeMismatch, std::format(
								"AssetReferenceSchemaMismatch: {}::{} has no current property metadata.",
								Field.DeclaringClass, Field.Name));
						continue;
					}
					const bool bCurrentContainsReference = ContainsAssetReferenceProperty(Property);
					const bool bStoredContainsReference =
						Field.TypeSignature.find("SoftObject:") != std::string::npos
						|| Field.TypeSignature.find("Object:") != std::string::npos;
					if (Property->GetKind() != Field.Kind
						|| !IsSerializedTypeSignatureCompatible(Property, Field.TypeSignature))
					{
						if (bCurrentContainsReference || bStoredContainsReference)
							return Error(EAssetError::TypeMismatch, std::format(
								"AssetReferenceSchemaMismatch: {}::{} has incompatible kind or signature.",
								Field.DeclaringClass, Field.Name));
						continue;
					}
					if (!bCurrentContainsReference) continue;
					FReferenceExtractionContext Context{
						.SourcePackage = SourcePackage,
						.Fingerprint = Inspection.Fingerprint,
						.Object = Object,
						.DeclaringType = Field.DeclaringClass,
						.FieldName = Field.Name,
						.ObjectKind = Inspection.Header.EntryKind
							== EAssetRegistryEntryKind::Redirector
							? EAssetReferenceKind::Redirect
							: EAssetReferenceKind::HardObject,
						.References = References};
					std::vector<FAssetReferenceRouteSegment> Route;
					FAssetResult Result = ExtractReferencePropertyValues(
						Property, Field.Payload, Context, Route, Field.Name, 0);
					if (!Result) return Result;
				}
			}
			std::ranges::sort(References, &AssetReferenceLess);
			OutReferences = std::move(References);
			return {};
		}
	}

	auto ExtractAssetReferences(
		const FAssetPath& SourcePackage,
		const FAssetPackageInspection& Inspection,
		std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult
	{
		return ExtractAssetReferencesInternal(
			SourcePackage, Inspection, OutReferences, true);
	}

	namespace Private
	{
		auto ExtractAssetReferencesForCook(
			const FAssetPackageInspection& Inspection,
			std::vector<FAssetReferenceEdge>& OutReferences) -> FAssetResult
		{
			return ExtractAssetReferencesInternal(
				{}, Inspection, OutReferences, false);
		}
	}
}
