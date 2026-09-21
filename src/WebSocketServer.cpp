#include "WebSocketServer.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bcrypt.h>
#include <cctype>
#include <iostream>
#include <map>
#include <sstream>
#include <syncstream>
#include <thread>

namespace Taze
{
namespace
{
struct Socket
{
    SOCKET Value;
    explicit Socket(SOCKET Value) : Value(Value) {}
    ~Socket()
    {
        if (Value != INVALID_SOCKET)
            closesocket(Value);
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
};

void Read(SOCKET Socket, char *Data, std::size_t Size)
{
    while (Size)
    {
        int Count = recv(Socket, Data, int(std::min<std::size_t>(Size, 65536)), 0);
        if (Count <= 0)
            throw Error("WebSocket disconnected or receive timed out");
        Data += Count;
        Size -= Count;
    }
}

void Send(SOCKET Socket, std::string_view Data)
{
    while (!Data.empty())
    {
        int Count = send(Socket, Data.data(), int(std::min<std::size_t>(Data.size(), 65536)), 0);
        if (Count <= 0)
            throw Error("WebSocket send failed");
        Data.remove_prefix(Count);
    }
}

std::string Base64(std::string_view Data)
{
    constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string Result;
    for (std::size_t I = 0; I < Data.size(); I += 3)
    {
        unsigned Bits = unsigned(static_cast<unsigned char>(Data[I])) << 16;
        if (I + 1 < Data.size())
            Bits |= unsigned(static_cast<unsigned char>(Data[I + 1])) << 8;
        if (I + 2 < Data.size())
            Bits |= static_cast<unsigned char>(Data[I + 2]);
        Result += Alphabet[(Bits >> 18) & 63];
        Result += Alphabet[(Bits >> 12) & 63];
        Result += I + 1 < Data.size() ? Alphabet[(Bits >> 6) & 63] : '=';
        Result += I + 2 < Data.size() ? Alphabet[Bits & 63] : '=';
    }
    return Result;
}

std::string AcceptKey(std::string Key)
{
    Key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0)
        throw Error("Cannot initialize WebSocket SHA-1");
    std::array<unsigned char, 20> Hash{};
    auto Status =
        BCryptHash(Algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(Key.data()), ULONG(Key.size()), Hash.data(), ULONG(Hash.size()));
    BCryptCloseAlgorithmProvider(Algorithm, 0);
    if (Status < 0)
        throw Error("Cannot hash WebSocket handshake");
    return Base64({reinterpret_cast<const char *>(Hash.data()), Hash.size()});
}

std::string Lower(std::string Text)
{
    for (auto &C : Text)
        C = char(std::tolower(static_cast<unsigned char>(C)));
    return Text;
}

std::string Trim(std::string Text)
{
    auto Start = Text.find_first_not_of(" \t\r\n");
    if (Start == std::string::npos)
        return {};
    return Text.substr(Start, Text.find_last_not_of(" \t\r\n") - Start + 1);
}

bool HasToken(const std::string &Header, const std::string &Token)
{
    std::istringstream Input(Lower(Header));
    std::string Part;
    while (std::getline(Input, Part, ','))
        if (Trim(Part) == Token)
            return true;
    return false;
}

void Handshake(SOCKET Client)
{
    std::string Header;
    while (!Header.ends_with("\r\n\r\n"))
    {
        if (Header.size() >= 8192)
            throw Error("WebSocket handshake exceeds 8 KiB");
        char C;
        Read(Client, &C, 1);
        Header += C;
    }
    std::istringstream Input(Header);
    std::string Line;
    std::getline(Input, Line);
    if (Line != "GET /decompile HTTP/1.1\r")
        throw Error("Expected GET /decompile HTTP/1.1");
    std::map<std::string, std::string> Fields;
    while (std::getline(Input, Line) && Line != "\r")
    {
        auto Colon = Line.find(':');
        if (Colon == std::string::npos)
            throw Error("Malformed handshake header");
        auto Name = Lower(Trim(Line.substr(0, Colon)));
        if (!Fields.emplace(Name, Trim(Line.substr(Colon + 1))).second)
            throw Error("Duplicate handshake header");
    }
    auto Key = Fields["sec-websocket-key"];
    bool ValidKey = Key.size() == 24 && Key.ends_with("==") &&
                    std::all_of(Key.begin(), Key.begin() + 22, [](unsigned char C) { return std::isalnum(C) || C == '+' || C == '/'; });
    if (!ValidKey || Fields["sec-websocket-version"] != "13" || Lower(Fields["upgrade"]) != "websocket" ||
        !HasToken(Fields["connection"], "upgrade") || Fields["host"].empty())
        throw Error("Invalid WebSocket upgrade");
    // Native executors may send their ws:// loopback origin; ordinary browser pages use http(s):// origins.
    if (Fields.count("origin") && Fields["origin"] != "ws://" + Fields["host"])
        throw Error("Browser-origin connections are not supported");
    Send(Client, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                     AcceptKey(Key) + "\r\n\r\n");
}

void Frame(SOCKET Client, unsigned Opcode, std::string_view Payload)
{
    std::string Header(1, char(0x80 | Opcode));
    auto Size = std::uint64_t(Payload.size());
    if (Size < 126)
        Header += char(Size);
    else
    {
        unsigned Bytes = Size <= 65535 ? 2 : 8;
        Header += char(Bytes == 2 ? 126 : 127);
        for (unsigned I = Bytes; I-- > 0;)
            Header += char(Size >> (I * 8));
    }
    Send(Client, Header);
    Send(Client, Payload);
}

std::string Process(std::string_view Request, const Options &Settings)
{
    auto Newline = Request.find('\n');
    if (Newline == std::string_view::npos || Newline == 0 || Newline > 16 ||
        !std::all_of(Request.begin(), Request.begin() + Newline, [](unsigned char C) { return C >= '0' && C <= '9'; }))
        throw Error("Expected request ID followed by hex bytecode");
    std::string Id(Request.substr(0, Newline));
    Request.remove_prefix(Newline + 1);
    try
    {
        if (Request.empty() || Request.size() % 2 || Request.size() / 2 > Settings.MaxInputBytes)
            throw Error("Invalid bytecode length");
        auto Digit = [](char C) -> unsigned
        {
            if (C >= '0' && C <= '9')
                return unsigned(C - '0');
            if (C >= 'a' && C <= 'f')
                return unsigned(C - 'a' + 10);
            if (C >= 'A' && C <= 'F')
                return unsigned(C - 'A' + 10);
            throw Error("Bytecode must be hex encoded");
        };
        std::string Bytes(Request.size() / 2, '\0');
        for (std::size_t I = 0; I < Bytes.size(); ++I)
            Bytes[I] = char((Digit(Request[I * 2]) << 4) | Digit(Request[I * 2 + 1]));
        auto Result = Decompile(Bytes, Settings);
        std::osyncstream(std::cout) << "Request " << Id << ": " << Bytes.size() << " bytecode bytes -> " << Result.Source.size()
                                    << " source bytes, " << Result.StateMachineFunctions << " fallback functions\n";
        return Id + "\nok\n" + Result.Source;
    }
    catch (const std::exception &Failure)
    {
        return Id + "\nerror\n" + Failure.what();
    }
}

void Session(SOCKET Client, const Options &Settings)
{
    DWORD Timeout = 10000;
    setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&Timeout), sizeof(Timeout));
    setsockopt(Client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&Timeout), sizeof(Timeout));
    Handshake(Client);
    Timeout = 600000;
    setsockopt(Client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&Timeout), sizeof(Timeout));
    std::string Message;
    bool Fragmented = false;
    for (;;)
    {
        std::array<unsigned char, 2> Header{};
        Read(Client, reinterpret_cast<char *>(Header.data()), Header.size());
        unsigned Opcode = Header[0] & 15;
        bool Final = (Header[0] & 128) != 0;
        if ((Header[0] & 112) || !(Header[1] & 128))
            throw Error("Invalid frame flags or missing client mask");
        std::uint64_t Size = Header[1] & 127;
        auto Marker = Size;
        if (Size >= 126)
        {
            unsigned Count = Size == 126 ? 2 : 8;
            Size = 0;
            for (unsigned I = 0; I < Count; ++I)
            {
                unsigned char Byte;
                Read(Client, reinterpret_cast<char *>(&Byte), 1);
                Size = (Size << 8) | Byte;
            }
            if ((Marker == 126 && Size < 126) || (Marker == 127 && Size <= 65535) || (Size >> 63))
                throw Error("Invalid frame length encoding");
        }
        if (Opcode >= 8 && (!Final || Size > 125))
            throw Error("Invalid control frame");
        if (Size > Settings.MaxInputBytes * 2 + 32 || Size + Message.size() > Settings.MaxInputBytes * 2 + 32)
            throw Error("WebSocket message exceeds bytecode size limit");
        std::array<char, 4> Mask{};
        Read(Client, Mask.data(), Mask.size());
        std::string Payload(std::size_t(Size), '\0');
        Read(Client, Payload.data(), Payload.size());
        for (std::size_t I = 0; I < Payload.size(); ++I)
            Payload[I] ^= Mask[I % 4];
        if (Opcode == 8)
        {
            if (Payload.size() == 1)
                throw Error("Invalid close payload");
            Frame(Client, 8, std::string_view("\x03\xe8", 2));
            return;
        }
        if (Opcode == 9)
        {
            Frame(Client, 10, Payload);
            continue;
        }
        if (Opcode == 10)
            continue;
        if (Opcode != 0 && Opcode != 1)
            throw Error("Only text requests are supported");
        if ((Opcode == 0) != Fragmented)
            throw Error("Unexpected continuation or data frame");
        Message += Payload;
        Fragmented = !Final;
        if (Final)
        {
            Frame(Client, 1, Process(Message, Settings));
            Message.clear();
        }
    }
}
} // namespace

