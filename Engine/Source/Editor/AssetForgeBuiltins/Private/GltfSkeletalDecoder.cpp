#include "GltfSkeletalDecoder.h"

#include "Math/Operations.h"

namespace Durin::AssetForge::Builtins::Private
{
	using namespace Durin;
	namespace
	{
		constexpr uint32 GltfUnsignedByte = 5121;
		constexpr uint32 GltfUnsignedShort = 5123;
		constexpr uint32 GltfUnsignedInt = 5125;
		constexpr uint32 GltfFloat = 5126;
		constexpr uint32 GltfTriangles = 4;
		constexpr double MatrixTolerance = 1.0e-8;

		struct FDecodedAccessor
		{
			std::span<const std::byte> Bytes;
			uint64 Count = 0;
			uint64 Offset = 0;
			uint64 Stride = 0;
			uint32 ComponentType = 0;
			uint32 ComponentCount = 0;
			uint32 ComponentSize = 0;
			bool bNormalized = false;
		};

		struct FSourceNode
		{
			std::string Name;
			int32 Parent = -1;
			std::vector<uint32> Children;
			FMatrix Local{1.0};
			FMatrix Global{1.0};
			std::optional<uint32> Mesh;
			std::optional<uint32> Skin;
			uint32 TraversalBegin = 0;
			uint32 TraversalEnd = 0;
			bool bReachable = false;
		};

		struct FSourceSkin
		{
			std::string Name;
			std::optional<uint32> SkeletonNode;
			std::vector<uint32> Joints;
			std::optional<uint32> InverseBindAccessor;
			std::unordered_map<uint32, uint16> BoneBySourceNode;
			uint32 OutputIndex = 0;
		};

		auto IsFinite(const FMatrix& Matrix) -> bool
		{
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					if (!std::isfinite(Matrix[Column][Row])) return false;
			return true;
		}

		auto CleanNumber(double Value) -> float
		{
			if (std::abs(Value) < 1.0e-7) Value = 0.0;
			Value = std::round(Value * 10000000.0) / 10000000.0;
			float Result = static_cast<float>(Value);
			if (Result == 0.0f) Result = 0.0f;
			return Result;
		}

		auto SourceToDurinMatrix() -> FMatrix
		{
			FMatrix Result(0.0);
			Result[2][0] = -1.0;
			Result[0][1] = 1.0;
			Result[1][2] = 1.0;
			Result[3][3] = 1.0;
			return Result;
		}

		auto ConvertMatrix(const FMatrix& Source) -> FMatrix4f
		{
			const FMatrix Conversion = SourceToDurinMatrix();
			const FMatrix Converted = Conversion * Source * Math::Inverse(Conversion);
			FMatrix4f Result(0.0f);
			for (uint32 Column = 0; Column < 4; ++Column)
				for (uint32 Row = 0; Row < 4; ++Row)
					Result[Column][Row] = CleanNumber(Converted[Column][Row]);
			return Result;
		}

		auto NormalizeOr(const FVector3f& Value, const FVector3f& Fallback) -> FVector3f
		{
			const float LengthSquared = Math::Dot(Value, Value);
			if (!std::isfinite(LengthSquared) || LengthSquared <= 1.0e-20f) return Fallback;
			return Value / std::sqrt(LengthSquared);
		}

		auto MakePerpendicularTangent(const FVector3f& Normal) -> FVector3f
		{
			const FVector3f Axis = std::abs(Normal.z) < 0.999f
				? FVector3f(0.0f, 0.0f, 1.0f)
				: FVector3f(0.0f, 1.0f, 0.0f);
			return NormalizeOr(Math::Cross(Axis, Normal), FVector3f(1.0f, 0.0f, 0.0f));
		}

		auto GenerateNormals(
			std::span<const FVector3f> Positions,
			std::span<const uint32> Indices,
			uint32 VertexBase,
			std::span<FVector3f> OutNormals) -> void
		{
			std::vector<FVector3f> Accumulated(Positions.size(), FVector3f(0.0f));
			for (size_t Index = 0; Index < Indices.size(); Index += 3)
			{
				const uint32 A = Indices[Index] - VertexBase;
				const uint32 B = Indices[Index + 1] - VertexBase;
				const uint32 C = Indices[Index + 2] - VertexBase;
				const FVector3f Face = Math::Cross(Positions[B] - Positions[A], Positions[C] - Positions[A]);
				if (!std::isfinite(Math::Dot(Face, Face))) continue;
				Accumulated[A] += Face;
				Accumulated[B] += Face;
				Accumulated[C] += Face;
			}
			for (size_t Vertex = 0; Vertex < Positions.size(); ++Vertex)
			{
				const FVector3f Normal = NormalizeOr(Accumulated[Vertex], FVector3f(0.0f, 0.0f, 1.0f));
				OutNormals[Vertex] = FVector3f(
					CleanNumber(Normal.x), CleanNumber(Normal.y), CleanNumber(Normal.z));
			}
		}

		auto GenerateTangents(
			std::span<const FVector3f> Positions,
			std::span<const FVector3f> Normals,
			std::span<const FVector2f> UVs,
			std::span<const uint32> Indices,
			uint32 VertexBase,
			std::span<FVector4f> OutTangents) -> void
		{
			std::vector<FVector3f> AccumulatedTangents(Positions.size(), FVector3f(0.0f));
			std::vector<FVector3f> AccumulatedBitangents(Positions.size(), FVector3f(0.0f));
			if (UVs.size() == Positions.size())
			{
				for (size_t Index = 0; Index < Indices.size(); Index += 3)
				{
					const uint32 A = Indices[Index] - VertexBase;
					const uint32 B = Indices[Index + 1] - VertexBase;
					const uint32 C = Indices[Index + 2] - VertexBase;
					const FVector3f Edge1 = Positions[B] - Positions[A];
					const FVector3f Edge2 = Positions[C] - Positions[A];
					const FVector2f Delta1 = UVs[B] - UVs[A];
					const FVector2f Delta2 = UVs[C] - UVs[A];
					const float Determinant = Delta1.x * Delta2.y - Delta1.y * Delta2.x;
					if (!std::isfinite(Determinant) || std::abs(Determinant) <= 1.0e-20f) continue;
					const float Reciprocal = 1.0f / Determinant;
					const FVector3f Tangent = (Edge1 * Delta2.y - Edge2 * Delta1.y) * Reciprocal;
					const FVector3f Bitangent = (Edge2 * Delta1.x - Edge1 * Delta2.x) * Reciprocal;
					for (uint32 Vertex : {A, B, C})
					{
						AccumulatedTangents[Vertex] += Tangent;
						AccumulatedBitangents[Vertex] += Bitangent;
					}
				}
			}
			for (size_t Vertex = 0; Vertex < Positions.size(); ++Vertex)
			{
				const FVector3f Normal = NormalizeOr(Normals[Vertex], FVector3f(0.0f, 0.0f, 1.0f));
				const FVector3f Orthogonal = AccumulatedTangents[Vertex]
					- Normal * Math::Dot(Normal, AccumulatedTangents[Vertex]);
				const FVector3f Tangent = NormalizeOr(Orthogonal, MakePerpendicularTangent(Normal));
				const float Handedness = Math::Dot(Math::Cross(Normal, Tangent), AccumulatedBitangents[Vertex]) < 0.0f
					? -1.0f : 1.0f;
				OutTangents[Vertex] = FVector4f(
					CleanNumber(Tangent.x), CleanNumber(Tangent.y), CleanNumber(Tangent.z), Handedness);
			}
		}

		auto ToSkeletonTransform(const FMatrix4f& Matrix) -> FSkeletonTransform
		{
			return {
				.Row0 = FVector4(Matrix[0][0], Matrix[1][0], Matrix[2][0], Matrix[3][0]),
				.Row1 = FVector4(Matrix[0][1], Matrix[1][1], Matrix[2][1], Matrix[3][1]),
				.Row2 = FVector4(Matrix[0][2], Matrix[1][2], Matrix[2][2], Matrix[3][2]),
				.Row3 = FVector4(Matrix[0][3], Matrix[1][3], Matrix[2][3], Matrix[3][3])};
		}

		auto ReadIndex(FJsonNodeView Node, uint32& OutValue) -> bool
		{
			uint64 Value = 0;
			if (!Node.GetValue(Value) || Value > std::numeric_limits<uint32>::max()) return false;
			OutValue = static_cast<uint32>(Value);
			return true;
		}

		auto ReadOptionalIndex(
			FJsonNodeView Object,
			std::string_view Key,
			std::optional<uint32>& OutValue) -> bool
		{
			const FJsonNodeView Node = Object.GetView(Key);
			if (!Node.IsValid())
			{
				OutValue.reset();
				return true;
			}
			uint32 Value = 0;
			if (!ReadIndex(Node, Value)) return false;
			OutValue = Value;
			return true;
		}

		auto ReadFiniteArray(
			FJsonNodeView Node,
			uint32 Count,
			std::span<double> OutValues) -> bool
		{
			if (!Node.IsArray() || Node.Num() != Count || OutValues.size() != Count) return false;
			for (uint32 Index = 0; Index < Count; ++Index)
			{
				const FJsonNodeView Value = Node.GetView(Index);
				if (!Value.IsNumber()) return false;
				OutValues[Index] = Value.GetDouble();
				if (!std::isfinite(OutValues[Index])) return false;
			}
			return true;
		}

