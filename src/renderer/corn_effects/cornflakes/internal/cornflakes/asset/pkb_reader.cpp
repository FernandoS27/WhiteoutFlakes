#include <cornflakes/asset/field_defs.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/asset/pkb_reader.hpp>

#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

namespace {

Issue assetFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Asset;
    issue.code = code;
    issue.message = message;
    return issue;
}

u8 readU8(const std::byte* p) noexcept {
    return static_cast<u8>(p[0]);
}

u16 readU16Le(const std::byte* p) noexcept {
    return static_cast<u16>(static_cast<u8>(p[0]) |
                            (static_cast<u16>(static_cast<u8>(p[1])) << 8U));
}

u32 readU32Le(const std::byte* p) noexcept {
    return static_cast<u32>(static_cast<u8>(p[0])) |
           (static_cast<u32>(static_cast<u8>(p[1])) << 8U) |
           (static_cast<u32>(static_cast<u8>(p[2])) << 16U) |
           (static_cast<u32>(static_cast<u8>(p[3])) << 24U);
}

struct PString {
    std::string_view view;
    std::size_t bytesConsumed = 0;
};

PString readPString(const std::byte* base, std::size_t off, std::size_t endOff) noexcept {
    PString out;
    if (off >= endOff) {
        return out;
    }
    const u8 b0 = readU8(base + off);
    std::size_t headerLen = 1;
    std::size_t payloadLen = 0;
    if ((b0 & 0x80U) == 0U) {
        payloadLen = b0;
    } else {
        if (off + 1 >= endOff) {
            return out;
        }
        const u8 b1 = readU8(base + off + 1);
        payloadLen = (static_cast<std::size_t>(b0 & 0x7FU) << 8U) | b1;
        headerLen = 2;
    }
    if (off + headerLen + payloadLen > endOff) {
        return out;
    }
    const auto* charPtr = reinterpret_cast<const char*>(base + off + headerLen);
    out.view = std::string_view{charPtr, payloadLen};
    out.bytesConsumed = headerLen + payloadLen;
    return out;
}

bool isArrayType(std::string_view t) noexcept {
    return t.size() >= 2 && t[t.size() - 2] == '[' && t[t.size() - 1] == ']';
}

std::string_view stripArraySuffix(std::string_view t) noexcept {
    if (isArrayType(t)) {
        return t.substr(0, t.size() - 2);
    }
    return t;
}

std::size_t scalarBytes(std::string_view t) noexcept {
    if (t == "bool") {
        return 1;
    }
    if (t == "int" || t == "uint" || t == "float" || t == "link" || t == "string" ||
        t == "string_unicode" || t == "unknown") {
        return 4;
    }
    if (t == "string_localized") {
        return 0;
    }

    if (t.size() >= 4 && t.size() <= 6) {
        const char tail = t.back();
        if (tail == '2' || tail == '3' || tail == '4') {
            const std::size_t dim = static_cast<std::size_t>(tail - '0');
            const auto prefix = t.substr(0, t.size() - 1);
            if (prefix == "bool") {
                return dim;
            }
            if (prefix == "int" || prefix == "uint" || prefix == "float") {
                return dim * 4;
            }
        }
    }
    return 0;
}

std::size_t elementBytesAt(std::string_view baseType, const std::byte* body, std::size_t off,
                           std::size_t end) noexcept {
    if (baseType == "string_localized") {
        if (off + 4 > end) {
            return 0;
        }
        const u32 n = readU32Le(body + off);
        const std::size_t total = 4U + static_cast<std::size_t>(n) * 8U;
        if (off + total > end) {
            return 0;
        }
        return total;
    }
    return scalarBytes(baseType);
}

