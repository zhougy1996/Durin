#include "Asset/PackageTrailer.h"

#include "BulkContainerInfrastructure.h"

namespace Durin::Asset::PackageTrailer
{
	namespace
	{
		using BulkContainer::FBoundedReader;
		using BulkContainer::FBoundedWriter;

		struct FHeader
		{
			uint32 Magic = 0;
			uint32 Version = 0;
			uint32 HeaderSize = 0;
			uint32 EntrySize = 0;
			uint64 EntryCount = 0;
			uint64 DirectoryOffset = 0;
			uint64 ObjectStreamEnd = 0;
			FXxHash128 DirectoryHash;
			uint64 Reserved = 0;
		};

		struct FFooter
		{
			uint32 Magic = 0;
			uint32 Version = 0;
			uint32 FooterSize = 0;
			uint32 Flags = 0;
			uint64 TrailerOffset = 0;
			uint64 TrailerSize = 0;
			uint64 ObjectStreamEnd = 0;
			FXxHash128 TrailerHash;
			uint64 Reserved = 0;
		};

		auto Fail(std::string Message, std::string* OutError) -> bool
		{
			if (OutError) *OutError = std::move(Message);
			return false;
		}

		auto IsValidEntry(const FEntry& Entry) -> bool
		{
			return Entry.PayloadId.IsValid()
				&& Entry.Placement == EPlacement::ExternalDabkV1
				&& Entry.LogicalByteCount == Entry.StoredByteCount
				&& !Entry.ContentHash.IsZero()
				&& !Entry.ContainerHash.IsZero();
		}

		auto WriteHeader(FBoundedWriter& Writer, const FHeader& Header) -> bool
		{
			return Writer.Write(Header.Magic)
				&& Writer.Write(Header.Version)
				&& Writer.Write(Header.HeaderSize)
				&& Writer.Write(Header.EntrySize)
				&& Writer.Write(Header.EntryCount)
				&& Writer.Write(Header.DirectoryOffset)
				&& Writer.Write(Header.ObjectStreamEnd)
				&& Writer.Write(Header.DirectoryHash.HashLow)
				&& Writer.Write(Header.DirectoryHash.HashHigh)
				&& Writer.Write(Header.Reserved);
		}

		auto ReadHeader(FBoundedReader& Reader, FHeader& OutHeader) -> bool
		{
			FHeader Header;
			Reader.Read(Header.Magic);
			Reader.Read(Header.Version);
			Reader.Read(Header.HeaderSize);
			Reader.Read(Header.EntrySize);
			Reader.Read(Header.EntryCount);
			Reader.Read(Header.DirectoryOffset);
			Reader.Read(Header.ObjectStreamEnd);
			Reader.Read(Header.DirectoryHash.HashLow);
			Reader.Read(Header.DirectoryHash.HashHigh);
			Reader.Read(Header.Reserved);
			if (!Reader.IsValid()) return false;
			OutHeader = Header;
			return true;
		}

		auto WriteEntry(FBoundedWriter& Writer, const FEntry& Entry) -> bool
		{
			return Writer.WriteGuid(Entry.PayloadId)
				&& Writer.Write(static_cast<uint32>(Entry.Placement))
				&& Writer.Write(uint32{0})
				&& Writer.Write(Entry.LogicalByteCount)
				&& Writer.Write(Entry.StoredByteCount)
				&& Writer.Write(Entry.ContentHash.HashLow)
				&& Writer.Write(Entry.ContentHash.HashHigh)
				&& Writer.Write(Entry.ContainerHash.HashLow)
				&& Writer.Write(Entry.ContainerHash.HashHigh)
				&& Writer.Write(uint64{0});
		}

		auto ReadEntry(FBoundedReader& Reader, FEntry& OutEntry) -> bool
		{
			FEntry Entry;
			uint32 Placement = 0;
			uint32 Flags = 0;
			uint64 Reserved = 0;
			Reader.ReadGuid(Entry.PayloadId);
			Reader.Read(Placement);
			Reader.Read(Flags);
			Reader.Read(Entry.LogicalByteCount);
			Reader.Read(Entry.StoredByteCount);
			Reader.Read(Entry.ContentHash.HashLow);
			Reader.Read(Entry.ContentHash.HashHigh);
			Reader.Read(Entry.ContainerHash.HashLow);
			Reader.Read(Entry.ContainerHash.HashHigh);
			Reader.Read(Reserved);
			if (!Reader.IsValid() || Flags != 0 || Reserved != 0) return false;
			Entry.Placement = static_cast<EPlacement>(Placement);
			OutEntry = Entry;
			return true;
		}