		auto MakeTrsMatrix(FJsonNodeView Node, FMatrix& OutMatrix) -> bool
		{
			const bool bHasMatrix = Node.GetView("matrix").IsValid();
			const bool bHasTrs = Node.GetView("translation").IsValid()
				|| Node.GetView("rotation").IsValid() || Node.GetView("scale").IsValid();
			if (bHasMatrix && bHasTrs) return false;
			if (bHasMatrix)
			{
				std::array<double, 16> Values{};
				if (!ReadFiniteArray(Node.GetView("matrix"), 16, Values)) return false;
				for (uint32 Column = 0; Column < 4; ++Column)
					for (uint32 Row = 0; Row < 4; ++Row)
						OutMatrix[Column][Row] = Values[Column * 4 + Row];
			}
			else
			{
				std::array<double, 3> Translation{0.0, 0.0, 0.0};
				std::array<double, 4> Rotation{0.0, 0.0, 0.0, 1.0};
				std::array<double, 3> Scale{1.0, 1.0, 1.0};
				if ((Node.GetView("translation").IsValid()
						&& !ReadFiniteArray(Node.GetView("translation"), 3, Translation))
					|| (Node.GetView("rotation").IsValid()
						&& !ReadFiniteArray(Node.GetView("rotation"), 4, Rotation))
					|| (Node.GetView("scale").IsValid()
						&& !ReadFiniteArray(Node.GetView("scale"), 3, Scale))) return false;
				const double Length = std::sqrt(
					Rotation[0] * Rotation[0] + Rotation[1] * Rotation[1]
					+ Rotation[2] * Rotation[2] + Rotation[3] * Rotation[3]);
				if (!std::isfinite(Length) || Length <= MatrixTolerance
					|| std::ranges::any_of(Scale, [](double Value) {
						return std::abs(Value) <= MatrixTolerance;
					})) return false;
				const double X = Rotation[0] / Length;
				const double Y = Rotation[1] / Length;
				const double Z = Rotation[2] / Length;
				const double W = Rotation[3] / Length;
				OutMatrix = FMatrix(1.0);
				OutMatrix[0][0] = (1.0 - 2.0 * (Y * Y + Z * Z)) * Scale[0];
				OutMatrix[0][1] = (2.0 * (X * Y + Z * W)) * Scale[0];
				OutMatrix[0][2] = (2.0 * (X * Z - Y * W)) * Scale[0];
				OutMatrix[1][0] = (2.0 * (X * Y - Z * W)) * Scale[1];
				OutMatrix[1][1] = (1.0 - 2.0 * (X * X + Z * Z)) * Scale[1];
				OutMatrix[1][2] = (2.0 * (Y * Z + X * W)) * Scale[1];
				OutMatrix[2][0] = (2.0 * (X * Z + Y * W)) * Scale[2];
				OutMatrix[2][1] = (2.0 * (Y * Z - X * W)) * Scale[2];
				OutMatrix[2][2] = (1.0 - 2.0 * (X * X + Y * Y)) * Scale[2];
				OutMatrix[3][0] = Translation[0];
				OutMatrix[3][1] = Translation[1];
				OutMatrix[3][2] = Translation[2];
			}
			if (!IsFinite(OutMatrix)
				|| std::abs(OutMatrix[0][3]) > MatrixTolerance
				|| std::abs(OutMatrix[1][3]) > MatrixTolerance
				|| std::abs(OutMatrix[2][3]) > MatrixTolerance
				|| std::abs(OutMatrix[3][3] - 1.0) > MatrixTolerance) return false;
			const FVector3d C0(OutMatrix[0]);
			const FVector3d C1(OutMatrix[1]);
			const FVector3d C2(OutMatrix[2]);
			const double L0 = Math::Length(C0);
			const double L1 = Math::Length(C1);
			const double L2 = Math::Length(C2);
			return L0 > MatrixTolerance && L1 > MatrixTolerance && L2 > MatrixTolerance
				&& std::abs(Math::Dot(C0 / L0, C1 / L1)) <= 1.0e-5
				&& std::abs(Math::Dot(C0 / L0, C2 / L2)) <= 1.0e-5
				&& std::abs(Math::Dot(C1 / L1, C2 / L2)) <= 1.0e-5;
		}

		class FGltfSkeletalDecoder
		{
		public:
			FGltfSkeletalDecoder(
				FJsonNodeView InRoot,
				const std::vector<FByteArray>& InBuffers,
				FSceneDecodeResult& InResult)
				: Root(InRoot), Buffers(InBuffers), Result(InResult) {}

			auto Decode() -> bool
			{
				const FJsonNodeView SkinsNode = Root.GetView("skins");
				const FJsonNodeView AnimationsNode = Root.GetView("animations");
				if (!SkinsNode.IsValid() && !AnimationsNode.IsValid()) return true;
				if (!SkinsNode.IsArray() || SkinsNode.Num() == 0)
					return Unsupported("skins", "Skeletal glTF requires a non-empty skins array.");
				if (!ParseNodes() || !ParseSkins() || !ParseMeshes() || !ParseAnimations()) return false;
				Result.Scene.Nodes = std::move(ImportedNodes);
				Result.Scene.Skeletons = std::move(ImportedSkeletons);
				Result.Scene.SkeletalMeshes = std::move(ImportedMeshes);
				Result.Scene.AnimationClips = std::move(ImportedClips);
				return true;
			}

		private:
			auto Malformed(std::string Subject, std::string Message) -> bool
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::MalformedSource,
					std::move(Subject), std::move(Message));
			}

