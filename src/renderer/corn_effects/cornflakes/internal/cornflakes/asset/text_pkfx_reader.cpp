#include <cornflakes/asset/text_pkfx_reader.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>

#include <cctype>
#include <cstring>
#include <string_view>
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

bool isIdentifierStart(char c) noexcept {
    return (c == '_') || std::isalpha(static_cast<unsigned char>(c)) != 0;
}

bool isIdentifierPart(char c) noexcept {
    return (c == '_') || std::isalnum(static_cast<unsigned char>(c)) != 0;
}

class Scanner {
public:
    explicit Scanner(std::string_view text) noexcept : m_text(text) {}

    bool eof() const noexcept {
        return m_pos >= m_text.size();
    }

    char peek(std::size_t ahead = 0) const noexcept {
        return (m_pos + ahead < m_text.size()) ? m_text[m_pos + ahead] : '\0';
    }

    char take() noexcept {
        return eof() ? '\0' : m_text[m_pos++];
    }

    std::size_t pos() const noexcept {
        return m_pos;
    }

    void setPos(std::size_t p) noexcept {
        m_pos = p;
    }

    void skipBom() noexcept {
        if (m_text.size() >= 3 && static_cast<u8>(m_text[0]) == 0xEFU &&
            static_cast<u8>(m_text[1]) == 0xBBU && static_cast<u8>(m_text[2]) == 0xBFU) {
            m_pos = 3;
        }
    }

    void skipShebang() noexcept {
        if (peek() == '#' && peek(1) == '!') {
            while (!eof() && take() != '\n') {
            }
        }
    }

    void skipWsAndComments() noexcept {
        for (;;) {
            while (!eof() && std::isspace(static_cast<unsigned char>(peek())) != 0) {
                ++m_pos;
            }
            if (peek() == '/' && peek(1) == '/') {
                while (!eof() && take() != '\n') {
                }
                continue;
            }
            if (peek() == '/' && peek(1) == '*') {
                m_pos += 2;
                while (!eof() && !(peek() == '*' && peek(1) == '/')) {
                    ++m_pos;
                }
                if (!eof()) {
                    m_pos += 2;
                }
                continue;
            }
            return;
        }
    }

    std::string_view readIdentifier() noexcept {
        const std::size_t start = m_pos;
        if (eof() || !isIdentifierStart(peek())) {
            return {};
        }
        ++m_pos;
        while (!eof() && isIdentifierPart(peek())) {
            ++m_pos;
        }
        return m_text.substr(start, m_pos - start);
    }

    std::string_view readUidToken() noexcept {
        if (peek() != '$') {
            return {};
        }
        const std::size_t start = m_pos;
        ++m_pos;
        while (!eof() && std::isxdigit(static_cast<unsigned char>(peek())) != 0) {
            ++m_pos;
        }
        if (m_pos - start <= 1) {
            m_pos = start;
            return {};
        }
        return m_text.substr(start, m_pos - start);
    }

private:
    std::string_view m_text;
    std::size_t m_pos = 0;
};

std::string_view internIntoArena(std::string_view src, IArena& arena) {
    if (src.empty()) {
        return {};
    }
    auto* p = static_cast<char*>(arena.allocate(src.size(), 1));
    std::memcpy(p, src.data(), src.size());
    return {p, src.size()};
}

bool readValueUntilSemicolon(Scanner& scan, std::string_view& outValue,
                             std::string_view source) noexcept {
    scan.skipWsAndComments();
    const std::size_t start = scan.pos();
    int braceDepth = 0;
    bool inString = false;

    while (!scan.eof()) {
        const char c = scan.peek();
        if (inString) {
            if (c == '\\' && scan.peek(1) != '\0') {
                scan.take();
                scan.take();
                continue;
            }
            if (c == '"') {
                inString = false;
            }
            scan.take();
            continue;
        }
        if (c == '"') {
            inString = true;
            scan.take();
            continue;
        }
        if (c == '{') {
            ++braceDepth;
            scan.take();
            continue;
        }
        if (c == '}') {
            if (braceDepth == 0) {
                return false;
            }
            --braceDepth;
            scan.take();
            continue;
        }
        if (c == ';' && braceDepth == 0) {
            const std::size_t end = scan.pos();
            scan.take();
            std::string_view raw = source.substr(start, end - start);
            while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())) != 0) {
                raw.remove_prefix(1);
            }
            while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())) != 0) {
                raw.remove_suffix(1);
            }
            outValue = raw;
            return true;
        }
        scan.take();
    }
    return false;
}

std::string_view unquote(std::string_view value) noexcept {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

BakerGenerator parseGeneratorToken(std::string_view token, bool& ok) noexcept {
    char prefix[4] = {0, 0, 0, 0};
    for (std::size_t i = 0; i < token.size() && i < 4; ++i) {
        prefix[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(token[i])));
    }
    if (std::memcmp(prefix, "BAKE", 4) == 0) {
        ok = true;
        return BakerGenerator::Baker;
    }
    if (std::memcmp(prefix, "EDIT", 4) == 0) {
        ok = true;
        return BakerGenerator::Editor;
    }
    ok = false;
    return BakerGenerator::Editor;
}

