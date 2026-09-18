#include "CookOutputInternal.h"
#include "Serialization/BinaryFormat.h"
namespace Durin
{
	using namespace AssetPrivate;
	namespace
	{
		auto AppendString(FBinaryWriter& Writer, std::string_view Value) -> bool
		{
			if (Value.empty() || Value.find('\0') != std::string_view::npos || Value.size() > 4096
				|| Value.size() > std::numeric_limits<uint32>::max()) return false;
			Writer.WriteU32(static_cast<uint32>(Value.size()));
			Writer.WriteBytes(std::as_bytes(std::span(Value)));
			return !Writer.HasError();
		}

		class FStateReader
		{
		public:
			explicit FStateReader(FByteView InBytes)
				: Reader(InBytes, {MaximumCookStateBytes, MaximumCookDependencyBytes})
			{
			}

			template<typename TValue>
			auto Read(TValue& OutValue) -> bool
			{
				return Reader.ReadInteger(OutValue);
			}

			auto ReadString(std::string& OutValue) -> bool
			{
				uint32 Size = 0;
				if (!Read(Size) || Size == 0 || Size > 4096
					|| Size > Reader.GetRemainingBytes()) return false;
				FByteView Encoded;
				if (!Reader.ReadRegion(Encoded, Size, 4096)) return false;
				OutValue.assign(reinterpret_cast<const char*>(Encoded.data()), Encoded.size());
				return OutValue.find('\0') == std::string::npos;
			}

			auto ReadDependencies(std::vector<FCookBuildDependency>& Out) -> FCookStateResult
			{
				uint32 Size = 0;
				FByteView Bytes;
				if (!Read(Size) || Size > MaximumCookDependencyBytes
					|| !Reader.ReadRegion(Bytes, Size, MaximumCookDependencyBytes))
					return {.Error = ECookStateError::DependencyFrame, .Actual = Size,
						.Maximum = MaximumCookDependencyBytes, .RemainingBytes = Reader.GetRemainingBytes()};
				if (const auto Decoded = DecodeCookBuildDependencies(Bytes, Out); !Decoded)
					return {.Error = ECookStateError::Dependency, .DependencyCause = Decoded};
				return {};
			}

			auto GetRemainingBytes() const -> uint64 { return Reader.GetRemainingBytes(); }

			auto IsAtEnd() const -> bool { return Reader.IsAtEnd(); }

		private:
			FBinaryReader Reader;
		};

	}

