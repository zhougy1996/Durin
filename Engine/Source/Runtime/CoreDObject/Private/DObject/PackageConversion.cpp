#include "DObject/PackageFormat.h"

namespace Durin::ObjectPackage
{
	namespace
	{
		auto FailConversion(FPackageV8ConversionDiagnostic* Diagnostic,
			EPackageV8ConversionFailure Failure, std::string Message,
			std::string Path = {}) -> bool
		{
			if (Diagnostic)
			{
				Diagnostic->Failure = Failure;
				Diagnostic->Message = std::move(Message);
				Diagnostic->LogicalPath = std::move(Path);
			}
			return false;
		}

		auto FormerMainObjectPath(const FPackagePath& PackagePath,
			FObjectPath& Out, std::string* OutError = nullptr) -> bool
		{
			FTopLevelAssetPath AssetPath;
			if (!FTopLevelAssetPath::TryCreate(
				PackagePath, PackagePath.GetPackageName(), AssetPath, OutError)) return false;
			return FObjectPath::TryCreate(
				AssetPath, std::span<const std::string>{}, Out, OutError);
		}

		auto RewriteSoftPaths(const FSerializedType& Type, FSerializedValue& Value,
			std::vector<FPackagePath>& Dependencies, uint32 Depth,
			FPackageV8ConversionDiagnostic* Diagnostic, std::string_view Path) -> bool
		{
			if (Depth > DastV8MaximumValueDepth)
				return FailConversion(Diagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
					"A v8 value exceeds the conversion depth bound.", std::string(Path));
			if (Type.Kind == EValueKind::SoftReference)
			{
				if (Value.Text.empty()) return true;
				FPackagePath PackagePath;
				FObjectPath ObjectPath;
				std::string Error;
				if (!FPackagePath::TryCreate(Value.Text, PackagePath, &Error)
					|| !FormerMainObjectPath(PackagePath, ObjectPath, &Error))
				{
					return FailConversion(Diagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
						"A v8 soft reference cannot be mapped to its former main asset: " + Error,
						std::string(Path));
				}
				Value.Text = ObjectPath.ToString();
				Dependencies.push_back(std::move(PackagePath));
				return true;
			}
			if (Type.Kind == EValueKind::Struct)
			{
				if (Type.Children.size() != Value.Elements.size())
					return FailConversion(Diagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
						"A v8 struct value has mismatched child topology.", std::string(Path));
				for (size_t Index = 0; Index < Type.Children.size(); ++Index)
					if (!RewriteSoftPaths(Type.Children[Index], Value.Elements[Index], Dependencies,
						Depth + 1, Diagnostic, Path)) return false;
			}
			else if (Type.Kind == EValueKind::Array || Type.Kind == EValueKind::FixedArray)
			{
				if (Type.Children.size() != 1) return true;
				for (FSerializedValue& Element : Value.Elements)
					if (!RewriteSoftPaths(Type.Children.front(), Element, Dependencies,
						Depth + 1, Diagnostic, Path)) return false;
			}
			else if (Type.Kind == EValueKind::Map)
			{
				if (Type.Children.size() != 2 || (Value.Elements.size() % 2) != 0) return true;
				for (size_t Index = 0; Index < Value.Elements.size(); ++Index)
					if (!RewriteSoftPaths(Type.Children[Index % 2], Value.Elements[Index], Dependencies,
						Depth + 1, Diagnostic, Path)) return false;
			}
			return true;
		}