			auto Unsupported(std::string Subject, std::string Message) -> bool
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::UnsupportedFeature,
					std::move(Subject), std::move(Message));
			}

			auto Limit(std::string Subject, std::string Message) -> bool
			{
				return FailImport(Result, ESceneImportDiagnosticCategory::ResourceLimitExceeded,
					std::move(Subject), std::move(Message));
			}

			auto Poll(std::string_view Subject) -> bool
			{
				return CheckSceneDecodeCancellation(Result, Subject);
			}

			auto AddDecodedBytes(uint64 Count, uint64 Size, std::string_view Subject) -> bool
			{
				if (Size != 0 && Count > MaxImportedSkeletalDecodedBytes / Size)
					return Limit(std::string(Subject), "Decoded skeletal data exceeds the scene byte limit.");
				const uint64 Bytes = Count * Size;
				if (DecodedBytes > MaxImportedSkeletalDecodedBytes - Bytes)
					return Limit(std::string(Subject), "Decoded skeletal data exceeds the scene byte limit.");
				DecodedBytes += Bytes;
				return true;
			}

			auto ParseNodes() -> bool
			{
				const FJsonNodeView NodeArray = Root.GetView("nodes");
				if (!NodeArray.IsArray()) return Malformed("nodes", "Skeletal glTF nodes must be an array.");
				if (NodeArray.Num() > MaxImportedSceneNodes)
					return Limit("nodes", "Source node count exceeds the supported limit.");
				Nodes.resize(NodeArray.Num());
				for (size_t Index = 0; Index < NodeArray.Num(); ++Index)
				{
					if (Poll("nodes")) return false;
					const FJsonNodeView Node = NodeArray.GetView(Index);
					if (!Node.IsObject() || !MakeTrsMatrix(Node, Nodes[Index].Local))
						return Malformed(std::format("node:{}", Index), "glTF node transform is invalid or not decomposable TRS.");
					Nodes[Index].Name = Node.GetView("name").GetString();
					if (!ReadOptionalIndex(Node, "mesh", Nodes[Index].Mesh)
						|| !ReadOptionalIndex(Node, "skin", Nodes[Index].Skin))
						return Malformed(std::format("node:{}", Index), "glTF node mesh or skin index is invalid.");
					const FJsonNodeView Children = Node.GetView("children");
					if (Children.IsValid() && !Children.IsArray())
						return Malformed(std::format("node:{}", Index), "glTF node children must be an array.");
					std::unordered_set<uint32> UniqueChildren;
					for (size_t ChildOffset = 0; ChildOffset < Children.Num(); ++ChildOffset)
					{
						uint32 Child = 0;
						if (!ReadIndex(Children.GetView(ChildOffset), Child) || Child >= Nodes.size()
							|| Child == Index || !UniqueChildren.insert(Child).second)
							return Malformed(std::format("node:{}", Index), "glTF node contains an invalid child reference.");
						if (Nodes[Child].Parent != -1)
							return Malformed(std::format("node:{}", Child), "glTF node has more than one parent.");
						Nodes[Child].Parent = static_cast<int32>(Index);
						Nodes[Index].Children.push_back(Child);
					}
				}

				struct FTraversalStep
				{
					uint32 Node = 0;
					bool bExit = false;
				};
				std::vector<FTraversalStep> Pending;
				Pending.reserve(Nodes.size());
				for (uint32 Index = 0; Index < Nodes.size(); ++Index)
					if (Nodes[Index].Parent == -1) Pending.push_back({.Node = Index});
				std::ranges::reverse(Pending);
				uint32 TraversalIndex = 0;
				while (!Pending.empty())
				{
					const FTraversalStep Step = Pending.back();
					Pending.pop_back();
					if (Step.bExit)
					{
						Nodes[Step.Node].TraversalEnd = TraversalIndex;
						continue;
					}
					if ((TraversalIndex & 0xfffu) == 0 && Poll("node-hierarchy")) return false;
					const uint32 Index = Step.Node;
					Nodes[Index].TraversalBegin = TraversalIndex++;
					Nodes[Index].Global = Nodes[Index].Parent >= 0
						? Nodes[Nodes[Index].Parent].Global * Nodes[Index].Local
						: Nodes[Index].Local;
					if (!IsFinite(Nodes[Index].Global))
						return Malformed(std::format("node:{}", Index), "glTF node global transform is non-finite.");
					Pending.push_back({.Node = Index, .bExit = true});
					for (auto It = Nodes[Index].Children.rbegin(); It != Nodes[Index].Children.rend(); ++It)
						Pending.push_back({.Node = *It});
				}
				if (TraversalIndex != Nodes.size())
					return Malformed("nodes", "glTF node hierarchy contains a parent cycle.");

				const FJsonNodeView Scenes = Root.GetView("scenes");
				if (!Scenes.IsArray() || Scenes.Num() == 0)
					return Malformed("scenes", "Skeletal glTF scenes must be a non-empty array.");
				const uint64 SceneIndex = Root.GetView("scene").GetUInt(0);
				if (SceneIndex >= Scenes.Num()) return Malformed("scene", "glTF default scene index is invalid.");
				const FJsonNodeView SceneRoots = Scenes.GetView(SceneIndex).GetView("nodes");
				if (!SceneRoots.IsArray()) return Malformed("scene", "glTF default scene roots must be an array.");
				std::vector<uint32> Reachable;
				for (size_t Offset = 0; Offset < SceneRoots.Num(); ++Offset)
				{
					uint32 Index = 0;
					if (!ReadIndex(SceneRoots.GetView(Offset), Index) || Index >= Nodes.size())
						return Malformed("scene", "glTF default scene contains an invalid root node.");
					Reachable.push_back(Index);
				}
				for (size_t Offset = 0; Offset < Reachable.size(); ++Offset)
				{
					if ((Offset & 0xfffu) == 0 && Poll("scene-nodes")) return false;
					FSourceNode& Node = Nodes[Reachable[Offset]];
					if (Node.bReachable) continue;
					Node.bReachable = true;
					Reachable.insert(Reachable.end(), Node.Children.begin(), Node.Children.end());
				}
				ImportedNodes.reserve(Nodes.size());
				for (uint32 Index = 0; Index < Nodes.size(); ++Index)
				{
					ImportedNodes.push_back({
						.SourceNodeIndex = Index,
						.ParentNodeIndex = Nodes[Index].Parent,
						.SourceName = Nodes[Index].Name,
						.LocalTransform = ConvertMatrix(Nodes[Index].Local),
						.GlobalTransform = ConvertMatrix(Nodes[Index].Global),
						.MeshIndex = Nodes[Index].Mesh,
						.SkinIndex = Nodes[Index].Skin});
				}
				return AddDecodedBytes(Nodes.size(), sizeof(FImportedSceneNode), "nodes");
			}

			auto IsDescendantOf(uint32 Node, uint32 Ancestor) const -> bool
			{
				return Nodes[Ancestor].TraversalBegin <= Nodes[Node].TraversalBegin
					&& Nodes[Node].TraversalBegin < Nodes[Ancestor].TraversalEnd;
			}

			auto MakeCanonicalBoneName(
				uint32 SourceNode,
				std::unordered_set<std::string>& Used) const -> std::string
			{
				std::string Base = Nodes[SourceNode].Name;
				if (Base.empty()) Base = std::format("Bone_{}", SourceNode);
				if (Base == "$DurinRoot") Base = "$DurinRoot_1";
				std::string Candidate = Base;
				for (uint32 Suffix = 1; !Used.insert(Candidate).second; ++Suffix)
					Candidate = std::format("{}_{}", Base, Suffix);
				return Candidate;
			}

			auto ParseSkins() -> bool
			{
				const FJsonNodeView SkinArray = Root.GetView("skins");
				if (SkinArray.Num() > MaxImportedSkins)
					return Limit("skins", "Source skin count exceeds the supported limit.");
				Skins.resize(SkinArray.Num());
				ImportedSkeletons.reserve(SkinArray.Num());
				for (uint32 SkinIndex = 0; SkinIndex < SkinArray.Num(); ++SkinIndex)
				{
					if (Poll("skins")) return false;
					const FJsonNodeView SkinNode = SkinArray.GetView(SkinIndex);
					const FJsonNodeView JointArray = SkinNode.GetView("joints");
					if (!SkinNode.IsObject() || !JointArray.IsArray() || JointArray.Num() == 0)
						return Malformed(std::format("skin:{}", SkinIndex), "glTF skin requires a non-empty joint array.");
					if (JointArray.Num() > MaximumSkeletonBones)
						return Limit(std::format("skin:{}", SkinIndex), "Skeleton bone count exceeds the supported limit.");
					FSourceSkin& Skin = Skins[SkinIndex];
					Skin.Name = SkinNode.GetView("name").GetString();
					if (!ReadOptionalIndex(SkinNode, "skeleton", Skin.SkeletonNode)
						|| !ReadOptionalIndex(SkinNode, "inverseBindMatrices", Skin.InverseBindAccessor))
						return Malformed(std::format("skin:{}", SkinIndex), "glTF skin skeleton or inverse-bind reference is invalid.");
					if (Skin.SkeletonNode && *Skin.SkeletonNode >= Nodes.size())
						return Malformed(std::format("skin:{}", SkinIndex), "glTF skin skeleton node is out of range.");
					std::unordered_set<uint32> UniqueJoints;
					for (size_t JointOffset = 0; JointOffset < JointArray.Num(); ++JointOffset)
					{
						uint32 Joint = 0;
						if (!ReadIndex(JointArray.GetView(JointOffset), Joint) || Joint >= Nodes.size()
							|| !UniqueJoints.insert(Joint).second || !Nodes[Joint].bReachable
							|| (Skin.SkeletonNode && !IsDescendantOf(Joint, *Skin.SkeletonNode)))
							return Malformed(std::format("skin:{}", SkinIndex), "glTF skin contains an invalid, duplicate, or disconnected joint.");
						Skin.Joints.push_back(Joint);
					}
					std::unordered_set<uint32> JointSet(Skin.Joints.begin(), Skin.Joints.end());
					std::unordered_map<uint32, int32> ParentJoint;
					std::map<int32, std::vector<uint32>> ChildrenByJoint;
					for (uint32 Joint : Skin.Joints)
					{
						int32 Parent = Nodes[Joint].Parent;
						while (Parent >= 0 && !JointSet.contains(static_cast<uint32>(Parent)))
							Parent = Nodes[Parent].Parent;
						ParentJoint[Joint] = Parent;
						ChildrenByJoint[Parent].push_back(Joint);
					}
					for (auto& [_, Children] : ChildrenByJoint) std::ranges::sort(Children);
					std::vector<uint32> CanonicalOrder;
					std::vector<uint32> Pending;
					for (auto It = ChildrenByJoint[-1].rbegin(); It != ChildrenByJoint[-1].rend(); ++It)
						Pending.push_back(*It);
					while (!Pending.empty())
					{
						if ((CanonicalOrder.size() & 0xfffu) == 0 && Poll("skin-hierarchy")) return false;
						const uint32 Joint = Pending.back();
						Pending.pop_back();
						CanonicalOrder.push_back(Joint);
						const std::vector<uint32>& Children = ChildrenByJoint[static_cast<int32>(Joint)];
						for (auto It = Children.rbegin(); It != Children.rend(); ++It) Pending.push_back(*It);
					}
					if (CanonicalOrder.size() != Skin.Joints.size())
						return Malformed(std::format("skin:{}", SkinIndex), "glTF skin joint hierarchy is cyclic or disconnected.");
					const bool bSyntheticRoot = ChildrenByJoint[-1].size() > 1;
					if (CanonicalOrder.size() + (bSyntheticRoot ? 1u : 0u) > MaximumSkeletonBones)
						return Limit(std::format("skin:{}", SkinIndex), "Canonical Skeleton exceeds the bone limit.");

					FImportedSkeletonData Imported{
						.StableIdentity = std::format("skeleton:skin/{}", SkinIndex),
						.SuggestedName = Skin.Name.empty() ? std::format("Skeleton_{}", SkinIndex) : Skin.Name,
						.SourceSkinIndex = SkinIndex};
					std::unordered_set<std::string> UsedNames;
					if (bSyntheticRoot)
					{
						UsedNames.insert("$DurinRoot");
						Imported.Bones.push_back({
							.Name = FName("$DurinRoot"),
							.ParentIndex = -1,
							.ReferenceTransform = {}});
					}
					for (uint32 Joint : CanonicalOrder)
					{
						const int32 ParentSource = ParentJoint[Joint];
						const FMatrix Local = ParentSource >= 0
							? Math::Inverse(Nodes[ParentSource].Global) * Nodes[Joint].Global
							: Nodes[Joint].Global;
						if (!IsFinite(Local))
							return Malformed(std::format("skin:{}", SkinIndex), "Skeleton local reference transform is non-finite.");
						const int32 ParentBone = ParentSource >= 0
							? static_cast<int32>(Skin.BoneBySourceNode[static_cast<uint32>(ParentSource)])
							: bSyntheticRoot ? 0 : -1;
						const uint16 BoneIndex = static_cast<uint16>(Imported.Bones.size());
						Skin.BoneBySourceNode[Joint] = BoneIndex;
						const std::string CanonicalBoneName = MakeCanonicalBoneName(Joint, UsedNames);
						Imported.Bones.push_back({
							.Name = FName(CanonicalBoneName),
							.ParentIndex = ParentBone,
							.ReferenceTransform = ToSkeletonTransform(ConvertMatrix(Local))});
					}
					std::string Error;
					if (!DSkeleton::ComputeCompatibilityIdentity(
						Imported.Bones, Imported.CompatibilityIdentity, Error))
						return Malformed(std::format("skin:{}", SkinIndex), std::format("Canonical Skeleton is invalid: {}", Error));
					Skin.OutputIndex = static_cast<uint32>(ImportedSkeletons.size());
					ImportedSkeletons.push_back(std::move(Imported));
				}
				return AddDecodedBytes(ImportedSkeletons.size(), sizeof(FImportedSkeletonData), "skins");
			}

			auto ComponentSize(uint32 ComponentType) const -> uint32
			{
				switch (ComponentType)
				{
				case GltfUnsignedByte: return 1;
				case GltfUnsignedShort: return 2;
				case GltfUnsignedInt:
				case GltfFloat: return 4;
				default: return 0;
				}
			}

			auto TypeComponents(std::string_view Type) const -> uint32
			{
				if (Type == "SCALAR") return 1;
				if (Type == "VEC2") return 2;
				if (Type == "VEC3") return 3;
				if (Type == "VEC4") return 4;
				if (Type == "MAT4") return 16;
				return 0;
			}

			auto GetAccessor(
				uint32 AccessorIndex,
				std::string_view ExpectedType,
				uint64 CountLimit,
				std::string_view Subject,
				FDecodedAccessor& Out) -> bool
			{
				const FJsonNodeView Accessors = Root.GetView("accessors");
				const FJsonNodeView Views = Root.GetView("bufferViews");
				if (!Accessors.IsArray() || AccessorIndex >= Accessors.Num())
					return Malformed(std::string(Subject), "glTF accessor reference is out of range.");
				const FJsonNodeView Accessor = Accessors.GetView(AccessorIndex);
				if (!Accessor.IsObject()) return Malformed(std::string(Subject), "glTF accessor must be an object.");
				if (Accessor.GetView("sparse").IsValid())
					return Unsupported(std::string(Subject), "Sparse glTF accessors are unsupported.");
				uint32 ViewIndex = 0;
				uint64 Count = 0;
				uint64 ComponentType64 = 0;
				if (!ReadIndex(Accessor.GetView("bufferView"), ViewIndex)
					|| !Accessor.GetView("count").GetValue(Count)
					|| !Accessor.GetView("componentType").GetValue(ComponentType64)
					|| ComponentType64 > std::numeric_limits<uint32>::max()
					|| !Accessor.GetView("type").IsString())
					return Malformed(std::string(Subject), "glTF accessor metadata is invalid.");
				if (Count == 0) return Malformed(std::string(Subject), "glTF accessor count must be non-zero.");
				if (Count > CountLimit) return Limit(std::string(Subject), "glTF accessor count exceeds the supported limit.");
				const uint32 ComponentType = static_cast<uint32>(ComponentType64);
				const uint32 Size = ComponentSize(ComponentType);
				const std::string Type = Accessor.GetView("type").GetString();
				const uint32 Components = TypeComponents(Type);
				if (Size == 0 || Components == 0 || Type != ExpectedType)
					return Unsupported(std::string(Subject), "glTF accessor component or vector type is unsupported.");
				if (!Views.IsArray() || ViewIndex >= Views.Num())
					return Malformed(std::string(Subject), "glTF accessor buffer view is out of range.");
				const FJsonNodeView View = Views.GetView(ViewIndex);
				uint32 BufferIndex = 0;
				uint64 ViewOffset = 0;
				uint64 ViewLength = 0;
				uint64 AccessorOffset = 0;
				uint64 Stride = static_cast<uint64>(Size) * Components;
				if (!View.IsObject() || !ReadIndex(View.GetView("buffer"), BufferIndex)
					|| !View.GetView("byteLength").GetValue(ViewLength))
					return Malformed(std::string(Subject), "glTF buffer-view metadata is invalid.");
				if (View.GetView("byteOffset").IsValid() && !View.GetView("byteOffset").GetValue(ViewOffset))
					return Malformed(std::string(Subject), "glTF buffer-view offset is invalid.");
				if (Accessor.GetView("byteOffset").IsValid()
					&& !Accessor.GetView("byteOffset").GetValue(AccessorOffset))
					return Malformed(std::string(Subject), "glTF accessor offset is invalid.");
				if (View.GetView("byteStride").IsValid() && !View.GetView("byteStride").GetValue(Stride))
					return Malformed(std::string(Subject), "glTF buffer-view stride is invalid.");
				const uint64 ElementSize = static_cast<uint64>(Size) * Components;
				if (BufferIndex >= Buffers.size() || Stride < ElementSize || Stride % Size != 0
					|| AccessorOffset % Size != 0 || ViewOffset % Size != 0
					|| ViewOffset > Buffers[BufferIndex].size()
					|| ViewLength > Buffers[BufferIndex].size() - ViewOffset
					|| AccessorOffset > ViewLength
					|| Count - 1 > (std::numeric_limits<uint64>::max() - ElementSize) / Stride
					|| (Count - 1) * Stride + ElementSize > ViewLength - AccessorOffset)
					return Malformed(std::string(Subject), "glTF accessor byte range, alignment, or stride is invalid.");
				Out = {
					.Bytes = Buffers[BufferIndex],
					.Count = Count,
					.Offset = ViewOffset + AccessorOffset,
					.Stride = Stride,
					.ComponentType = ComponentType,
					.ComponentCount = Components,
					.ComponentSize = Size,
					.bNormalized = Accessor.GetView("normalized").GetBool(false)};
				return true;
			}

			auto ReadUnsigned(const FDecodedAccessor& Accessor, uint64 Element, uint32 Component) const -> uint32
			{
				const size_t Offset = static_cast<size_t>(Accessor.Offset + Element * Accessor.Stride
					+ static_cast<uint64>(Component) * Accessor.ComponentSize);
				if (Accessor.ComponentType == GltfUnsignedByte) return std::to_integer<uint32>(Accessor.Bytes[Offset]);
				if (Accessor.ComponentType == GltfUnsignedShort)
					return std::to_integer<uint32>(Accessor.Bytes[Offset])
						| (std::to_integer<uint32>(Accessor.Bytes[Offset + 1]) << 8);
				return std::to_integer<uint32>(Accessor.Bytes[Offset])
					| (std::to_integer<uint32>(Accessor.Bytes[Offset + 1]) << 8)
					| (std::to_integer<uint32>(Accessor.Bytes[Offset + 2]) << 16)
					| (std::to_integer<uint32>(Accessor.Bytes[Offset + 3]) << 24);
			}

			auto ReadComponent(const FDecodedAccessor& Accessor, uint64 Element, uint32 Component) const -> double
			{
				if (Accessor.ComponentType == GltfFloat)
					return std::bit_cast<float>(ReadUnsigned(Accessor, Element, Component));
				const uint32 Value = ReadUnsigned(Accessor, Element, Component);
				if (!Accessor.bNormalized) return Value;
				return Accessor.ComponentType == GltfUnsignedByte
					? static_cast<double>(Value) / 255.0
					: static_cast<double>(Value) / 65535.0;
			}

			auto DecodeFloatVectors(
				uint32 AccessorIndex,
				std::string_view Type,
				uint64 LimitCount,
				std::string_view Subject,
				std::initializer_list<uint32> AllowedComponents,
				bool bRequireNormalizedInteger,
				std::vector<std::array<double, 16>>& Out) -> bool
			{
				FDecodedAccessor Accessor;
				if (!GetAccessor(AccessorIndex, Type, LimitCount, Subject, Accessor)) return false;
				if (std::ranges::find(AllowedComponents, Accessor.ComponentType) == AllowedComponents.end()
					|| (Accessor.ComponentType != GltfFloat
						&& Accessor.bNormalized != bRequireNormalizedInteger))
					return Unsupported(std::string(Subject), "glTF accessor encoding is unsupported for this semantic.");
				Out.resize(static_cast<size_t>(Accessor.Count));
				for (uint64 Element = 0; Element < Accessor.Count; ++Element)
				{
					if ((Element & 0xfffu) == 0 && Poll(Subject)) return false;
					for (uint32 Component = 0; Component < Accessor.ComponentCount; ++Component)
					{
						Out[Element][Component] = ReadComponent(Accessor, Element, Component);
						if (!std::isfinite(Out[Element][Component]))
							return Malformed(std::string(Subject), "glTF accessor contains a non-finite value.");
					}
				}
				return AddDecodedBytes(Accessor.Count, Accessor.ComponentCount * sizeof(double), Subject);
			}

			auto DecodeIndices(
				uint32 AccessorIndex,
				uint64 CountLimit,
				std::string_view Subject,
				std::vector<uint32>& Out) -> bool
			{
				FDecodedAccessor Accessor;
				if (!GetAccessor(AccessorIndex, "SCALAR", CountLimit, Subject, Accessor)) return false;
				if (Accessor.bNormalized || (Accessor.ComponentType != GltfUnsignedByte
					&& Accessor.ComponentType != GltfUnsignedShort
					&& Accessor.ComponentType != GltfUnsignedInt))
					return Unsupported(std::string(Subject), "glTF index accessor encoding is unsupported.");
				Out.resize(static_cast<size_t>(Accessor.Count));
				for (uint64 Index = 0; Index < Accessor.Count; ++Index)
				{
					if ((Index & 0xfffu) == 0 && Poll(Subject)) return false;
					Out[Index] = ReadUnsigned(Accessor, Index, 0);
				}
				return AddDecodedBytes(Accessor.Count, sizeof(uint32), Subject);
			}

			auto AddLossyWarning(std::string Subject, std::string Message) -> bool
			{
				return AddDiagnostic(Result.Scene, EImportDiagnosticSeverity::Warning,
					ESceneImportDiagnosticCategory::LossyNormalization,
					"root", std::move(Subject), std::move(Message))
					|| Limit("diagnostics", "Import diagnostic limit exceeded.");
			}

			auto BuildInfluence(
				const std::array<double, 16>& JointValues,
				const std::array<double, 16>& WeightValues,
				const FSourceSkin& Skin,
				std::string_view Subject,
				FSkeletalMeshVertexInfluences& Out,
				bool& bOutLossy) -> bool
			{
				std::map<uint16, double> Merged;
				double SourceSum = 0.0;
				for (uint32 Slot = 0; Slot < 4; ++Slot)
				{
					const double JointValue = JointValues[Slot];
					const double Weight = WeightValues[Slot];
					if (JointValue < 0.0 || JointValue > std::numeric_limits<uint32>::max()
						|| std::floor(JointValue) != JointValue || !std::isfinite(Weight) || Weight < 0.0)
						return Malformed(std::string(Subject), "Vertex influence contains an invalid joint or weight.");
					if (Weight == 0.0) continue;
					const uint32 JointSlot = static_cast<uint32>(JointValue);
					if (JointSlot >= Skin.Joints.size())
						return Malformed(std::string(Subject), "Vertex influence references a joint outside the skin table.");
					const uint16 Bone = Skin.BoneBySourceNode.at(Skin.Joints[JointSlot]);
					if (Merged.contains(Bone)) bOutLossy = true;
					Merged[Bone] += Weight;
					SourceSum += Weight;
				}
				if (!std::isfinite(SourceSum) || SourceSum <= 0.0 || Merged.empty())
					return Malformed(std::string(Subject), "Vertex influence has no positive finite weight.");
				if (std::abs(SourceSum - 1.0) > 1.0e-6) bOutLossy = true;
				std::vector<std::pair<uint16, double>> Ordered(Merged.begin(), Merged.end());
				std::ranges::sort(Ordered, [](const auto& A, const auto& B) {
					return A.second != B.second ? A.second > B.second : A.first < B.first;
				});
				if (Ordered.size() > MaximumSkeletalMeshInfluences)
					return Unsupported(std::string(Subject), "Vertex influence requires more than four stored bones.");
				Out.Count = static_cast<uint8>(Ordered.size());
				float StoredSum = 0.0f;
				for (uint8 Index = 0; Index < Out.Count; ++Index)
				{
					Out.BoneIndices[Index] = Ordered[Index].first;
					Out.Weights[Index] = static_cast<float>(Ordered[Index].second / SourceSum);
					StoredSum += Out.Weights[Index];
				}
				const float Residual = 1.0f - StoredSum;
				if (Residual != 0.0f) bOutLossy = true;
				Out.Weights[0] += Residual;
				if (!std::isfinite(Out.Weights[0]) || Out.Weights[0] <= 0.0f)
					return Malformed(std::string(Subject), "Vertex influence normalization produced an invalid weight.");
				std::array<std::pair<uint16, float>, MaximumSkeletalMeshInfluences> Canonical{};
				for (uint8 Index = 0; Index < Out.Count; ++Index)
					Canonical[Index] = {Out.BoneIndices[Index], Out.Weights[Index]};
				std::ranges::sort(Canonical.begin(), Canonical.begin() + Out.Count,
					[](const auto& A, const auto& B) {
						return A.second != B.second ? A.second > B.second : A.first < B.first;
					});
				for (uint8 Index = 0; Index < Out.Count; ++Index)
				{
					Out.BoneIndices[Index] = Canonical[Index].first;
					Out.Weights[Index] = Canonical[Index].second;
				}
				return true;
			}

			auto PrimitiveUsesSemantic(FJsonNodeView Primitives, std::string_view Semantic) const -> bool
			{
				for (size_t Index = 0; Index < Primitives.Num(); ++Index)
					if (Primitives.GetView(Index).GetView("attributes").Contains(Semantic)) return true;
				return false;
			}

			auto ParseMeshes() -> bool
			{
				const FJsonNodeView MeshArray = Root.GetView("meshes");
				if (!MeshArray.IsArray() || MeshArray.Num() > MaxImportedSourceMeshes)
					return MeshArray.IsArray()
						? Limit("meshes", "Source mesh count exceeds the supported limit.")
						: Malformed("meshes", "Skeletal glTF meshes must be an array.");
				std::unordered_map<uint32, uint32> SkinByMesh;
				for (uint32 NodeIndex = 0; NodeIndex < Nodes.size(); ++NodeIndex)
				{
					const FSourceNode& Node = Nodes[NodeIndex];
					if (!Node.Skin) continue;
					if (!Node.Mesh || *Node.Skin >= Skins.size() || *Node.Mesh >= MeshArray.Num()
						|| !Node.bReachable)
						return Malformed(std::format("node:{}", NodeIndex), "Skinned node requires valid reachable mesh and skin references.");
					if (const auto It = SkinByMesh.find(*Node.Mesh);
						It != SkinByMesh.end() && It->second != *Node.Skin)
						return Unsupported(std::format("mesh:{}", *Node.Mesh), "One mesh definition cannot be associated with multiple skins in version 1.");
					SkinByMesh[*Node.Mesh] = *Node.Skin;
					if (ImportedMeshes.size() >= MaxImportedSkeletalMeshes)
						return Limit("skeletal-meshes", "SkeletalMesh output count exceeds the supported limit.");
					if (!ParseMeshNode(NodeIndex, *Node.Mesh, *Node.Skin, MeshArray.GetView(*Node.Mesh))) return false;
				}
				return true;
			}

			auto ParseMeshNode(
				uint32 NodeIndex,
				uint32 MeshIndex,
				uint32 SkinIndex,
				FJsonNodeView MeshNode) -> bool
			{
				const FJsonNodeView Primitives = MeshNode.GetView("primitives");
				if (!MeshNode.IsObject() || !Primitives.IsArray() || Primitives.Num() == 0)
					return Malformed(std::format("mesh:{}", MeshIndex), "Skinned mesh requires a non-empty primitive array.");
				if (Primitives.Num() > MaxImportedPrimitivesPerMesh)
					return Limit(std::format("mesh:{}", MeshIndex), "Mesh primitive count exceeds the supported limit.");
				const bool bUseColors = PrimitiveUsesSemantic(Primitives, "COLOR_0");
				std::array<bool, MaximumSkeletalMeshUVChannels> UseUV{};
				for (uint32 Channel = 0; Channel < UseUV.size(); ++Channel)
					UseUV[Channel] = PrimitiveUsesSemantic(Primitives, std::format("TEXCOORD_{}", Channel));
				auto Payload = std::make_shared<FSkeletalMeshPayloadData>();
				std::vector<uint32> SourceMaterialOrder;
				std::unordered_map<uint32, uint32> MaterialSlotBySource;
				std::unordered_set<uint16> UsedBones;
				FVector3f BoundsMin(std::numeric_limits<float>::max());
				FVector3f BoundsMax(std::numeric_limits<float>::lowest());
				for (uint32 PrimitiveIndex = 0; PrimitiveIndex < Primitives.Num(); ++PrimitiveIndex)
				{
					if (Poll("skeletal-primitives")) return false;
					const FJsonNodeView Primitive = Primitives.GetView(PrimitiveIndex);
					const FJsonNodeView Attributes = Primitive.GetView("attributes");
					const std::string Subject = std::format("mesh:{}:primitive:{}", MeshIndex, PrimitiveIndex);
					if (!Primitive.IsObject() || !Attributes.IsObject())
						return Malformed(Subject, "glTF skeletal primitive and attributes must be objects.");
					if (Primitive.GetView("targets").IsValid()
						|| Attributes.Contains("JOINTS_1") || Attributes.Contains("WEIGHTS_1"))
						return Unsupported(Subject, "Morph targets and secondary influence sets are unsupported.");
					if (Primitive.GetView("mode").GetUInt(GltfTriangles) != GltfTriangles)
						return Unsupported(Subject, "Only indexed TRIANGLES skeletal primitives are supported.");
					uint32 PositionAccessor = 0;
					uint32 JointAccessor = 0;
					uint32 WeightAccessor = 0;
					uint32 IndexAccessor = 0;
					if (!ReadIndex(Attributes.GetView("POSITION"), PositionAccessor)
						|| !ReadIndex(Attributes.GetView("JOINTS_0"), JointAccessor)
						|| !ReadIndex(Attributes.GetView("WEIGHTS_0"), WeightAccessor)
						|| !ReadIndex(Primitive.GetView("indices"), IndexAccessor))
						return Malformed(Subject, "Skeletal primitive lacks a required accessor.");
					std::vector<std::array<double, 16>> Positions;
					std::vector<std::array<double, 16>> Joints;
					std::vector<std::array<double, 16>> Weights;
					if (!DecodeFloatVectors(PositionAccessor, "VEC3", MaximumSkeletalMeshVertices,
						Subject + ":POSITION", {GltfFloat}, false, Positions)
						|| !DecodeFloatVectors(JointAccessor, "VEC4", MaximumSkeletalMeshVertices,
							Subject + ":JOINTS_0", {GltfUnsignedByte, GltfUnsignedShort}, false, Joints)
						|| !DecodeFloatVectors(WeightAccessor, "VEC4", MaximumSkeletalMeshVertices,
							Subject + ":WEIGHTS_0", {GltfFloat, GltfUnsignedByte, GltfUnsignedShort}, true, Weights)) return false;
					if (Positions.size() != Joints.size() || Positions.size() != Weights.size()
						|| Positions.size() > MaximumSkeletalMeshVertices - Payload->Positions.size())
						return Malformed(Subject, "Skeletal primitive vertex stream counts do not match.");
					const uint32 VertexBase = static_cast<uint32>(Payload->Positions.size());
					for (const auto& Value : Positions)
					{
						const FVector3f Converted(
							CleanNumber(-Value[2]), CleanNumber(Value[0]), CleanNumber(Value[1]));
						Payload->Positions.push_back(Converted);
						BoundsMin = Math::Min(BoundsMin, Converted);
						BoundsMax = Math::Max(BoundsMax, Converted);
					}

					auto DecodeOptionalVector = [&](std::string_view Semantic, std::string_view Type,
						std::initializer_list<uint32> Components, bool bNormalized,
						std::vector<std::array<double, 16>>& Out) -> bool {
						if (!Attributes.Contains(Semantic)) return true;
						uint32 Accessor = 0;
						return ReadIndex(Attributes.GetView(Semantic), Accessor)
							&& DecodeFloatVectors(Accessor, Type, MaximumSkeletalMeshVertices,
								std::format("{}:{}", Subject, Semantic), Components, bNormalized, Out)
							&& Out.size() == Positions.size();
					};
					std::vector<std::array<double, 16>> Normals;
					std::vector<std::array<double, 16>> Tangents;
					std::vector<std::array<double, 16>> Colors;
					bool bColorsHaveAlpha = true;
					auto DecodeOptionalColor = [&]() -> bool {
						if (!Attributes.Contains("COLOR_0")) return true;
						uint32 Accessor = 0;
						if (!ReadIndex(Attributes.GetView("COLOR_0"), Accessor)) return false;
						const FJsonNodeView AccessorNode = Root.GetView("accessors").GetView(Accessor);
						const std::string Type = AccessorNode.GetView("type").GetString();
						bColorsHaveAlpha = Type == "VEC4";
						return DecodeFloatVectors(Accessor, Type == "VEC3" ? "VEC3" : "VEC4",
							MaximumSkeletalMeshVertices, Subject + ":COLOR_0",
							{GltfFloat, GltfUnsignedByte, GltfUnsignedShort}, true, Colors)
							&& Colors.size() == Positions.size();
					};
					if (!DecodeOptionalVector("NORMAL", "VEC3", {GltfFloat}, false, Normals)
						|| !DecodeOptionalVector("TANGENT", "VEC4", {GltfFloat}, false, Tangents)
						|| !DecodeOptionalColor())
						return Malformed(Subject, "Skeletal primitive optional vertex stream is invalid.");
					for (size_t Vertex = 0; Vertex < Positions.size(); ++Vertex)
					{
						Payload->Normals.push_back(Normals.empty()
							? FVector3f(0.0f, 0.0f, 1.0f)
							: FVector3f(CleanNumber(-Normals[Vertex][2]), CleanNumber(Normals[Vertex][0]), CleanNumber(Normals[Vertex][1])));
						Payload->Tangents.push_back(Tangents.empty()
							? FVector4f(1.0f, 0.0f, 0.0f, 1.0f)
							: FVector4f(CleanNumber(-Tangents[Vertex][2]), CleanNumber(Tangents[Vertex][0]),
								CleanNumber(Tangents[Vertex][1]), CleanNumber(-Tangents[Vertex][3])));
						if (bUseColors) Payload->Colors.push_back(Colors.empty()
							? FVector4f(1.0f)
							: FVector4f(CleanNumber(Colors[Vertex][0]), CleanNumber(Colors[Vertex][1]),
								CleanNumber(Colors[Vertex][2]), bColorsHaveAlpha ? CleanNumber(Colors[Vertex][3]) : 1.0f));
					}
					for (uint32 Channel = 0; Channel < UseUV.size(); ++Channel)
					{
						if (!UseUV[Channel]) continue;
						std::vector<std::array<double, 16>> UV;
						const std::string Semantic = std::format("TEXCOORD_{}", Channel);
						if (!DecodeOptionalVector(Semantic, "VEC2",
							{GltfFloat, GltfUnsignedByte, GltfUnsignedShort}, true, UV))
							return Malformed(Subject, "Skeletal primitive UV stream is invalid.");
						for (size_t Vertex = 0; Vertex < Positions.size(); ++Vertex)
							Payload->UVChannels[Channel].push_back(UV.empty()
								? FVector2f(0.0f)
								: FVector2f(CleanNumber(UV[Vertex][0]), CleanNumber(UV[Vertex][1])));
					}

					bool bLossy = false;
					for (size_t Vertex = 0; Vertex < Positions.size(); ++Vertex)
					{
						FSkeletalMeshVertexInfluences Influence;
						if (!BuildInfluence(Joints[Vertex], Weights[Vertex], Skins[SkinIndex], Subject, Influence, bLossy)) return false;
						for (uint8 InfluenceIndex = 0; InfluenceIndex < Influence.Count; ++InfluenceIndex)
							UsedBones.insert(Influence.BoneIndices[InfluenceIndex]);
						Payload->Influences.push_back(Influence);
					}
					if (bLossy && !AddLossyWarning(Subject, "Skeletal influences required deterministic normalization.")) return false;

					std::vector<uint32> Indices;
					if (!DecodeIndices(IndexAccessor, MaximumSkeletalMeshIndices, Subject + ":indices", Indices)) return false;
					if (Indices.size() % 3 != 0 || Indices.size() > MaximumSkeletalMeshIndices - Payload->Indices.size())
						return Malformed(Subject, "Skeletal primitive index count is invalid.");
					for (uint32 Index : Indices)
						if (Index >= Positions.size()) return Malformed(Subject, "Skeletal primitive contains an out-of-range index.");
					for (size_t Triangle = 0; Triangle < Indices.size(); Triangle += 3)
						std::swap(Indices[Triangle + 1], Indices[Triangle + 2]);
					uint32 MinIndex = std::numeric_limits<uint32>::max();
					uint32 MaxIndex = 0;
					for (uint32& Index : Indices)
					{
						MinIndex = std::min(MinIndex, Index + VertexBase);
						MaxIndex = std::max(MaxIndex, Index + VertexBase);
						Index += VertexBase;
					}
					const std::span<const FVector3f> PrimitivePositions(Payload->Positions.data() + VertexBase, Positions.size());
					std::span<FVector3f> PrimitiveNormals(Payload->Normals.data() + VertexBase, Positions.size());
					if (Normals.empty())
						GenerateNormals(PrimitivePositions, Indices, VertexBase, PrimitiveNormals);
					if (Tangents.empty())
					{
						const std::span<const FVector2f> PrimitiveUVs = UseUV[0]
							? std::span<const FVector2f>(Payload->UVChannels[0].data() + VertexBase, Positions.size())
							: std::span<const FVector2f>{};
						GenerateTangents(
							PrimitivePositions,
							PrimitiveNormals,
							PrimitiveUVs,
							Indices,
							VertexBase,
							std::span<FVector4f>(Payload->Tangents.data() + VertexBase, Positions.size()));
					}
					const uint32 FirstIndex = static_cast<uint32>(Payload->Indices.size());
					Payload->Indices.insert(Payload->Indices.end(), Indices.begin(), Indices.end());
					const FJsonNodeView Material = Primitive.GetView("material");
					uint32 MaterialIndex = 0;
					if (!Material.IsValid())
					{
						if (!Result.DefaultGltfMaterialIndex)
						{
							if (Result.Scene.Materials.size() >= MaxImportedSourceMaterials)
								return Limit("materials", "The implicit glTF default material exceeds the material limit.");
							Result.DefaultGltfMaterialIndex = static_cast<uint32>(Result.Scene.Materials.size());
							Result.Scene.Materials.push_back({
								.SourceMaterialIndex = *Result.DefaultGltfMaterialIndex,
								.SourceName = "Default"});
						}
						MaterialIndex = *Result.DefaultGltfMaterialIndex;
					}
					else if (!ReadIndex(Material, MaterialIndex))
						return Malformed(Subject, "Skeletal primitive material reference is invalid.");
					if (MaterialIndex >= Result.Scene.Materials.size())
						return Malformed(Subject, "Skeletal primitive material reference is invalid.");
					uint32 MaterialSlot = 0;
					if (const auto It = MaterialSlotBySource.find(MaterialIndex); It != MaterialSlotBySource.end())
						MaterialSlot = It->second;
					else
					{
						MaterialSlot = static_cast<uint32>(SourceMaterialOrder.size());
						MaterialSlotBySource[MaterialIndex] = MaterialSlot;
						SourceMaterialOrder.push_back(MaterialIndex);
					}
					FVector3f SectionMin(std::numeric_limits<float>::max());
					FVector3f SectionMax(std::numeric_limits<float>::lowest());
					for (uint32 Index : Indices)
					{
						SectionMin = Math::Min(SectionMin, Payload->Positions[Index]);
						SectionMax = Math::Max(SectionMax, Payload->Positions[Index]);
					}
					Payload->Sections.push_back({
						.Name = FName(std::format("Section_{}", PrimitiveIndex)),
						.FirstIndex = FirstIndex,
						.IndexCount = static_cast<uint32>(Indices.size()),
						.MinVertexIndex = MinIndex,
						.MaxVertexIndex = MaxIndex,
						.MaterialSlotIndex = MaterialSlot,
						.LocalBounds = FBox(SectionMin, SectionMax)});
				}
				Payload->LocalBounds = FBox(BoundsMin, BoundsMax);
				std::vector<FMatrix4f> InverseByJoint;
				const FSourceSkin& Skin = Skins[SkinIndex];
				if (Skin.InverseBindAccessor)
				{
					std::vector<std::array<double, 16>> Matrices;
					if (!DecodeFloatVectors(*Skin.InverseBindAccessor, "MAT4", MaximumSkeletonBones,
						std::format("skin:{}:inverseBindMatrices", SkinIndex), {GltfFloat}, false, Matrices)) return false;
					if (Matrices.size() != Skin.Joints.size())
						return Malformed(std::format("skin:{}", SkinIndex), "Inverse-bind count does not match skin joints.");
					for (const auto& Values : Matrices)
					{
						FMatrix Matrix(0.0);
						for (uint32 Column = 0; Column < 4; ++Column)
							for (uint32 Row = 0; Row < 4; ++Row) Matrix[Column][Row] = Values[Column * 4 + Row];
						InverseByJoint.push_back(ConvertMatrix(Matrix));
					}
				}
				else InverseByJoint.assign(Skin.Joints.size(), FMatrix4f(1.0f));
				std::vector<uint16> SortedBones(UsedBones.begin(), UsedBones.end());
				std::ranges::sort(SortedBones);
				for (uint16 Bone : SortedBones)
				{
					const auto It = std::ranges::find_if(Skin.Joints, [&](uint32 Joint) {
						return Skin.BoneBySourceNode.at(Joint) == Bone;
					});
					if (It == Skin.Joints.end()) return Malformed("palette", "Skeletal palette bone has no source joint.");
					Payload->PaletteBoneIndices.push_back(Bone);
					Payload->InverseBindMatrices.push_back(InverseByJoint[std::distance(Skin.Joints.begin(), It)]);
				}
				std::unordered_map<std::string, uint32> SlotNames;
				std::vector<FMeshMaterialSlotDefinition> Slots;
				for (uint32 SourceMaterial : SourceMaterialOrder)
				{
					const FImportedMaterial& Material = Result.Scene.Materials[SourceMaterial];
					Slots.push_back({
						.Name = FName(MakeUniqueName(Material.SourceName, SourceMaterial, SlotNames)),
						.SourceName = Material.SourceName,
						.SourceMaterialIndex = SourceMaterial});
				}
				const std::string MeshName = MeshNode.GetView("name").GetString();
				const std::string SuggestedName = !Nodes[NodeIndex].Name.empty()
					? Nodes[NodeIndex].Name
					: !MeshName.empty() ? MeshName : std::format("SkeletalMesh_{}", NodeIndex);
				ImportedMeshes.push_back({
					.StableIdentity = std::format("skeletal-mesh:node/{}/mesh/{}", NodeIndex, MeshIndex),
					.SuggestedName = SuggestedName,
					.SourceNodeIndex = NodeIndex,
					.SourceMeshIndex = MeshIndex,
					.SkeletonIndex = Skins[SkinIndex].OutputIndex,
					.MeshNodeBindTransform = ToSkeletonTransform(ConvertMatrix(Nodes[NodeIndex].Global)),
					.MaterialSlots = std::move(Slots),
					.Payload = std::move(Payload)});
				return AddDecodedBytes(1, sizeof(FImportedSkeletalMeshData), "skeletal-meshes");
			}

			auto ConvertedQuaternion(const std::array<double, 16>& Value, FVector4f& Out) -> bool
			{
				const double Length = std::sqrt(Value[0] * Value[0] + Value[1] * Value[1]
					+ Value[2] * Value[2] + Value[3] * Value[3]);
				if (!std::isfinite(Length) || Length <= MatrixTolerance) return false;
				const double X = Value[0] / Length;
				const double Y = Value[1] / Length;
				const double Z = Value[2] / Length;
				const double W = Value[3] / Length;
				FMatrix Rotation(1.0);
				Rotation[0][0] = 1.0 - 2.0 * (Y * Y + Z * Z);
				Rotation[0][1] = 2.0 * (X * Y + Z * W);
				Rotation[0][2] = 2.0 * (X * Z - Y * W);
				Rotation[1][0] = 2.0 * (X * Y - Z * W);
				Rotation[1][1] = 1.0 - 2.0 * (X * X + Z * Z);
				Rotation[1][2] = 2.0 * (Y * Z + X * W);
				Rotation[2][0] = 2.0 * (X * Z + Y * W);
				Rotation[2][1] = 2.0 * (Y * Z - X * W);
				Rotation[2][2] = 1.0 - 2.0 * (X * X + Y * Y);
				const FMatrix Converted = SourceToDurinMatrix() * Rotation * Math::Inverse(SourceToDurinMatrix());
				std::array<double, 4> Quaternion{};
				const double Trace = Converted[0][0] + Converted[1][1] + Converted[2][2];
				if (Trace > 0.0)
				{
					const double Scale = std::sqrt(Trace + 1.0) * 2.0;
					Quaternion = {(Converted[1][2] - Converted[2][1]) / Scale,
						(Converted[2][0] - Converted[0][2]) / Scale,
						(Converted[0][1] - Converted[1][0]) / Scale, 0.25 * Scale};
				}
				else if (Converted[0][0] > Converted[1][1] && Converted[0][0] > Converted[2][2])
				{
					const double Scale = std::sqrt(1.0 + Converted[0][0] - Converted[1][1] - Converted[2][2]) * 2.0;
					Quaternion = {0.25 * Scale, (Converted[1][0] + Converted[0][1]) / Scale,
						(Converted[2][0] + Converted[0][2]) / Scale,
						(Converted[1][2] - Converted[2][1]) / Scale};
				}
				else if (Converted[1][1] > Converted[2][2])
				{
					const double Scale = std::sqrt(1.0 + Converted[1][1] - Converted[0][0] - Converted[2][2]) * 2.0;
					Quaternion = {(Converted[1][0] + Converted[0][1]) / Scale, 0.25 * Scale,
						(Converted[2][1] + Converted[1][2]) / Scale,
						(Converted[2][0] - Converted[0][2]) / Scale};
				}
				else
				{
					const double Scale = std::sqrt(1.0 + Converted[2][2] - Converted[0][0] - Converted[1][1]) * 2.0;
					Quaternion = {(Converted[2][0] + Converted[0][2]) / Scale,
						(Converted[2][1] + Converted[1][2]) / Scale, 0.25 * Scale,
						(Converted[0][1] - Converted[1][0]) / Scale};
				}
				const double QLength = std::sqrt(
					Quaternion[0] * Quaternion[0] + Quaternion[1] * Quaternion[1]
					+ Quaternion[2] * Quaternion[2] + Quaternion[3] * Quaternion[3]);
				if (!std::isfinite(QLength) || QLength <= MatrixTolerance) return false;
				for (double& Component : Quaternion) Component /= QLength;
				const std::array<double, 4> Lexical{Quaternion[3], Quaternion[2], Quaternion[1], Quaternion[0]};
				if (Lexical < std::array<double, 4>{0.0, 0.0, 0.0, 0.0})
					for (double& Component : Quaternion) Component = -Component;
				Out = FVector4f(CleanNumber(Quaternion[0]), CleanNumber(Quaternion[1]),
					CleanNumber(Quaternion[2]), CleanNumber(Quaternion[3]));
				return true;
			}

			auto ParseAnimations() -> bool
			{
				const FJsonNodeView Animations = Root.GetView("animations");
				if (!Animations.IsValid()) return true;
				if (!Animations.IsArray()) return Malformed("animations", "glTF animations must be an array.");
				if (Animations.Num() > MaxImportedAnimations)
					return Limit("animations", "Source animation count exceeds the supported limit.");
				for (uint32 AnimationIndex = 0; AnimationIndex < Animations.Num(); ++AnimationIndex)
				{
					if (Poll("animations")) return false;
					const FJsonNodeView Animation = Animations.GetView(AnimationIndex);
					const FJsonNodeView Channels = Animation.GetView("channels");
					const FJsonNodeView Samplers = Animation.GetView("samplers");
					if (!Animation.IsObject() || !Channels.IsArray() || Channels.Num() == 0 || !Samplers.IsArray())
						return Malformed(std::format("animation:{}", AnimationIndex), "glTF animation channels or samplers are invalid.");
					if (Channels.Num() > MaximumAnimationClipTracks || Samplers.Num() > MaximumAnimationClipTracks)
						return Limit(std::format("animation:{}", AnimationIndex), "Animation channel or sampler count exceeds the supported limit.");
					std::vector<uint32> CompatibleSkins;
					for (uint32 SkinIndex = 0; SkinIndex < Skins.size(); ++SkinIndex)
					{
						if (Poll("animation-skins")) return false;
						bool bCompatible = true;
						for (size_t ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
						{
							if ((ChannelIndex & 0xfffu) == 0 && Poll("animation-skin-channels")) return false;
							uint32 TargetNode = 0;
							if (!ReadIndex(Channels.GetView(ChannelIndex).GetView("target").GetView("node"), TargetNode)
								|| !Skins[SkinIndex].BoneBySourceNode.contains(TargetNode))
							{
								bCompatible = false;
								break;
							}
						}
						if (bCompatible) CompatibleSkins.push_back(SkinIndex);
					}
					for (size_t ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
					{
						if ((ChannelIndex & 0xfffu) == 0 && Poll("animation-channels")) return false;
						uint32 TargetNode = 0;
						if (!ReadIndex(Channels.GetView(ChannelIndex).GetView("target").GetView("node"), TargetNode)
							|| TargetNode >= Nodes.size())
							return Malformed(std::format("animation:{}:channel:{}", AnimationIndex, ChannelIndex), "Animation target node is invalid.");
					}
					if (CompatibleSkins.empty())
						return Unsupported(std::format("animation:{}", AnimationIndex), "Animation channels do not map wholly to one skin.");
					for (uint32 SkinIndex : CompatibleSkins)
					{
						if (ImportedClips.size() >= MaxImportedAnimationClips)
							return Limit("animation-clips", "AnimationClip output count exceeds the supported limit.");
						auto Payload = std::make_shared<FAnimationClipPayloadData>();
						std::set<std::pair<uint16, EAnimationTrackPath>> TrackKeys;
						uint64 TotalKeys = 0;
						for (uint32 ChannelIndex = 0; ChannelIndex < Channels.Num(); ++ChannelIndex)
						{
							const FJsonNodeView Channel = Channels.GetView(ChannelIndex);
							const FJsonNodeView Target = Channel.GetView("target");
							uint32 SamplerIndex = 0;
							uint32 TargetNode = 0;
							if (!Channel.IsObject() || !ReadIndex(Channel.GetView("sampler"), SamplerIndex)
								|| SamplerIndex >= Samplers.Num() || !ReadIndex(Target.GetView("node"), TargetNode))
								return Malformed(std::format("animation:{}:channel:{}", AnimationIndex, ChannelIndex), "Animation channel references are invalid.");
							const std::string Path = Target.GetView("path").GetString();
							EAnimationTrackPath TrackPath;
							std::string Type;
							if (Path == "translation") { TrackPath = EAnimationTrackPath::Translation; Type = "VEC3"; }
							else if (Path == "rotation") { TrackPath = EAnimationTrackPath::Rotation; Type = "VEC4"; }
							else if (Path == "scale") { TrackPath = EAnimationTrackPath::Scale; Type = "VEC3"; }
							else return Unsupported(std::format("animation:{}:channel:{}", AnimationIndex, ChannelIndex), "Animation target path is unsupported.");
							const uint16 Bone = Skins[SkinIndex].BoneBySourceNode.at(TargetNode);
							if (!TrackKeys.emplace(Bone, TrackPath).second)
								return Malformed(std::format("animation:{}", AnimationIndex), "Animation contains duplicate bone/path tracks.");
							const FJsonNodeView Sampler = Samplers.GetView(SamplerIndex);
							uint32 InputAccessor = 0;
							uint32 OutputAccessor = 0;
							if (!Sampler.IsObject() || !ReadIndex(Sampler.GetView("input"), InputAccessor)
								|| !ReadIndex(Sampler.GetView("output"), OutputAccessor))
								return Malformed(std::format("animation:{}:sampler:{}", AnimationIndex, SamplerIndex), "Animation sampler accessors are invalid.");
							const std::string Interpolation = Sampler.GetView("interpolation").GetString("LINEAR");
							EAnimationInterpolation RuntimeInterpolation;
							if (Interpolation == "LINEAR") RuntimeInterpolation = EAnimationInterpolation::Linear;
							else if (Interpolation == "STEP") RuntimeInterpolation = EAnimationInterpolation::Step;
							else return Unsupported(std::format("animation:{}:sampler:{}", AnimationIndex, SamplerIndex), "Animation interpolation is unsupported.");
							std::vector<std::array<double, 16>> Times;
							std::vector<std::array<double, 16>> Values;
							const std::string Subject = std::format("animation:{}:channel:{}", AnimationIndex, ChannelIndex);
							if (!DecodeFloatVectors(InputAccessor, "SCALAR", MaximumAnimationKeysPerTrack,
								Subject + ":times", {GltfFloat}, false, Times)
								|| !DecodeFloatVectors(OutputAccessor, Type, MaximumAnimationKeysPerTrack,
									Subject + ":values", {GltfFloat}, false, Values)) return false;
							if (Times.size() != Values.size()
								|| TotalKeys > MaximumAnimationKeysPerClip - Times.size())
								return Malformed(Subject, "Animation input/output key counts do not match.");
							TotalKeys += Times.size();
							FAnimationTrackData Track{
								.BoneIndex = Bone,
								.Path = TrackPath,
								.Interpolation = RuntimeInterpolation};
							for (size_t Key = 0; Key < Times.size(); ++Key)
							{
								const float Time = CleanNumber(Times[Key][0]);
								if (Time < 0.0f || (Key > 0 && Time <= Track.Times.back()))
									return Malformed(Subject, "Animation key times must be finite, non-negative, and strictly increasing.");
								Track.Times.push_back(Time);
								Payload->DurationSeconds = std::max(Payload->DurationSeconds, Time);
								if (TrackPath == EAnimationTrackPath::Translation)
									Track.VectorValues.emplace_back(CleanNumber(-Values[Key][2]), CleanNumber(Values[Key][0]), CleanNumber(Values[Key][1]));
								else if (TrackPath == EAnimationTrackPath::Scale)
								{
									const FVector3f Scale(CleanNumber(Values[Key][2]), CleanNumber(Values[Key][0]), CleanNumber(Values[Key][1]));
									if (std::abs(Scale.x) <= 1.0e-8f || std::abs(Scale.y) <= 1.0e-8f || std::abs(Scale.z) <= 1.0e-8f)
										return Malformed(Subject, "Animation scale key is singular.");
									Track.VectorValues.push_back(Scale);
								}
								else
								{
									FVector4f Rotation;
									if (!ConvertedQuaternion(Values[Key], Rotation))
										return Malformed(Subject, "Animation rotation key is invalid.");
									if (!Track.RotationValues.empty())
									{
										const float Dot = Math::Dot(Track.RotationValues.back(), Rotation);
										if (Dot < 0.0f) Rotation = -Rotation;
										else if (Dot == 0.0f)
										{
											const std::array<float, 4> Lexical{Rotation.w, Rotation.z, Rotation.y, Rotation.x};
											if (Lexical < std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}) Rotation = -Rotation;
										}
									}
									Track.RotationValues.push_back(Rotation);
								}
							}
							Payload->Tracks.push_back(std::move(Track));
						}
						const std::string AnimationName = Animation.GetView("name").GetString();
						ImportedClips.push_back({
							.StableIdentity = std::format("animation-clip:animation/{}/skin/{}", AnimationIndex, SkinIndex),
							.SuggestedName = AnimationName.empty() ? std::format("Animation_{}", AnimationIndex) : AnimationName,
							.SourceAnimationIndex = AnimationIndex,
							.SkeletonIndex = Skins[SkinIndex].OutputIndex,
							.Payload = std::move(Payload)});
					}
				}
				return AddDecodedBytes(ImportedClips.size(), sizeof(FImportedAnimationClipData), "animation-clips");
			}

			FJsonNodeView Root;
			const std::vector<FByteArray>& Buffers;
			FSceneDecodeResult& Result;
			std::vector<FSourceNode> Nodes;
			std::vector<FSourceSkin> Skins;
			std::vector<FImportedSceneNode> ImportedNodes;
			std::vector<FImportedSkeletonData> ImportedSkeletons;
			std::vector<FImportedSkeletalMeshData> ImportedMeshes;
			std::vector<FImportedAnimationClipData> ImportedClips;
			uint64 DecodedBytes = 0;
		};
	}

	auto ImportGltfSkeletalData(
		FJsonNodeView Root,
		const std::vector<FByteArray>& Buffers,
		FSceneDecodeResult& Result) -> bool
	{
		return FGltfSkeletalDecoder(Root, Buffers, Result).Decode();
	}
}
