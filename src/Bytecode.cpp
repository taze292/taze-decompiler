#include "Bytecode.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace Taze
{
namespace
{
class Reader
{
  public:
    std::string_view Data;
    std::size_t Offset = 0;
    explicit Reader(std::string_view Bytes) : Data(Bytes) {}

    [[noreturn]] void Fail(const std::string &Message) const
    {
        throw Error("Byte " + std::to_string(Offset) + ": " + Message);
    }

    std::string_view Take(std::size_t Count)
    {
        if (Count > Data.size() - Offset)
            Fail("truncated bytecode");
        auto Result = Data.substr(Offset, Count);
        Offset += Count;
        return Result;
    }

    template <typename T> T Read()
    {
        auto Bytes = Take(sizeof(T));
        std::array<unsigned char, sizeof(T)> Buffer{};
        std::memcpy(Buffer.data(), Bytes.data(), sizeof(T));
        if constexpr (std::endian::native == std::endian::big)
            std::reverse(Buffer.begin(), Buffer.end());
        return std::bit_cast<T>(Buffer);
    }

    std::uint64_t Var(unsigned Bits = 32)
    {
        std::uint64_t Value = 0;
        for (unsigned Shift = 0; Shift < Bits; Shift += 7)
        {
            unsigned Byte = Read<std::uint8_t>();
            unsigned Remaining = std::min(7u, Bits - Shift);
            if ((Byte & 127) >= (1u << Remaining))
                Fail("overflowing varint");
            Value |= std::uint64_t(Byte & 127) << Shift;
            if (!(Byte & 128))
                return Value;
        }
        Fail("unterminated varint");
    }

    unsigned Count(unsigned MinimumBytes = 1)
    {
        auto Value = Var();
        if (Value > 1000000 || Value > (Data.size() - Offset) / MinimumBytes)
            Fail("count exceeds input or allocation limit");
        return unsigned(Value);
    }
};

void Decode(Chunk &C, OpcodeEncoding Encoding)
{
    auto Op = [Encoding](std::uint32_t Word) -> LuauOpcode
    {
        unsigned Code = Word & 255;
        if (Encoding == OpcodeEncoding::Roblox)
            Code = (Code * 203) & 255; // Multiplicative inverse of 227 modulo 256.
        if (Code >= LOP__COUNT)
            throw Error("Unknown opcode " + std::to_string(Code));
        return LuauOpcode(Code);
    };
    for (unsigned Id = 0; Id < C.Prototypes.size(); ++Id)
    {
        auto &P = C.Prototypes[Id];
        P.Instructions.clear();
        P.PcToInstruction.clear();
        auto K = [&](unsigned Index) -> const Constant &
        {
            if (Index >= P.Constants.size())
                throw Error("Constant index out of range");
            return P.Constants[Index];
        };
        auto Reg = [&](unsigned Index)
        {
            if (Index >= P.Stack)
                throw Error("Register index out of range");
        };
        for (unsigned Pc = 0; Pc < P.Code.size();)
        {
            std::uint32_t Word = P.Code[Pc];
            Instruction I;
            I.Op = Op(Word);
            I.Pc = Pc;
            I.Next = Pc + Luau::getOpLength(I.Op);
            I.A = LUAU_INSN_A(Word);
            I.B = LUAU_INSN_B(Word);
            I.C = LUAU_INSN_C(Word);
            I.D = LUAU_INSN_D(Word);
            I.E = LUAU_INSN_E(Word);
            if (I.Next > P.Code.size())
                throw Error("Missing AUX word");
            if (I.Next > Pc + 1)
                I.Aux = P.Code[Pc + 1];
            if (I.Op == LOP_NEWCLOSURE || I.Op == LOP_DUPCLOSURE)
            {
                unsigned Child;
                if (I.Op == LOP_NEWCLOSURE)
                {
                    if (unsigned(I.D) >= P.Children.size())
                        throw Error("Child index out of range");
                    Child = P.Children[I.D];
                }
                else
                {
                    const auto &Constant = K(unsigned(I.D));
                    if (Constant.Tag != LBC_CONSTANT_CLOSURE)
                        throw Error("DUPCLOSURE needs a closure constant");
                    Child = Constant.Index;
                }
                if (Child >= Id)
                    throw Error("Closure references must precede their parent");
                for (unsigned U = 0; U < C.Prototypes[Child].Upvalues; ++U)
                {
                    if (I.Next >= P.Code.size())
                        throw Error("Missing CAPTURE instruction");
                    auto Capture = P.Code[I.Next++];
                    if (Op(Capture) != LOP_CAPTURE)
                        throw Error("Expected CAPTURE instruction");
                    auto Kind = LUAU_INSN_A(Capture), Index = LUAU_INSN_B(Capture);
                    if (Kind > LCT_UPVAL)
                        throw Error("Invalid capture kind");
                    if (Kind == LCT_UPVAL)
                    {
                        if (Index >= P.Upvalues)
                            throw Error("Captured upvalue index out of range");
                    }
                    else
                        Reg(Index);
                    I.Captures.emplace_back(Kind, Index);
                }
            }
            switch (I.Op)
            {
            case LOP_CAPTURE:
                throw Error("Orphan CAPTURE instruction");
            case LOP_LOADK:
            case LOP_DUPTABLE:
                K(unsigned(I.D));
                break;
            case LOP_LOADKX:
                K(I.Aux);
                break;
            case LOP_GETGLOBAL:
            case LOP_SETGLOBAL:
            case LOP_GETTABLEKS:
            case LOP_SETTABLEKS:
            case LOP_NAMECALL:
            case LOP_NEWCLASSMEMBER:
                if (K(I.Aux).Tag != LBC_CONSTANT_STRING)
                    throw Error("Expected a string constant");
                break;
            case LOP_GETUDATAKS:
            case LOP_SETUDATAKS:
            case LOP_NAMECALLUDATA:
                if (K(I.Aux & 65535).Tag != LBC_CONSTANT_STRING)
                    throw Error("Expected userdata member name");
                break;
            case LOP_GETIMPORT:
                K(unsigned(I.D));
                if (!(I.Aux >> 30))
                    throw Error("Empty import path");
                for (unsigned J = 0; J < (I.Aux >> 30); ++J)
                    if (K((I.Aux >> (20 - 10 * J)) & 1023).Tag != LBC_CONSTANT_STRING)
                        throw Error("Expected import path string");
                break;
            case LOP_ADDK:
            case LOP_SUBK:
            case LOP_MULK:
            case LOP_DIVK:
            case LOP_MODK:
            case LOP_POWK:
            case LOP_IDIVK:
            case LOP_ANDK:
            case LOP_ORK:
                K(I.C);
                break;
            case LOP_SUBRK:
            case LOP_DIVRK:
                K(I.B);
                break;
            case LOP_JUMPXEQKN:
            case LOP_JUMPXEQKS:
                K(I.Aux & 0xffffff);
                break;
            case LOP_GETUPVAL:
            case LOP_SETUPVAL:
                if (I.B >= P.Upvalues)
                    throw Error("Upvalue index out of range");
                break;
            default:
                break;
            }
            // Validate operands that actually designate stack slots (not cache hints, constants, or builtin ids).
            for (unsigned R : Writes(I, P))
                Reg(R);
            switch (I.Op)
            {
            case LOP_MOVE:
            case LOP_GETTABLEKS:
            case LOP_GETUDATAKS:
            case LOP_NAMECALL:
            case LOP_NAMECALLUDATA:
            case LOP_NOT:
            case LOP_MINUS:
            case LOP_LENGTH:
                Reg(I.B);
                break;
            case LOP_SETTABLE:
            case LOP_GETTABLE:
                Reg(I.A);
                Reg(I.B);
                Reg(I.C);
                break;
            case LOP_SETTABLEKS:
            case LOP_SETUDATAKS:
            case LOP_SETTABLEN:
                Reg(I.A);
                Reg(I.B);
                break;
            case LOP_GETTABLEN:
                Reg(I.B);
                break;
            case LOP_SETGLOBAL:
            case LOP_SETUPVAL:
            case LOP_JUMPIF:
            case LOP_JUMPIFNOT:
            case LOP_JUMPXEQKNIL:
            case LOP_JUMPXEQKB:
            case LOP_JUMPXEQKN:
            case LOP_JUMPXEQKS:
                Reg(I.A);
                break;
            case LOP_JUMPIFEQ:
            case LOP_JUMPIFLE:
            case LOP_JUMPIFLT:
            case LOP_JUMPIFNOTEQ:
            case LOP_JUMPIFNOTLE:
            case LOP_JUMPIFNOTLT:
                Reg(I.A);
                Reg(I.Aux);
                break;
            case LOP_ADD:
            case LOP_SUB:
            case LOP_MUL:
            case LOP_DIV:
            case LOP_MOD:
            case LOP_POW:
            case LOP_AND:
            case LOP_OR:
            case LOP_IDIV:
                Reg(I.B);
                Reg(I.C);
                break;
            case LOP_ADDK:
            case LOP_SUBK:
            case LOP_MULK:
            case LOP_DIVK:
            case LOP_MODK:
            case LOP_POWK:
            case LOP_ANDK:
            case LOP_ORK:
            case LOP_IDIVK:
                Reg(I.B);
                break;
            case LOP_SUBRK:
            case LOP_DIVRK:
                Reg(I.C);
                break;
            case LOP_CONCAT:
                Reg(I.B);
                Reg(I.C);
                if (I.B > I.C)
                    throw Error("Invalid CONCAT range");
                break;
            case LOP_CALL:
            case LOP_CALLFB:
                Reg(I.A);
                if (I.B)
                    Reg(I.A + I.B - 1);
                break;
            case LOP_RETURN:
                if (I.B != 1)
                    Reg(I.A);
                if (I.B > 1)
                    Reg(I.A + I.B - 2);
                break;
            case LOP_SETLIST:
                Reg(I.A);
                Reg(I.B);
                if (I.C > 1)
                    Reg(I.B + I.C - 2);
                break;
            case LOP_FORNPREP:
            case LOP_FORNLOOP:
                Reg(I.A + 2);
                break;
            case LOP_FORGPREP:
            case LOP_FORGPREP_NEXT:
            case LOP_FORGPREP_INEXT:
                Reg(I.A + 2);
                break;
            case LOP_FORGLOOP:
                Reg(I.A + 2 + (I.Aux & 255));
                if (!(I.Aux & 255))
                    throw Error("Empty generic loop");
                break;
            default:
                break;
            }
            P.PcToInstruction.emplace(Pc, unsigned(P.Instructions.size()));
            P.Instructions.push_back(I);
            Pc = I.Next;
        }
        for (const auto &I : P.Instructions)
        {
            auto Target = I.Target();
            if (Target != -1 && (Target < 0 || !P.PcToInstruction.count(unsigned(Target))))
                throw Error("Jump target is outside code or inside AUX/CAPTURE words at PC " + std::to_string(I.Pc));
            if (Luau::isFastCall(I.Op))
            {
                auto Call = P.PcToInstruction.find(I.Pc + I.C + 1);
                if (Call == P.PcToInstruction.end() ||
                    (P.Instructions[Call->second].Op != LOP_CALL && P.Instructions[Call->second].Op != LOP_CALLFB))
                    throw Error("FASTCALL has no matching fallback CALL");
            }
        }
        if (P.Instructions.empty())
            throw Error("Empty prototype");
    }
    C.Encoding = Encoding;
}
} // namespace

int Instruction::Target() const
{
    if (Luau::isJumpD(Op))
        return int(Pc) + 1 + D;
    if (Op == LOP_JUMPX)
        return int(Pc) + 1 + E;
    if (Op == LOP_LOADB && C)
        return int(Pc) + 1 + int(C);
    return -1;
}

std::vector<unsigned> Writes(const Instruction &I, const Prototype &P)
{
    switch (I.Op)
    {
    case LOP_NOP:
    case LOP_BREAK:
    case LOP_SETGLOBAL:
    case LOP_SETUPVAL:
    case LOP_CLOSEUPVALS:
    case LOP_SETTABLE:
    case LOP_SETTABLEKS:
    case LOP_SETTABLEN:
    case LOP_SETUDATAKS:
    case LOP_RETURN:
    case LOP_JUMP:
    case LOP_JUMPBACK:
    case LOP_JUMPX:
    case LOP_JUMPIF:
    case LOP_JUMPIFNOT:
    case LOP_JUMPIFEQ:
    case LOP_JUMPIFLE:
    case LOP_JUMPIFLT:
    case LOP_JUMPIFNOTEQ:
    case LOP_JUMPIFNOTLE:
    case LOP_JUMPIFNOTLT:
    case LOP_JUMPXEQKNIL:
    case LOP_JUMPXEQKB:
    case LOP_JUMPXEQKN:
    case LOP_JUMPXEQKS:
    case LOP_SETLIST:
    case LOP_PREPVARARGS:
    case LOP_COVERAGE:
    case LOP_CAPTURE:
    case LOP_FASTCALL:
    case LOP_FASTCALL1:
    case LOP_FASTCALL2:
    case LOP_FASTCALL2K:
    case LOP_FASTCALL3:
    case LOP_FASTPCALL:
    case LOP_NATIVECALL:
    case LOP_CMPPROTO:
    case LOP_NEWCLASSMEMBER:
        return {};
    case LOP_NAMECALL:
    case LOP_NAMECALLUDATA:
        return {I.A, I.A + 1};
    case LOP_CALL:
    case LOP_CALLFB:
    case LOP_GETVARARGS:
    {
        unsigned Count = I.Op == LOP_GETVARARGS ? I.B : I.C;
        unsigned End = Count ? I.A + Count - 1 : P.Stack;
        std::vector<unsigned> Result;
        for (unsigned R = I.A; R < End; ++R)
            Result.push_back(R);
        return Result;
    }
    case LOP_FORNLOOP:
        return {I.A + 2};
    case LOP_FORNPREP:
        return {};
    case LOP_FORGPREP:
    case LOP_FORGPREP_NEXT:
    case LOP_FORGPREP_INEXT:
        return {};
    case LOP_FORGLOOP:
    {
        std::vector<unsigned> Result{I.A + 2};
        for (unsigned J = 0; J < (I.Aux & 255); ++J)
            Result.push_back(I.A + 3 + J);
        return Result;
    }
    default:
        return {I.A};
    }
}

Chunk ReadBytecode(std::string_view Data, const Options &Settings)
{
    if (Data.size() > Settings.MaxInputBytes)
        throw Error("Input exceeds configured size limit");
    Reader R(Data);
    Chunk C;
    C.Version = R.Read<std::uint8_t>();
    if (!C.Version)
        throw Error("Compiler error: " + std::string(R.Take(Data.size() - R.Offset)));
    if (C.Version < 3 || (C.Version > 14 && C.Version != 100))
        throw Error("Unsupported bytecode version " + std::to_string(C.Version) + "; expected raw Luau bytecode 3..14 or 100");
    if (C.Version >= 4)
    {
        C.Types = R.Read<std::uint8_t>();
        if (C.Types < 1 || C.Types > 3)
            R.Fail("unsupported type information version");
    }
    std::vector<std::string> Strings(1);
    unsigned StringCount = R.Count();
    for (unsigned I = 0; I < StringCount; ++I)
    {
        unsigned Length = unsigned(R.Var());
        Strings.emplace_back(R.Take(Length));
    }
    auto String = [&]() -> std::string
    {
        auto Id = R.Var();
        if (Id >= Strings.size())
            R.Fail("string index out of range");
        return Strings[std::size_t(Id)];
    };
    if (C.Types == 3)
        while (R.Read<std::uint8_t>() != 0)
            String();
    unsigned ProtoCount = R.Count(8);
    if (!ProtoCount)
        R.Fail("missing main prototype");
    for (unsigned Id = 0; Id < ProtoCount; ++Id)
    {
        unsigned ProtoSize = C.Version >= 12 ? unsigned(R.Var()) : 0;
        std::size_t ProtoStart = R.Offset;
        if (C.Version >= 12 && ProtoSize > Data.size() - ProtoStart)
            R.Fail("prototype size exceeds input");
        Prototype P;
        P.Stack = R.Read<std::uint8_t>();
        P.Parameters = R.Read<std::uint8_t>();
        P.Upvalues = R.Read<std::uint8_t>();
        P.Vararg = R.Read<std::uint8_t>() != 0;
        if (P.Parameters > P.Stack || P.Upvalues > 200)
            R.Fail("invalid prototype header");
        if (C.Version >= 4)
        {
            P.Flags = R.Read<std::uint8_t>();
            auto TypeSize = R.Var();
            R.Take(std::size_t(TypeSize));
        }
        unsigned CodeCount = R.Count(4);
        for (unsigned J = 0; J < CodeCount; ++J)
            P.Code.push_back(R.Read<std::uint32_t>());
        unsigned ConstantCount = R.Count();
        for (unsigned J = 0; J < ConstantCount; ++J)
        {
            Constant K;
            K.Tag = R.Read<std::uint8_t>();
            switch (K.Tag)
            {
            case LBC_CONSTANT_NIL:
                break;
            case LBC_CONSTANT_BOOLEAN:
                K.Index = R.Read<std::uint8_t>();
                break;
            case LBC_CONSTANT_NUMBER:
                K.Number = R.Read<double>();
                break;
            case LBC_CONSTANT_STRING:
                K.Text = String();
                break;
            case LBC_CONSTANT_IMPORT:
                K.Index = R.Read<std::uint32_t>();
                break;
            case LBC_CONSTANT_CLOSURE:
                K.Index = unsigned(R.Var());
                if (K.Index >= Id)
                    R.Fail("invalid closure prototype reference");
                break;
            case LBC_CONSTANT_VECTOR:
                for (auto &V : K.Vector)
                    V = R.Read<float>();
                break;
            case LBC_CONSTANT_VECTORD:
                for (auto &V : K.Vector)
                    V = R.Read<double>();
                break;
            case LBC_CONSTANT_INTEGER:
                K.Negative = R.Read<std::uint8_t>() != 0;
                K.Integer = R.Var(64);
                break;
            case LBC_CONSTANT_TABLE:
            case LBC_CONSTANT_TABLE_WITH_CONSTANTS:
            {
                unsigned Count = R.Count();
                for (unsigned N = 0; N < Count; ++N)
                {
                    unsigned Key = unsigned(R.Var());
                    int Value = K.Tag == LBC_CONSTANT_TABLE_WITH_CONSTANTS ? R.Read<std::int32_t>() : -1;
                    if (Key >= J || Value >= int(J))
                        R.Fail("invalid table template reference");
                    K.Entries.emplace_back(Key, Value);
                }
                break;
            }
            case LBC_CONSTANT_CLASS_SHAPE:
            {
                K.Index = unsigned(R.Var());
                unsigned Properties = unsigned(R.Var()), Methods = unsigned(R.Var());
                if (std::uint64_t(Properties) + Methods > 1000000)
                    R.Fail("class shape too large");
                for (unsigned N = 0; N < Properties + Methods; ++N)
                    K.Entries.emplace_back(unsigned(R.Var()), N < Properties ? 0 : 1);
                break;
            }
            default:
                R.Fail("unknown constant tag " + std::to_string(K.Tag));
            }
            P.Constants.push_back(std::move(K));
        }
        unsigned ChildCount = R.Count();
        for (unsigned J = 0; J < ChildCount; ++J)
        {
            auto Child = unsigned(R.Var());
            if (Child >= Id)
                R.Fail("invalid child prototype reference");
            P.Children.push_back(Child);
        }
        P.Line = unsigned(R.Var());
        P.Name = String();
        if (R.Read<std::uint8_t>())
        {
            auto Gap = R.Read<std::uint8_t>();
            if (Gap > 31 || !CodeCount)
                R.Fail("invalid line info interval");
            std::uint8_t Delta = 0;
            for (unsigned J = 0; J < CodeCount; ++J)
            {
                Delta = std::uint8_t(Delta + R.Read<std::uint8_t>());
                P.Lines.push_back(Delta);
            }
            std::int64_t Base = 0;
            unsigned Intervals = ((CodeCount - 1) >> Gap) + 1;
            for (unsigned J = 0; J < Intervals; ++J)
            {
                Base += R.Read<std::int32_t>();
                if (Base < 0 || Base > std::numeric_limits<int>::max())
                    R.Fail("invalid line base");
                auto End = std::min<std::uint64_t>(CodeCount, (std::uint64_t(J) + 1) << Gap);
                for (auto N = std::uint64_t(J) << Gap; N < End; ++N)
                    P.Lines[std::size_t(N)] += unsigned(Base);
            }
        }
        if (R.Read<std::uint8_t>())
        {
            unsigned LocalCount = R.Count(4);
            for (unsigned J = 0; J < LocalCount; ++J)
            {
                Local L;
                L.Name = String();
                L.Start = unsigned(R.Var());
                L.End = unsigned(R.Var());
                L.Register = R.Read<std::uint8_t>();
                if (L.Start > L.End || L.End > CodeCount || L.Register >= P.Stack)
                    R.Fail("invalid debug local range");
                P.Locals.push_back(std::move(L));
            }
            unsigned UpvalueCount = R.Count();
            if (UpvalueCount != P.Upvalues)
                R.Fail("invalid debug upvalue count");
            for (unsigned J = 0; J < UpvalueCount; ++J)
                P.UpvalueNames.push_back(String());
        }
        if (C.Version >= 11)
        {
            unsigned FeedbackCount = R.Count(2);
            for (unsigned J = 0; J < FeedbackCount; ++J)
            {
                if (R.Read<std::uint8_t>() != 0)
                    R.Fail("unknown feedback slot kind");
                if (R.Var() >= CodeCount)
                    R.Fail("invalid feedback PC");
            }
        }
        if (C.Version >= 12)
        {
            if (P.Flags & LPF_INLINABLE)
                R.Var(64);
            if (R.Offset > ProtoStart + ProtoSize)
                R.Fail("prototype size is too small");
            R.Take(ProtoStart + ProtoSize - R.Offset);
        }
        C.Prototypes.push_back(std::move(P));
    }
    C.Main = unsigned(R.Var());
    if (C.Main >= C.Prototypes.size())
        R.Fail("main prototype index out of range");
    auto TrailingBytes = Data.size() - R.Offset;
    if (Settings.Encoding != OpcodeEncoding::Auto)
        Decode(C, Settings.Encoding);
    else
    {
        try
        {
            Decode(C, OpcodeEncoding::Plain);
        }
        catch (const Error &Plain)
        {
            std::string First = Plain.what();
            try
            {
                Decode(C, OpcodeEncoding::Roblox);
            }
            catch (const Error &Encoded)
            {
                throw Error("Neither opcode encoding validates. Plain: " + First + "; Roblox: " + Encoded.what());
            }
        }
    }
    // Roblox's getscriptbytecode currently appends a 24-byte opaque trailer.
    // Only accept that exact envelope after the complete encoded instruction stream validates.
    if (TrailingBytes && !(TrailingBytes == 24 && C.Encoding == OpcodeEncoding::Roblox))
        R.Fail("unexpected trailing data (input must be decompressed raw bytecode)");
    return C;
}

std::string Quote(std::string_view Text)
{
    std::string Result = "\"";
    for (unsigned char C : Text)
    {
        if (C == '\\' || C == '"')
        {
            Result += '\\';
            Result += char(C);
        }
        else if (C == '\n')
            Result += "\\n";
        else if (C == '\r')
            Result += "\\r";
        else if (C == '\t')
            Result += "\\t";
        else if (C < 32 || C >= 127)
        {
            Result += '\\';
            Result += char('0' + C / 100);
            Result += char('0' + (C / 10) % 10);
            Result += char('0' + C % 10);
        }
        else
            Result += char(C);
    }
    return Result + '"';
}

std::string Number(double Value)
{
    if (std::isnan(Value))
        return "(0 / 0)";
    if (std::isinf(Value))
        return Value < 0 ? "(-1 / 0)" : "(1 / 0)";
    if (Value == 0 && std::signbit(Value))
        return "-0.0";
    std::ostringstream Out;
    Out.imbue(std::locale::classic());
    Out << std::setprecision(17) << Value;
    return Out.str();
}

std::string OpcodeName(LuauOpcode Op)
{
    static const char *Names[] = {"NOP",
                                  "BREAK",
                                  "LOADNIL",
                                  "LOADB",
                                  "LOADN",
                                  "LOADK",
                                  "MOVE",
                                  "GETGLOBAL",
                                  "SETGLOBAL",
                                  "GETUPVAL",
                                  "SETUPVAL",
                                  "CLOSEUPVALS",
                                  "GETIMPORT",
                                  "GETTABLE",
                                  "SETTABLE",
                                  "GETTABLEKS",
                                  "SETTABLEKS",
                                  "GETTABLEN",
                                  "SETTABLEN",
                                  "NEWCLOSURE",
                                  "NAMECALL",
                                  "CALL",
                                  "RETURN",
                                  "JUMP",
                                  "JUMPBACK",
                                  "JUMPIF",
                                  "JUMPIFNOT",
                                  "JUMPIFEQ",
                                  "JUMPIFLE",
                                  "JUMPIFLT",
                                  "JUMPIFNOTEQ",
                                  "JUMPIFNOTLE",
                                  "JUMPIFNOTLT",
                                  "ADD",
                                  "SUB",
                                  "MUL",
                                  "DIV",
                                  "MOD",
                                  "POW",
                                  "ADDK",
                                  "SUBK",
                                  "MULK",
                                  "DIVK",
                                  "MODK",
                                  "POWK",
                                  "AND",
                                  "OR",
                                  "ANDK",
                                  "ORK",
                                  "CONCAT",
                                  "NOT",
                                  "MINUS",
                                  "LENGTH",
                                  "NEWTABLE",
                                  "DUPTABLE",
                                  "SETLIST",
                                  "FORNPREP",
                                  "FORNLOOP",
                                  "FORGLOOP",
                                  "FORGPREP_INEXT",
                                  "FASTCALL3",
                                  "FORGPREP_NEXT",
                                  "NATIVECALL",
                                  "GETVARARGS",
                                  "DUPCLOSURE",
                                  "PREPVARARGS",
                                  "LOADKX",
                                  "JUMPX",
                                  "FASTCALL",
                                  "COVERAGE",
                                  "CAPTURE",
                                  "SUBRK",
                                  "DIVRK",
                                  "FASTCALL1",
                                  "FASTCALL2",
                                  "FASTCALL2K",
                                  "FORGPREP",
                                  "JUMPXEQKNIL",
                                  "JUMPXEQKB",
                                  "JUMPXEQKN",
                                  "JUMPXEQKS",
                                  "IDIV",
                                  "IDIVK",
                                  "GETUDATAKS",
                                  "SETUDATAKS",
                                  "NAMECALLUDATA",
                                  "NEWCLASSMEMBER",
                                  "CALLFB",
                                  "CMPPROTO",
                                  "FASTPCALL",
                                  "NEWCLASS"};
    static_assert(std::size(Names) == LOP__COUNT, "Update opcode names for the supplied Luau version");
    return Names[unsigned(Op)];
}

std::string Disassemble(std::string_view Bytecode, const Options &Settings)
{
    auto C = ReadBytecode(Bytecode, Settings);
    std::ostringstream Out;
    Out << "; Bytecode v" << C.Version << ", types v" << C.Types << "\n";
    for (unsigned Id = 0; Id < C.Prototypes.size(); ++Id)
    {
        const auto &P = C.Prototypes[Id];
        Out << "\nFunction " << Id << " " << Quote(P.Name) << " (line " << P.Line << ")\n";
        for (const auto &L : P.Locals)
            Out << "  Local R" << L.Register << " " << Quote(L.Name) << " [" << L.Start << "," << L.End << ")\n";
        for (unsigned K = 0; K < P.Constants.size(); ++K)
        {
            const auto &V = P.Constants[K];
            Out << "  K" << K << " [" << V.Tag << "] ";
            if (V.Tag == LBC_CONSTANT_STRING)
                Out << Quote(V.Text);
            else if (V.Tag == LBC_CONSTANT_NUMBER)
                Out << Number(V.Number);
            else
                Out << V.Index;
            Out << '\n';
        }
        for (const auto &I : P.Instructions)
        {
            Out << std::setw(6) << I.Pc << "  " << std::left << std::setw(18) << OpcodeName(I.Op) << std::right << " A=" << I.A
                << " B=" << I.B << " C=" << I.C << " D=" << I.D;
            if (Luau::getOpLength(I.Op) == 2)
                Out << " AUX=" << I.Aux;
            if (I.Target() != -1)
                Out << " -> " << I.Target();
            Out << '\n';
            for (auto [Kind, Index] : I.Captures)
                Out << "        CAPTURE " << Kind << " " << Index << '\n';
        }
    }
    return Out.str();
}
} // namespace Taze