		auto SortUniquePaths(std::vector<FPackagePath>& Paths) -> void
		{
			std::ranges::sort(Paths);
			Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());
		}
	}

	auto ConvertPackageV8ToV9(std::span<const std::byte> PackageBytes,
		std::span<const std::byte> BulkBytes, const FPackagePath& PackagePath,
		std::vector<std::byte>& OutPackageBytes,
		std::vector<std::byte>& OutBulkBytes,
		FPackageV8ConversionDiagnostic* OutDiagnostic,
		const FPackageReaderLimits& Limits) -> bool
	{
		if (OutDiagnostic) OutDiagnostic->Reset();
		FLinkerTables Linker;
		FPackageReaderDiagnostic ReaderDiagnostic;
		if (!ReadPackageV8(PackageBytes, BulkBytes, PackagePath.GetView(),
			Linker, &ReaderDiagnostic, Limits))
		{
			if (OutDiagnostic) OutDiagnostic->Reader = ReaderDiagnostic;
			return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::ReadFailure,
				"The v8 closure failed construct-free validation: " + ReaderDiagnostic.Message,
				ReaderDiagnostic.LogicalPath);
		}
		if (!Linker.Summary.MainExport.IsExport()
			|| Linker.Summary.MainExport.GetTableIndex() >= Linker.Exports.size())
			return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
				"The v8 main export is missing or invalid.", "Registry.MainExport");
		const FPackageExport& MainExport =
			Linker.Exports[Linker.Summary.MainExport.GetTableIndex()];
		if (!MainExport.Outer.IsNull() || MainExport.ClassName != Linker.Summary.AssetClass)
			return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
				"The v8 main export is not a package-outer export with the Registry class.",
				"Registry.MainExport");

		FPackageSummary NewSummary;
		NewSummary.PackagePath = PackagePath;
		NewSummary.SearchableNames = Linker.Summary.SearchableNames;
		for (uint32 ExportIndex = 0; ExportIndex < Linker.Exports.size(); ++ExportIndex)
		{
			const FPackageExport& Export = Linker.Exports[ExportIndex];
			if (!Export.Outer.IsNull()) continue;
			FTopLevelAssetPath AssetPath;
			std::string Error;
			if (!FTopLevelAssetPath::TryCreate(
				PackagePath, Export.ObjectName, AssetPath, &Error))
				return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
					"A v8 package-outer export cannot form a top-level asset path: " + Error,
					Export.ObjectName);
			FPackageIndex ExportId;
			if (!FPackageIndex::TryExport(ExportIndex, ExportId))
				return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
					"A v8 package-outer export id cannot be represented.", Export.ObjectName);
			FObjectPath RedirectDestination;
			if (ExportIndex == Linker.Summary.MainExport.GetTableIndex()
				&& Linker.Summary.bRedirect)
			{
				FPackagePath TargetPackage;
				if (!FPackagePath::TryCreate(Linker.Summary.RedirectDestination, TargetPackage, &Error)
					|| !FormerMainObjectPath(TargetPackage, RedirectDestination, &Error))
					return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
						"The v8 redirect destination cannot be mapped: " + Error,
						"Registry.RedirectDestination");
			}
			NewSummary.TopLevelAssets.push_back({
				.Export = ExportId,
				.AssetPath = std::move(AssetPath),
				.ClassName = Export.ClassName,
				.RedirectDestination = std::move(RedirectDestination)});
		}
		std::ranges::sort(NewSummary.TopLevelAssets,
			[](const auto& Left, const auto& Right) {
				return Left.AssetPath.GetView() < Right.AssetPath.GetView();
			});

		for (FPackageImport& Import : Linker.Imports)
		{
			if (!Import.Outer.IsNull())
				return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
					"A nested v8 import cannot be mapped to the v9 top-level import contract.",
					"Imports");
			FPackagePath TargetPackage;
			FTopLevelAssetPath TargetAsset;
			FObjectPath TargetObject;
			std::string Error;
			if (!FPackagePath::TryCreate(Import.PackageName, TargetPackage, &Error)
				|| !FTopLevelAssetPath::TryCreate(TargetPackage,
					TargetPackage.GetPackageName(), TargetAsset, &Error)
				|| !FObjectPath::TryCreate(TargetAsset,
					std::span<const std::string>{}, TargetObject, &Error))
				return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::AmbiguousIdentity,
					"A v8 import cannot be mapped to its former main asset: " + Error,
					"Imports");
			Import.ObjectPath = std::move(TargetObject);
			Import.PackageName.clear();
			Import.ObjectName.clear();
			NewSummary.HardPackageDependencies.push_back(std::move(TargetPackage));
		}

		for (FPackageExport& Export : Linker.Exports)
			for (FPropertyTag& Property : Export.Properties)
				if (!RewriteSoftPaths(Property.Type, Property.Value,
					NewSummary.SoftPackageDependencies, 0, OutDiagnostic,
					Export.ObjectName + "." + Property.FieldName)) return false;
		SortUniquePaths(NewSummary.HardPackageDependencies);
		SortUniquePaths(NewSummary.SoftPackageDependencies);
		Linker.Summary = std::move(NewSummary);

		std::vector<std::byte> ConvertedMain;
		std::vector<std::byte> ConvertedBulk;
		FPackageWriterDiagnostic WriterDiagnostic;
		if (!WritePackageV9(Linker, ConvertedMain, ConvertedBulk, &WriterDiagnostic))
		{
			if (OutDiagnostic) OutDiagnostic->Writer = WriterDiagnostic;
			return FailConversion(OutDiagnostic, EPackageV8ConversionFailure::WriteFailure,
				"The converted v9 linker failed canonical emission: " + WriterDiagnostic.Message,
				WriterDiagnostic.LogicalPath);
		}
		OutPackageBytes = std::move(ConvertedMain);
		OutBulkBytes = std::move(ConvertedBulk);
		return true;
	}
}
