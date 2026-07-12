#include "StaticMesh/StaticMesh.h"

#include "AssetCore.h"
#include "AssetSystem.h"
#include "DObject/DObjectGlobals.h"
#include "Misc/Paths.h"
#include "StaticMesh/StaticMeshResources.h"

#include "RHICommandList.h"

namespace Durin
{
	namespace
	{
		auto ResolveMountedFile(std::string_view VirtualPath) -> std::filesystem::path
		{
			for (const PathUtilities::FMountPoint& Mount : PathUtilities::GetRegisteredMountPoints())
			{
				if (VirtualPath.starts_with(Mount.VirtualRoot))
				{
					return (std::filesystem::path(Mount.PhysicalPath) / std::string(VirtualPath.substr(Mount.VirtualRoot.size()))).lexically_normal();
				}
			}
			return std::filesystem::path(VirtualPath).lexically_normal();
		}

		auto ResolveStaticMeshSource(const DStaticMesh& Mesh) -> std::filesystem::path
		{
			const std::filesystem::path StoredPath(Mesh.GetSourceFile());
			const std::filesystem::path PackageFile = ResolveMountedFile(Mesh.GetPackage()->GetPackagePath());
			if (!StoredPath.is_absolute() && !Mesh.GetSourceFile().starts_with('/'))
			{
				return (PackageFile.parent_path() / StoredPath).lexically_normal();
			}

			const std::filesystem::path LegacyPath = ResolveMountedFile(Mesh.GetSourceFile());
			if (std::filesystem::is_regular_file(LegacyPath)) return LegacyPath;
			return (PackageFile.parent_path() / StoredPath.filename()).lexically_normal();
		}
	}

	DStaticMesh::DStaticMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{
		static const bool RegisteredMoveContributor = [] {
			Asset::RegisterAssetMoveContributor(DStaticMesh::StaticClass(), [](DObject* Object, const FAssetPath& OldPath, const FAssetPath& NewPath, Asset::FAssetMoveContribution& Out) -> Asset::FAssetResult {
				auto* Mesh = Cast<DStaticMesh>(Object);
				if (!Mesh || Mesh->SourceFile.empty()) return {};
				const std::string Original = Mesh->SourceFile;
				const std::filesystem::path OldPackage = ResolveMountedFile(OldPath.ToString());
				const std::filesystem::path NewPackage = ResolveMountedFile(NewPath.ToString());
				const std::filesystem::path SourceName(Original);
				const std::filesystem::path OldSource = SourceName.is_absolute() ? SourceName : OldPackage.parent_path() / SourceName;
				const std::string NewFileName = OldPath.GetAssetName() == NewPath.GetAssetName()
					? SourceName.filename().generic_string()
					: std::string(NewPath.GetAssetName()) + SourceName.extension().generic_string();
				const std::filesystem::path NewSource = NewPackage.parent_path() / NewFileName;
				if (OldSource.lexically_normal() != NewSource.lexically_normal()) Out.Files.emplace_back(OldSource.lexically_normal(), NewSource.lexically_normal());
				if (NewFileName != Original)
				{
					Out.Apply = [Mesh, NewFileName] { Mesh->SourceFile = NewFileName; };
					Out.Rollback = [Mesh, Original] { Mesh->SourceFile = Original; };
				}
				return {};
			});
			return true;
		}();
		(void)RegisteredMoveContributor;
	}

	DStaticMesh::~DStaticMesh() = default;

	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetRenderData() -> FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::SetRenderData(std::unique_ptr<FStaticMeshRenderData> InRenderData) -> void
	{
		if (InRenderData != nullptr) InRenderData->RecalculateBounds();
		RenderData = std::move(InRenderData);
	}

