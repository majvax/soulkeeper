#include "shared/net/net.hpp"

#include <enet/enet.h>

#include <cstring>
#include <unordered_map>

namespace net {
namespace {

// ENet channels: 0 = unreliable (snapshots/input), 1 = reliable (handshake).
constexpr enet_uint8 channel_unreliable = 0;
constexpr enet_uint8 channel_reliable = 1;
constexpr std::size_t channel_count = 2;

ENetPacket* make_packet(std::span<const std::byte> data, bool reliable)
{
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    return enet_packet_create(data.data(), data.size(), flags);
}

std::vector<std::byte> copy_payload(const ENetPacket* packet)
{
    const auto* begin = reinterpret_cast<const std::byte*>(packet->data);
    return { begin, begin + packet->dataLength };
}

} // namespace

// --- ScopedInit ------------------------------------------------------------
ScopedInit::ScopedInit() : ok_{ enet_initialize() == 0 } {}
ScopedInit::~ScopedInit()
{
    if (ok_) { enet_deinitialize(); }
}

// --- Server ----------------------------------------------------------------
struct Server::Impl
{
    ENetHost* host = nullptr;
    std::unordered_map<std::uint32_t, ENetPeer*> peers;
    std::uint32_t next_peer_id = 1;
};

Server::Server(std::unique_ptr<Impl> impl) noexcept : impl_{ std::move(impl) } {}
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;
Server::~Server()
{
    if (impl_ && impl_->host) { enet_host_destroy(impl_->host); }
}

std::optional<Server> Server::create(std::uint16_t port, std::size_t max_peers)
{
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = enet_host_create(&address, max_peers, channel_count, 0, 0);
    if (host == nullptr) { return std::nullopt; }

    auto impl = std::make_unique<Impl>();
    impl->host = host;
    return Server{ std::move(impl) };
}

std::vector<Event> Server::poll()
{
    std::vector<Event> events;
    ENetEvent event;
    while (enet_host_service(impl_->host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            const std::uint32_t id = impl_->next_peer_id++;
            event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
            impl_->peers[id] = event.peer;
            events.push_back({ .type = EventType::Connect, .peer_id = id, .payload = {} });
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const auto id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(event.peer->data));
            impl_->peers.erase(id);
            event.peer->data = nullptr;
            events.push_back({ .type = EventType::Disconnect, .peer_id = id, .payload = {} });
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            const auto id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(event.peer->data));
            events.push_back({ .type = EventType::Receive, .peer_id = id, .payload = copy_payload(event.packet) });
            enet_packet_destroy(event.packet);
            break;
        }
        default:
            break;
        }
    }
    return events;
}

void Server::send(std::uint32_t peer_id, std::span<const std::byte> data, bool reliable)
{
    const auto it = impl_->peers.find(peer_id);
    if (it == impl_->peers.end()) { return; }
    const enet_uint8 channel = reliable ? channel_reliable : channel_unreliable;
    enet_peer_send(it->second, channel, make_packet(data, reliable));
}

void Server::broadcast(std::span<const std::byte> data, bool reliable)
{
    const enet_uint8 channel = reliable ? channel_reliable : channel_unreliable;
    enet_host_broadcast(impl_->host, channel, make_packet(data, reliable));
}

// --- Client ----------------------------------------------------------------
struct Client::Impl
{
    ENetHost* host = nullptr;
    ENetPeer* server = nullptr;
};

Client::Client(std::unique_ptr<Impl> impl) noexcept : impl_{ std::move(impl) } {}
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;
Client::~Client()
{
    if (impl_ && impl_->host) { enet_host_destroy(impl_->host); }
}

std::optional<Client> Client::connect(const char* host, std::uint16_t port, std::uint32_t timeout_ms)
{
    ENetHost* client_host = enet_host_create(nullptr, 1, channel_count, 0, 0);
    if (client_host == nullptr) { return std::nullopt; }

    ENetAddress address{};
    enet_address_set_host(&address, host);
    address.port = port;

    ENetPeer* peer = enet_host_connect(client_host, &address, channel_count, 0);
    if (peer == nullptr) {
        enet_host_destroy(client_host);
        return std::nullopt;
    }

    // Wait for the connection to be acknowledged.
    ENetEvent event;
    if (enet_host_service(client_host, &event, timeout_ms) > 0 && event.type == ENET_EVENT_TYPE_CONNECT) {
        auto impl = std::make_unique<Impl>();
        impl->host = client_host;
        impl->server = peer;
        return Client{ std::move(impl) };
    }

    enet_peer_reset(peer);
    enet_host_destroy(client_host);
    return std::nullopt;
}

std::vector<Event> Client::poll()
{
    std::vector<Event> events;
    ENetEvent event;
    while (enet_host_service(impl_->host, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_RECEIVE:
            events.push_back({ .type = EventType::Receive, .peer_id = 0, .payload = copy_payload(event.packet) });
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            events.push_back({ .type = EventType::Disconnect, .peer_id = 0, .payload = {} });
            break;
        default:
            break;
        }
    }
    return events;
}

void Client::send(std::span<const std::byte> data, bool reliable)
{
    const enet_uint8 channel = reliable ? channel_reliable : channel_unreliable;
    enet_peer_send(impl_->server, channel, make_packet(data, reliable));
}

} // namespace net
