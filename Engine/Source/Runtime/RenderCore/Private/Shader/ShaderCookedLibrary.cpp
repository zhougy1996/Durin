#include "Shader/ShaderCookedLibrary.h"

#include "Hash/XxHash.h"
#include "Misc/FileHelper.h"
#include "Serialization/BinaryFormat.h"
#include "Shader/Shader.h"
#include "Shader/ShaderCompiledOutput.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 LibraryMagic = 0x424c5344; // DSLB
		constexpr uint32 LibrarySchemaVersion = 1;
		constexpr uint32 LibraryBuilderVersion = 1;
		constexpr uint32 LibraryHeaderSize = 112;
		constexpr uint32 LibraryDirectoryRecordSize = 80;
		constexpr uint64 LibraryAlignment = 16;
		constexpr uint32 MaximumLibraryRecords = 65'536;
		constexpr uint64 MaximumLibraryBytes = 4ull * 1024ull * 1024ull * 1024ull;
		constexpr uint64 FileDigestOffset = 96;

		struct FRegisteredRequest
		{
			uint64 Handle = 0;
			FShaderRuntimeRequest Request;
			std::vector<const FShaderType*> BuildTypes;
			EShaderRequestEligibility Eligibility =
				EShaderRequestEligibility::GameAndEditor;
		};

		struct FDirectoryRecord
		{
			FXxHash128 RuntimeIdentity;
			FXxHash128 ProductionIdentity;
			FXxHash128 PayloadDigest;
			uint64 Offset = 0;
			uint64 Size = 0;
			uint32 MemberCount = 0;
			uint32 Flags = 0;
		};

		struct FRegistry
		{
			std::mutex Mutex;
			std::vector<FRegisteredRequest> Requests;
			uint64 NextHandle = 1;
			bool bFrozen = false;
			EShaderTargetPlatform FrozenPlatform = EShaderTargetPlatform::Invalid;
			EShaderTargetProfile FrozenProfile = EShaderTargetProfile::Invalid;
			std::vector<FShaderRuntimeRequest> FrozenRequests;
		};

		auto Registry() -> FRegistry&
		{
			// Process-lifetime registry: cross-DLL registrations may retire during
			// CRT teardown after ordinary function-local static destruction.
			static FRegistry* Value = new FRegistry;
			return *Value;
		}

		auto Fail(std::string& OutError, std::string Message) -> bool
		{
			OutError = std::move(Message);
			return false;
		}

		auto IsValidFrequency(EShaderFrequency Frequency) -> bool
		{
			return static_cast<uint32>(Frequency)
				<= static_cast<uint32>(EShaderFrequency::RayMiss);
		}

		auto IsValidTarget(
			EShaderTargetPlatform Platform,
			EShaderTargetProfile Profile) -> bool
		{
			return Platform == EShaderTargetPlatform::Win64
				&& (Profile == EShaderTargetProfile::Game
					|| Profile == EShaderTargetProfile::EditorValidation);
		}

		auto IsEligible(
			EShaderRequestEligibility Eligibility,
			EShaderTargetProfile Profile) -> bool
		{
			return Eligibility == EShaderRequestEligibility::GameAndEditor
				|| Profile == EShaderTargetProfile::EditorValidation;
		}

		template<typename TBuilder>
		auto UpdateString(TBuilder& Builder, std::string_view Value) -> void
		{
			Builder.UpdateValue(static_cast<uint64>(Value.size()));
			Builder.Update(Value);
		}

		auto AlignUp(uint64 Value, uint64 Alignment, uint64& OutValue) -> bool
		{
			if (Alignment == 0 || Value > std::numeric_limits<uint64>::max()
				- (Alignment - 1)) return false;
			OutValue = (Value + Alignment - 1) & ~(Alignment - 1);
			return true;
		}

		auto AddChecked(uint64 Left, uint64 Right, uint64& OutValue) -> bool
		{
			if (Right > std::numeric_limits<uint64>::max() - Left) return false;
			OutValue = Left + Right;
			return true;
		}

		auto HashWithZeroedFileDigest(
			std::span<const std::byte> Bytes) -> FXxHash128
		{
			FXxHash128Builder Builder;
			Builder.Update(Bytes.first(FileDigestOffset));
			constexpr std::array<std::byte, 16> Zeros{};
			Builder.Update(Zeros);
			Builder.Update(Bytes.subspan(FileDigestOffset + Zeros.size()));
			return Builder.Finalize();
		}

		auto MakeCompileOptions(
			const FShaderRuntimeRequest& Request,
			FShaderCompileOptions& OutOptions,
			std::vector<std::string>& OutEntryStorage) -> bool
		{
			OutOptions = {};
			OutEntryStorage.clear();
			OutEntryStorage.reserve(Request.Members.size());
			for (const FShaderRuntimeRequestMember& Member : Request.Members)
				OutEntryStorage.push_back(Member.EntryPoint);
			OutOptions.EntryPoints.reserve(OutEntryStorage.size());
			OutOptions.Frequencies.reserve(OutEntryStorage.size());
			for (size_t Index = 0; Index < Request.Members.size(); ++Index)
			{
				OutOptions.EntryPoints.push_back(OutEntryStorage[Index].c_str());
				OutOptions.Frequencies.push_back(Request.Members[Index].Frequency);
			}
			return !OutOptions.EntryPoints.empty();
		}

		auto WriteHash(FBinaryWriter& Writer, const FXxHash128& Hash) -> void
		{
			Writer.WriteU64(Hash.HashLow);
			Writer.WriteU64(Hash.HashHigh);
		}

		auto ReadHash(FBinaryReader& Reader, FXxHash128& Hash) -> bool
		{
			return Reader.ReadU64(Hash.HashLow) && Reader.ReadU64(Hash.HashHigh);
		}
	}

	struct FShaderCookedLibrary::FState
	{
		std::shared_ptr<const std::vector<std::byte>> Bytes;
		std::vector<FDirectoryRecord> Directory;
		EShaderTargetPlatform TargetPlatform = EShaderTargetPlatform::Invalid;
		EShaderTargetProfile TargetProfile = EShaderTargetProfile::Invalid;
		FXxHash128 GenerationIdentity;
	};

	FShaderRequestRegistration::~FShaderRequestRegistration()
	{
		Reset();
	}

	FShaderRequestRegistration::FShaderRequestRegistration(
		FShaderRequestRegistration&& Other) noexcept
		: Handle(std::exchange(Other.Handle, 0))
	{
	}

	auto FShaderRequestRegistration::operator=(
		FShaderRequestRegistration&& Other) noexcept
		-> FShaderRequestRegistration&
	{
		if (this != &Other)
		{
			Reset();
			Handle = std::exchange(Other.Handle, 0);
		}
		return *this;
	}

	auto FShaderRequestRegistration::Reset(std::string* OutError) -> bool
	{
		if (Handle == 0)
		{
			if (OutError) OutError->clear();
			return true;
		}
		FRegistry& State = Registry();
		std::lock_guard Lock(State.Mutex);
		if (State.bFrozen)
		{
			if (OutError) *OutError =
				"Shader request retirement is forbidden after inventory freeze.";
			return false;
		}
		std::erase_if(State.Requests,
			[Handle = Handle](const FRegisteredRequest& Request) {
				return Request.Handle == Handle;
			});
		Handle = 0;
		if (OutError) OutError->clear();
		return true;
	}

	auto BuildShaderRuntimeRequestIdentity(
		const FShaderRuntimeRequest& Request,
		FXxHash128& OutIdentity,
		std::string& OutError) -> bool
	{
		OutIdentity = {};
		if (!IsValidTarget(Request.TargetPlatform, Request.TargetProfile)
			|| (Request.Category != EShaderRuntimeRequestCategory::GlobalSet
				&& Request.Category != EShaderRuntimeRequestCategory::FeatureProgram)
			|| Request.Owner.empty() || Request.Owner.size() > 256
			|| Request.Name.empty() || Request.Name.size() > 512
			|| Request.Members.empty() || Request.Members.size() > 32)
			return Fail(OutError, "Shader runtime request header is invalid.");
		std::string Previous;
		for (const FShaderRuntimeRequestMember& Member : Request.Members)
		{
			if (Member.TypeName.empty() || Member.TypeName.size() > 512
				|| Member.EntryPoint.empty() || Member.EntryPoint.size() > 512
				|| !IsValidFrequency(Member.Frequency)
				|| (!Previous.empty() && !(Previous < Member.TypeName)))
				return Fail(OutError,
					"Shader runtime request members are invalid or non-canonical.");
			Previous = Member.TypeName;
		}
		FXxHash128Builder Builder;
		UpdateString(Builder, "DurinShaderRuntimeRequest");
		Builder.UpdateValue(LibrarySchemaVersion);
		Builder.UpdateValue(static_cast<uint32>(Request.TargetPlatform));
		Builder.UpdateValue(static_cast<uint32>(Request.TargetProfile));
		Builder.UpdateValue(static_cast<uint32>(Request.Category));
		UpdateString(Builder, Request.Owner);
		UpdateString(Builder, Request.Name);
		Builder.UpdateValue(static_cast<uint32>(Request.Members.size()));
		for (const FShaderRuntimeRequestMember& Member : Request.Members)
		{
			UpdateString(Builder, Member.TypeName);
			UpdateString(Builder, Member.EntryPoint);
			Builder.UpdateValue(static_cast<uint32>(Member.Frequency));
		}
		OutIdentity = Builder.Finalize();
		OutError.clear();
		return true;
	}

	auto RegisterShaderRuntimeRequest(
		FShaderRuntimeRequest Request,
		EShaderRequestEligibility Eligibility,
		std::span<const FShaderType* const> BuildTypes,
		std::string* OutError) -> FShaderRequestRegistration
	{
		std::vector<const FShaderType*> CanonicalBuildTypes(
			BuildTypes.begin(), BuildTypes.end());
		if (!CanonicalBuildTypes.empty())
		{
			if (std::ranges::any_of(CanonicalBuildTypes,
				[](const FShaderType* Type) { return Type == nullptr; }))
			{
				if (OutError) *OutError = "Shader request contains a null build type.";
				return {};
			}
			std::ranges::sort(CanonicalBuildTypes, {},
				[](const FShaderType* Type) { return Type->GetName(); });
			Request.Members.clear();
			for (const FShaderType* Type : CanonicalBuildTypes)
				Request.Members.push_back({std::string(Type->GetName()),
					std::string(Type->GetEntryPoint()), Type->GetFrequency()});
		}
		else
		{
			std::ranges::sort(Request.Members, {},
				&FShaderRuntimeRequestMember::TypeName);
		}
		Request.TargetPlatform = EShaderTargetPlatform::Win64;
		Request.TargetProfile = Eligibility == EShaderRequestEligibility::EditorOnly
			? EShaderTargetProfile::EditorValidation : EShaderTargetProfile::Game;
		FXxHash128 Identity;
		std::string Error;
		if (!BuildShaderRuntimeRequestIdentity(Request, Identity, Error))
		{
			if (OutError) *OutError = std::move(Error);
			return {};
		}
		FRegistry& State = Registry();
		std::lock_guard Lock(State.Mutex);
		if (State.bFrozen)
		{
			if (OutError) *OutError =
				"Shader request registration is forbidden after inventory freeze.";
			return {};
		}
		for (const FRegisteredRequest& Existing : State.Requests)
		{
			if (Existing.Request.Owner == Request.Owner
				&& Existing.Request.Name == Request.Name)
			{
				if (OutError) *OutError =
					"Shader request owner/name is already registered.";
				return {};
			}
		}
		const uint64 Handle = State.NextHandle++;
		if (!CanonicalBuildTypes.empty())
		{
			if (CanonicalBuildTypes.size() != Request.Members.size())
			{
				if (OutError) *OutError =
					"Shader request build-type count does not match its members.";
				return {};
			}
			for (size_t Index = 0; Index < CanonicalBuildTypes.size(); ++Index)
			{
				const FShaderType* Type = CanonicalBuildTypes[Index];
				const FShaderRuntimeRequestMember& Member = Request.Members[Index];
				if (Type == nullptr || Type->GetName() != Member.TypeName
					|| Type->GetEntryPoint() != Member.EntryPoint
					|| Type->GetFrequency() != Member.Frequency)
				{
					if (OutError) *OutError =
						"Shader request build types do not match canonical members.";
					return {};
				}
			}
		}
		State.Requests.push_back({Handle, std::move(Request),
			std::move(CanonicalBuildTypes),
			Eligibility});
		if (OutError) OutError->clear();
		return FShaderRequestRegistration(Handle);
	}

	auto GetShaderRuntimeRequestBuildTypes(
		const FShaderRuntimeRequest& Request,
		std::vector<const FShaderType*>& OutTypes,
		std::string& OutError) -> bool
	{
		OutTypes.clear();
		FRegistry& State = Registry();
		std::lock_guard Lock(State.Mutex);
		const auto Found = std::ranges::find_if(State.Requests,
			[&Request](const FRegisteredRequest& Registered) {
				return Registered.Request.Owner == Request.Owner
					&& Registered.Request.Name == Request.Name;
			});
		if (Found == State.Requests.end() || Found->BuildTypes.empty())
			return Fail(OutError,
				"Shader request has no registered build-type contribution.");
		OutTypes = Found->BuildTypes;
		OutError.clear();
		return true;
	}

	FShaderProgramRegistration::FShaderProgramRegistration(
		std::string_view Owner,
		std::string_view Name,
		EShaderRequestEligibility Eligibility,
		std::initializer_list<const FShaderType*> Types)
	{
		FShaderRuntimeRequest Request;
		Request.Category = EShaderRuntimeRequestCategory::FeatureProgram;
		Request.Owner = Owner;
		Request.Name = Name;
		std::vector<const FShaderType*> BuildTypes(Types.begin(), Types.end());
		std::string Error;
		Registration = RegisterShaderRuntimeRequest(
			std::move(Request), Eligibility, BuildTypes, &Error);
		requiref(Registration.IsValid(),
			"Shader program registration failed for '{}': {}", Name, Error);
	}

	auto FreezeShaderRuntimeInventory(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::vector<FShaderRuntimeRequest>& OutRequests,
		std::string& OutError) -> bool
	{
		OutRequests.clear();
		if (!IsValidTarget(TargetPlatform, TargetProfile))
			return Fail(OutError, "Shader inventory target is invalid.");
		FRegistry& State = Registry();
		std::lock_guard Lock(State.Mutex);
		if (State.bFrozen)
		{
			if (State.FrozenPlatform != TargetPlatform
				|| State.FrozenProfile != TargetProfile)
				return Fail(OutError,
					"Shader inventory is already frozen for another target.");
			OutRequests = State.FrozenRequests;
			OutError.clear();
			return true;
		}
		struct FIdentifiedRequest
		{
			FXxHash128 Identity;
			FShaderRuntimeRequest Request;
		};
		std::vector<FIdentifiedRequest> Selected;
		std::unordered_set<std::string> SelectedNames;
		for (const FRegisteredRequest& Registered : State.Requests)
		{
			if (!IsEligible(Registered.Eligibility, TargetProfile)) continue;
			if (!SelectedNames.emplace(Registered.Request.Name).second)
				return Fail(OutError,
					"Shader inventory contains an ambiguous request name.");
			FShaderRuntimeRequest Request = Registered.Request;
			Request.TargetPlatform = TargetPlatform;
			Request.TargetProfile = TargetProfile;
			FXxHash128 Identity;
			if (!BuildShaderRuntimeRequestIdentity(Request, Identity, OutError))
				return false;
			Selected.push_back({Identity, std::move(Request)});
		}
		std::ranges::sort(Selected, [](const auto& Left, const auto& Right) {
			return std::tie(Left.Identity.HashHigh, Left.Identity.HashLow)
				< std::tie(Right.Identity.HashHigh, Right.Identity.HashLow);
		});
		for (size_t Index = 1; Index < Selected.size(); ++Index)
			if (Selected[Index - 1].Identity == Selected[Index].Identity)
				return Fail(OutError, "Shader inventory contains a duplicate identity.");
		State.FrozenPlatform = TargetPlatform;
		State.FrozenProfile = TargetProfile;
		for (auto& Item : Selected)
			State.FrozenRequests.push_back(std::move(Item.Request));
		State.bFrozen = true;
		OutRequests = State.FrozenRequests;
		OutError.clear();
		return true;
	}

	auto ResetShaderRuntimeInventoryForTesting() -> void
	{
		FRegistry& State = Registry();
		std::lock_guard Lock(State.Mutex);
		State.Requests.clear();
		State.FrozenRequests.clear();
		State.FrozenPlatform = EShaderTargetPlatform::Invalid;
		State.FrozenProfile = EShaderTargetProfile::Invalid;
		State.NextHandle = 1;
		State.bFrozen = false;
	}

	auto EncodeShaderCookedLibrary(
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::span<const FShaderCookedLibraryRecord> Records,
		std::vector<std::byte>& OutBytes,
		std::string& OutError) -> bool
	{
		OutBytes.clear();
		if (!IsValidTarget(TargetPlatform, TargetProfile)
			|| Records.empty() || Records.size() > MaximumLibraryRecords)
			return Fail(OutError, "Shader library target or record count is invalid.");
		struct FEncoded
		{
			FDirectoryRecord Directory;
			std::vector<std::byte> Payload;
		};
		std::vector<FEncoded> Encoded;
		Encoded.reserve(Records.size());
		for (const FShaderCookedLibraryRecord& Record : Records)
		{
			if (Record.Request.TargetPlatform != TargetPlatform
				|| Record.Request.TargetProfile != TargetProfile
				|| Record.ProductionIdentity.IsZero())
				return Fail(OutError, "Shader library record target or production identity is invalid.");
			FEncoded Item;
			if (!BuildShaderRuntimeRequestIdentity(
				Record.Request, Item.Directory.RuntimeIdentity, OutError)) return false;
			std::vector<std::string> Entries;
			FShaderCompileOptions Options;
			MakeCompileOptions(Record.Request, Options, Entries);
			if (!ShaderCompiledOutput::Encode(
				Options, Record.Output, Item.Payload, OutError)) return false;
			Item.Directory.ProductionIdentity = Record.ProductionIdentity;
			Item.Directory.PayloadDigest = FXxHash128::HashBuffer(Item.Payload);
			Item.Directory.Size = Item.Payload.size();
			Item.Directory.MemberCount =
				static_cast<uint32>(Record.Request.Members.size());
			Encoded.push_back(std::move(Item));
		}
		std::ranges::sort(Encoded, [](const auto& Left, const auto& Right) {
			return std::tie(Left.Directory.RuntimeIdentity.HashHigh,
				Left.Directory.RuntimeIdentity.HashLow)
				< std::tie(Right.Directory.RuntimeIdentity.HashHigh,
					Right.Directory.RuntimeIdentity.HashLow);
		});
		for (size_t Index = 1; Index < Encoded.size(); ++Index)
			if (Encoded[Index - 1].Directory.RuntimeIdentity
				== Encoded[Index].Directory.RuntimeIdentity)
				return Fail(OutError, "Shader library contains a duplicate runtime identity.");

		uint64 DirectorySize = static_cast<uint64>(Encoded.size())
			* LibraryDirectoryRecordSize;
		uint64 PayloadOffset = 0;
		if (!AlignUp(LibraryHeaderSize + DirectorySize,
			LibraryAlignment, PayloadOffset))
			return Fail(OutError, "Shader library directory extent overflowed.");
		uint64 Cursor = PayloadOffset;
		for (FEncoded& Item : Encoded)
		{
			if (!AlignUp(Cursor, LibraryAlignment, Cursor))
				return Fail(OutError, "Shader library payload offset overflowed.");
			Item.Directory.Offset = Cursor;
			if (!AddChecked(Cursor, Item.Directory.Size, Cursor)
				|| Cursor > MaximumLibraryBytes)
				return Fail(OutError, "Shader library exceeds its byte bound.");
		}

		FXxHash128Builder InventoryBuilder;
		for (const FEncoded& Item : Encoded)
			InventoryBuilder.UpdateValue(Item.Directory.RuntimeIdentity);
		const FXxHash128 InventoryDigest = InventoryBuilder.Finalize();

		FBinaryWriter Writer;
		Writer.WriteHeader({LibraryMagic, LibrarySchemaVersion,
			LibraryBuilderVersion});
		Writer.WriteU32(static_cast<uint32>(TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(TargetProfile));
		Writer.WriteU32(LibraryHeaderSize);
		Writer.WriteU32(LibraryDirectoryRecordSize);
		Writer.WriteU32(static_cast<uint32>(Encoded.size()));
		Writer.WriteU32(0);
		Writer.WriteU64(LibraryHeaderSize);
		Writer.WriteU64(DirectorySize);
		Writer.WriteU64(PayloadOffset);
		Writer.WriteU64(Cursor - PayloadOffset);
		Writer.WriteU64(Cursor);
		WriteHash(Writer, InventoryDigest);
		WriteHash(Writer, {});
		for (const FEncoded& Item : Encoded)
		{
			WriteHash(Writer, Item.Directory.RuntimeIdentity);
			WriteHash(Writer, Item.Directory.ProductionIdentity);
			WriteHash(Writer, Item.Directory.PayloadDigest);
			Writer.WriteU64(Item.Directory.Offset);
			Writer.WriteU64(Item.Directory.Size);
			Writer.WriteU32(Item.Directory.MemberCount);
			Writer.WriteU32(0);
			Writer.WriteU64(0);
		}
		std::vector<std::byte> Candidate = Writer.TakeBytes();
		Candidate.resize(static_cast<size_t>(Cursor));
		for (const FEncoded& Item : Encoded)
			std::ranges::copy(Item.Payload,
				Candidate.begin() + static_cast<size_t>(Item.Directory.Offset));
		const FXxHash128 FileDigest = HashWithZeroedFileDigest(Candidate);
		for (size_t Index = 0; Index < sizeof(uint64); ++Index)
		{
			Candidate[FileDigestOffset + Index] = static_cast<std::byte>(
				(FileDigest.HashLow >> (Index * 8)) & 0xff);
			Candidate[FileDigestOffset + 8 + Index] = static_cast<std::byte>(
				(FileDigest.HashHigh >> (Index * 8)) & 0xff);
		}
		OutBytes = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto FShaderCookedLibrary::Open(
		const std::filesystem::path& Path,
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::span<const FShaderRuntimeRequest> RequiredRequests,
		FShaderCookedLibrary& OutLibrary,
		std::string& OutError) -> bool
	{
		OutLibrary = {};
		auto Bytes = std::make_shared<std::vector<std::byte>>();
		if (!FFileHelper::LoadFileToArray(*Bytes, Path))
			return Fail(OutError, std::format(
				"Shader library could not be read: {}", Path.generic_string()));
		return OpenBytes(std::move(Bytes), TargetPlatform, TargetProfile,
			RequiredRequests, OutLibrary, OutError);
	}

	auto FShaderCookedLibrary::OpenBytes(
		std::shared_ptr<const std::vector<std::byte>> Bytes,
		EShaderTargetPlatform TargetPlatform,
		EShaderTargetProfile TargetProfile,
		std::span<const FShaderRuntimeRequest> RequiredRequests,
		FShaderCookedLibrary& OutLibrary,
		std::string& OutError) -> bool
	{
		OutLibrary = {};
		if (!Bytes || Bytes->size() < LibraryHeaderSize
			|| Bytes->size() > MaximumLibraryBytes)
			return Fail(OutError, "Shader library byte extent is invalid.");
		FBinaryReader Reader(*Bytes);
		uint32 Platform = 0;
		uint32 Profile = 0;
		uint32 HeaderSize = 0;
		uint32 DirectoryRecordSize = 0;
		uint32 RecordCount = 0;
		uint32 Reserved = 0;
		uint64 DirectoryOffset = 0;
		uint64 DirectorySize = 0;
		uint64 PayloadOffset = 0;
		uint64 PayloadSize = 0;
		uint64 FileSize = 0;
		FXxHash128 InventoryDigest;
		FXxHash128 FileDigest;
		if (!Reader.ReadAndValidateHeader(LibraryMagic, LibrarySchemaVersion,
				LibraryBuilderVersion)
			|| !Reader.ReadU32(Platform) || !Reader.ReadU32(Profile)
			|| !Reader.ReadU32(HeaderSize)
			|| !Reader.ReadU32(DirectoryRecordSize)
			|| !Reader.ReadU32(RecordCount) || !Reader.ReadU32(Reserved)
			|| !Reader.ReadU64(DirectoryOffset) || !Reader.ReadU64(DirectorySize)
			|| !Reader.ReadU64(PayloadOffset) || !Reader.ReadU64(PayloadSize)
			|| !Reader.ReadU64(FileSize) || !ReadHash(Reader, InventoryDigest)
			|| !ReadHash(Reader, FileDigest)
			|| Reserved != 0 || HeaderSize != LibraryHeaderSize
			|| DirectoryRecordSize != LibraryDirectoryRecordSize
			|| RecordCount == 0 || RecordCount > MaximumLibraryRecords
			|| Platform != static_cast<uint32>(TargetPlatform)
			|| Profile != static_cast<uint32>(TargetProfile)
			|| !IsValidTarget(TargetPlatform, TargetProfile)
			|| DirectoryOffset != LibraryHeaderSize
			|| DirectorySize != static_cast<uint64>(RecordCount)
				* LibraryDirectoryRecordSize
			|| PayloadOffset % LibraryAlignment != 0
			|| FileSize != Bytes->size()
			|| PayloadOffset > FileSize || PayloadSize != FileSize - PayloadOffset
			|| HashWithZeroedFileDigest(*Bytes) != FileDigest)
			return Fail(OutError, "Shader library header is incompatible or corrupt.");

		auto Candidate = std::make_shared<FState>();
		Candidate->Bytes = std::move(Bytes);
		Candidate->TargetPlatform = TargetPlatform;
		Candidate->TargetProfile = TargetProfile;
		Candidate->GenerationIdentity = FileDigest;
		Candidate->Directory.reserve(RecordCount);
		FXxHash128Builder InventoryBuilder;
		uint64 PreviousEnd = PayloadOffset;
		for (uint32 Index = 0; Index < RecordCount; ++Index)
		{
			FDirectoryRecord Entry;
			uint64 Reserved64 = 0;
			if (!ReadHash(Reader, Entry.RuntimeIdentity)
				|| !ReadHash(Reader, Entry.ProductionIdentity)
				|| !ReadHash(Reader, Entry.PayloadDigest)
				|| !Reader.ReadU64(Entry.Offset) || !Reader.ReadU64(Entry.Size)
				|| !Reader.ReadU32(Entry.MemberCount)
				|| !Reader.ReadU32(Entry.Flags) || !Reader.ReadU64(Reserved64)
				|| Entry.RuntimeIdentity.IsZero()
				|| Entry.ProductionIdentity.IsZero()
				|| Entry.PayloadDigest.IsZero()
				|| Entry.MemberCount == 0 || Entry.MemberCount > 32
				|| Entry.Flags != 0 || Reserved64 != 0
				|| Entry.Offset % LibraryAlignment != 0
				|| Entry.Offset < PreviousEnd
				|| Entry.Size == 0
				|| Entry.Size > ShaderCompiledOutput::MaximumValueBytes
				|| Entry.Offset > FileSize || Entry.Size > FileSize - Entry.Offset)
				return Fail(OutError, "Shader library directory record is invalid.");
			if (!Candidate->Directory.empty())
			{
				const FDirectoryRecord& Previous = Candidate->Directory.back();
				if (!(std::tie(Previous.RuntimeIdentity.HashHigh,
					Previous.RuntimeIdentity.HashLow)
					< std::tie(Entry.RuntimeIdentity.HashHigh,
						Entry.RuntimeIdentity.HashLow)))
					return Fail(OutError,
						"Shader library directory is unsorted or duplicated.");
			}
			InventoryBuilder.UpdateValue(Entry.RuntimeIdentity);
			PreviousEnd = Entry.Offset + Entry.Size;
			Candidate->Directory.push_back(Entry);
		}
		if (Reader.GetRemainingBytes() != FileSize
			- (LibraryHeaderSize + DirectorySize)
			|| InventoryBuilder.Finalize() != InventoryDigest
			|| PreviousEnd != FileSize)
			return Fail(OutError, "Shader library extent or inventory digest is invalid.");

		for (const FShaderRuntimeRequest& Request : RequiredRequests)
		{
			FXxHash128 Identity;
			if (!BuildShaderRuntimeRequestIdentity(Request, Identity, OutError))
				return false;
			const auto Found = std::ranges::lower_bound(
				Candidate->Directory,
				std::pair{Identity.HashHigh, Identity.HashLow}, {},
				[](const FDirectoryRecord& Entry) {
					return std::pair{Entry.RuntimeIdentity.HashHigh,
						Entry.RuntimeIdentity.HashLow};
				});
			if (Found == Candidate->Directory.end()
				|| Found->RuntimeIdentity != Identity
				|| Found->MemberCount != Request.Members.size())
				return Fail(OutError,
					"Shader library is missing a required runtime request.");
		}
		OutLibrary.State = std::move(Candidate);
		OutError.clear();
		return true;
	}

	auto FShaderCookedLibrary::Load(
		const FShaderRuntimeRequest& Request,
		FShaderCompilerOutput& OutOutput,
		std::string& OutError) const -> bool
	{
		OutOutput = {};
		if (!State) return Fail(OutError, "Shader library is not open.");
		FXxHash128 Identity;
		if (!BuildShaderRuntimeRequestIdentity(Request, Identity, OutError))
			return false;
		const auto Found = std::ranges::lower_bound(
			State->Directory, std::pair{Identity.HashHigh, Identity.HashLow}, {},
			[](const FDirectoryRecord& Entry) {
				return std::pair{Entry.RuntimeIdentity.HashHigh,
					Entry.RuntimeIdentity.HashLow};
			});
		if (Found == State->Directory.end()
			|| Found->RuntimeIdentity != Identity
			|| Found->MemberCount != Request.Members.size())
			return Fail(OutError, "Shader library request is unavailable.");
		const std::span<const std::byte> Payload(
			State->Bytes->data() + static_cast<size_t>(Found->Offset),
			static_cast<size_t>(Found->Size));
		if (FXxHash128::HashBuffer(Payload) != Found->PayloadDigest)
			return Fail(OutError, "Shader library payload digest is invalid.");
		std::vector<std::string> Entries;
		FShaderCompileOptions Options;
		MakeCompileOptions(Request, Options, Entries);
		return ShaderCompiledOutput::Decode(Payload, Options, OutOutput, OutError);
	}

	auto FShaderCookedLibrary::GetRecordCount() const -> uint32
	{
		return State ? static_cast<uint32>(State->Directory.size()) : 0;
	}

	auto FShaderCookedLibrary::GetGenerationIdentity() const -> FXxHash128
	{
		return State ? State->GenerationIdentity : FXxHash128{};
	}
}