	auto DStaticMesh::CreateDebugTriangle(DObject* Outer) -> DStaticMesh*
	{
		DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, "DebugStaticMesh");
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->Positions = {
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		};
		RenderData->Indices = {0, 1, 2};
		RenderData->Normals = {
			FVector3f(0.0f, 0.0f, 1.0f),
			FVector3f(0.0f, 0.0f, 1.0f),
			FVector3f(0.0f, 0.0f, 1.0f)
		};
		RenderData->IndexCount = static_cast<uint32>(RenderData->Indices.size());
		Mesh->SetRenderData(std::move(RenderData));
		return Mesh;
	}

	auto DStaticMesh::BuildRenderData(std::string_view FilePath, std::string& OutError) -> bool
	{
		std::vector<Asset::FTestAssetData> ImportedMeshes;
		if (!Asset::ImportFromFile(FilePath, ImportedMeshes))
		{
			OutError = std::format("Failed to import static mesh source file: {}", FilePath);
			return false;
		}

		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		for (const Asset::FTestAssetData& ImportedMesh : ImportedMeshes)
		{
			if (ImportedMesh.Positions.empty() || ImportedMesh.Indices.empty())
			{
				continue;
			}

			const uint32 BaseVertexIndex = static_cast<uint32>(RenderData->Positions.size());
			RenderData->Positions.insert(RenderData->Positions.end(), ImportedMesh.Positions.begin(), ImportedMesh.Positions.end());
			RenderData->Normals.insert(RenderData->Normals.end(), ImportedMesh.Normals.begin(), ImportedMesh.Normals.end());
			RenderData->Indices.reserve(RenderData->Indices.size() + ImportedMesh.Indices.size());
			for (uint32 Index : ImportedMesh.Indices)
			{
				RenderData->Indices.push_back(BaseVertexIndex + Index);
			}
		}

		if (RenderData->Positions.empty() || RenderData->Indices.empty())
		{
			OutError = std::format("Static mesh source has no renderable geometry: {}", FilePath);
			return false;
		}

		RenderData->RecalculateBounds();
		const FVector3f BoundsMin(RenderData->LocalBounds.Min);
		const FVector3f BoundsMax(RenderData->LocalBounds.Max);

		const FVector3f BoundsCenter = (BoundsMin + BoundsMax) * 0.5f;
		const FVector3f BoundsExtent = BoundsMax - BoundsMin;
		const float MaxDimension = std::max(BoundsExtent.x, std::max(BoundsExtent.y, BoundsExtent.z));
		if (MaxDimension <= 0.0f)
		{
			OutError = std::format("Static mesh source has invalid bounds: {}", FilePath);
			return false;
		}

		const float Scale = NormalizedSize / MaxDimension;
		for (FVector3f& Position : RenderData->Positions)
		{
			Position = (Position - BoundsCenter) * Scale;
		}
		RenderData->RecalculateBounds();

		RenderData->IndexCount = static_cast<uint32>(RenderData->Indices.size());
		if (RenderData->Normals.size() != RenderData->Positions.size())
		{
			RenderData->Normals.assign(RenderData->Positions.size(), FVector3f(0.0f));
			for (size_t Index = 0; Index + 2 < RenderData->Indices.size(); Index += 3)
			{
				const uint32 I0 = RenderData->Indices[Index];
				const uint32 I1 = RenderData->Indices[Index + 1];
				const uint32 I2 = RenderData->Indices[Index + 2];
				if (I0 >= RenderData->Positions.size() || I1 >= RenderData->Positions.size() || I2 >= RenderData->Positions.size()) continue;
				const FVector3f FaceNormal = glm::cross(RenderData->Positions[I1] - RenderData->Positions[I0], RenderData->Positions[I2] - RenderData->Positions[I0]);
				RenderData->Normals[I0] += FaceNormal;
				RenderData->Normals[I1] += FaceNormal;
				RenderData->Normals[I2] += FaceNormal;
			}
		}
		for (FVector3f& Normal : RenderData->Normals)
		{
			const float Length = glm::length(Normal);
			Normal = Length > 0.00001f ? Normal / Length : FVector3f(0.0f, 0.0f, 1.0f);
		}

		SetRenderData(std::move(RenderData));
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PostLoad(std::string& OutError) -> bool
	{
		if (SourceFile.empty())
		{
			OutError = "Static mesh asset has no source file.";
			return false;
		}
		const std::string PhysicalPath = ResolveStaticMeshSource(*this).generic_string();
		if (!std::filesystem::is_regular_file(PhysicalPath))
		{
			OutError = std::format("Static mesh source file does not exist: {}", SourceFile);
			return false;
		}
		return BuildRenderData(PhysicalPath, OutError);
	}

	auto DStaticMesh::ImportAsset(std::string_view FilePath, std::string_view AssetPath) -> FStaticMeshImportResult
	{
		const std::filesystem::path Input = std::filesystem::absolute(FilePath).lexically_normal();
		if (!std::filesystem::is_regular_file(Input)) return {false, "Source file does not exist.", nullptr};

		FAssetPath ParsedAssetPath;
		std::string PathError;
		if (!FAssetPath::TryCreate(AssetPath, ParsedAssetPath, &PathError)) return {false, std::move(PathError), nullptr};
		if (Asset::GetAssetRegistry().FindAsset(ParsedAssetPath) || Asset::FindLoadedPackage(ParsedAssetPath))
			return {false, std::format("Asset {} already exists.", ParsedAssetPath.ToString()), nullptr};

		const std::string Extension = Input.extension().generic_string();
		const std::string SourceFileName = std::string(ParsedAssetPath.GetAssetName()) + Extension;
		const std::filesystem::path Destination = std::filesystem::path(ResolveMountedFile(ParsedAssetPath.ToString())).replace_extension(Extension);
		if (std::filesystem::exists(Destination)) return {false, std::format("Imported source already exists: {}", Destination.generic_string()), nullptr};

		DStaticMesh* Mesh = nullptr;
		Asset::FAssetResult CreateResult = Asset::CreateAsset(ParsedAssetPath, Mesh);
		if (!CreateResult) return {false, CreateResult.Message, nullptr};
		std::string BuildError;
		if (!Mesh->BuildRenderData(Input.generic_string(), BuildError))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::move(BuildError), nullptr};
		}

		std::error_code Ec;
		std::filesystem::create_directories(Destination.parent_path(), Ec);
		if (Ec || !std::filesystem::copy_file(Input, Destination, std::filesystem::copy_options::none, Ec))
		{
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, std::format("Failed to copy source file to {}: {}", Destination.generic_string(), Ec.message()), nullptr};
		}
		Mesh->SourceFile = SourceFileName;
		Asset::FAssetResult SaveResult = Asset::SavePackage(Mesh->GetPackage());
		if (!SaveResult)
		{
			std::filesystem::remove(Destination, Ec);
			Asset::UnloadPackage(ParsedAssetPath);
			return {false, SaveResult.Message, nullptr};
		}
		return {true, {}, Mesh};
	}

	auto FStaticMeshRenderData::InitResources(FRHICommandListImmediate& RHICmdList) -> void
	{
		if (PositionVertexBufferRHI == nullptr && !Positions.empty())
		{
			FRHIBufferCreateDesc VertexBufferDesc = FRHIBufferCreateDesc::CreateVertex(
				"StaticMeshPositionVertexBuffer",
				static_cast<uint32>(Positions.size() * sizeof(FVector3f))
			);
			VertexBufferDesc.Usage |= EBufferUsageFlags::Static;
			VertexBufferDesc.InitialData.Data = Positions.data();
			VertexBufferDesc.InitialData.Size = static_cast<uint32>(Positions.size() * sizeof(FVector3f));
			PositionVertexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, VertexBufferDesc);
		}

		if (IndexBufferRHI == nullptr && !Indices.empty())
		{
			FRHIBufferCreateDesc IndexBufferDesc = FRHIBufferCreateDesc::CreateIndex(
				"StaticMeshIndexBuffer",
				static_cast<uint32>(Indices.size() * sizeof(uint32)),
				sizeof(uint32)
			);
			IndexBufferDesc.Usage |= EBufferUsageFlags::Static;
			IndexBufferDesc.InitialData.Data = Indices.data();
			IndexBufferDesc.InitialData.Size = static_cast<uint32>(Indices.size() * sizeof(uint32));
			IndexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, IndexBufferDesc);
		}

		if (NormalVertexBufferRHI == nullptr && Normals.size() == Positions.size())
		{
			FRHIBufferCreateDesc NormalBufferDesc = FRHIBufferCreateDesc::CreateVertex(
				"StaticMeshNormalVertexBuffer", static_cast<uint32>(Normals.size() * sizeof(FVector3f)));
			NormalBufferDesc.Usage |= EBufferUsageFlags::Static;
			NormalBufferDesc.InitialData.Data = Normals.data();
			NormalBufferDesc.InitialData.Size = static_cast<uint32>(Normals.size() * sizeof(FVector3f));
			NormalVertexBufferRHI = GDynamicRHI->RHICreateBuffer(RHICmdList, NormalBufferDesc);
		}

		IndexCount = static_cast<uint32>(Indices.size());
	}

	auto FStaticMeshRenderData::ReleaseResources() -> void
	{
		PositionVertexBufferRHI = nullptr;
		NormalVertexBufferRHI = nullptr;
		IndexBufferRHI = nullptr;
	}

	auto FStaticMeshRenderData::IsReadyForRendering() const -> bool
	{
		return PositionVertexBufferRHI != nullptr && NormalVertexBufferRHI != nullptr && IndexBufferRHI != nullptr && IndexCount > 0;
	}

	auto FStaticMeshRenderData::RecalculateBounds() -> void
	{
		LocalBounds.Reset();
		for (const FVector3f& Position : Positions)
		{
			LocalBounds.AddPoint(FVector3(Position));
		}
	}
}
