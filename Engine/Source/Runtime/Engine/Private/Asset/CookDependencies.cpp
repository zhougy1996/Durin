#include "Asset/CookDependencies.h"
#include "Serialization/BinaryFormat.h"

namespace Durin
{
	namespace
	{
		auto CookDependencyFail(std::string_view Message, std::string* Error) -> bool
		{
			if (Error) *Error = Message;
			return false;
		}

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

	auto EncodeCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FByteBuffer& OutBytes, std::string* OutError) -> bool
	{
		OutBytes.clear();
		if (Records.size() > MaximumCookDependencyRecords)
			return CookDependencyFail("Cook dependency record limit exceeded.", OutError);
		std::vector<const FCookBuildDependency*> Sorted;
		Sorted.reserve(Records.size());
		for (const auto& Record : Records)
		{
			if (!Valid(Record)) return CookDependencyFail("Invalid Cook dependency kind, name or value.", OutError);
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
				return CookDependencyFail("Duplicate Cook dependency identity.", OutError);
			Writer.WriteU8(static_cast<uint8>(Record.Kind));
			Writer.WriteU32(static_cast<uint32>(Record.LogicalName.size()));
			Writer.WriteBytes(std::as_bytes(std::span(Record.LogicalName)));
			Writer.WriteU32(static_cast<uint32>(Record.Value.size()));
			Writer.WriteBytes(Record.Value);
		}
		if (Writer.HasError()) return CookDependencyFail("Cook dependency byte limit exceeded.", OutError);
		OutBytes = Writer.TakeBytes();
		if (OutError) OutError->clear();
		return true;
	}

	auto DecodeCookBuildDependencies(FByteView Bytes,
		std::vector<FCookBuildDependency>& OutRecords, std::string* OutError) -> bool
	{
		OutRecords.clear();
		if (Bytes.size() > MaximumCookDependencyBytes) return CookDependencyFail("Cook dependency byte limit exceeded.", OutError);
		FBinaryReader Reader(Bytes, {MaximumCookDependencyBytes, MaximumCookDependencyValueBytes});
		uint32 Version = 0, Count = 0;
		if (!Reader.ReadU32(Version) || Version != 1 || !Reader.ReadU32(Count)
			|| Count > MaximumCookDependencyRecords || Count > Reader.GetRemainingBytes() / 10)
			return CookDependencyFail("Invalid Cook dependency header.", OutError);
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
				return CookDependencyFail("Truncated or oversized Cook dependency.", OutError);
			Record.Kind = static_cast<ECookBuildDependencyKind>(Kind);
			Record.LogicalName.assign(reinterpret_cast<const char*>(Name.data()), Name.size());
			Record.Value.assign(Value.begin(), Value.end());
			if (!Valid(Record) || (Index && !(Identity(Candidate.back()) < Identity(Record))))
				return CookDependencyFail("Invalid, duplicate or unsorted Cook dependency.", OutError);
			Candidate.push_back(std::move(Record));
		}
		if (!Reader.IsAtEnd()) return CookDependencyFail("Trailing Cook dependency bytes.", OutError);
		OutRecords = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto FingerprintCookBuildDependencies(std::span<const FCookBuildDependency> Records,
		FXxHash128& OutFingerprint, std::string* OutError) -> bool
	{
		OutFingerprint = {};
		FByteBuffer Bytes;
		if (!EncodeCookBuildDependencies(Records, Bytes, OutError)) return false;
		OutFingerprint = FXxHash128::HashBuffer(Bytes);
		return true;
	}