int ServeWebSocket(unsigned Port, const Options &Settings)
{
    if (!Port || Port > 65535)
        throw Error("Port must be between 1 and 65535");
    WSADATA Data{};
    if (WSAStartup(MAKEWORD(2, 2), &Data))
        throw Error("Cannot initialize Winsock");
    struct Cleanup
    {
        ~Cleanup()
        {
            WSACleanup();
        }
    } Cleanup;
    Socket Listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (Listener.Value == INVALID_SOCKET)
        throw Error("Cannot create listener socket");
    BOOL Exclusive = TRUE;
    setsockopt(Listener.Value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&Exclusive), sizeof(Exclusive));
    sockaddr_in Address{};
    Address.sin_family = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Address.sin_port = htons(static_cast<unsigned short>(Port));
    if (bind(Listener.Value, reinterpret_cast<sockaddr *>(&Address), sizeof(Address)) == SOCKET_ERROR)
        throw Error("Cannot bind 127.0.0.1:" + std::to_string(Port) + " (port may already be in use)");
    if (listen(Listener.Value, 8) == SOCKET_ERROR)
        throw Error("Cannot listen on socket");
    std::cout << "Taze listening at ws://127.0.0.1:" << Port
              << "/decompile\nRun tools/Decompile.luau in your executor. Ctrl+C stops the server.\n"
              << std::flush;
    static std::atomic<unsigned> Clients = 0;
    for (;;)
    {
        SOCKET Client = accept(Listener.Value, nullptr, nullptr);
        if (Client == INVALID_SOCKET)
            throw Error("Accept failed");
        if (Clients.fetch_add(1) >= 8)
        {
            --Clients;
            closesocket(Client);
            continue;
        }
        try
        {
            std::thread(
                [Client, Settings]()
                {
                    Socket Owned(Client);
                    struct Release
                    {
                        ~Release()
                        {
                            --Clients;
                        }
                    } Release;
                    try
                    {
                        Session(Client, Settings);
                    }
                    catch (const std::exception &Failure)
                    {
                        try
                        {
                            Frame(Client, 8, std::string_view("\x03\xea", 2));
                        }
                        catch (...)
                        {
                        }
                        std::osyncstream(std::cerr) << "taze: " << Failure.what() << '\n';
                    }
                })
                .detach();
        }
        catch (...)
        {
            --Clients;
            closesocket(Client);
            throw;
        }
    }
}
} // namespace Taze
#else
namespace Taze
{
int ServeWebSocket(unsigned, const Options &)
{
    throw Error("WebSocket server mode currently requires Windows");
}
} // namespace Taze
#endif