std::size_t resolveFieldBytes(std::string_view t, const std::byte* body, std::size_t off,
                              std::size_t end) noexcept {
    if (!isArrayType(t)) {
        return elementBytesAt(t, body, off, end);
    }

    if (off + 4 > end) {
        return 0;
    }
    const u32 count = readU32Le(body + off);
    const std::string_view base = stripArraySuffix(t);
    std::size_t total = 4U;
    std::size_t cursor = off + 4U;
    for (u32 i = 0; i < count; ++i) {
        const std::size_t es = elementBytesAt(base, body, cursor, end);
        if (es == 0) {
            return 0;
        }
        cursor += es;
        total += es;
    }
    return total;
}

std::string_view resolveStringField(std::string_view t, const std::byte* body, std::size_t off,
                                    std::size_t end, std::span<const std::string_view> strings,
                                    IArena& arena) noexcept;

constexpr u32 kStringIndexInvalid = 0xFFFFFFFFU;

std::string_view internIntoArena(std::string_view src, IArena& arena) {
    if (src.empty()) {
        return {};
    }
    auto* p = static_cast<char*>(arena.allocate(src.size(), 1));
    std::memcpy(p, src.data(), src.size());
    return {p, src.size()};
}

std::string_view resolveStringField(std::string_view t, const std::byte* body, std::size_t off,
                                    std::size_t end, std::span<const std::string_view> strings,
                                    IArena& arena) noexcept {
    if (t != "string" && t != "string_unicode") {
        return {};
    }
    if (off + 4 > end) {
        return {};
    }
    const u32 idx = readU32Le(body + off);
    if (idx == kStringIndexInvalid) {
        return {};
    }
    if (idx >= strings.size()) {
        return {};
    }
    return internIntoArena(strings[idx], arena);
}

struct ObjectHeader {
    u8 flags = 0;
    u32 handlerId = 0;
    u16 fieldCount = 0;
};

bool readObjectHeader(const std::byte* base, std::size_t bodyOff, std::size_t bodyEnd,
                      ObjectHeader& out) noexcept {
    constexpr std::size_t kHeaderBytes = 1U + 4U + 2U;
    if (bodyOff + kHeaderBytes > bodyEnd) {
        return false;
    }
    out.flags = readU8(base + bodyOff);
    out.handlerId = readU32Le(base + bodyOff + 1);
    out.fieldCount = readU16Le(base + bodyOff + 5);
    return true;
}

std::span<const FieldRaw> decodeObjectFields(const HandlerDef* handler, const std::byte* body,
                                             std::size_t bodyOff, std::size_t bodyEnd,
                                             u16 fieldCount,
                                             std::span<const std::string_view> strings,
                                             IArena& arena, IssueBag& issues) {
    if (handler == nullptr || fieldCount == 0) {
        return {};
    }

    constexpr std::size_t kObjectHeaderBytes = 7U;
    std::size_t off = bodyOff + kObjectHeaderBytes;

    std::vector<FieldRaw> decoded;
    decoded.reserve(fieldCount);

    for (u16 i = 0; i < fieldCount; ++i) {
        if (off + 2 > bodyEnd) {
            issues.push(assetFatal(issues::asset::kPkbTooShort,
                                   "PKB object body truncated reading fieldId"));
            return {};
        }
        const u16 fieldId = readU16Le(body + off);
        off += 2;

        if (fieldId >= handler->fields.size()) {

            return {};
        }

        const auto& fd = handler->fields[fieldId];
        const std::size_t valueStart = off;
        const std::size_t consumed = resolveFieldBytes(fd.type, body, off, bodyEnd);
        if (consumed == 0) {

            issues.push(assetFatal(issues::asset::kPkbTooShort,
                                   "PKB field decode failed (unsupported type or truncated)"));
            return {};
        }
        if (off + consumed > bodyEnd) {
            issues.push(
                assetFatal(issues::asset::kPkbTooShort, "PKB field body extends past object end"));
            return {};
        }

        FieldRaw raw;
        raw.name = fd.name;
        raw.type = fd.type;
        raw.bytes = std::span<const std::byte>{body + valueStart, consumed};
        if (fd.type == "string" || fd.type == "string_unicode") {
            raw.stringValue =
                resolveStringField(fd.type, body, valueStart, bodyEnd, strings, arena);
        }
        decoded.push_back(raw);

        off += consumed;
    }

    const auto view = arenaArray<FieldRaw>(arena, decoded.size());
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        view[i] = decoded[i];
    }
    return view;
}

} // namespace