	auto FCookBuildDependencyGraph::Initialize(std::span<const FCookPackageBuildInputs> Graph,
		std::string* OutError) -> bool
	{
		Nodes.clear(); SourceIdentities.clear();
		FCookBuildDependencyGraph Candidate;
		if (Graph.size() > MaximumCookDependencyRecords) return CookDependencyFail("Cook dependency graph limit exceeded.", OutError);
		uint64 TotalBytes = 0, TotalEdges = 0;
		for (const auto& Node : Graph)
		{
			if (!Node.Package.IsValid() || Candidate.Nodes.contains(Node.Package))
				return CookDependencyFail("Invalid or duplicate Cook dependency package.", OutError);
			FByteBuffer Canonical;
			if (!EncodeCookBuildDependencies(Node.Inputs, Canonical, OutError)) return false;
			TotalBytes += Canonical.size();
			TotalEdges += Node.Packages.size();
			if (TotalBytes > MaximumCookDependencyBytes || TotalEdges > MaximumCookDependencyRecords)
				return CookDependencyFail("Cook dependency graph bound exceeded.", OutError);
			Candidate.Nodes.emplace(Node.Package, Node);
			std::unordered_set<FPackagePath> Unique;
			for (const auto& Dependency : Node.Packages)
				if (!Dependency.Package.IsValid() || !Unique.insert(Dependency.Package).second)
					return CookDependencyFail("Invalid or duplicate package dependency declaration.", OutError);
			std::vector<FCookBuildDependency> Source;
			for (const auto& Input : Node.Inputs)
				if (Input.Kind == ECookBuildDependencyKind::SourcePackage
					|| Input.Kind == ECookBuildDependencyKind::OwnedBulk) Source.push_back(Input);
			if (Source.empty()) return CookDependencyFail("Cook dependency package lacks source identity.", OutError);
			FXxHash128 Digest;
			if (!FingerprintCookBuildDependencies(Source, Digest, OutError)) return false;
			FBinaryWriter Writer;
			Writer.WriteHash128(Digest);
			Candidate.SourceIdentities.emplace(Node.Package, Writer.TakeBytes());
		}
		*this = std::move(Candidate);
		if (OutError) OutError->clear();
		return true;
	}

	auto FCookBuildDependencyGraph::Expand(const FPackagePath& Root,
		std::vector<FCookBuildDependency>& OutRecords, std::string* OutError) const -> bool
	{
		OutRecords.clear();
		std::unordered_set<FPackagePath> Visited;
		std::vector<FPackagePath> Pending{Root};
		std::map<std::pair<ECookBuildDependencyKind, std::string>, FCookBuildDependency> Records;
		uint64 RetainedBytes = 0;
		auto Add = [&](const FCookBuildDependency& Input) -> bool {
			const auto Key = std::pair(Input.Kind, Input.LogicalName);
			const auto Existing = Records.find(Key);
			if (Existing != Records.end())
				return Existing->second == Input || CookDependencyFail("Conflicting Cook dependency values.", OutError);
			RetainedBytes += Input.LogicalName.size() + Input.Value.size() + 9;
			if (Records.size() >= MaximumCookDependencyRecords || RetainedBytes > MaximumCookDependencyBytes - 8)
				return CookDependencyFail("Expanded Cook dependency bound exceeded.", OutError);
			Records.emplace(Key, Input);
			return true;
		};
		while (!Pending.empty())
		{
			const auto Path = Pending.back(); Pending.pop_back();
			if (!Visited.insert(Path).second) continue;
			const auto Found = Nodes.find(Path);
			if (Found == Nodes.end()) return CookDependencyFail("Missing Cook build dependency package.", OutError);
			for (const auto& Input : Found->second.Inputs) if (!Add(Input)) return false;
			for (const auto& Dependency : Found->second.Packages)
			{
				const auto Source = SourceIdentities.find(Dependency.Package);
				if (Source == SourceIdentities.end()) return CookDependencyFail("Missing Cook build dependency source.", OutError);
				if (!Add({Dependency.bTransitive ? ECookBuildDependencyKind::TransitivePackage
					: ECookBuildDependencyKind::DirectPackage, Dependency.Package.ToString(), Source->second})) return false;
				if (Dependency.bTransitive) Pending.push_back(Dependency.Package);
			}
		}
		for (auto& [Key, Record] : Records) OutRecords.push_back(std::move(Record));
		if (OutError) OutError->clear();
		return true;
	}
	auto ExpandCookBuildDependencies(const FPackagePath& Root,
		std::span<const FCookPackageBuildInputs> Graph,
		std::vector<FCookBuildDependency>& OutRecords, std::string* OutError) -> bool
	{
		OutRecords.clear();
		FCookBuildDependencyGraph Prepared;
		return Prepared.Initialize(Graph, OutError) && Prepared.Expand(Root, OutRecords, OutError);
	}

}
