#include <cornflakes/vm/bytecode_decoder.hpp>

#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>

#include <cstring>
#include <vector>

namespace whiteout::cornflakes {

namespace {

inline constexpr std::size_t kOpcodeByteSize = sizeof(u8);
inline constexpr std::size_t kVecSwizzleMaskBytes = 3U;
inline constexpr std::size_t kFunctionCallArgEntrySize = sizeof(u8) + sizeof(u32);
inline constexpr std::size_t kFunctionCallHeaderSize =
    kOpcodeByteSize + sizeof(u8) + sizeof(u16) + sizeof(u16) + sizeof(u8) + sizeof(u32);

Issue vmFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Vm;
    issue.code = code;
    issue.message = message;
    return issue;
}

u16 readU16Le(const u8* p) noexcept {
    return static_cast<u16>(static_cast<u16>(p[0]) | (static_cast<u16>(p[1]) << 8U));
}

u32 readU32Le(const u8* p) noexcept {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8U) |
           (static_cast<u32>(p[2]) << 16U) | (static_cast<u32>(p[3]) << 24U);
}

// Sequential reader over the bytecode stream. Each opcode handler positions
// it past the opcode byte, reserves the payload up-front via ensure(), then
// walks operands in order. Replaces the manual `off + kFooOffset` arithmetic
// the per-opcode case blocks used to carry.
class Cursor {
public:
    Cursor(const u8* base, std::size_t off, std::size_t end, IssueBag& issues) noexcept
        : base_(base), off_(off), end_(end), issues_(&issues) {}

    std::size_t offset() const noexcept { return off_; }

    bool ensure(std::size_t need, std::string_view what) noexcept {
        if (off_ + need > end_) {
            issues_->push(vmFatal(issues::vm::kOperandCount, what));
            return false;
        }
        return true;
    }

    u8 readU8() noexcept {
        const u8 v = base_[off_];
        off_ += sizeof(u8);
        return v;
    }
    u16 readU16() noexcept {
        const u16 v = readU16Le(base_ + off_);
        off_ += sizeof(u16);
        return v;
    }
    u32 readU32() noexcept {
        const u32 v = readU32Le(base_ + off_);
        off_ += sizeof(u32);
        return v;
    }
    u32 readU24() noexcept {
        const u32 v = static_cast<u32>(base_[off_]) |
                      (static_cast<u32>(base_[off_ + 1U]) << 8U) |
                      (static_cast<u32>(base_[off_ + 2U]) << 16U);
        off_ += kVecSwizzleMaskBytes;
        return v;
    }
    void skip(std::size_t n) noexcept {
        off_ += n;
    }

private:
    const u8* base_;
    std::size_t off_;
    std::size_t end_;
    IssueBag* issues_;
};

std::span<const u32> emitExtra(const std::vector<u32>& src, IArena& arena) {
    if (src.empty()) {
        return {};
    }
    auto dst = arenaArray<u32>(arena, src.size());
    std::memcpy(dst.data(), src.data(), src.size() * sizeof(u32));
    return std::span<const u32>{dst.data(), dst.size()};
}

// ----- Generic operand-shape decoders -----------------------------------
//
// Most opcodes share one of two payloads:
//   - decodeU32s<N>:        [u32]*N         (e.g. Select, Madd, Broadcast)
//   - decodeTaggedU32s<N>:  [u8] [u32]*N    (e.g. MathOp, MathFunc1/2/3)
// `operands[0]` carries the tag for the tagged form; the u32s land in
// operands[tag ? 1 : 0..N-1].

template <std::size_t U32Count>
bool decodeU32s(Cursor& c, CBEMInstruction& ins, std::string_view what) noexcept {
    if (!c.ensure(U32Count * sizeof(u32), what)) {
        return false;
    }
    for (std::size_t i = 0; i < U32Count; ++i) {
        ins.operands[i] = c.readU32();
    }
    ins.operandCount = static_cast<u8>(U32Count);
    return true;
}