bool parseVersionToken(std::string_view token, AssetVersion& out) noexcept {

    u32 parts[4] = {0, 0, 0, 0};
    std::size_t field = 0;
    u32 cur = 0;
    bool anyDigit = false;
    for (std::size_t i = 0; i <= token.size(); ++i) {
        const char c = (i < token.size()) ? token[i] : '.';
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            cur = cur * 10U + static_cast<u32>(c - '0');
            anyDigit = true;
        } else if (c == '.') {
            if (!anyDigit) {
                return false;
            }
            if (field < 4) {
                parts[field++] = cur;
            }
            cur = 0;
            anyDigit = false;
        } else {
            return false;
        }
    }
    out.major = static_cast<u16>(parts[0]);
    out.minor = static_cast<u16>(parts[1]);
    out.patch = static_cast<u16>(parts[2]);
    out.revisionId = parts[3];
    return true;
}

} // namespace

bool TextPkfxReader::canHandle(const BakedSource& src) const noexcept {

    if (src.bytes.empty()) {
        return false;
    }

    if (src.bytes.size() >= 3 && static_cast<u8>(src.bytes[0]) == 0x11U &&
        static_cast<u8>(src.bytes[1]) == 0x0BU && static_cast<u8>(src.bytes[2]) == 0x00U) {
        return false;
    }

    const auto asText =
        std::string_view{reinterpret_cast<const char*>(src.bytes.data()), src.bytes.size()};
    Scanner scan(asText);
    scan.skipBom();
    scan.skipShebang();
    scan.skipWsAndComments();
    if (scan.eof()) {
        return false;
    }

    const char c = scan.peek();
    return isIdentifierStart(c);
}

std::optional<EffectAssetModel> TextPkfxReader::read(const BakedSource& src, IArena& arena,
                                                     IssueBag& issues) {
    if (src.bytes.empty()) {
        issues.push(assetFatal(issues::asset::kTextEmpty, "text PKFX source is empty"));
        return std::nullopt;
    }

    const auto text =
        std::string_view{reinterpret_cast<const char*>(src.bytes.data()), src.bytes.size()};
    Scanner scan(text);
    scan.skipBom();
    scan.skipShebang();

    EffectAssetModel model;
    model.format = AssetFormat::TextPkfx;
    model.generator = BakerGenerator::Editor;

    std::vector<AssetObject> owned;

    for (;;) {
        scan.skipWsAndComments();
        if (scan.eof()) {
            break;
        }

        const std::size_t headStart = scan.pos();
        const std::string_view head = scan.readIdentifier();
        if (head.empty()) {
            issues.push(assetFatal(issues::asset::kTextUnterminated,
                                   "text PKFX: expected identifier at top level"));
            return std::nullopt;
        }

        scan.skipWsAndComments();

        if (scan.peek() == '=') {
            scan.take();
            std::string_view value;
            if (!readValueUntilSemicolon(scan, value, text)) {
                issues.push(assetFatal(issues::asset::kTextUnterminated,
                                       "text PKFX: preamble field missing terminating ';'"));
                return std::nullopt;
            }
            if (head == "Version") {
                AssetVersion v;
                if (!parseVersionToken(unquote(value), v)) {
                    issues.push(assetFatal(issues::asset::kTextBadVersion,
                                           "text PKFX: malformed Version token"));
                    return std::nullopt;
                }
                model.version = v;
            } else if (head == "Generator") {
                bool ok = false;
                const BakerGenerator gen = parseGeneratorToken(unquote(value), ok);
                if (!ok) {
                    issues.push(assetFatal(issues::asset::kTextBadGenerator,
                                           "text PKFX: Generator is neither EDITOR nor BAKER"));
                    return std::nullopt;
                }
                model.generator = gen;
            }

            continue;
        }

        const std::string_view typeName = head;
        const std::string_view uid = scan.readUidToken();
        if (uid.empty()) {

            scan.setPos(headStart);
            while (!scan.eof() && scan.take() != '\n') {
            }
            continue;
        }

        scan.skipWsAndComments();
        if (scan.peek() != '{') {
            issues.push(assetFatal(issues::asset::kTextUnterminated,
                                   "text PKFX: object header missing opening '{'"));
            return std::nullopt;
        }
        scan.take();

        AssetObject obj;
        obj.type = internIntoArena(typeName, arena);
        obj.uid = internIntoArena(uid, arena);

        const bool isNodeGraph = typeName == "CParticleNodeGraph";
        const bool isEffect = typeName == "CParticleEffect";

        for (;;) {
            scan.skipWsAndComments();
            if (scan.peek() == '}') {
                scan.take();
                break;
            }
            if (scan.eof()) {
                issues.push(assetFatal(issues::asset::kTextUnterminated,
                                       "text PKFX: object body missing closing '}'"));
                return std::nullopt;
            }
            const std::string_view fieldName = scan.readIdentifier();
            if (fieldName.empty()) {

                scan.take();
                continue;
            }
            scan.skipWsAndComments();
            if (scan.peek() != '=') {

                continue;
            }
            scan.take();
            std::string_view value;
            if (!readValueUntilSemicolon(scan, value, text)) {
                issues.push(assetFatal(issues::asset::kTextUnterminated,
                                       "text PKFX: field missing terminating ';'"));
                return std::nullopt;
            }
            if (isNodeGraph && fieldName == "CustomName") {
                obj.customName = internIntoArena(unquote(value), arena);
            }
        }

        if (isEffect && model.rootEffectUid.empty()) {
            model.rootEffectUid = obj.uid;
        }
        if (isNodeGraph && obj.customName == "Root" && model.rootLayerUid.empty()) {
            model.rootLayerUid = obj.uid;
        }

        owned.push_back(obj);
    }

    const std::span<AssetObject> view = arenaArray<AssetObject>(arena, owned.size());
    for (std::size_t i = 0; i < owned.size(); ++i) {
        view[i] = owned[i];
    }
    model.objects = view;

    return model;
}

} // namespace whiteout::cornflakes
