#include "Panels/WorldOutlinerHierarchyModel.h"

#include <gtest/gtest.h>

namespace
{
	using namespace Durin;

	struct FNodeKey
	{
		int Value = 0;
	};

	auto MakeInput(FNodeKey& Key, FNodeKey* Parent, std::string Name, std::string Type = {}) -> FWorldOutlinerHierarchyModel::FInput
	{
		return {
			.Key = &Key,
			.ParentKey = Parent,
			.Name = std::move(Name),
			.TypeLabel = std::move(Type)};
	}

	auto FindNode(const FWorldOutlinerHierarchyModel& Model, const void* Key) -> const FWorldOutlinerHierarchyModel::FNode*
	{
		for (const auto& Node : Model.GetNodes())
			if (Node.Key == Key) return &Node;
		return nullptr;
	}
} // namespace

TEST(WorldOutlinerHierarchyModelTests, BuildsSortedRootsAndNestedTraversal)
{
	FNodeKey Root{1};
	FNodeKey Child{2};
	FNodeKey OtherRoot{3};
	FWorldOutlinerHierarchyModel Model;
	const std::array Inputs{
		MakeInput(Child, &Root, "Child", "Mesh Component"),
		MakeInput(OtherRoot, nullptr, "Other Root", "Camera"),
		MakeInput(Root, nullptr, "Root", "Actor")};
	Model.Rebuild(&Model, 4, Inputs);

	ASSERT_EQ(Model.GetRootNodeIndices().size(), 2);
	const auto* RootNode = FindNode(Model, &Root);
	const auto* ChildNode = FindNode(Model, &Child);
	const auto* OtherRootNode = FindNode(Model, &OtherRoot);
	ASSERT_NE(RootNode, nullptr);
	ASSERT_NE(ChildNode, nullptr);
	ASSERT_NE(OtherRootNode, nullptr);
	EXPECT_EQ(RootNode->Children.size(), 1);
	EXPECT_EQ(RootNode->Children.front(), static_cast<uint32>(ChildNode - Model.GetNodes().data()));
	EXPECT_EQ(RootNode->Depth, 0);
	EXPECT_EQ(ChildNode->Depth, 1);
	EXPECT_EQ(OtherRootNode->Depth, 0);
	EXPECT_TRUE(Model.IsDescendantOf(&Child, &Root));
	EXPECT_FALSE(Model.IsDescendantOf(&Root, &Root));
	EXPECT_FALSE(Model.IsDescendantOf(&Root, &OtherRoot));
	EXPECT_EQ(Model.GetDepth(&Child), 1);
}

TEST(WorldOutlinerHierarchyModelTests, BreaksCyclesAndKeepsEveryNodeReachable)
{
	FNodeKey A{1};
	FNodeKey B{2};
	FNodeKey C{3};
	FWorldOutlinerHierarchyModel Model;
	const std::array Inputs{
		MakeInput(A, &C, "A"),
		MakeInput(B, &A, "B"),
		MakeInput(C, &B, "C")};
	Model.Rebuild(&Model, 1, Inputs);

	ASSERT_EQ(Model.GetRootNodeIndices().size(), 1);
	EXPECT_EQ(Model.GetNodes().size(), 3);
	EXPECT_EQ(Model.GetRootNodeIndices().front(), static_cast<uint32>(1));
	EXPECT_TRUE(Model.IsDescendantOf(&A, &B));
	EXPECT_TRUE(Model.IsDescendantOf(&C, &B));
	EXPECT_FALSE(Model.IsDescendantOf(&A, &A));
}

TEST(WorldOutlinerHierarchyModelTests, KeepsFilterAncestorsVisible)
{
	FNodeKey Root{1};
	FNodeKey Branch{2};
	FNodeKey Leaf{3};
	FNodeKey Sibling{4};
	FWorldOutlinerHierarchyModel Model;
	const std::array Inputs{
		MakeInput(Root, nullptr, "Root"),
		MakeInput(Branch, &Root, "Branch"),
		MakeInput(Leaf, &Branch, "Leaf", "Target Type"),
		MakeInput(Sibling, &Root, "Sibling")};
	Model.Rebuild(&Model, 2, Inputs);

	Model.SetFilter("target");
	const auto* RootNode = FindNode(Model, &Root);
	const auto* BranchNode = FindNode(Model, &Branch);
	const auto* LeafNode = FindNode(Model, &Leaf);
	const auto* SiblingNode = FindNode(Model, &Sibling);
	ASSERT_NE(RootNode, nullptr);
	ASSERT_NE(BranchNode, nullptr);
	ASSERT_NE(LeafNode, nullptr);
	ASSERT_NE(SiblingNode, nullptr);
	EXPECT_TRUE(Model.IsNodeVisible(static_cast<uint32>(RootNode - Model.GetNodes().data())));
	EXPECT_TRUE(Model.IsNodeVisible(static_cast<uint32>(BranchNode - Model.GetNodes().data())));
	EXPECT_TRUE(Model.IsNodeVisible(static_cast<uint32>(LeafNode - Model.GetNodes().data())));
	EXPECT_FALSE(Model.IsNodeVisible(static_cast<uint32>(SiblingNode - Model.GetNodes().data())));
}

TEST(WorldOutlinerHierarchyModelTests, InvalidatesByRevisionAndDropsDeletedNodes)
{
	FNodeKey Root{1};
	FNodeKey Deleted{2};
	FWorldOutlinerHierarchyModel Model;
	EXPECT_TRUE(Model.NeedsRebuild(&Model, 1));
	const std::array InitialInputs{MakeInput(Root, nullptr, "Root"), MakeInput(Deleted, &Root, "Deleted")};
	Model.Rebuild(&Model, 1, InitialInputs);
	EXPECT_FALSE(Model.NeedsRebuild(&Model, 1));
	EXPECT_TRUE(Model.Contains(&Deleted));

	const std::array ReducedInputs{MakeInput(Root, nullptr, "Root")};
	Model.Rebuild(&Model, 2, ReducedInputs);
	EXPECT_TRUE(Model.NeedsRebuild(&Model, 1));
	EXPECT_FALSE(Model.NeedsRebuild(&Model, 2));
	EXPECT_FALSE(Model.Contains(&Deleted));
	EXPECT_EQ(Model.GetDepth(&Deleted), 0);
}