		auto WriteFooter(FBoundedWriter& Writer, const FFooter& Footer) -> bool
		{
			return Writer.Write(Footer.Magic)
				&& Writer.Write(Footer.Version)
				&& Writer.Write(Footer.FooterSize)
				&& Writer.Write(Footer.Flags)
				&& Writer.Write(Footer.TrailerOffset)
				&& Writer.Write(Footer.TrailerSize)
				&& Writer.Write(Footer.ObjectStreamEnd)
				&& Writer.Write(Footer.TrailerHash.HashLow)
				&& Writer.Write(Footer.TrailerHash.HashHigh)
				&& Writer.Write(Footer.Reserved);
		}

		auto ReadFooter(FBoundedReader& Reader, FFooter& OutFooter) -> bool
		{
			FFooter Footer;
			Reader.Read(Footer.Magic);
			Reader.Read(Footer.Version);
			Reader.Read(Footer.FooterSize);
			Reader.Read(Footer.Flags);
			Reader.Read(Footer.TrailerOffset);
			Reader.Read(Footer.TrailerSize);
			Reader.Read(Footer.ObjectStreamEnd);
			Reader.Read(Footer.TrailerHash.HashLow);
			Reader.Read(Footer.TrailerHash.HashHigh);
			Reader.Read(Footer.Reserved);
			if (!Reader.IsValid()) return false;
			OutFooter = Footer;
			return true;
		}
	}

