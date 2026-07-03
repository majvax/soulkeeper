#pragma once
#include "shared/net/net.hpp"
#include "shared/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace client {

struct RosterRow
{
    std::uint32_t net_id;
    std::string name;
    bool is_host;
    bool connected;
};

// Owns the client's connection + control-plane state (roster, game state, our
// id, latest snapshot). Created in main() and injected into every scene — no
// singleton. Scenes read state via the accessors and act via the send helpers.
class Session
{
public:
    Session(std::string host, std::string name)
      : host_{ std::move(host) }, name_{ std::move(name) }, token_{ std::hash<std::string>{}(name_) }
    {}

    // Set before connect(): the local plugin-set hash sent in Join. The server
    // denies the join if it differs from its own.
    void set_mods_hash(std::uint64_t hash) noexcept { mods_hash_ = hash; }

    void connect() { try_connect(); }

    // Drive the connection once per frame: reconnect if dropped, then drain the
    // socket and fold control messages into our state.
    void poll(float dt)
    {
        if (denied_) { return; } // mod mismatch is not transient — don't hammer the server
        if (!client_) {
            reconnect_timer_ += dt;
            if (reconnect_timer_ >= 1.0f) {
                reconnect_timer_ = 0.0f;
                try_connect();
            }
            return;
        }
        for (const net::Event& ev : client_->poll()) {
            if (ev.type == net::EventType::Disconnect) {
                client_.reset(); // reconnect loop takes over next poll
                return;
            }
            if (ev.type == net::EventType::Receive) { handle(ev.payload); }
        }
    }

    // --- state ---
    [[nodiscard]] bool connected() const noexcept { return client_.has_value(); }
    [[nodiscard]] proto::GameState game_state() const noexcept { return state_; }
    [[nodiscard]] bool is_host() const noexcept { return is_host_; }
    [[nodiscard]] bool has_id() const noexcept { return has_id_; }
    [[nodiscard]] std::uint32_t my_net_id() const noexcept { return my_net_id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::vector<RosterRow>& roster() const noexcept { return roster_; }
    [[nodiscard]] bool leveling() const noexcept { return leveling_; }
    [[nodiscard]] bool join_denied() const noexcept { return denied_; }
    [[nodiscard]] std::uint64_t mods_hash() const noexcept { return mods_hash_; }
    [[nodiscard]] std::uint64_t server_mods_hash() const noexcept { return server_mods_hash_; }
    [[nodiscard]] const std::array<proto::LevelUpChoice, proto::level_up_choices>& choices() const noexcept
    {
        return choices_;
    }

    [[nodiscard]] std::string name_of(std::uint32_t net_id) const
    {
        const auto it = names_.find(net_id);
        return it != names_.end() ? it->second : std::string{};
    }

    // Consume the most recent snapshot payload (bytes after the MsgType tag).
    [[nodiscard]] std::optional<std::vector<std::byte>> take_snapshot()
    {
        if (!latest_snapshot_) { return std::nullopt; }
        std::optional<std::vector<std::byte>> out = std::move(latest_snapshot_);
        latest_snapshot_.reset();
        return out;
    }

    // --- send ---
    void send_input(const proto::Input& input)
    {
        if (!client_) { return; }
        proto::ByteWriter writer;
        writer.put(proto::MsgType::Input);
        writer.put(input);
        client_->send(writer.bytes(), false);
    }

    void send_start()
    {
        if (!client_) { return; }
        proto::ByteWriter writer;
        writer.put(proto::MsgType::StartGame);
        client_->send(writer.bytes(), true);
    }

    void send_command(proto::Command command)
    {
        if (!client_) { return; }
        proto::ByteWriter writer;
        writer.put(proto::MsgType::Command);
        writer.put(command);
        client_->send(writer.bytes(), true);
    }

    void send_select(std::uint8_t index)
    {
        leveling_ = false; // close the overlay; server resumes once all have chosen
        if (!client_) { return; }
        proto::ByteWriter writer;
        writer.put(proto::MsgType::SelectUpgrade);
        writer.put(proto::SelectUpgrade{ .index = index });
        client_->send(writer.bytes(), true);
    }

private:
    void try_connect()
    {
        auto opened = net::Client::connect(host_.c_str(), proto::default_port, 500);
        if (!opened) {
            client_.reset();
            return;
        }
        client_ = std::move(*opened);
        proto::Join join{ .token = token_, .mods_hash = mods_hash_, .name = {} };
        proto::write_name(join.name, name_);
        proto::ByteWriter writer;
        writer.put(proto::MsgType::Join);
        writer.put(join);
        client_->send(writer.bytes(), true);
    }

    void handle(std::span<const std::byte> payload)
    {
        proto::ByteReader reader(payload);
        const auto type = reader.get<proto::MsgType>();
        if (type == proto::MsgType::Welcome) {
            if (const auto welcome = reader.get<proto::Welcome>()) {
                is_host_ = welcome->is_host != 0;
                my_net_id_ = welcome->player_net_id;
                has_id_ = true;
            }
        } else if (type == proto::MsgType::State) {
            if (const auto msg = reader.get<proto::StateMsg>()) {
                state_ = static_cast<proto::GameState>(msg->state);
            }
        } else if (type == proto::MsgType::Roster) {
            read_roster(reader);
        } else if (type == proto::MsgType::LevelUp) {
            for (auto& choice : choices_) {
                if (const auto c = reader.get<proto::LevelUpChoice>()) { choice = *c; }
            }
            leveling_ = true;
        } else if (type == proto::MsgType::Snapshot) {
            // Keep the payload after the 1-byte MsgType for GameScene to apply.
            latest_snapshot_ = std::vector<std::byte>(payload.begin() + 1, payload.end());
        } else if (type == proto::MsgType::JoinDenied) {
            if (const auto denied = reader.get<proto::JoinDenied>()) {
                denied_ = true;
                server_mods_hash_ = denied->server_hash;
                client_.reset(); // the server kicks us anyway; stop cleanly
            }
        }
    }

    void read_roster(proto::ByteReader& reader)
    {
        const auto header = reader.get<proto::RosterHeader>();
        if (!header) { return; }
        roster_.clear();
        names_.clear();
        for (std::uint8_t i = 0; i < header->count; ++i) {
            const auto entry = reader.get<proto::RosterEntry>();
            if (!entry) { break; }
            std::string name = proto::read_name(entry->name);
            names_[entry->net_id] = name;
            roster_.push_back({ .net_id = entry->net_id, .name = std::move(name),
                                .is_host = entry->is_host != 0, .connected = entry->connected != 0 });
        }
    }

    std::string host_;
    std::string name_;
    std::uint64_t token_;
    std::uint64_t mods_hash_ = 0;
    std::uint64_t server_mods_hash_ = 0;
    bool denied_ = false;
    std::optional<net::Client> client_;
    float reconnect_timer_ = 0.0f;

    proto::GameState state_ = proto::GameState::Lobby;
    bool is_host_ = false;
    bool has_id_ = false;
    std::uint32_t my_net_id_ = 0;
    std::vector<RosterRow> roster_;
    std::unordered_map<std::uint32_t, std::string> names_;
    std::optional<std::vector<std::byte>> latest_snapshot_;
    bool leveling_ = false;
    std::array<proto::LevelUpChoice, proto::level_up_choices> choices_{};
};

} // namespace client
