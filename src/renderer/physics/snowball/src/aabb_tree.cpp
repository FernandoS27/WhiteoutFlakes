#include "snowball/aabb_tree.h"

#include <algorithm>
#include <cmath>

namespace snowball {
namespace {

bool Finite(const Vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

f32 SurfaceArea(const Aabb& a) {
    const Vec4 d = a.upper - a.lower;
    return 2.0f * (d.x * d.y + d.y * d.z + d.z * d.x);
}

Aabb Combine(const Aabb& a, const Aabb& b) {
    return Aabb{Vec4{std::min(a.lower.x, b.lower.x), std::min(a.lower.y, b.lower.y),
                     std::min(a.lower.z, b.lower.z)},
                Vec4{std::max(a.upper.x, b.upper.x), std::max(a.upper.y, b.upper.y),
                     std::max(a.upper.z, b.upper.z)}};
}

bool Overlaps(const Aabb& a, const Aabb& b) {
    return !(b.lower.x > a.upper.x || b.upper.x < a.lower.x || b.lower.y > a.upper.y ||
             b.upper.y < a.lower.y || b.lower.z > a.upper.z || b.upper.z < a.lower.z);
}

i32 AabbTree::AllocateNode() {
    if (freeList_ == kNullNode) {
        nodes_.emplace_back();
        return static_cast<i32>(nodes_.size()) - 1;
    }
    const i32 node = freeList_;
    freeList_ = nodes_[static_cast<usize>(node)].parent;
    nodes_[static_cast<usize>(node)] = TreeNode{};
    return node;
}

void AabbTree::FreeNode(i32 node) {
    nodes_[static_cast<usize>(node)] = TreeNode{};
    nodes_[static_cast<usize>(node)].parent = freeList_;
    freeList_ = node;
}

i32 AabbTree::CreateProxy(const Aabb& aabb, u64 userData) {
    if (!Finite(aabb.lower) || !Finite(aabb.upper) || aabb.upper.x < aabb.lower.x ||
        aabb.upper.y < aabb.lower.y) {
        return kNullNode;
    }

    const i32 proxy = AllocateNode();
    TreeNode& node = nodes_[static_cast<usize>(proxy)];
    const Vec4 extension = Vec4::Splat(kAabbExtension);
    node.aabb.lower = aabb.lower - extension;
    node.aabb.upper = aabb.upper + extension;
    node.aabb.lower.w = 0.0f;
    node.aabb.upper.w = 0.0f;
    node.userData = userData;
    node.height = 0;

    InsertLeaf(proxy);
    ++proxyCount_;
    return proxy;
}

bool AabbTree::MoveProxy(i32 proxy, const Aabb& aabb) {
    if (proxy == kNullNode || !Finite(aabb.lower) || !Finite(aabb.upper)) {
        return false;
    }
    TreeNode& node = nodes_[static_cast<usize>(proxy)];
    if (Overlaps(node.aabb, aabb) && node.aabb.lower.x <= aabb.lower.x &&
        node.aabb.lower.y <= aabb.lower.y && node.aabb.lower.z <= aabb.lower.z &&
        node.aabb.upper.x >= aabb.upper.x && node.aabb.upper.y >= aabb.upper.y &&
        node.aabb.upper.z >= aabb.upper.z) {
        return false;  // still inside its fat bounds; nothing to do
    }

    RemoveLeaf(proxy);
    const Vec4 extension = Vec4::Splat(kAabbExtension);
    node.aabb.lower = aabb.lower - extension;
    node.aabb.upper = aabb.upper + extension;
    node.aabb.lower.w = 0.0f;
    node.aabb.upper.w = 0.0f;
    InsertLeaf(proxy);
    return true;
}

void AabbTree::DestroyProxy(i32 proxy) {
    RemoveLeaf(proxy);
    FreeNode(proxy);
    --proxyCount_;
}

void AabbTree::InsertLeaf(i32 leaf) {
    if (root_ == kNullNode) {
        root_ = leaf;
        nodes_[static_cast<usize>(leaf)].parent = kNullNode;
        return;
    }

    // Descend to the sibling whose subtree costs least to grow. The comparison is between
    // creating a new parent here and pushing the leaf further down; `inheritanceCost` is what
    // every node on the way would have to grow by regardless.
    const Aabb leafAabb = nodes_[static_cast<usize>(leaf)].aabb;
    i32 index = root_;
    while (!nodes_[static_cast<usize>(index)].IsLeaf()) {
        const i32 child1 = nodes_[static_cast<usize>(index)].child1;
        const i32 child2 = nodes_[static_cast<usize>(index)].child2;

        const f32 area = SurfaceArea(nodes_[static_cast<usize>(index)].aabb);
        const Aabb combined = Combine(nodes_[static_cast<usize>(index)].aabb, leafAabb);
        const f32 combinedArea = SurfaceArea(combined);

        const f32 cost = 2.0f * combinedArea;
        const f32 inheritanceCost = 2.0f * (combinedArea - area);

        const auto descentCost = [&](i32 child) {
            const Aabb grown = Combine(leafAabb, nodes_[static_cast<usize>(child)].aabb);
            if (nodes_[static_cast<usize>(child)].IsLeaf()) {
                return SurfaceArea(grown) + inheritanceCost;
            }
            const f32 oldArea = SurfaceArea(nodes_[static_cast<usize>(child)].aabb);
            return (SurfaceArea(grown) - oldArea) + inheritanceCost;
        };
        const f32 cost1 = descentCost(child1);
        const f32 cost2 = descentCost(child2);

        if (cost < cost1 && cost < cost2) {
            break;
        }
        index = cost1 < cost2 ? child1 : child2;
    }

    const i32 sibling = index;
    const i32 oldParent = nodes_[static_cast<usize>(sibling)].parent;
    const i32 newParent = AllocateNode();
    {
        TreeNode& parent = nodes_[static_cast<usize>(newParent)];
        parent.parent = oldParent;
        parent.userData = 0;
        parent.aabb = Combine(leafAabb, nodes_[static_cast<usize>(sibling)].aabb);
        parent.height = nodes_[static_cast<usize>(sibling)].height + 1;
        parent.child1 = sibling;
        parent.child2 = leaf;
    }
    nodes_[static_cast<usize>(sibling)].parent = newParent;
    nodes_[static_cast<usize>(leaf)].parent = newParent;

    if (oldParent != kNullNode) {
        if (nodes_[static_cast<usize>(oldParent)].child1 == sibling) {
            nodes_[static_cast<usize>(oldParent)].child1 = newParent;
        } else {
            nodes_[static_cast<usize>(oldParent)].child2 = newParent;
        }
    } else {
        root_ = newParent;
    }

    // Walk back to the root, rebalancing and refitting.
    index = nodes_[static_cast<usize>(leaf)].parent;
    while (index != kNullNode) {
        index = Balance(index);
        TreeNode& node = nodes_[static_cast<usize>(index)];
        const TreeNode& c1 = nodes_[static_cast<usize>(node.child1)];
        const TreeNode& c2 = nodes_[static_cast<usize>(node.child2)];
        node.height = 1 + std::max(c1.height, c2.height);
        node.aabb = Combine(c1.aabb, c2.aabb);
        index = node.parent;
    }
}

void AabbTree::RemoveLeaf(i32 leaf) {
    if (leaf == root_) {
        root_ = kNullNode;
        return;
    }

    const i32 parent = nodes_[static_cast<usize>(leaf)].parent;
    const i32 grandParent = nodes_[static_cast<usize>(parent)].parent;
    const i32 sibling = nodes_[static_cast<usize>(parent)].child1 == leaf
                            ? nodes_[static_cast<usize>(parent)].child2
                            : nodes_[static_cast<usize>(parent)].child1;

    if (grandParent == kNullNode) {
        root_ = sibling;
        nodes_[static_cast<usize>(sibling)].parent = kNullNode;
        FreeNode(parent);
        return;
    }

    if (nodes_[static_cast<usize>(grandParent)].child1 == parent) {
        nodes_[static_cast<usize>(grandParent)].child1 = sibling;
    } else {
        nodes_[static_cast<usize>(grandParent)].child2 = sibling;
    }
    nodes_[static_cast<usize>(sibling)].parent = grandParent;
    FreeNode(parent);

    i32 index = grandParent;
    while (index != kNullNode) {
        index = Balance(index);
        TreeNode& node = nodes_[static_cast<usize>(index)];
        const TreeNode& c1 = nodes_[static_cast<usize>(node.child1)];
        const TreeNode& c2 = nodes_[static_cast<usize>(node.child2)];
        node.aabb = Combine(c1.aabb, c2.aabb);
        node.height = 1 + std::max(c1.height, c2.height);
        index = node.parent;
    }
}

i32 AabbTree::Balance(i32 iA) {
    TreeNode& a = nodes_[static_cast<usize>(iA)];
    if (a.IsLeaf() || a.height < 2) {
        return iA;
    }

    const i32 iB = a.child1;
    const i32 iC = a.child2;
    const i32 balance = nodes_[static_cast<usize>(iC)].height -
                        nodes_[static_cast<usize>(iB)].height;

    // Rotate the taller branch up, promoting its taller grandchild. Symmetric in the two
    // directions, so it is written once and applied to whichever side is heavy.
    const auto rotate = [&](i32 iUp, i32 iDown) {
        TreeNode& up = nodes_[static_cast<usize>(iUp)];
        const i32 iF = up.child1;
        const i32 iG = up.child2;

        up.child1 = iA;
        up.parent = nodes_[static_cast<usize>(iA)].parent;
        nodes_[static_cast<usize>(iA)].parent = iUp;

        if (up.parent != kNullNode) {
            TreeNode& grand = nodes_[static_cast<usize>(up.parent)];
            if (grand.child1 == iA) {
                grand.child1 = iUp;
            } else {
                grand.child2 = iUp;
            }
        } else {
            root_ = iUp;
        }

        const bool promoteF =
            nodes_[static_cast<usize>(iF)].height > nodes_[static_cast<usize>(iG)].height;
        const i32 keep = promoteF ? iF : iG;
        const i32 demote = promoteF ? iG : iF;

        up.child2 = keep;
        // iA keeps the shorter grandchild in the slot the rotated branch vacated.
        if (nodes_[static_cast<usize>(iA)].child1 == iUp) {
            nodes_[static_cast<usize>(iA)].child1 = demote;
        } else {
            nodes_[static_cast<usize>(iA)].child2 = demote;
        }
        nodes_[static_cast<usize>(demote)].parent = iA;

        TreeNode& aRef = nodes_[static_cast<usize>(iA)];
        const TreeNode& down = nodes_[static_cast<usize>(iDown)];
        const TreeNode& demoted = nodes_[static_cast<usize>(demote)];
        aRef.aabb = Combine(down.aabb, demoted.aabb);
        aRef.height = 1 + std::max(down.height, demoted.height);

        TreeNode& upRef = nodes_[static_cast<usize>(iUp)];
        upRef.aabb = Combine(aRef.aabb, nodes_[static_cast<usize>(keep)].aabb);
        upRef.height = 1 + std::max(aRef.height, nodes_[static_cast<usize>(keep)].height);
        return iUp;
    };

    if (balance > 1) {
        return rotate(iC, iB);
    }
    if (balance < -1) {
        return rotate(iB, iC);
    }
    return iA;
}

}  // namespace snowball