bool PkbReader::canHandle(const BakedSource& src) const noexcept {
    if (src.bytes.size() < 4) {
        return false;
    }
    const auto* data = src.bytes.data();
    return readU8(data + 0) == kMagicPrefix[0] && readU8(data + 1) == kMagicPrefix[1] &&
           readU8(data + 2) == kMagicPrefix[2];
}

std::optional<EffectAssetModel> PkbReader::read(const BakedSource& src, IArena& arena,
                                                IssueBag& issues) {
    if (src.bytes.size() < kHeaderBytes) {
        issues.push(
            assetFatal(issues::asset::kPkbTooShort, "PKB source shorter than 28-byte header"));
        return std::nullopt;
    }

    const auto* data = src.bytes.data();
    const std::size_t totalSize = src.bytes.size();

    if (readU8(data + 0) != kMagicPrefix[0] || readU8(data + 1) != kMagicPrefix[1] ||
        readU8(data + 2) != kMagicPrefix[2]) {
        issues.push(
            assetFatal(issues::asset::kPkbBadMagic, "PKB magic bytes do not match 0x11 0x0B 0x00"));
        return std::nullopt;
    }

    const u8 versionByte = readU8(data + 3);
    for (const u8 stale : kMagicRejectedVersions) {
        if (versionByte == stale) {
            issues.push(assetFatal(issues::asset::kPkbStaleVersion,
                                   "PKB version byte rejected; please rebake"));
            return std::nullopt;
        }
    }
    if (versionByte != kMagicCurrentVersion) {
        issues.push(
            assetFatal(issues::asset::kPkbStaleVersion, "PKB version byte is not 0xCA (current)"));
        return std::nullopt;
    }

    EffectAssetModel model;
    model.format = AssetFormat::Pkb;

    model.version.major = readU8(data + 4);
    model.version.minor = readU8(data + 5);
    model.version.patch = readU8(data + 6);
    model.version.revisionId = readU32Le(data + 8);

    const u8 gen = readU8(data + kGeneratorByteOffset);
    if (gen == static_cast<u8>(BakerGenerator::Editor)) {
        model.generator = BakerGenerator::Editor;
    } else if (gen == static_cast<u8>(BakerGenerator::Baker)) {
        model.generator = BakerGenerator::Baker;
    } else {
        issues.push(assetFatal(issues::asset::kPkbGenerator,
                               "PKB Generator byte is neither EDITOR (0) nor BAKER (1)"));
        return std::nullopt;
    }

    const u32 objectCount = readU32Le(data + 12);
    const u32 objectTypeCount = readU32Le(data + 16);
    const u32 stringTableOffset = readU32Le(data + 20);

    if (stringTableOffset >= totalSize) {
        issues.push(
            assetFatal(issues::asset::kPkbTooShort, "PKB stringTableOffset is past end of buffer"));
        return std::nullopt;
    }

    std::vector<std::string_view> strings;
    {
        std::size_t off = stringTableOffset;
        if (off + 4 > totalSize) {
            issues.push(assetFatal(issues::asset::kPkbTooShort,
                                   "PKB string-table count u32 runs past end"));
            return std::nullopt;
        }
        const u32 stringCount = readU32Le(data + off);
        off += 4;
        strings.reserve(stringCount);
        for (u32 i = 0; i < stringCount; ++i) {
            const auto ps = readPString(data, off, totalSize);
            if (ps.bytesConsumed == 0 && ps.view.empty()) {
                issues.push(
                    assetFatal(issues::asset::kPkbTooShort, "PKB PString runs past end of buffer"));
                return std::nullopt;
            }
            strings.push_back(ps.view);
            off += ps.bytesConsumed;
        }
    }

    std::vector<std::string_view> typeNames;
    {
        std::size_t off = kHeaderBytes;
        const std::size_t typeTableEnd = off + (static_cast<std::size_t>(objectTypeCount) * 8U);
        if (typeTableEnd > totalSize) {
            issues.push(
                assetFatal(issues::asset::kPkbTooShort, "PKB type table runs past end of buffer"));
            return std::nullopt;
        }
        typeNames.reserve(objectTypeCount);
        for (u32 i = 0; i < objectTypeCount; ++i) {
            const u32 nameId = readU32Le(data + off);
            off += 4;

            off += 4;
            if (nameId >= strings.size()) {
                issues.push(assetFatal(issues::asset::kPkbTooShort,
                                       "PKB type-table entry references string id out of range"));
                return std::nullopt;
            }
            typeNames.push_back(strings[nameId]);
        }
    }

    const std::span<const std::string_view> stringSpan{strings.data(), strings.size()};

    std::vector<AssetObject> owned;
    owned.reserve(objectCount);
    {
        std::size_t off = kHeaderBytes + (static_cast<std::size_t>(objectTypeCount) * 8U);
        for (u32 i = 0; i < objectCount; ++i) {
            if (off + 4 > stringTableOffset) {
                issues.push(assetFatal(issues::asset::kPkbTooShort,
                                       "PKB object header runs into string table"));
                return std::nullopt;
            }
            const u32 bodySize = readU32Le(data + off);
            off += 4;
            const std::size_t bodyOff = off;
            const std::size_t bodyEnd = bodyOff + bodySize;
            if (bodyEnd > stringTableOffset) {
                issues.push(assetFatal(issues::asset::kPkbTooShort,
                                       "PKB object body runs into string table"));
                return std::nullopt;
            }

            ObjectHeader hdr{};
            if (!readObjectHeader(data, bodyOff, bodyEnd, hdr)) {
                issues.push(assetFatal(issues::asset::kPkbTooShort, "PKB object header truncated"));
                return std::nullopt;
            }
            if (hdr.handlerId >= typeNames.size()) {
                issues.push(assetFatal(issues::asset::kPkbTooShort,
                                       "PKB object handlerId is out of type-table range"));
                return std::nullopt;
            }

            AssetObject obj;
            obj.type = internIntoArena(typeNames[hdr.handlerId], arena);

            char uidBuf[10];
            const u32 uidValue = i + 1U;
            uidBuf[0] = '$';
            for (int d = 0; d < 8; ++d) {
                const u32 nybble = (uidValue >> ((7 - d) * 4)) & 0xFU;
                uidBuf[1 + d] = static_cast<char>(nybble < 10 ? '0' + nybble : 'A' + (nybble - 10));
            }
            obj.uid = internIntoArena(std::string_view{uidBuf, sizeof(uidBuf) - 1}, arena);

            const HandlerDef* handler = findHandlerDef(typeNames[hdr.handlerId]);
            obj.fields = decodeObjectFields(handler, data, bodyOff, bodyEnd, hdr.fieldCount,
                                            stringSpan, arena, issues);
            if (issues.hasFatal()) {
                return std::nullopt;
            }

            for (const auto& f : obj.fields) {
                if (f.name == "CustomName" && (f.type == "string" || f.type == "string_unicode")) {
                    obj.customName = f.stringValue;
                    break;
                }
            }

            off = bodyEnd;

            if (typeNames[hdr.handlerId] == "CParticleEffect" && model.rootEffectUid.empty()) {
                model.rootEffectUid = obj.uid;
            }
            if (typeNames[hdr.handlerId] == "CParticleNodeGraph" && obj.customName == "Root" &&
                model.rootLayerUid.empty()) {
                model.rootLayerUid = obj.uid;
            }

            owned.push_back(obj);
        }
    }

    const auto view = arenaArray<AssetObject>(arena, owned.size());
    for (std::size_t i = 0; i < owned.size(); ++i) {
        view[i] = owned[i];
    }
    model.objects = view;

    return model;
}

} // namespace whiteout::cornflakes
