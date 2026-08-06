#include "AssetCompatibility.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Misc/DerivedDataCache.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint32 AssetMagic = 0x54534144; // DAST
		constexpr uint32 AssetVersion = 2;
		constexpr uint64 MaximumPackageStringBytes = 1024 * 1024;
		constexpr uint64 MaximumPackageDependencies = 100000;
		constexpr uint64 MaximumPackageObjects = 1000000;
		constexpr uint64 MaximumObjectFields = 100000;

		auto SerializedTypeSignature(FProperty* Property) -> std::string
		{
			if (!Property) return "Invalid";
			const auto Kind = Property->GetKind();
			if (Kind == DurinCodeGen::EPropertyGenFlags::Array)
				return std::format("Array<{}>", SerializedTypeSignature(static_cast<FArrayProperty*>(Property)->GetInner()));
			if (Kind == DurinCodeGen::EPropertyGenFlags::Map)
			{
				auto* Map = static_cast<FMapProperty*>(Property);
				return std::format("Map<{},{}>", SerializedTypeSignature(Map->GetKeyProp()), SerializedTypeSignature(Map->GetValueProp()));
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Object)
				return std::format("Object:{}:{}", Property->GetReferencedClass()
					? Property->GetReferencedClass()->GetQualifiedName().ToString() : "DObject", Property->IsObjectPtrWrapper());
			if (Kind == DurinCodeGen::EPropertyGenFlags::SoftObject)
			{
				auto* SoftObject = static_cast<FSoftObjectProperty*>(Property);
				return std::format("SoftObject:{}:v1", SoftObject->GetExpectedClass()
					? SoftObject->GetExpectedClass()->GetQualifiedName().ToString() : "DObject");
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Enum)
			{
				auto* Enum = static_cast<FEnumProperty*>(Property);
				return std::format("Enum:{}:{}", Enum->GetEnum() ? Enum->GetEnum()->GetQualifiedName().ToString() : "", Property->GetElementSize());
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::Struct)
			{
				auto* Struct = static_cast<FStructProperty*>(Property);
				return std::format("Struct<{}>", Struct->GetStruct() ? Struct->GetStruct()->GetQualifiedName().ToString() : "");
			}
			if (Kind == DurinCodeGen::EPropertyGenFlags::String
				|| Kind == DurinCodeGen::EPropertyGenFlags::Name
				|| Kind == DurinCodeGen::EPropertyGenFlags::Guid)
				return std::format("{}:v1", static_cast<uint32>(Kind));
			return std::format("{}:{}", static_cast<uint32>(Kind), Property->GetElementSize());
		}

		auto NormalizeLegacyLogicalSignatures(std::string_view Signature) -> std::string
		{
			std::string Result;
			for (size_t Index = 0; Index < Signature.size();)
			{
				bool bNormalized = false;
				for (const std::string_view Prefix : {std::string_view("12:"), std::string_view("18:"), std::string_view("19:")})
				{
					if (!Signature.substr(Index).starts_with(Prefix)) continue;
					size_t End = Index + Prefix.size();
					while (End < Signature.size() && Signature[End] >= '0' && Signature[End] <= '9') ++End;
					if (End == Index + Prefix.size()) continue;
					Result.append(Prefix).append("v1");
					Index = End;
					bNormalized = true;
					break;
				}
				if (!bNormalized) Result.push_back(Signature[Index++]);
			}
			return Result;
		}

		struct FStreamingReader
		{
			std::ifstream Stream;
			uint64 FileSize = 0;
			uint64 Offset = 0;
			uint64 MetadataBytesRead = 0;

			explicit FStreamingReader(std::string_view Path) : Stream(std::string(Path), std::ios::binary)
			{
				if (!Stream) return;
				Stream.seekg(0, std::ios::end);
				const std::streamoff Size = Stream.tellg();
				if (Size < 0) { Stream.setstate(std::ios::failbit); return; }
				FileSize = static_cast<uint64>(Size);
				Stream.seekg(0, std::ios::beg);
			}

			auto IsOpen() const -> bool { return Stream.is_open() && !Stream.fail(); }

			template<typename T> auto Read(T& Value) -> bool
			{
				if (sizeof(T) > FileSize - std::min(Offset, FileSize)) return false;
				Stream.read(reinterpret_cast<char*>(&Value), sizeof(T));
				if (!Stream) return false;
				Offset += sizeof(T);
				MetadataBytesRead += sizeof(T);
				return true;
			}

			auto ReadString(std::string& Value) -> bool
			{
				uint64 Size = 0;
				if (!Read(Size) || Size > MaximumPackageStringBytes || Size > FileSize - std::min(Offset, FileSize)) return false;
				Value.resize(static_cast<size_t>(Size));
				if (Size != 0)
				{
					Stream.read(Value.data(), static_cast<std::streamsize>(Size));
					if (!Stream) return false;
				}
				Offset += Size;
				MetadataBytesRead += Size;
				return true;
			}

			auto Skip(uint64 Size) -> bool
			{
				if (Size > FileSize - std::min(Offset, FileSize)) return false;
				Stream.seekg(static_cast<std::streamoff>(Size), std::ios::cur);
				if (!Stream) return false;
				Offset += Size;
				return true;
			}
		};

		auto JsonEscape(std::string_view Value) -> std::string
		{
			std::string Result;
			for (const unsigned char Character : Value)
			{
				switch (Character)
				{
				case '\\': Result += "\\\\"; break;
				case '"': Result += "\\\""; break;
				case '\b': Result += "\\b"; break;
				case '\f': Result += "\\f"; break;
				case '\n': Result += "\\n"; break;
				case '\r': Result += "\\r"; break;
				case '\t': Result += "\\t"; break;
				default:
					if (Character < 0x20) Result += std::format("\\u{:04x}", Character);
					else Result.push_back(static_cast<char>(Character));
				}
			}
			return Result;
		}

		auto InspectionName(EAssetCompatibilityInspection Value) -> std::string_view
		{
			switch (Value) { case EAssetCompatibilityInspection::NotChecked: return "NotChecked"; case EAssetCompatibilityInspection::Ready: return "Ready"; case EAssetCompatibilityInspection::Failed: return "Failed"; }
			return "Failed";
		}

		auto CompatibilityName(EAssetPackageCompatibility Value) -> std::string_view
		{
			switch (Value) { case EAssetPackageCompatibility::Compatible: return "Compatible"; case EAssetPackageCompatibility::Incompatible: return "Incompatible"; case EAssetPackageCompatibility::Unsupported: return "Unsupported"; }
			return "Unsupported";
		}

		auto FreshnessName(EAssetCompatibilityFreshness Value) -> std::string_view
		{
			return Value == EAssetCompatibilityFreshness::Current ? "Current" : "Stale";
		}

		auto PropertyKindName(DurinCodeGen::EPropertyGenFlags Value) -> std::string_view
		{
			switch (Value)
			{
			case DurinCodeGen::EPropertyGenFlags::None: return "None";
			case DurinCodeGen::EPropertyGenFlags::Bool: return "Bool";
			case DurinCodeGen::EPropertyGenFlags::Int8: return "Int8";
			case DurinCodeGen::EPropertyGenFlags::Int16: return "Int16";
			case DurinCodeGen::EPropertyGenFlags::Int32: return "Int32";
			case DurinCodeGen::EPropertyGenFlags::Int64: return "Int64";
			case DurinCodeGen::EPropertyGenFlags::UInt8: return "UInt8";
			case DurinCodeGen::EPropertyGenFlags::UInt16: return "UInt16";
			case DurinCodeGen::EPropertyGenFlags::UInt32: return "UInt32";
			case DurinCodeGen::EPropertyGenFlags::UInt64: return "UInt64";
			case DurinCodeGen::EPropertyGenFlags::Float: return "Float";
			case DurinCodeGen::EPropertyGenFlags::Double: return "Double";
			case DurinCodeGen::EPropertyGenFlags::String: return "String";
			case DurinCodeGen::EPropertyGenFlags::Enum: return "Enum";
			case DurinCodeGen::EPropertyGenFlags::Object: return "Object";
			case DurinCodeGen::EPropertyGenFlags::Array: return "Array";
			case DurinCodeGen::EPropertyGenFlags::Map: return "Map";
			case DurinCodeGen::EPropertyGenFlags::Struct: return "Struct";
			case DurinCodeGen::EPropertyGenFlags::Name: return "Name";
			case DurinCodeGen::EPropertyGenFlags::Guid: return "Guid";
			}
			return "Unknown";
		}

		auto AddTerminalFailure(
			FAssetPackageCompatibilityRecord& Record,
			EAssetCompatibilityFindingCode Code,
			std::string Diagnostic) -> void
		{
			Record.Inspection = EAssetCompatibilityInspection::Failed;
			Record.Compatibility = EAssetPackageCompatibility::Unsupported;
			Record.Findings.push_back({.Code = Code, .Diagnostic = std::move(Diagnostic)});
		}

		auto EstimateMetadataBytes(const FAssetPackageCompatibilityRecord& Record) -> uint64
		{
			uint64 Total = Record.PhysicalPath.size() + Record.PackagePath.GetView().size();
			for (const FAssetCompatibilityFinding& Finding : Record.Findings)
				Total += Finding.ObjectPath.size() + Finding.ClassIdentity.size() + Finding.DeclaringType.size()
					+ Finding.FieldName.size() + Finding.StoredTypeSignature.size()
					+ Finding.ExpectedTypeSignature.size() + Finding.Diagnostic.size();
			return Total;
		}
	}

	auto FReflectionCompatibilityCatalog::Capture() -> FReflectionCompatibilityCatalog
	{
		FReflectionCompatibilityCatalog Result;
		for (DClass* Class : GetDerivedClasses(DObject::StaticClass(), true))
		{
			if (!Class || Class->GetQualifiedName().IsNone()) continue;
			FReflectionCompatibilityClass Entry;
			Entry.QualifiedName = Class->GetQualifiedName().ToString();
			Entry.bConstructible = Class->ClassConstructor != nullptr;
			for (DClass* Ancestor = Class; Ancestor; Ancestor = Ancestor->GetSuperClass())
				Entry.Ancestry.push_back(Ancestor->GetQualifiedName().ToString());
			for (DClass* Declaring = Class; Declaring; Declaring = Declaring->GetSuperClass())
			{
				Declaring->ForEachProperty([&](FProperty* Property) {
					Entry.Fields.push_back({
						.DeclaringType = Declaring->GetQualifiedName().ToString(),
						.Name = Property->NamePrivate.ToString(),
						.Kind = Property->GetKind(),
						.TypeSignature = SerializedTypeSignature(Property)});
				}, false);
			}
			std::ranges::sort(Entry.Ancestry);
			std::ranges::sort(Entry.Fields, [](const auto& Left, const auto& Right) {
				return std::tie(Left.DeclaringType, Left.Name) < std::tie(Right.DeclaringType, Right.Name);
			});
			Result.Classes.push_back(std::move(Entry));
		}
		std::ranges::sort(Result.Classes, {}, &FReflectionCompatibilityClass::QualifiedName);
		return Result;
	}

	auto FReflectionCompatibilityCatalog::FindClass(std::string_view QualifiedName) const -> const FReflectionCompatibilityClass*
	{
		const auto It = std::ranges::lower_bound(Classes, QualifiedName, {}, &FReflectionCompatibilityClass::QualifiedName);
		return It != Classes.end() && It->QualifiedName == QualifiedName ? &*It : nullptr;
	}

	auto FReflectionCompatibilityCatalog::FindField(
		const FReflectionCompatibilityClass& ObjectClass,
		std::string_view DeclaringType,
		std::string_view Name) const -> const FReflectionCompatibilityField*
	{
		if (!std::ranges::binary_search(ObjectClass.Ancestry, DeclaringType)) return nullptr;
		const auto It = std::ranges::lower_bound(ObjectClass.Fields, std::pair(DeclaringType, Name), {}, [](const auto& Field) {
			return std::pair(std::string_view(Field.DeclaringType), std::string_view(Field.Name));
		});
		return It != ObjectClass.Fields.end() && It->DeclaringType == DeclaringType && It->Name == Name ? &*It : nullptr;
	}

	auto ProbeAssetPackageCompatibility(
		const FAssetPackageCompatibilityProbeInput& Input,
		const FReflectionCompatibilityCatalog& Catalog,
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested) -> FAssetPackageCompatibilityProbeResult
	{
		FAssetPackageCompatibilityProbeResult Result;
		auto IsCancelled = [&]() { return IsCancellationRequested && IsCancellationRequested(); };
		if (IsCancelled()) { Result.Status = EAssetCompatibilityProbeStatus::Cancelled; return Result; }

		FAssetPackageCompatibilityRecord Record{
			.PackagePath = Input.PackagePath,
			.PhysicalPath = Input.PhysicalPath,
			.Inspection = EAssetCompatibilityInspection::Ready,
			.Compatibility = EAssetPackageCompatibility::Compatible};
		FStreamingReader Reader(Input.PhysicalPath);
		if (!Reader.IsOpen())
		{
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::IoFailure,
				std::format("Failed to open asset package {}.", Input.PhysicalPath));
			Result.Record = std::move(Record);
			return Result;
		}
		Record.Fingerprint.FileSize = Reader.FileSize;
		std::error_code TimeError;
		const auto InitialTime = std::filesystem::last_write_time(Input.PhysicalPath, TimeError);
		if (TimeError)
		{
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::IoFailure,
				std::format("Failed to read package timestamp for {}.", Input.PhysicalPath));
			Result.Record = std::move(Record);
			return Result;
		}
		Record.Fingerprint.LastWriteTimeTicks = DerivedDataCache::FileTimeToStableTicks(InitialTime);

		uint32 Magic = 0, Version = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || Magic != AssetMagic)
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Invalid or truncated asset package header.");
		else if (Version != AssetVersion)
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::UnsupportedPackageFormat,
				std::format("Unsupported asset package format version {}.", Version));
		else
		{
			std::string AssetClass;
			uint64 DependencyCount = 0, ObjectCount = 0;
			bool bValidHeader = Reader.ReadString(AssetClass) && Reader.Read(DependencyCount)
				&& DependencyCount <= MaximumPackageDependencies;
			for (uint64 Index = 0; bValidHeader && Index < DependencyCount; ++Index)
			{
				std::string Dependency;
				bValidHeader = Reader.ReadString(Dependency);
			}
			bValidHeader = bValidHeader && Reader.Read(ObjectCount) && ObjectCount > 0 && ObjectCount <= MaximumPackageObjects;
			if (!bValidHeader)
				AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Invalid asset package header metadata.");
			else
			{
				std::vector<std::string> ObjectPaths(static_cast<size_t>(ObjectCount));
				for (uint64 ObjectIndex = 0; ObjectIndex < ObjectCount && Record.Inspection == EAssetCompatibilityInspection::Ready; ++ObjectIndex)
				{
					if (IsCancelled()) { Result.Status = EAssetCompatibilityProbeStatus::Cancelled; return Result; }
					uint64 Id = 0, OuterId = 0, FieldCount = 0;
					std::string ClassName, ObjectName;
					if (!Reader.Read(Id) || !Reader.Read(OuterId) || !Reader.ReadString(ClassName)
						|| !Reader.ReadString(ObjectName) || !Reader.Read(FieldCount) || FieldCount > MaximumObjectFields)
					{
						AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Invalid or truncated object descriptor.");
						break;
					}
					if (Id != ObjectIndex + 1 || (Id == 1 ? OuterId != 0 : (OuterId == 0 || OuterId >= Id)))
					{
						AddTerminalFailure(Record, EAssetCompatibilityFindingCode::InvalidObjectGraph, "Invalid object identifiers or outer ordering.");
						break;
					}
					ObjectPaths[static_cast<size_t>(ObjectIndex)] = Id == 1
						? Input.PackagePath.ToString()
						: std::format("{}{}{}", ObjectPaths[static_cast<size_t>(OuterId - 1)], OuterId == 1 ? ":" : ".", ObjectName);
					const FReflectionCompatibilityClass* Class = Catalog.FindClass(ClassName);
					if (!Class || !Class->bConstructible)
					{
						Record.Compatibility = EAssetPackageCompatibility::Unsupported;
						Record.Findings.push_back({
							.Code = EAssetCompatibilityFindingCode::UnavailableClass,
							.ObjectPath = ObjectPaths[static_cast<size_t>(ObjectIndex)],
							.ClassIdentity = ClassName,
							.Diagnostic = std::format("Serialized class {} is unavailable.", ClassName)});
						Class = nullptr;
					}
					for (uint64 FieldIndex = 0; FieldIndex < FieldCount; ++FieldIndex)
					{
						if (IsCancelled()) { Result.Status = EAssetCompatibilityProbeStatus::Cancelled; return Result; }
						std::string DeclaringType, FieldName, StoredSignature;
						uint8 StoredKindByte = 0;
						uint64 PayloadSize = 0;
						if (!Reader.ReadString(DeclaringType) || !Reader.ReadString(FieldName) || !Reader.Read(StoredKindByte)
							|| !Reader.ReadString(StoredSignature) || !Reader.Read(PayloadSize))
						{
							AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Invalid or truncated field descriptor.");
							break;
						}
						const uint64 PayloadOffset = Reader.Offset;
						if (!Reader.Skip(PayloadSize))
						{
							AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Field payload extends beyond the package bounds.");
							break;
						}
						Result.Stats.PayloadBytesSkipped += PayloadSize;
						if (!Class) continue;
						const auto StoredKind = static_cast<DurinCodeGen::EPropertyGenFlags>(StoredKindByte);
						const FReflectionCompatibilityField* Field = Catalog.FindField(*Class, DeclaringType, FieldName);
						if (!Field)
						{
							if (Record.Compatibility == EAssetPackageCompatibility::Compatible) Record.Compatibility = EAssetPackageCompatibility::Incompatible;
							Record.Findings.push_back({
								.Code = EAssetCompatibilityFindingCode::UnknownField,
								.ObjectPath = ObjectPaths[static_cast<size_t>(ObjectIndex)], .ClassIdentity = ClassName,
								.DeclaringType = DeclaringType, .FieldName = FieldName, .StoredKind = StoredKind,
								.StoredTypeSignature = StoredSignature, .PayloadSize = PayloadSize, .PayloadOffset = PayloadOffset,
								.Diagnostic = std::format("Serialized field {}::{} is unknown or retired.", DeclaringType, FieldName)});
						}
						else if (Field->Kind != StoredKind || Field->TypeSignature != NormalizeLegacyLogicalSignatures(StoredSignature))
						{
							if (Record.Compatibility == EAssetPackageCompatibility::Compatible) Record.Compatibility = EAssetPackageCompatibility::Incompatible;
							Record.Findings.push_back({
								.Code = EAssetCompatibilityFindingCode::IncompatibleFieldSignature,
								.ObjectPath = ObjectPaths[static_cast<size_t>(ObjectIndex)], .ClassIdentity = ClassName,
								.DeclaringType = DeclaringType, .FieldName = FieldName, .StoredKind = StoredKind,
								.StoredTypeSignature = StoredSignature, .ExpectedKind = Field->Kind,
								.ExpectedTypeSignature = Field->TypeSignature, .PayloadSize = PayloadSize, .PayloadOffset = PayloadOffset,
								.Diagnostic = std::format("Serialized field {}::{} has an incompatible type signature.", DeclaringType, FieldName)});
						}
					}
				}
				if (Record.Inspection == EAssetCompatibilityInspection::Ready && Reader.Offset != Reader.FileSize)
					AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Trailing bytes after asset package data.");
			}
		}

		Result.Stats.MetadataBytesRead = Reader.MetadataBytesRead;
		Result.Stats.PeakMetadataBytes = EstimateMetadataBytes(Record);
		std::error_code FinalError;
		const uintmax_t FinalSize = std::filesystem::file_size(Input.PhysicalPath, FinalError);
		const auto FinalTime = std::filesystem::last_write_time(Input.PhysicalPath, FinalError);
		const int64 FinalTicks = FinalError ? 0 : DerivedDataCache::FileTimeToStableTicks(FinalTime);
		if (FinalError || FinalSize != Input.ExpectedFileSize || FinalTicks != Input.ExpectedLastWriteTimeTicks
			|| FinalSize != Record.Fingerprint.FileSize || FinalTicks != Record.Fingerprint.LastWriteTimeTicks)
			Record.Freshness = EAssetCompatibilityFreshness::Stale;
		Result.Record = std::move(Record);
		return Result;
	}

	auto IsAssetPackageCompatibilityRecordCurrent(
		const FAssetPackageCompatibilityRecord& Record,
		uintmax_t FileSize,
		int64 LastWriteTimeTicks) -> bool
	{
		return Record.Freshness == EAssetCompatibilityFreshness::Current
			&& Record.Fingerprint.FileSize == FileSize
			&& Record.Fingerprint.LastWriteTimeTicks == LastWriteTimeTicks;
	}

	auto AssetCompatibilityFindingCodeName(EAssetCompatibilityFindingCode Code) -> std::string_view
	{
		switch (Code)
		{
		case EAssetCompatibilityFindingCode::UnknownField: return "UnknownField";
		case EAssetCompatibilityFindingCode::IncompatibleFieldSignature: return "IncompatibleFieldSignature";
		case EAssetCompatibilityFindingCode::UnavailableClass: return "UnavailableClass";
		case EAssetCompatibilityFindingCode::UnsupportedPackageFormat: return "UnsupportedPackageFormat";
		case EAssetCompatibilityFindingCode::InvalidObjectGraph: return "InvalidObjectGraph";
		case EAssetCompatibilityFindingCode::CorruptPackage: return "CorruptPackage";
		case EAssetCompatibilityFindingCode::IoFailure: return "IoFailure";
		}
		return "CorruptPackage";
	}

	auto SerializeAssetCompatibilityReportV1(std::span<const FAssetPackageCompatibilityRecord> Records) -> std::string
	{
		std::vector<const FAssetPackageCompatibilityRecord*> Sorted;
		Sorted.reserve(Records.size());
		for (const auto& Record : Records) Sorted.push_back(&Record);
		std::ranges::sort(Sorted, [](const auto* Left, const auto* Right) {
			return Left->PackagePath.GetView() < Right->PackagePath.GetView();
		});
		std::string Json = std::format("{{\"schemaVersion\":{},\"packages\":[", AssetCompatibilityReportSchemaVersion);
		for (size_t RecordIndex = 0; RecordIndex < Sorted.size(); ++RecordIndex)
		{
			if (RecordIndex != 0) Json += ',';
			const auto& Record = *Sorted[RecordIndex];
			Json += std::format("{{\"packagePath\":\"{}\",\"physicalPath\":\"{}\",\"inspection\":\"{}\",\"compatibility\":\"{}\",\"freshness\":\"{}\",\"fileSize\":{},\"lastWriteTimeTicks\":{},\"findings\":[",
				JsonEscape(Record.PackagePath.GetView()), JsonEscape(Record.PhysicalPath), InspectionName(Record.Inspection),
				CompatibilityName(Record.Compatibility), FreshnessName(Record.Freshness), Record.Fingerprint.FileSize, Record.Fingerprint.LastWriteTimeTicks);
			for (size_t FindingIndex = 0; FindingIndex < Record.Findings.size(); ++FindingIndex)
			{
				if (FindingIndex != 0) Json += ',';
				const auto& Finding = Record.Findings[FindingIndex];
				Json += std::format("{{\"code\":\"{}\",\"objectPath\":\"{}\",\"classIdentity\":\"{}\",\"declaringType\":\"{}\",\"fieldName\":\"{}\",\"storedKind\":\"{}\",\"storedTypeSignature\":\"{}\",\"expectedKind\":\"{}\",\"expectedTypeSignature\":\"{}\",\"payloadSize\":{},\"payloadOffset\":{},\"diagnostic\":\"{}\"}}",
					AssetCompatibilityFindingCodeName(Finding.Code), JsonEscape(Finding.ObjectPath), JsonEscape(Finding.ClassIdentity),
					JsonEscape(Finding.DeclaringType), JsonEscape(Finding.FieldName), PropertyKindName(Finding.StoredKind),
					JsonEscape(Finding.StoredTypeSignature), PropertyKindName(Finding.ExpectedKind), JsonEscape(Finding.ExpectedTypeSignature),
					Finding.PayloadSize, Finding.PayloadOffset, JsonEscape(Finding.Diagnostic));
			}
			Json += "]}";
		}
		Json += "]}";
		return Json;
	}
}
