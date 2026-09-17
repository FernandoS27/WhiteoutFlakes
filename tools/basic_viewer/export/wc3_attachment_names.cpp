#include "wc3_attachment_names.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <string_view>
#include <variant>

namespace whiteout::flakes {

namespace {

/// Lower-cased, runs of blanks collapsed, ends trimmed.
std::string NormalizedWords(const std::string& name) {
    std::string out;
    bool blank = false;
    for (const char raw : name) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isspace(c) != 0) {
            blank = !out.empty();
            continue;
        }
        if (blank) {
            out += ' ';
            blank = false;
        }
        out += static_cast<char>(std::tolower(c));
    }
    return out;
}

/// Every word's first letter up, the rest down: `hand left` -> `Hand Left`.
std::string TitleCase(const std::string& words) {
    std::string out = words;
    bool start = true;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        c = static_cast<char>(start ? std::toupper(u) : std::tolower(u));
        start = c == ' ';
    }
    return out;
}

} // namespace

Wc3AttachmentName Wc3AttachmentNameToSc2(const std::string& name) {
    // The name without its `ref` suffix: ` ref`, ` - ref`, `ref01`, ` ref 01`.
    std::string base = NormalizedWords(name);
    const std::size_t ref = base.rfind("ref");
    if (ref != std::string::npos) {
        bool suffix = true;
        for (std::size_t i = ref + 3; i < base.size(); ++i) {
            suffix = suffix && (base[i] == ' ' || std::isdigit(static_cast<unsigned char>(base[i])));
        }
        const bool wordStart = ref == 0 || base[ref - 1] == ' ' || base[ref - 1] == '-';
        if (suffix && wordStart) {
            base.erase(ref);
            while (!base.empty() && (base.back() == ' ' || base.back() == '-')) {
                base.pop_back();
            }
        }
    }

    // Sprites are hardpoints: the numbered ones by position, the named ones by
    // what they are for. `Ref_Hardpoint NN` is the spelling 154 of Blizzard's
    // models share; their per-model hand choices are nothing the source says.
    constexpr const char* kOrdinals[] = {"first", "second", "third", "fourth", "fifth", "sixth"};
    if (base.rfind("sprite ", 0) == 0) {
        const std::string what = base.substr(7);
        for (std::size_t i = 0; i < std::size(kOrdinals); ++i) {
            if (what == kOrdinals[i]) {
                return {"Ref_Hardpoint 0" + std::to_string(i + 1), true};
            }
        }
        if (what == "rallypoint" || what == "ralypoint") {
            return {"Ref_RallyPoint", true};
        }
        if (what == "eattree") {
            return {"Ref_Hardpoint EatTree", true};
        }
        if (what == "medium" || what == "large" || what == "small") {
            return {"Ref_Hardpoint " + TitleCase(what), true};
        }
        return {"Ref_Hardpoint " + TitleCase(what), false};
    }

    // The anatomy every unit states, and its `alternate` twin.
    constexpr const char* kKnown[] = {"origin",
                                      "overhead",
                                      "chest",
                                      "head",
                                      "hand left",
                                      "hand right",
                                      "foot left",
                                      "foot right",
                                      "foot left rear",
                                      "foot right rear",
                                      "weapon",
                                      "weapon left",
                                      "weapon right",
                                      "chest mount",
                                      "chest mount left",
                                      "chest mount right",
                                      "chest mount rear",
                                      "head mount"};
    std::string anatomy = base;
    constexpr std::string_view kAlternate = " alternate";
    if (anatomy.size() > kAlternate.size() &&
        anatomy.compare(anatomy.size() - kAlternate.size(), kAlternate.size(), kAlternate) == 0) {
        anatomy.erase(anatomy.size() - kAlternate.size());
    }
    bool known = false;
    for (const char* k : kKnown) {
        known = known || anatomy == k;
    }
    return {"Ref_" + TitleCase(base), known};
}

Wc3TargetVolume Wc3TargetVolumeOf(const models::wem::Model& model) {
    namespace wem = models::wem;
    constexpr f32 kUnbounded = std::numeric_limits<f32>::max();
    Vector3f lo{kUnbounded, kUnbounded, kUnbounded};
    Vector3f hi{-kUnbounded, -kUnbounded, -kUnbounded};
    bool any = false;
    const auto grow = [&](const Vector3f& a, const Vector3f& b) {
        lo = Vector3f{(std::min)(lo.x, a.x), (std::min)(lo.y, a.y), (std::min)(lo.z, a.z)};
        hi = Vector3f{(std::max)(hi.x, b.x), (std::max)(hi.y, b.y), (std::max)(hi.z, b.z)};
        any = true;
    };
    for (const wem::Node& node : model.nodes.nodes) {
        const auto* payload = std::get_if<wem::CollisionPayload>(&node.payload);
        if (node.kind != wem::NodeKind::CollisionShape || payload == nullptr) {
            continue;
        }
        const wem::CollisionShapeDesc& shape = payload->shape;
        if (shape.kind == wem::CollisionShapeKind::Sphere) {
            const Vector3f& c = shape.sphere.center;
            const f32 r = shape.sphere.radius;
            grow(Vector3f{c.x - r, c.y - r, c.z - r}, Vector3f{c.x + r, c.y + r, c.z + r});
        } else if (shape.kind == wem::CollisionShapeKind::Box) {
            // A box's corners are relative to its pivot: 151 of 179 paired
            // boxes say so, none reads as absolute.
            const Vector3f& a = shape.box.minimum;
            const Vector3f& b = shape.box.maximum;
            const Vector3f& p = node.pivot;
            grow(Vector3f{(std::min)(a.x, b.x) + p.x, (std::min)(a.y, b.y) + p.y,
                          (std::min)(a.z, b.z) + p.z},
                 Vector3f{(std::max)(a.x, b.x) + p.x, (std::max)(a.y, b.y) + p.y,
                          (std::max)(a.z, b.z) + p.z});
        }
    }
    Wc3TargetVolume volume;
    if (!any) {
        return volume;
    }
    const Vector3f half{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f, (hi.z - lo.z) * 0.5f};
    volume.center = Vector3f{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    // 1.44 times the half extent in StarCraft II units (1/100 of Warcraft
    // III's), the axes swapped as the basis swaps them.
    constexpr f32 kScalePerHalfExtent = 0.0144f;
    volume.scale = Vector3f{kScalePerHalfExtent * half.y, kScalePerHalfExtent * half.x,
                            kScalePerHalfExtent * half.z};
    volume.fromCollision = true;
    return volume;
}

} // namespace whiteout::flakes
