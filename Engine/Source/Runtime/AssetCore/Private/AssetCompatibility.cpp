#include "AssetCompatibility.h"
#include "AssetPackageV4Reader.h"
#include "AssetPackageCodec.h"
#include "AssetPackageVersionPolicy.h"

#include "DObject/Class.h"
#include "DObject/DurinPropertyTypes.h"
#include "Hash/XxHash.h"
#include "Misc/DerivedDataCache.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace Durin::Asset
{
	namespace
	{
		constexpr uint64 MaximumPackageStringBytes = 1024 * 1024;
		constexpr size_t FingerprintBufferSize = 64 * 1024;

		class FSha256
		{
		public:
			auto Update(const void* Data, size_t Size) -> void
			{
				const auto* Bytes = static_cast<const uint8*>(Data);
				TotalBytes += Size;
				while (Size != 0)
				{
					const size_t Count = std::min(Size, Block.size() - BlockSize);
					std::memcpy(Block.data() + BlockSize, Bytes, Count);
					BlockSize += Count;
					Bytes += Count;
					Size -= Count;
					if (BlockSize == Block.size()) { Transform(Block.data()); BlockSize = 0; }
				}
			}

			auto Finalize() -> std::string
			{
				const uint64 BitCount = TotalBytes * 8;
				Block[BlockSize++] = 0x80;
				if (BlockSize > 56)
				{
					std::fill(Block.begin() + static_cast<ptrdiff_t>(BlockSize), Block.end(), 0);
					Transform(Block.data());
					BlockSize = 0;
				}
				std::fill(Block.begin() + static_cast<ptrdiff_t>(BlockSize), Block.begin() + 56, 0);
				for (size_t Index = 0; Index < 8; ++Index) Block[63 - Index] = static_cast<uint8>(BitCount >> (Index * 8));
				Transform(Block.data());
				std::string Hex = "sha256:";
				for (const uint32 Value : State) Hex += std::format("{:08x}", Value);
				return Hex;
			}

		private:
			auto Transform(const uint8* Data) -> void
			{
				static constexpr std::array<uint32, 64> K = {
					0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
					0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
					0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
					0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
					0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
					0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
					0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
					0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
				std::array<uint32, 64> Words{};
				for (size_t Index = 0; Index < 16; ++Index)
					Words[Index] = uint32(Data[Index * 4]) << 24 | uint32(Data[Index * 4 + 1]) << 16 | uint32(Data[Index * 4 + 2]) << 8 | Data[Index * 4 + 3];
				for (size_t Index = 16; Index < Words.size(); ++Index)
				{
					const uint32 S0 = std::rotr(Words[Index - 15], 7) ^ std::rotr(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
					const uint32 S1 = std::rotr(Words[Index - 2], 17) ^ std::rotr(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
					Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
				}
				auto [A, B, C, D, E, F, G, H] = State;
				for (size_t Index = 0; Index < Words.size(); ++Index)
				{
					const uint32 S1 = std::rotr(E, 6) ^ std::rotr(E, 11) ^ std::rotr(E, 25);
					const uint32 Choice = (E & F) ^ (~E & G);
					const uint32 Temp1 = H + S1 + Choice + K[Index] + Words[Index];
					const uint32 S0 = std::rotr(A, 2) ^ std::rotr(A, 13) ^ std::rotr(A, 22);
					const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
					const uint32 Temp2 = S0 + Majority;
					H=G; G=F; F=E; E=D+Temp1; D=C; C=B; B=A; A=Temp1+Temp2;
				}
				State[0]+=A; State[1]+=B; State[2]+=C; State[3]+=D; State[4]+=E; State[5]+=F; State[6]+=G; State[7]+=H;
			}

			std::array<uint32, 8> State = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
			std::array<uint8, 64> Block{};
			size_t BlockSize = 0;
			uint64 TotalBytes = 0;
		};

		auto CaptureFingerprint(
			const std::filesystem::path& Path,
			const FAssetCompatibilityCancellationCheck& IsCancelled,
			FAssetPackageFingerprint& OutFingerprint,
			std::string& OutReportContentHash,
			std::string& OutError) -> EAssetPackageSnapshotStatus
		{
			std::error_code Error;
			const uintmax_t InitialSize = std::filesystem::file_size(Path, Error);
			if (Error)
			{
				OutError = std::format("Failed to fingerprint package '{}': {}", Path.generic_string(), Error.message());
				return EAssetPackageSnapshotStatus::Failed;
			}
			const auto InitialTime = std::filesystem::last_write_time(Path, Error);
			if (Error)
			{
				OutError = std::format("Failed to fingerprint package '{}': {}", Path.generic_string(), Error.message());
				return EAssetPackageSnapshotStatus::Failed;
			}
			std::ifstream Stream(Path, std::ios::binary);
			if (!Stream.is_open())
			{
				OutError = std::format("Failed to open package '{}' for fingerprinting.", Path.generic_string());
				return EAssetPackageSnapshotStatus::Failed;
			}
			FXxHash128Builder Builder;
			FSha256 Sha256;
			std::array<char, FingerprintBufferSize> Buffer{};
			while (Stream)
			{
				if (IsCancelled && IsCancelled()) return EAssetPackageSnapshotStatus::Cancelled;
				Stream.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
				const std::streamsize Read = Stream.gcount();
				if (Read > 0)
				{
					Builder.Update(Buffer.data(), static_cast<uint64>(Read));
					Sha256.Update(Buffer.data(), static_cast<size_t>(Read));
				}
			}
			if (Stream.bad())
			{
				OutError = std::format("Failed while hashing package '{}'.", Path.generic_string());
				return EAssetPackageSnapshotStatus::Failed;
			}
			const uintmax_t FinalSize = std::filesystem::file_size(Path, Error);
			if (Error)
			{
				OutError = std::format("Failed to recheck package '{}': {}", Path.generic_string(), Error.message());
				return EAssetPackageSnapshotStatus::Failed;
			}
			const auto FinalTime = std::filesystem::last_write_time(Path, Error);
			if (Error)
			{
				OutError = std::format("Failed to recheck package '{}': {}", Path.generic_string(), Error.message());
				return EAssetPackageSnapshotStatus::Failed;
			}
			const int64 InitialTicks = DerivedDataCache::FileTimeToStableTicks(InitialTime);
			const int64 FinalTicks = DerivedDataCache::FileTimeToStableTicks(FinalTime);
			if (InitialSize != FinalSize || InitialTicks != FinalTicks)
			{
				OutError = std::format("Package '{}' changed while its fingerprint was captured.", Path.generic_string());
				return EAssetPackageSnapshotStatus::Failed;
			}
			OutFingerprint = {
				.FileSize = FinalSize,
				.LastWriteTimeTicks = FinalTicks,
				.ContentHash = Builder.Finalize()};
			std::ifstream HeaderStream(Path, std::ios::binary);
			uint32 Magic = 0;
			uint32 Version = 0;
			if (HeaderStream.read(reinterpret_cast<char*>(&Magic), sizeof(Magic))
				&& HeaderStream.read(reinterpret_cast<char*>(&Version), sizeof(Version))
				&& Magic == DastPackageMagic)
				OutFingerprint.ReaderVersion = Version;
			OutReportContentHash = Sha256.Finalize();
			return EAssetPackageSnapshotStatus::Completed;
		}

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
		for (const FSerializedReflectionAlias& Alias : CaptureSerializedReflectionAliases())
		{
			EAssetReflectedIdentityKind Kind = EAssetReflectedIdentityKind::Class;
			if (Alias.Kind == ESerializedReflectedKind::Struct) Kind = EAssetReflectedIdentityKind::Struct;
			else if (Alias.Kind == ESerializedReflectedKind::Enum) Kind = EAssetReflectedIdentityKind::Enum;
			Result.SerializedAliases.push_back({Alias.StoredName, Alias.CurrentName, Kind});
		}
		std::ranges::sort(Result.SerializedAliases, {}, &FReflectionSerializedAlias::StoredIdentity);
		for (const FSerializedPropertyAlias& Alias : CaptureSerializedPropertyAliases())
			Result.SerializedPropertyAliases.push_back({
				Alias.DeclaringType, Alias.StoredName, Alias.CurrentName});
		std::ranges::sort(Result.SerializedPropertyAliases, [](const auto& Left, const auto& Right) {
			return std::tie(Left.DeclaringType, Left.StoredName)
				< std::tie(Right.DeclaringType, Right.StoredName);
		});
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

	auto FReflectionCompatibilityCatalog::FindSerializedAlias(std::string_view StoredIdentity) const
		-> const FReflectionSerializedAlias*
	{
		const auto It = std::ranges::lower_bound(
			SerializedAliases, StoredIdentity, {}, &FReflectionSerializedAlias::StoredIdentity);
		return It != SerializedAliases.end() && It->StoredIdentity == StoredIdentity ? &*It : nullptr;
	}

	auto FReflectionCompatibilityCatalog::FindSerializedPropertyAlias(
		std::string_view DeclaringType,
		std::string_view StoredName) const -> const FReflectionSerializedPropertyAlias*
	{
		const auto Key = std::pair(DeclaringType, StoredName);
		const auto It = std::ranges::lower_bound(
			SerializedPropertyAliases, Key, {}, [](const auto& Alias) {
				return std::pair(std::string_view(Alias.DeclaringType), std::string_view(Alias.StoredName));
			});
		return It != SerializedPropertyAliases.end()
			&& It->DeclaringType == DeclaringType && It->StoredName == StoredName ? &*It : nullptr;
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

	auto CaptureMountedAssetPackageSnapshot(
		const FAssetCompatibilityCancellationCheck& IsCancellationRequested)
		-> FAssetPackageDiscoverySnapshot
	{
		FAssetPackageDiscoverySnapshot Result;
		for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
		{
			if (IsCancellationRequested && IsCancellationRequested())
			{
				Result.Status = EAssetPackageSnapshotStatus::Cancelled;
				return Result;
			}
			if (!Mount.bAutoScan) continue;
			const std::filesystem::path ContentRoot = Mount.GetContentDir();
			std::error_code Error;
			if (!std::filesystem::exists(ContentRoot, Error))
			{
				if (Error)
				{
					Result.Status = EAssetPackageSnapshotStatus::Failed;
					Result.Error = std::format("Failed to inspect mount {}: {}", Mount.VirtualRoot, Error.message());
					return Result;
				}
				continue;
			}
			std::filesystem::recursive_directory_iterator It(
				ContentRoot, std::filesystem::directory_options::skip_permission_denied, Error);
			const std::filesystem::recursive_directory_iterator End;
			if (Error)
			{
				Result.Status = EAssetPackageSnapshotStatus::Failed;
				Result.Error = std::format("Failed to enumerate mount {}: {}", Mount.VirtualRoot, Error.message());
				return Result;
			}
			while (It != End)
			{
				if (IsCancellationRequested && IsCancellationRequested())
				{
					Result.Status = EAssetPackageSnapshotStatus::Cancelled;
					return Result;
				}
				std::error_code FileError;
				if (It->is_regular_file(FileError) && It->path().extension() == ".dasset")
				{
					std::filesystem::path Relative = std::filesystem::relative(It->path(), ContentRoot, FileError);
					if (FileError)
					{
						Result.Status = EAssetPackageSnapshotStatus::Failed;
						Result.Error = std::format("Failed to classify package '{}': {}", It->path().generic_string(), FileError.message());
						return Result;
					}
					Relative.replace_extension();
					FAssetPath PackagePath;
					std::string PathError;
					if (!FAssetPath::TryCreate(Mount.VirtualRoot + Relative.generic_string(), PackagePath, &PathError))
					{
						Result.Status = EAssetPackageSnapshotStatus::Failed;
						Result.Error = std::format("Invalid mounted package path '{}': {}", It->path().generic_string(), PathError);
						return Result;
					}
					FAssetPackageFingerprint Fingerprint;
					std::string ReportContentHash;
					Result.Status = CaptureFingerprint(
						It->path(), IsCancellationRequested, Fingerprint, ReportContentHash, Result.Error);
					if (Result.Status != EAssetPackageSnapshotStatus::Completed) return Result;
					Result.Packages.push_back({
						.PackagePath = std::move(PackagePath),
						.PhysicalPath = It->path().generic_string(),
						.ExpectedFileSize = Fingerprint.FileSize,
						.ExpectedLastWriteTimeTicks = Fingerprint.LastWriteTimeTicks,
						.ExpectedContentHash = Fingerprint.ContentHash,
						.ExpectedReportContentHash = std::move(ReportContentHash)});
				}
				else if (FileError)
				{
					Result.Status = EAssetPackageSnapshotStatus::Failed;
					Result.Error = std::format("Failed to inspect package candidate '{}': {}", It->path().generic_string(), FileError.message());
					return Result;
				}
				It.increment(Error);
				if (Error)
				{
					Result.Status = EAssetPackageSnapshotStatus::Failed;
					Result.Error = std::format("Failed to enumerate mount {}: {}", Mount.VirtualRoot, Error.message());
					return Result;
				}
			}
		}
		std::ranges::sort(Result.Packages, [](const auto& Left, const auto& Right) {
			return Left.PackagePath.GetView() < Right.PackagePath.GetView();
		});
		const auto Duplicate = std::ranges::adjacent_find(Result.Packages, [](const auto& Left, const auto& Right) {
			return Left.PackagePath == Right.PackagePath;
		});
		if (Duplicate != Result.Packages.end())
		{
			Result.Status = EAssetPackageSnapshotStatus::Failed;
			Result.Error = std::format("Duplicate mounted package path {}.", Duplicate->PackagePath.ToString());
			Result.Packages.clear();
		}
		return Result;
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
		Record.Fingerprint.ContentHash = Input.ExpectedContentHash;
		bool bUsedCodec = false;
		Record.ReportContentHash = Input.ExpectedReportContentHash;

		uint32 Magic = 0, Version = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || Magic != DastPackageMagic)
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage, "Invalid or truncated asset package header.");
		else if (!Private::FindAssetPackageReader(Version))
			AddTerminalFailure(Record, EAssetCompatibilityFindingCode::UnsupportedPackageFormat,
				std::format("Unsupported asset package format version {}.", Version));
		else if (const Private::FAssetPackageCodec* Codec =
			Private::FindAssetPackageReader(Version))
		{
			bUsedCodec = true;
			std::vector<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, Input.PhysicalPath))
				AddTerminalFailure(Record, EAssetCompatibilityFindingCode::IoFailure,
					std::format("Failed to open asset package {}.", Input.PhysicalPath));
			else if (IsCancelled())
			{
				Result.Status = EAssetCompatibilityProbeStatus::Cancelled;
				return Result;
			}
			else
			{
				FAssetPackageCompatibilityRecord CodecRecord;
				FAssetResult ProbeResult = Codec->ProbeCompatibility(
					Bytes, Input.PackagePath, Catalog, CodecRecord, &Result.Stats);
				if (!ProbeResult)
					AddTerminalFailure(Record, EAssetCompatibilityFindingCode::CorruptPackage,
						ProbeResult.Message);
				else
				{
					CodecRecord.PhysicalPath = Input.PhysicalPath;
					CodecRecord.Fingerprint.FileSize = Record.Fingerprint.FileSize;
					CodecRecord.Fingerprint.LastWriteTimeTicks = Record.Fingerprint.LastWriteTimeTicks;
					CodecRecord.Fingerprint.ContentHash = Record.Fingerprint.ContentHash;
					CodecRecord.Fingerprint.ReaderVersion = Version;
					CodecRecord.ReportContentHash = Input.ExpectedReportContentHash;
					Record = std::move(CodecRecord);
				}
			}
		}
		if (!bUsedCodec)
		{
			Result.Stats.MetadataBytesRead = Reader.MetadataBytesRead;
			Result.Stats.PeakMetadataBytes = EstimateMetadataBytes(Record);
		}
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
			Json += std::format("{{\"packagePath\":\"{}\",\"physicalPath\":\"{}\",\"formatVersion\":{},\"inspection\":\"{}\",\"compatibility\":\"{}\",\"freshness\":\"{}\",\"fileSize\":{},\"lastWriteTimeTicks\":{},\"findings\":[",
				JsonEscape(Record.PackagePath.GetView()), JsonEscape(Record.PhysicalPath), Record.FormatVersion, InspectionName(Record.Inspection),
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
			Json += "],\"canonicalizationEvidence\":[";
			for (size_t EvidenceIndex = 0; EvidenceIndex < Record.CanonicalizationEvidence.size(); ++EvidenceIndex)
			{
				if (EvidenceIndex != 0) Json += ',';
				const auto& Evidence = Record.CanonicalizationEvidence[EvidenceIndex];
				auto KindName = [](EAssetReflectedIdentityKind Kind) -> std::string_view {
					switch (Kind)
					{
					case EAssetReflectedIdentityKind::Class: return "Class";
					case EAssetReflectedIdentityKind::Struct: return "Struct";
					case EAssetReflectedIdentityKind::Enum: return "Enum";
					case EAssetReflectedIdentityKind::Property: return "Property";
					}
					return "Class";
				};
				auto LocationName = [](EAssetSerializedIdentityLocation Location) -> std::string_view {
					switch (Location)
					{
					case EAssetSerializedIdentityLocation::PackageHeader: return "PackageHeader";
					case EAssetSerializedIdentityLocation::ObjectRecord: return "ObjectRecord";
					case EAssetSerializedIdentityLocation::Schema: return "Schema";
					case EAssetSerializedIdentityLocation::TypeDescriptor: return "TypeDescriptor";
					}
					return "PackageHeader";
				};
				Json += std::format("{{\"storedIdentity\":\"{}\",\"currentIdentity\":\"{}\",\"kind\":\"{}\",\"location\":\"{}\",\"logicalPath\":\"{}\"}}",
					JsonEscape(Evidence.StoredIdentity), JsonEscape(Evidence.CurrentIdentity),
					KindName(Evidence.Kind), LocationName(Evidence.Location), JsonEscape(Evidence.LogicalPath));
			}
			Json += "]}";
		}
		Json += "]}";
		return Json;
	}
}
