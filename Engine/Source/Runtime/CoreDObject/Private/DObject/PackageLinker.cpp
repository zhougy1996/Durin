#include "DObject/PackageLinker.h"

namespace Durin::ObjectPackage
{
	namespace
	{
		auto CompareBytes(std::string_view Left, std::string_view Right) -> std::strong_ordering
		{
			const size_t Common = std::min(Left.size(), Right.size());
			for (size_t Index = 0; Index < Common; ++Index)
			{
				const uint8 A = static_cast<uint8>(Left[Index]);
				const uint8 B = static_cast<uint8>(Right[Index]);
				if (A < B) return std::strong_ordering::less;
				if (A > B) return std::strong_ordering::greater;
			}
			return Left.size() <=> Right.size();
		}

		auto Fail(FLinkerDiagnostic* Diagnostic, ELinkerFailure Failure,
			std::string Message, std::string Path = {}) -> bool
		{
			if (Diagnostic) *Diagnostic = {Failure, std::move(Path), std::move(Message)};
			return false;
		}
	}

	auto FPackageIndex::TryFromRaw(int64 Raw, FPackageIndex& Out) -> bool
	{
		FPackageIndex Result;
		if (Raw > std::numeric_limits<int32>::max() || Raw < -int64(std::numeric_limits<int32>::max()))
			return false;
		if (Raw > 0)
		{
			Result.Kind = EKind::Export;
			Result.TableIndex = static_cast<uint32>(Raw - 1);
		}
		else if (Raw < 0)
		{
			Result.Kind = EKind::Import;
			Result.TableIndex = static_cast<uint32>(-Raw - 1);
		}
		Out = Result;
		return true;
	}

	auto FPackageIndex::TryImport(uint64 ZeroBasedIndex, FPackageIndex& Out) -> bool
	{
		if (ZeroBasedIndex >= uint64(std::numeric_limits<int32>::max())) return false;
		FPackageIndex Result;
		Result.Kind = EKind::Import;
		Result.TableIndex = static_cast<uint32>(ZeroBasedIndex);
		Out = Result;
		return true;
	}

	auto FPackageIndex::TryExport(uint64 ZeroBasedIndex, FPackageIndex& Out) -> bool
	{
		if (ZeroBasedIndex >= uint64(std::numeric_limits<int32>::max())) return false;
		FPackageIndex Result;
		Result.Kind = EKind::Export;
		Result.TableIndex = static_cast<uint32>(ZeroBasedIndex);
		Out = Result;
		return true;
	}

	auto FPackageIndex::ToRaw() const -> int64
	{
		if (Kind == EKind::Null) return 0;
		const int64 OneBased = int64(TableIndex) + 1;
		return Kind == EKind::Import ? -OneBased : OneBased;
	}

	auto FSerializedType::operator==(const FSerializedType& Other) const -> bool
	{
		return Kind == Other.Kind && QualifiedName == Other.QualifiedName
			&& Parameter == Other.Parameter && Children == Other.Children;
	}

	auto FSerializedType::operator<=>(const FSerializedType& Other) const -> std::strong_ordering
	{
		if (const auto Result = Kind <=> Other.Kind; Result != 0) return Result;
		if (const auto Result = CompareBytes(QualifiedName, Other.QualifiedName); Result != 0) return Result;
		if (const auto Result = Parameter <=> Other.Parameter; Result != 0) return Result;
		const size_t Common = std::min(Children.size(), Other.Children.size());
		for (size_t Index = 0; Index < Common; ++Index)
			if (const auto Result = Children[Index] <=> Other.Children[Index]; Result != 0) return Result;
		return Children.size() <=> Other.Children.size();
	}

	auto FLinkerTables::TryGetName(uint64 OneBasedIndex, std::string_view& Out) const -> bool
	{
		if (OneBasedIndex == 0 || OneBasedIndex > Names.size()) return false;
		Out = Names[static_cast<size_t>(OneBasedIndex - 1)];
		return true;
	}

	auto FLinkerTables::TryGetType(uint64 OneBasedIndex, const FSerializedType*& Out) const -> bool
	{
		if (OneBasedIndex == 0 || OneBasedIndex > Types.size()) return false;
		Out = &Types[static_cast<size_t>(OneBasedIndex - 1)];
		return true;
	}

	auto FLinkerTables::TryGetSchema(uint64 OneBasedIndex, const FSerializedSchema*& Out) const -> bool
	{
		if (OneBasedIndex == 0 || OneBasedIndex > Schemas.size()) return false;
		Out = &Schemas[static_cast<size_t>(OneBasedIndex - 1)];
		return true;
	}

	auto FLinkerTables::TryGetImport(FPackageIndex Index, const FPackageImport*& Out) const -> bool
	{
		if (!Index.IsImport() || Index.GetTableIndex() >= Imports.size()) return false;
		Out = &Imports[Index.GetTableIndex()];
		return true;
	}

	auto FLinkerTables::TryGetExport(FPackageIndex Index, const FPackageExport*& Out) const -> bool
	{
		if (!Index.IsExport() || Index.GetTableIndex() >= Exports.size()) return false;
		Out = &Exports[Index.GetTableIndex()];
		return true;
	}

	auto FLinkerTables::TryResolvePath(FPackageIndex Index, std::string& Out,
		FLinkerDiagnostic* OutDiagnostic) const -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		std::vector<FPackageIndex> Chain;
		FPackageIndex Current = Index;
		while (!Current.IsNull())
		{
			if (Chain.size() > Imports.size() + Exports.size())
				return Fail(OutDiagnostic, ELinkerFailure::InvalidTopology,
					"Package Outer topology contains a cycle.");
			if (std::ranges::find(Chain, Current) != Chain.end())
				return Fail(OutDiagnostic, ELinkerFailure::InvalidTopology,
					"Package Outer topology contains a cycle.");
			Chain.push_back(Current);
			if (Current.IsImport())
			{
				const FPackageImport* Import = nullptr;
				if (!TryGetImport(Current, Import))
					return Fail(OutDiagnostic, ELinkerFailure::InvalidIndex,
						"Package import index is out of range.");
				Current = Import->Outer;
			}
			else
			{
				const FPackageExport* Export = nullptr;
				if (!TryGetExport(Current, Export))
					return Fail(OutDiagnostic, ELinkerFailure::InvalidIndex,
						"Package export index is out of range.");
				Current = Export->Outer;
			}
		}

		std::string Result;
		for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
		{
			std::string Name;
			if (It->IsImport())
			{
				const FPackageImport& Import = Imports[It->GetTableIndex()];
				Name = Import.ObjectPath.ToString();
			}
			else Name = Exports[It->GetTableIndex()].ObjectName;
			if (Name.empty()) continue;
			if (!Result.empty()) Result.push_back('/');
			Result.append(Name);
		}
		Out = std::move(Result);
		return true;
	}
}