template <std::size_t U32Count>
bool decodeTaggedU32s(Cursor& c, CBEMInstruction& ins, std::string_view what) noexcept {
    if (!c.ensure(sizeof(u8) + U32Count * sizeof(u32), what)) {
        return false;
    }
    ins.operands[0] = c.readU8();
    for (std::size_t i = 0; i < U32Count; ++i) {
        ins.operands[i + 1U] = c.readU32();
    }
    ins.operandCount = static_cast<u8>(U32Count + 1U);
    return true;
}

// ----- Bespoke decoders for the structurally unique opcodes -------------

bool decodeLoadStoreExternal(Cursor& c, CBEMInstruction& ins) noexcept {
    if (!c.ensure(sizeof(u32) + sizeof(u16), "IR: LoadExternal/StoreToExternal truncated")) {
        return false;
    }
    ins.operands[0] = c.readU32();
    ins.operands[1] = c.readU16();
    ins.operandCount = 2;
    return true;
}

bool decodeVecCtor(Cursor& c, CBEMInstruction& ins, IArena& arena) noexcept {
    if (!c.ensure(sizeof(u8) + sizeof(u32), "IR: VecCtor header truncated")) {
        return false;
    }
    constexpr u32 kSrcCountBias = 1U;
    const u8 argc = c.readU8();
    const u32 dst = c.readU32();
    const u32 srcCount = static_cast<u32>(argc) + kSrcCountBias;
    if (!c.ensure(srcCount * sizeof(u32), "IR: VecCtor sources truncated")) {
        return false;
    }
    ins.operands[0] = argc;
    ins.operands[1] = dst;
    ins.operandCount = 2;
    std::vector<u32> srcs;
    srcs.reserve(srcCount);
    for (u32 i = 0; i < srcCount; ++i) {
        srcs.push_back(c.readU32());
    }
    ins.extraOperands = emitExtra(srcs, arena);
    return true;
}

bool decodeVecSwizzle(Cursor& c, CBEMInstruction& ins) noexcept {
    if (!c.ensure(kVecSwizzleMaskBytes + 2U * sizeof(u32), "IR: VecSwizzle truncated")) {
        return false;
    }
    ins.operands[0] = c.readU24();
    ins.operands[1] = c.readU32();
    ins.operands[2] = c.readU32();
    ins.operandCount = 3;
    return true;
}

bool decodeExternalClear(Cursor& c, CBEMInstruction& ins) noexcept {
    if (!c.ensure(sizeof(u16), "CBEM: ExternalClear truncated")) {
        return false;
    }
    ins.operands[0] = c.readU16();
    ins.operandCount = 1;
    return true;
}

bool decodeFunctionCall(Cursor& c, CBEMInstruction& ins, IArena& arena) noexcept {
    constexpr std::size_t kHeaderPayload = kFunctionCallHeaderSize - kOpcodeByteSize;
    if (!c.ensure(kHeaderPayload, "IR: FunctionCall header truncated")) {
        return false;
    }
    const u8 flags = c.readU8();
    const u16 objSlotRaw = c.readU16();
    const u16 extFunc = c.readU16();
    const u8 argc = c.readU8();
    const u32 retReg = c.readU32();
    const std::size_t argsBytes = static_cast<std::size_t>(argc) * kFunctionCallArgEntrySize;
    if (!c.ensure(argsBytes, "IR: FunctionCall args truncated")) {
        return false;
    }
    ins.operands[0] = flags;
    ins.operands[1] = static_cast<u32>(static_cast<i16>(objSlotRaw));
    ins.operands[2] = extFunc;
    ins.operands[3] = argc;
    ins.operands[4] = retReg;
    ins.operandCount = 5;

    constexpr std::size_t kArgFieldsPerEntry = 2U;
    std::vector<u32> args;
    args.reserve(static_cast<std::size_t>(argc) * kArgFieldsPerEntry);
    for (u8 i = 0; i < argc; ++i) {
        args.push_back(c.readU8());
        args.push_back(c.readU32());
    }
    ins.extraOperands = emitExtra(args, arena);
    return true;
}

