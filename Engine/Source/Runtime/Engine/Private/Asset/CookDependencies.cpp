#include "Asset/CookDependencies.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		auto Identity(const FCookBuildDependency& Record)
		{
			return std::tuple(Record.Kind, std::string_view(Record.LogicalName));
		}

		auto Valid(const FCookBuildDependency& Record) -> bool
		{
			return Record.Kind >= ECookBuildDependencyKind::SourcePackage
				&& Record.Kind <= ECookBuildDependencyKind::SchemaProducerVersion
				&& !Record.LogicalName.empty() && Record.LogicalName.size() <= 4096
				&& Record.LogicalName.find('\0') == std::string::npos
				&& Record.Value.size() <= MaximumCookDependencyValueBytes;
		}
	}

	auto FormatCookDependencyCodecError(const FCookDependencyCodecResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookDependencyCodecError::None: return {};
		case ECookDependencyCodecError::RecordLimit: return "Cook dependency record limit exceeded.";
		case ECookDependencyCodecError::InvalidRecord: return "Invalid Cook dependency kind, name or value: " + Result.LogicalName;
		case ECookDependencyCodecError::Duplicate: return "Duplicate Cook dependency identity: " + Result.LogicalName;
		case ECookDependencyCodecError::ByteLimit: return "Cook dependency byte limit exceeded.";
		case ECookDependencyCodecError::Header: return "Invalid Cook dependency header.";
		case ECookDependencyCodecError::TruncatedRecord: return "Truncated or oversized Cook dependency.";
		case ECookDependencyCodecError::Noncanonical: return "Invalid, duplicate or unsorted Cook dependency: " + Result.LogicalName;
		case ECookDependencyCodecError::TrailingBytes: return "Trailing Cook dependency bytes.";
		}
		return {};
	}

	auto EncodeCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FByteBuffer& OutBytes) -> FCookDependencyCodecResult
	{
		OutBytes.clear();
		if (Records.size() > MaximumCookDependencyRecords)
			return {.Error = ECookDependencyCodecError::RecordLimit, .Actual = Records.size(), .Maximum = MaximumCookDependencyRecords};
		std::vector<const FCookBuildDependency*> Sorted;
		Sorted.reserve(Records.size());
		for (const auto& Record : Records)
		{
			if (!Valid(Record)) return {.Error = ECookDependencyCodecError::InvalidRecord, .Kind = Record.Kind, .LogicalName = Record.LogicalName, .Actual = Record.Value.size(), .Maximum = MaximumCookDependencyValueBytes};
			Sorted.push_back(&Record);
		}
		std::ranges::sort(Sorted, [](const auto* A, const auto* B) { return Identity(*A) < Identity(*B); });
		FBinaryWriter Writer({MaximumCookDependencyBytes, MaximumCookDependencyValueBytes});
		Writer.WriteU32(1); // Canonical dependency encoding version.
		Writer.WriteU32(static_cast<uint32>(Sorted.size()));
		for (size_t Index = 0; Index < Sorted.size(); ++Index)
		{
			const auto& Record = *Sorted[Index];
			if (Index && Identity(*Sorted[Index - 1]) == Identity(Record))
				return {.Error = ECookDependencyCodecError::Duplicate, .Kind = Record.Kind, .LogicalName = Record.LogicalName, .RecordIndex = Index};
			Writer.WriteU8(static_cast<uint8>(Record.Kind));
			Writer.WriteU32(static_cast<uint32>(Record.LogicalName.size()));
			Writer.WriteBytes(std::as_bytes(std::span(Record.LogicalName)));
			Writer.WriteU32(static_cast<uint32>(Record.Value.size()));
			Writer.WriteBytes(Record.Value);
		}
		if (Writer.HasError()) return {.Error = ECookDependencyCodecError::ByteLimit, .Maximum = MaximumCookDependencyBytes};
		OutBytes = Writer.TakeBytes();
		return {};
	}

	auto DecodeCookBuildDependencies(FByteView Bytes,
		std::vector<FCookBuildDependency>& OutRecords) -> FCookDependencyCodecResult
	{
		OutRecords.clear();
		if (Bytes.size() > MaximumCookDependencyBytes) return {.Error = ECookDependencyCodecError::ByteLimit, .Actual = Bytes.size(), .Maximum = MaximumCookDependencyBytes};
		FBinaryReader Reader(Bytes, {MaximumCookDependencyBytes, MaximumCookDependencyValueBytes});
		uint32 Version = 0, Count = 0;
		if (!Reader.ReadU32(Version) || Version != 1 || !Reader.ReadU32(Count)
			|| Count > MaximumCookDependencyRecords || Count > Reader.GetRemainingBytes() / 10)
			return {.Error = ECookDependencyCodecError::Header, .Actual = Count, .Maximum = MaximumCookDependencyRecords, .RemainingBytes = Reader.GetRemainingBytes(), .Version = Version};
		std::vector<FCookBuildDependency> Candidate;
		Candidate.reserve(Count);
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			FCookBuildDependency Record;
			uint8 Kind = 0;
			uint32 NameSize = 0, ValueSize = 0;
			FByteView Name, Value;
			if (!Reader.ReadU8(Kind) || !Reader.ReadU32(NameSize)
				|| NameSize == 0 || NameSize > 4096 || !Reader.ReadRegion(Name, NameSize, 4096)
				|| !Reader.ReadU32(ValueSize) || ValueSize > MaximumCookDependencyValueBytes
				|| !Reader.ReadRegion(Value, ValueSize, MaximumCookDependencyValueBytes))
				return {.Error = ECookDependencyCodecError::TruncatedRecord, .Kind = static_cast<ECookBuildDependencyKind>(Kind), .RecordIndex = Index, .Actual = ValueSize, .Maximum = MaximumCookDependencyValueBytes, .RemainingBytes = Reader.GetRemainingBytes()};
			Record.Kind = static_cast<ECookBuildDependencyKind>(Kind);
			Record.LogicalName.assign(reinterpret_cast<const char*>(Name.data()), Name.size());
			Record.Value.assign(Value.begin(), Value.end());
			if (!Valid(Record) || (Index && !(Identity(Candidate.back()) < Identity(Record))))
				return {.Error = ECookDependencyCodecError::Noncanonical, .Kind = Record.Kind, .LogicalName = Record.LogicalName, .RecordIndex = Index};
			Candidate.push_back(std::move(Record));
		}
		if (!Reader.IsAtEnd()) return {.Error = ECookDependencyCodecError::TrailingBytes, .RemainingBytes = Reader.GetRemainingBytes()};
		OutRecords = std::move(Candidate);
		return {};
	}

	auto FingerprintCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FXxHash128& OutFingerprint) -> FCookDependencyCodecResult
	{
		OutFingerprint = {};
		FByteBuffer Bytes;
		if (const auto Encoded = EncodeCookBuildDependencies(Records, Bytes); !Encoded)
			return Encoded;
		OutFingerprint = FXxHash128::HashBuffer(Bytes);
		return {};
	}

	auto FormatCookDependencyGraphError(const FCookDependencyGraphResult& Result) -> std::string
	{
		switch (Result.Error)
		{
		case ECookDependencyGraphError::None: return {};
		case ECookDependencyGraphError::GraphLimit: return "Cook dependency graph limit exceeded.";
		case ECookDependencyGraphError::Package: return "Invalid or duplicate Cook dependency package.";
		case ECookDependencyGraphError::Codec: return Result.CodecCause ? FormatCookDependencyCodecError(*Result.CodecCause) : "Cook dependency codec failure.";
		case ECookDependencyGraphError::GraphBound: return "Cook dependency graph bound exceeded.";
		case ECookDependencyGraphError::Declaration: return "Invalid or duplicate package dependency declaration.";
		case ECookDependencyGraphError::SourceIdentity: return "Cook dependency package lacks source identity.";
		case ECookDependencyGraphError::Conflict: return "Conflicting Cook dependency values: " + Result.LogicalName;
		case ECookDependencyGraphError::ExpansionBound: return "Expanded Cook dependency bound exceeded.";
		case ECookDependencyGraphError::MissingPackage: return "Missing Cook build dependency package: " + Result.Dependency.ToString();
		case ECookDependencyGraphError::MissingSource: return "Missing Cook build dependency source: " + Result.Dependency.ToString();
		}
		return {};
	}

	auto FCookDependencyGraphResult::ToAssetResult() const -> FAssetResult
	{
		if (*this) return {};
		// The asset result's text contract remains a staged adapter; retain the
		// complete cause independently for callers and later presentation migration.
		FAssetResult Result{EAssetError::CorruptFile, FormatCookDependencyGraphError(*this)};
		Result.CookDependencyCause = std::make_shared<FCookDependencyGraphResult>(*this);
		return Result;
	}

	auto FCookBuildDependencyGraph::Initialize(std::span<const FCookPackageBuildInputs> Graph) -> FCookDependencyGraphResult
	{
		Nodes.clear(); SourceIdentities.clear();
		FCookBuildDependencyGraph Candidate;
		if (Graph.size() > MaximumCookDependencyRecords) return {.Error = ECookDependencyGraphError::GraphLimit, .Records = Graph.size()};
		uint64 TotalBytes = 0, TotalEdges = 0;
		for (const auto& Node : Graph)
		{
			if (!Node.Package.IsValid() || Candidate.Nodes.contains(Node.Package))
				return {.Error = ECookDependencyGraphError::Package, .Package = Node.Package};
			FByteBuffer Canonical;
			if (const auto Encoded = EncodeCookBuildDependencies(Node.Inputs, Canonical); !Encoded)
				return {.Error = ECookDependencyGraphError::Codec, .Package = Node.Package, .CodecCause = Encoded};
			TotalBytes += Canonical.size();
			TotalEdges += Node.Packages.size();
			if (TotalBytes > MaximumCookDependencyBytes || TotalEdges > MaximumCookDependencyRecords)
				return {.Error = ECookDependencyGraphError::GraphBound, .Package = Node.Package, .Bytes = TotalBytes, .Records = TotalEdges};
			Candidate.Nodes.emplace(Node.Package, Node);
			std::unordered_set<FPackagePath> Unique;
			for (const auto& Dependency : Node.Packages)
				if (!Dependency.Package.IsValid() || !Unique.insert(Dependency.Package).second)
					return {.Error = ECookDependencyGraphError::Declaration, .Package = Node.Package, .Dependency = Dependency.Package};
			std::vector<FCookBuildDependency> Source;
			for (const auto& Input : Node.Inputs)
				if (Input.Kind == ECookBuildDependencyKind::SourcePackage
					|| Input.Kind == ECookBuildDependencyKind::OwnedBulk) Source.push_back(Input);
			if (Source.empty()) return {.Error = ECookDependencyGraphError::SourceIdentity, .Package = Node.Package};
			FXxHash128 Digest;
			if (const auto Fingerprinted = FingerprintCookBuildDependencies(Source, Digest); !Fingerprinted)
				return {.Error = ECookDependencyGraphError::Codec, .Package = Node.Package, .CodecCause = Fingerprinted};
			FBinaryWriter Writer;
			Writer.WriteHash128(Digest);
			Candidate.SourceIdentities.emplace(Node.Package, Writer.TakeBytes());
		}
		*this = std::move(Candidate);
		return {};
	}

	auto FCookBuildDependencyGraph::Expand(const FPackagePath& Root,
		std::vector<FCookBuildDependency>& OutRecords) const -> FCookDependencyGraphResult
	{
		OutRecords.clear();
		std::unordered_set<FPackagePath> Visited;
		std::vector<FPackagePath> Pending{Root};
		std::map<std::pair<ECookBuildDependencyKind, std::string>, FCookBuildDependency> Records;
		uint64 RetainedBytes = 0;
		auto Add = [&](const FCookBuildDependency& Input) -> FCookDependencyGraphResult {
			const auto Key = std::pair(Input.Kind, Input.LogicalName);
			const auto Existing = Records.find(Key);
			if (Existing != Records.end())
				return Existing->second == Input ? FCookDependencyGraphResult{} : FCookDependencyGraphResult{.Error = ECookDependencyGraphError::Conflict, .Package = Root, .Kind = Input.Kind, .LogicalName = Input.LogicalName};
			RetainedBytes += Input.LogicalName.size() + Input.Value.size() + 9;
			if (Records.size() >= MaximumCookDependencyRecords || RetainedBytes > MaximumCookDependencyBytes - 8)
				return {.Error = ECookDependencyGraphError::ExpansionBound, .Package = Root, .Kind = Input.Kind, .LogicalName = Input.LogicalName, .Bytes = RetainedBytes, .Records = Records.size()};
			Records.emplace(Key, Input);
			return {};
		};
		while (!Pending.empty())
		{
			const auto Path = Pending.back(); Pending.pop_back();
			if (!Visited.insert(Path).second) continue;
			const auto Found = Nodes.find(Path);
			if (Found == Nodes.end()) return {.Error = ECookDependencyGraphError::MissingPackage, .Package = Root, .Dependency = Path};
			for (const auto& Input : Found->second.Inputs)
				if (const auto Added = Add(Input); !Added) return Added;
			for (const auto& Dependency : Found->second.Packages)
			{
				const auto Source = SourceIdentities.find(Dependency.Package);
				if (Source == SourceIdentities.end()) return {.Error = ECookDependencyGraphError::MissingSource, .Package = Path, .Dependency = Dependency.Package};
				if (const auto Added = Add({Dependency.bTransitive ? ECookBuildDependencyKind::TransitivePackage
					: ECookBuildDependencyKind::DirectPackage, Dependency.Package.ToString(), Source->second}); !Added) return Added;
				if (Dependency.bTransitive) Pending.push_back(Dependency.Package);
			}
		}
		for (auto& [Key, Record] : Records) OutRecords.push_back(std::move(Record));
		return {};
	}
	auto ExpandCookBuildDependencies(const FPackagePath& Root,
		std::span<const FCookPackageBuildInputs> Graph,
		std::vector<FCookBuildDependency>& OutRecords) -> FCookDependencyGraphResult
	{
		OutRecords.clear();
		FCookBuildDependencyGraph Prepared;
		if (const auto Initialized = Prepared.Initialize(Graph); !Initialized) return Initialized;
		return Prepared.Expand(Root, OutRecords);
	}

}