	auto Build(std::span<const FEntry> Entries, uint64 ObjectStreamEnd,
		std::vector<std::byte>& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (ObjectStreamEnd < 8 || ObjectStreamEnd > MaximumObjectStreamBytes)
			return Fail("Package trailer object-stream extent is outside the supported bound.", OutError);
		if (Entries.size() > MaximumEntryCount)
			return Fail("Package trailer entry count exceeds the supported bound.", OutError);

		std::vector<const FEntry*> Sorted;
		if (!BulkContainer::TryMakeSortedProjection<FEntry>(
				Entries, [](const FEntry& Entry) { return Entry.PayloadId; }, Sorted))
			return Fail("Package trailer payload ids are duplicate.", OutError);
		for (const FEntry* Entry : Sorted)
			if (!IsValidEntry(*Entry))
				return Fail("Package trailer entry is invalid or unsupported.", OutError);

		uint64 DirectoryBytes = 0;
		uint64 TrailerSize = 0;
		uint64 FooterOffset = 0;
		uint64 FileSize = 0;
		if (!BulkContainer::TryMultiply(
				Sorted.size(), TrailerEntryBytes, MaximumPackageBytes, DirectoryBytes)
			|| !BulkContainer::TryAdd(
				TrailerHeaderBytes, DirectoryBytes, MaximumPackageBytes, TrailerSize)
			|| !BulkContainer::TryAdd(
				ObjectStreamEnd, TrailerSize, MaximumPackageBytes, FooterOffset)
			|| !BulkContainer::TryAdd(
				FooterOffset, FooterBytes, MaximumPackageBytes, FileSize))
			return Fail("Package trailer exceeds the supported package bound.", OutError);

		FBoundedWriter DirectoryWriter(MaximumPackageBytes);
		for (const FEntry* Entry : Sorted)
			if (!WriteEntry(DirectoryWriter, *Entry))
				return Fail("Package trailer directory encoding failed.", OutError);
		const FXxHash128 DirectoryHash = FXxHash128::HashBuffer(DirectoryWriter.View());

		FBoundedWriter Writer(MaximumPackageBytes);
		const FHeader Header{
			.Magic = TrailerMagic,
			.Version = TrailerVersion,
			.HeaderSize = TrailerHeaderBytes,
			.EntrySize = TrailerEntryBytes,
			.EntryCount = Sorted.size(),
			.DirectoryOffset = TrailerHeaderBytes,
			.ObjectStreamEnd = ObjectStreamEnd,
			.DirectoryHash = DirectoryHash,
			.Reserved = 0};
		if (!WriteHeader(Writer, Header) || !Writer.Write(DirectoryWriter.View())
			|| Writer.Tell() != TrailerSize)
			return Fail("Package trailer encoding failed.", OutError);
		const FXxHash128 TrailerHash = FXxHash128::HashBuffer(Writer.View());
		const FFooter Footer{
			.Magic = FooterMagic,
			.Version = FooterVersion,
			.FooterSize = FooterBytes,
			.Flags = 0,
			.TrailerOffset = ObjectStreamEnd,
			.TrailerSize = TrailerSize,
			.ObjectStreamEnd = ObjectStreamEnd,
			.TrailerHash = TrailerHash,
			.Reserved = 0};
		if (!WriteFooter(Writer, Footer) || Writer.Tell() != TrailerSize + FooterBytes)
			return Fail("Package trailer footer encoding failed.", OutError);
		std::vector<std::byte> Candidate;
		if (!Writer.TryTake(Candidate))
			return Fail("Package trailer detached transfer failed.", OutError);
		OutBytes = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto Inspect(std::span<const std::byte> PackageBytes, FInspection& OutInspection,
		std::string* OutError) -> bool
	{
		OutInspection = {};
		if (PackageBytes.size() > MaximumPackageBytes
			|| PackageBytes.size() < 8 + TrailerHeaderBytes + FooterBytes)
			return Fail("Package trailer file size is outside the supported bound.", OutError);

		const uint64 FooterOffset = PackageBytes.size() - FooterBytes;
		std::span<const std::byte> FooterBytesView;
		if (!BulkContainer::TryProjectRange(
				PackageBytes, FooterOffset, FooterBytes, FooterBytesView))
			return Fail("Package trailer footer range is invalid.", OutError);
		FBoundedReader FooterReader(FooterBytesView, FooterBytes);
		FFooter Footer;
		if (!ReadFooter(FooterReader, Footer)
			|| FooterReader.Tell() != FooterBytes
			|| Footer.Magic != FooterMagic
			|| Footer.Version != FooterVersion
			|| Footer.FooterSize != FooterBytes
			|| Footer.Flags != 0
			|| Footer.Reserved != 0)
			return Fail("Package trailer footer is invalid or unsupported.", OutError);
		if (Footer.ObjectStreamEnd < 8
			|| Footer.ObjectStreamEnd > MaximumObjectStreamBytes
			|| Footer.TrailerOffset != Footer.ObjectStreamEnd
			|| Footer.TrailerSize < TrailerHeaderBytes)
			return Fail("Package trailer footer extent is invalid.", OutError);
		uint64 ExpectedFooterOffset = 0;
		if (!BulkContainer::TryAdd(
				Footer.TrailerOffset, Footer.TrailerSize,
				MaximumPackageBytes, ExpectedFooterOffset)
			|| ExpectedFooterOffset != FooterOffset)
			return Fail("Package trailer footer does not describe the physical EOF layout.", OutError);

		std::span<const std::byte> TrailerBytesView;
		if (!BulkContainer::TryProjectRange(
				PackageBytes, Footer.TrailerOffset, Footer.TrailerSize, TrailerBytesView))
			return Fail("Package trailer range is invalid.", OutError);
		if (FXxHash128::HashBuffer(TrailerBytesView) != Footer.TrailerHash)
			return Fail("Package trailer hash verification failed.", OutError);

		FBoundedReader TrailerReader(TrailerBytesView, MaximumPackageBytes);
		FHeader Header;
		if (!ReadHeader(TrailerReader, Header)
			|| Header.Magic != TrailerMagic
			|| Header.Version != TrailerVersion
			|| Header.HeaderSize != TrailerHeaderBytes
			|| Header.EntrySize != TrailerEntryBytes
			|| Header.DirectoryOffset != TrailerHeaderBytes
			|| Header.ObjectStreamEnd != Footer.ObjectStreamEnd
			|| Header.Reserved != 0
			|| Header.EntryCount > MaximumEntryCount)
			return Fail("Package trailer header is invalid or unsupported.", OutError);
		uint64 DirectoryBytes = 0;
		uint64 ExpectedTrailerSize = 0;
		if (!BulkContainer::TryMultiply(
				Header.EntryCount, TrailerEntryBytes, MaximumPackageBytes, DirectoryBytes)
			|| !BulkContainer::TryAdd(
				TrailerHeaderBytes, DirectoryBytes, MaximumPackageBytes, ExpectedTrailerSize)
			|| ExpectedTrailerSize != Footer.TrailerSize)
			return Fail("Package trailer directory extent is invalid.", OutError);
		std::span<const std::byte> DirectoryBytesView;
		if (!BulkContainer::TryProjectRange(
				TrailerBytesView, Header.DirectoryOffset, DirectoryBytes, DirectoryBytesView)
			|| FXxHash128::HashBuffer(DirectoryBytesView) != Header.DirectoryHash)
			return Fail("Package trailer directory hash verification failed.", OutError);

		FBoundedReader DirectoryReader(DirectoryBytesView, MaximumPackageBytes);
		FInspection Candidate{
			.ObjectStreamEnd = Footer.ObjectStreamEnd,
			.TrailerOffset = Footer.TrailerOffset,
			.TrailerSize = Footer.TrailerSize,
			.DirectoryHash = Header.DirectoryHash,
			.TrailerHash = Footer.TrailerHash};
		Candidate.Entries.reserve(static_cast<size_t>(Header.EntryCount));
		for (uint64 Index = 0; Index < Header.EntryCount; ++Index)
		{
			FEntry Entry;
			if (!ReadEntry(DirectoryReader, Entry) || !IsValidEntry(Entry))
				return Fail("Package trailer entry is invalid or unsupported.", OutError);
			if (!Candidate.Entries.empty()
				&& !(Candidate.Entries.back().PayloadId < Entry.PayloadId))
				return Fail("Package trailer payload ids are duplicate or noncanonical.", OutError);
			Candidate.Entries.push_back(Entry);
		}
		if (DirectoryReader.Tell() != DirectoryBytes || TrailerReader.Tell() != TrailerHeaderBytes)
			return Fail("Package trailer contains unconsumed bytes.", OutError);
		OutInspection = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}
}