	auto FormatCookStateError(const FCookStateResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookStateError::None: return {};
		case ECookStateError::Header: return "Cook state header is unsupported or corrupt.";
		case ECookStateError::DuplicatePath: return "Cook state contains a duplicate package path: " + Result.VirtualPath;
		case ECookStateError::String: return "Cook state string is invalid or exceeds its bound: " + Result.VirtualPath;
		case ECookStateError::ByteLimit: return "Cook state exceeds its byte bound.";
		case ECookStateError::Entry: return "Cook state entry is corrupt or noncanonical: " + Result.VirtualPath;
		case ECookStateError::DependencyFrame: return "Cook state dependency frame is truncated or oversized.";
		case ECookStateError::Dependency: return Result.DependencyCause ? FormatCookDependencyCodecError(*Result.DependencyCause) : "Cook state dependency is invalid.";
		case ECookStateError::TrailingBytes: return "Cook state has trailing bytes.";
		}
		return {};
	}

	auto EncodeCookState(const FCookState& State, FByteBuffer& OutBytes) -> FCookStateResult
	{
		OutBytes.clear();
		if (State.TargetPlatform != ECookTargetPlatform::Win64
			|| (State.TargetProfile != ECookTargetProfile::Game
				&& State.TargetProfile != ECookTargetProfile::EditorValidation)
			|| State.Entries.size() > MaximumCookStateEntries)
			return {.Error = ECookStateError::Header, .Actual = State.Entries.size(), .Maximum = MaximumCookStateEntries, .TargetPlatform = State.TargetPlatform, .TargetProfile = State.TargetProfile};
		std::vector<const FCookStateEntry*> Entries;
		Entries.reserve(State.Entries.size());
		for (const FCookStateEntry& Entry : State.Entries)
			Entries.push_back(&Entry);
		std::ranges::sort(Entries, {}, &FCookStateEntry::VirtualPackagePath);
		for (size_t Index = 1; Index < Entries.size(); ++Index)
			if (Entries[Index - 1]->VirtualPackagePath == Entries[Index]->VirtualPackagePath)
				return {.Error = ECookStateError::DuplicatePath, .VirtualPath = Entries[Index]->VirtualPackagePath, .EntryIndex = Index};
		FBinaryWriter Writer({MaximumCookStateBytes, MaximumCookDependencyBytes});
		Writer.WriteU32(CookStateMagic);
		Writer.WriteU32(CookStateVersion);
		Writer.WriteU32(static_cast<uint32>(State.TargetPlatform));
		Writer.WriteU32(static_cast<uint32>(State.TargetProfile));
		Writer.WriteU32(static_cast<uint32>(Entries.size()));
		Writer.WriteU32(0);
		for (const FCookStateEntry* Entry : Entries)
		{
			if (!AppendString(Writer, Entry->VirtualPackagePath)
				|| !AppendString(Writer, Entry->Contributor)
				|| !AppendString(Writer, Entry->BuildProvenance))
				return {.Error = ECookStateError::String, .VirtualPath = Entry->VirtualPackagePath, .Maximum = 4096};
			Writer.WriteHash128(Entry->InputFingerprint);
			Writer.WriteHash128(Entry->PackageDigest);
			Writer.WriteHash128(Entry->SegmentDigest);
			Writer.WriteU64(Entry->PackageSize);
			Writer.WriteU64(Entry->SegmentSize);
			Writer.WriteU32(Entry->ContributorVersion);
			Writer.WriteU32(Entry->FamilyProducerVersion);
			Writer.WriteU8(Entry->SegmentFlags);
			FByteBuffer Dependencies;
			if (const auto Encoded = EncodeCookBuildDependencies(Entry->BuildDependencies, Dependencies); !Encoded)
				return {.Error = ECookStateError::Dependency, .VirtualPath = Entry->VirtualPackagePath, .DependencyCause = Encoded};
			Writer.WriteU32(static_cast<uint32>(Dependencies.size()));
			Writer.WriteBytes(Dependencies);
		}
		if (Writer.HasError())
			return {.Error = ECookStateError::ByteLimit, .Maximum = MaximumCookStateBytes};
		OutBytes = Writer.TakeBytes();
		return {};
	}

	auto DecodeCookState(FByteView Bytes, FCookState& OutState) -> FCookStateResult
	{
		OutState = {};
		if (Bytes.size() > MaximumCookStateBytes)
			return {.Error = ECookStateError::ByteLimit, .Actual = Bytes.size(), .Maximum = MaximumCookStateBytes};
		FStateReader Reader(Bytes);
		uint32 Magic = 0, Version = 0, Platform = 0, Profile = 0, Count = 0, Reserved = 0;
		if (!Reader.Read(Magic) || !Reader.Read(Version) || !Reader.Read(Platform)
			|| !Reader.Read(Profile) || !Reader.Read(Count) || !Reader.Read(Reserved)
			|| Magic != CookStateMagic || Version != CookStateVersion || Reserved != 0
			|| Count > MaximumCookStateEntries || Count > Reader.GetRemainingBytes() / 100
			|| Platform != static_cast<uint32>(ECookTargetPlatform::Win64)
			|| (Profile != static_cast<uint32>(ECookTargetProfile::Game)
				&& Profile != static_cast<uint32>(ECookTargetProfile::EditorValidation)))
			return {.Error = ECookStateError::Header, .Actual = Count, .Maximum = MaximumCookStateEntries, .RemainingBytes = Reader.GetRemainingBytes(), .Version = Version, .TargetPlatform = static_cast<ECookTargetPlatform>(Platform), .TargetProfile = static_cast<ECookTargetProfile>(Profile)};
		FCookState Candidate{
			static_cast<ECookTargetPlatform>(Platform),
			static_cast<ECookTargetProfile>(Profile)
		};
		Candidate.Entries.reserve(Count);
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FCookStateEntry Entry;
			if (!Reader.ReadString(Entry.VirtualPackagePath)
				|| !Reader.ReadString(Entry.Contributor)
				|| !Reader.ReadString(Entry.BuildProvenance)
				|| !Reader.Read(Entry.InputFingerprint.HashLow)
				|| !Reader.Read(Entry.InputFingerprint.HashHigh)
				|| !Reader.Read(Entry.PackageDigest.HashLow)
				|| !Reader.Read(Entry.PackageDigest.HashHigh)
				|| !Reader.Read(Entry.SegmentDigest.HashLow)
				|| !Reader.Read(Entry.SegmentDigest.HashHigh)
				|| !Reader.Read(Entry.PackageSize) || !Reader.Read(Entry.SegmentSize)
				|| !Reader.Read(Entry.ContributorVersion)
				|| !Reader.Read(Entry.FamilyProducerVersion)
				|| !Reader.Read(Entry.SegmentFlags))
				return {.Error = ECookStateError::Entry, .VirtualPath = Entry.VirtualPackagePath, .EntryIndex = Index, .RemainingBytes = Reader.GetRemainingBytes()};
			if (auto Dependencies = Reader.ReadDependencies(Entry.BuildDependencies); !Dependencies)
			{
				Dependencies.VirtualPath = Entry.VirtualPackagePath;
				Dependencies.EntryIndex = Index;
				return Dependencies;
			}
			if ((Index && !(Candidate.Entries.back().VirtualPackagePath < Entry.VirtualPackagePath)) || Entry.PackageSize == 0
				|| (Entry.SegmentFlags & ~(CookStateSegmentRawFieldProjection | CookStateSegmentOpaque)) != 0)
				return {.Error = ECookStateError::Entry, .VirtualPath = Entry.VirtualPackagePath, .EntryIndex = Index, .RemainingBytes = Reader.GetRemainingBytes()};
			Candidate.Entries.push_back(std::move(Entry));
		}
		if (!Reader.IsAtEnd()) return {.Error = ECookStateError::TrailingBytes, .RemainingBytes = Reader.GetRemainingBytes()};
		OutState = std::move(Candidate);
		return {};
	}

}
