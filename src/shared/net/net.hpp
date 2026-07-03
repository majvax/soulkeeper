#pragma once
// Minimal ENet wrapper. ENet itself is kept out of this header (PImpl) so the
// rest of the codebase never includes <enet/enet.h> — only net.cpp does.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace net {

// Initialize/shutdown the ENet library once per process. Construct one in main().
struct ScopedInit
{
    ScopedInit();
    ~ScopedInit();
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

    ScopedInit(const ScopedInit&) = delete;
    ScopedInit& operator=(const ScopedInit&) = delete;
    ScopedInit(ScopedInit&&) = delete;
    ScopedInit& operator=(ScopedInit&&) = delete;

private:
    bool ok_ = false;
};

enum class EventType : std::uint8_t { Connect, Disconnect, Receive };

struct Event
{
    EventType type;
    std::uint32_t peer_id;          // which client (server side); 0 on the client
    std::vector<std::byte> payload; // bytes received (Receive only)
};

// Authoritative host: listens for clients and exchanges packets.
class Server
{
public:
    static std::optional<Server> create(std::uint16_t port, std::size_t max_peers);
    ~Server();
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Service the host and return everything that happened since last call.
    [[nodiscard]] std::vector<Event> poll();
    void send(std::uint32_t peer_id, std::span<const std::byte> data, bool reliable);
    void broadcast(std::span<const std::byte> data, bool reliable);
    // Graceful disconnect: queued reliable packets are delivered first, then
    // the peer surfaces as a Disconnect event in a later poll().
    void kick(std::uint32_t peer_id);

private:
    struct Impl;
    explicit Server(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Client connection to a single server.
class Client
{
public:
    static std::optional<Client> connect(const char* host, std::uint16_t port, std::uint32_t timeout_ms);
    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] std::vector<Event> poll();
    void send(std::span<const std::byte> data, bool reliable);

private:
    struct Impl;
    explicit Client(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace net