bool decodeFunctionProlog(Cursor& c, CBEMInstruction& ins) noexcept {
    if (!c.ensure(sizeof(u8), "CBEM: FunctionProlog truncated")) {
        return false;
    }
    c.skip(sizeof(u8));
    ins.operandCount = 0;
    return true;
}

// Dispatches `c` and `ins` to the per-opcode decoder. Returns false on
// truncation (the issue is already pushed). No-payload opcodes (Nop,
// FunctionEpilog) leave the cursor at the post-opcode position.
bool decodeOne(Opcode opcode, Cursor& c, CBEMInstruction& ins, IArena& arena,
               IssueBag& issues) noexcept {
    switch (opcode) {
    case Opcode::Nop:
    case Opcode::FunctionEpilog:
        ins.operandCount = 0;
        return true;

    case Opcode::LoadExternal:
    case Opcode::StoreToExternal:
        return decodeLoadStoreExternal(c, ins);

    case Opcode::Reinterpret:
    case Opcode::TypeConverter:
        return decodeU32s<2>(c, ins, "IR: Reinterpret/TypeConverter truncated");

    case Opcode::VecCtor:
        return decodeVecCtor(c, ins, arena);
    case Opcode::VecSwizzle:
        return decodeVecSwizzle(c, ins);

    case Opcode::MathOp:
        return decodeTaggedU32s<3>(c, ins, "IR: MathOp truncated");
    case Opcode::MathFunc1:
        return decodeTaggedU32s<2>(c, ins, "IR: MathFunc1 truncated");
    case Opcode::MathFunc2:
        return decodeTaggedU32s<3>(c, ins, "IR: MathFunc2 truncated");
    case Opcode::MathFunc3:
        return decodeTaggedU32s<4>(c, ins, "IR: MathFunc3 truncated");

    case Opcode::Select:
        return decodeU32s<4>(c, ins, "IR: Select truncated");
    case Opcode::FunctionCall:
        return decodeFunctionCall(c, ins, arena);

    case Opcode::ExternalClear:
        return decodeExternalClear(c, ins);
    case Opcode::Broadcast:
        return decodeU32s<2>(c, ins, "CBEM: Broadcast truncated");
    case Opcode::MathOpCMeta:
        return decodeTaggedU32s<3>(c, ins, "CBEM: MathOp truncated");
    case Opcode::MathOpAdd:
    case Opcode::MathOpSub:
    case Opcode::MathOpMul:
    case Opcode::MathOpDiv:
        return decodeU32s<3>(c, ins, "CBEM: MathOp* truncated");
    case Opcode::Madd:
        return decodeU32s<4>(c, ins, "CBEM: Madd truncated");
    case Opcode::IDivMulInv:
        return decodeU32s<5>(c, ins, "CBEM: IDivMulInv truncated");
    case Opcode::FunctionProlog:
        return decodeFunctionProlog(c, ins);
    }
    issues.push(vmFatal(issues::vm::kUnknownOpcode, "bytecode decoder: unknown opcode in stream"));
    return false;
}

} // namespace

DecodedProgram decodeBytecodeStream(std::span<const u8> bytes, IArena& arena, IssueBag& issues) {
    std::vector<CBEMInstruction> out;
    const u8* base = bytes.data();
    const std::size_t end = bytes.size();
    std::size_t off = 0;

    while (off < end) {
        const auto opcode = static_cast<Opcode>(base[off]);

        CBEMInstruction ins;
        ins.opcode = opcode;
        ins.streamOffset = static_cast<u32>(off);

        Cursor c{base, off + kOpcodeByteSize, end, issues};
        if (!decodeOne(opcode, c, ins, arena, issues)) {
            return {};
        }
        off = c.offset();
        out.push_back(ins);
    }

    if (out.empty()) {
        return {};
    }
    auto dst = arenaArray<CBEMInstruction>(arena, out.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        dst[i] = out[i];
    }
    return DecodedProgram{std::span<const CBEMInstruction>{dst.data(), dst.size()}};
}

} // namespace whiteout::cornflakes
