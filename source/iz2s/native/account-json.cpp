/* SPDX-License-Identifier: GPL-3.0-only */
#include "account-types.h"
extern "C" {
#include "twitch.h"
}
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

using json = nlohmann::json;

static std::string_view field(const json& obj, const char* key) {
    auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? std::string_view(it->get_ref<const std::string&>()) : std::string_view{};
}

static unsigned number(const json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_unsigned()) return 0;
    return static_cast<unsigned>(std::min(it->get<uint64_t>(), uint64_t(std::numeric_limits<unsigned>::max())));
}

static bool restricted(std::string_view value, char* out, size_t cap,
                       std::string_view alphabet, bool alnum = true) {
    if (value.empty() || value.size() >= cap) return false;
    for (unsigned char ch : value) {
        bool letter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        if (!(alnum && (letter || (ch >= '0' && ch <= '9'))) && alphabet.find(ch) == std::string_view::npos)
            return false;
    }
    std::memcpy(out, value.data(), value.size());
    out[value.size()] = 0;
    return true;
}

/* JSON parsing guarantees well-formed UTF-8. Keep complete code points and
 * collapse line breaks so API strings fit a single row. */
static void display_string(std::string_view value, char* out, size_t cap) {
    size_t size = std::min(value.size(), cap - 1);
    while (size && size < value.size() && (static_cast<unsigned char>(value[size]) & 0xc0) == 0x80) --size;
    for (size_t i = 0; i < size; ++i) out[i] = static_cast<unsigned char>(value[i]) < 32 ? ' ' : value[i];
    out[size] = 0;
}

static bool activation_link(std::string_view url, const char* code, char* out, size_t cap) {
    constexpr std::string_view origin = "https://www.twitch.tv/activate?";
    if (url.size() <= origin.size() || url.size() >= cap || url.substr(0, origin.size()) != origin) return false;
    auto query = url.substr(origin.size());
    bool matches = false;
    while (!query.empty()) {
        size_t separator = query.find('&');
        auto pair = query.substr(0, separator);
        constexpr std::string_view key = "device-code=";
        if (pair.substr(0, key.size()) == key) {
            if (matches || pair.substr(key.size()) != code) return false;
            matches = true;
        } else if (pair != "public=true" && pair != "public=false") return false;
        if (separator == std::string_view::npos) break;
        query.remove_prefix(separator + 1);
    }
    if (!matches) return false;
    std::memcpy(out, url.data(), url.size());
    out[url.size()] = 0;
    return true;
}

bool parse_device_code(const char* body, DeviceCode* out) try {
    const auto root = json::parse(body, nullptr, false);
    DeviceCode device{};
    device.expires = number(root, "expires_in");
    device.interval = number(root, "interval");
    if (!device.expires || device.expires > 3600 || !device.interval || device.interval > 120 ||
        !restricted(field(root, "device_code"), device.device, sizeof(device.device), "-._~+/=") ||
        !restricted(field(root, "user_code"), device.code, sizeof(device.code), "-") ||
        !activation_link(field(root, "verification_uri"), device.code, device.link, sizeof(device.link))) return false;
    *out = device;
    return true;
} catch (...) { return false; }

bool parse_account_tokens(const char* body, TwitchAccount* out, unsigned* expires) try {
    const auto root = json::parse(body, nullptr, false);
    TwitchAccount next = *out;
    *expires = number(root, "expires_in");
    if (!*expires || *expires > 86400 ||
        !restricted(field(root, "access_token"), next.access, sizeof(next.access), "-._~+/=") ||
        !restricted(field(root, "refresh_token"), next.refresh, sizeof(next.refresh), "-._~+/=")) return false;
    *out = next;
    return true;
} catch (...) { return false; }

bool parse_account_identity(const char* body, const char* client_id, TwitchAccount* out, unsigned* expires) try {
    const auto root = json::parse(body, nullptr, false);
    auto scope = root.find("scopes");
    if (field(root, "client_id") != client_id || scope == root.end() || !scope->is_array() ||
        std::find(scope->begin(), scope->end(), "user:read:follows") == scope->end()) return false;
    *expires = number(root, "expires_in");
    if (!*expires || *expires > 86400) return false;
    out->irc_read = std::find(scope->begin(), scope->end(), "chat:read") != scope->end();
    out->chat_write = std::find(scope->begin(), scope->end(), "user:write:chat") != scope->end();
    return restricted(field(root, "user_id"), out->id, sizeof(out->id), "0123456789", false) &&
           restricted(field(root, "login"), out->login, sizeof(out->login), "_");
} catch (...) { return false; }

bool parse_channel_list(const char* body, bool search, ChannelList* out) try {
    const auto root = json::parse(body, nullptr, false);
    auto data = root.find("data");
    if (data == root.end() || !data->is_array()) return false;
    out->count = 0;
    out->next[0] = 0;
    auto pagination = root.find("pagination");
    if (pagination != root.end() && pagination->is_object())
        restricted(field(*pagination, "cursor"), out->next, sizeof(out->next), "-._~+/=");
    for (const auto& item : *data) {
        if (out->count >= CHANNEL_ROWS) break;
        Channel row{};
        if (!restricted(field(item, search ? "broadcaster_login" : "user_login"), row.login, sizeof(row.login), "_")) continue;
        restricted(field(item, search ? "id" : "user_id"), row.id, sizeof(row.id), "0123456789", false);
        if (search && item.value("is_live", false) != true) continue;
        display_string(field(item, search ? "display_name" : "user_name"), row.name, sizeof(row.name));
        display_string(field(item, "title"), row.title, sizeof(row.title));
        if (search) std::snprintf(row.thumbnail, sizeof(row.thumbnail),
            "https://static-cdn.jtvnw.net/previews-ttv/live_user_%s-{width}x{height}.jpg", row.login);
        else display_string(field(item, "thumbnail_url"), row.thumbnail, sizeof(row.thumbnail));
        row.viewers = number(item, "viewer_count");
        out->items[out->count++] = row;
    }
    return true;
} catch (...) { return false; }

bool parse_oauth_error(const char* body, char out[64]) try {
    const auto root = json::parse(body, nullptr, false);
    auto value = field(root, "message");
    if (value.empty() || value.size() >= 64) return false;
    display_string(value, out, 64);
    return true;
} catch (...) { return false; }

bool parse_helix_user_id(const char* body, char out[32]) try {
    const auto root = json::parse(body, nullptr, false);
    auto data = root.find("data");
    return data != root.end() && data->is_array() && data->size() == 1 &&
        restricted(field((*data)[0], "id"), out, 32, "0123456789", false);
} catch (...) { return false; }

bool chat_payload(const char* broadcaster, const char* sender, const char* text, char* out, unsigned capacity) try {
    auto body = json{{"broadcaster_id", broadcaster}, {"sender_id", sender}, {"message", text}}.dump();
    if (body.size() >= capacity) return false;
    std::memcpy(out, body.c_str(), body.size() + 1);
    return true;
} catch (...) { return false; }

bool parse_chat_delivery(const char* body, char id[48], char error[128]) try {
    const auto root = json::parse(body, nullptr, false);
    auto data = root.find("data");
    if (data == root.end() || !data->is_array() || data->size() != 1 || !(*data)[0].is_object()) return false;
    const auto& result = (*data)[0];
    if (result.value("is_sent", false)) return restricted(field(result, "message_id"), id, 48, "-");
    auto reason = result.find("drop_reason");
    if (reason != result.end() && reason->is_object()) display_string(field(*reason, "message"), error, 128);
    return false;
} catch (...) { return false; }

/* Guest endpoints use the same public Twitch web API as stream resolution.
 * Login and search text are encoded as GraphQL string literals before JSON. */
bool directory_request(PublicFeed feed, const char* key, const char* cursor, char* out, unsigned capacity) try {
    const std::string argument = json(key ? key : "").dump();
    const std::string after = cursor && *cursor ? ",after:" + json(cursor).dump() : "";
    const std::string broadcast = "title viewersCount broadcaster{id login displayName} previewImageURL(width:480,height:270)";
    const std::string game = "id name displayName viewersCount boxArtURL(width:240,height:320)";
    const std::string video = "id title lengthSeconds previewThumbnailURL(width:320,height:180)";
    const std::string clip = "slug title durationSeconds thumbnailURL";
    const std::string page = "pageInfo{hasNextPage endCursor}";
    auto connection = [&](const char* name, unsigned count, const std::string& fieldset, const std::string& offset) {
        return std::string(name) + "(first:" + std::to_string(count) + offset + "){edges{cursor node{" +
            fieldset + "}}" + page + "}";
    };
    std::string query;
    switch (feed) {
        case FEED_POPULAR: query = "{" + connection("streams", 30, broadcast, after) + "}"; break;
        case FEED_TRENDS: query = "{" + connection("games", 30, game, after) + "}"; break;
        case FEED_CATEGORY:
            query = "{game(id:" + argument + "){name " + connection("streams", 30, broadcast, after) + "}}";
            break;
        case FEED_SEARCH:
            query = "{searchFor(userQuery:" + argument + ",platform:\"web\"){channels{items{id login displayName profileImageURL(width:300)"
                    " stream{title viewersCount previewImageURL(width:480,height:270)}}} games{items{" + game +
                    "}} videos{items{" + video + " owner{login displayName}}}}}";
            break;
        case FEED_CHANNEL:
            query = "{user(login:" + argument + "){id login displayName profileImageURL(width:300) description"
                " stream{title viewersCount previewImageURL(width:480,height:270)} " +
                connection("videos", 18, video, "") + " " + connection("clips", 18, clip, "") + "}}";
            break;
        case FEED_VIDEOS:
        case FEED_CLIPS:
            query = "{user(login:" + argument + "){id login displayName " +
                (feed == FEED_VIDEOS ? connection("videos", 18, video, after) : connection("clips", 18, clip, after)) + "}}";
            break;
        case FEED_FAVORITES: {
            std::string_view remaining = key ? key : "";
            json logins = json::array();
            while (!remaining.empty() && logins.size() < CHANNEL_ROWS) {
                auto comma = remaining.find(',');
                auto name = remaining.substr(0, comma);
                char safe[CHANNEL_LOGIN_BYTES];
                if (!restricted(name, safe, sizeof(safe), "_")) return false;
                logins.push_back(safe);
                if (comma == std::string_view::npos) break;
                remaining.remove_prefix(comma + 1);
            }
            if (logins.empty()) return false;
            query = "{users(logins:" + logins.dump() +
                    "){id login displayName profileImageURL(width:300) stream{title viewersCount previewImageURL(width:480,height:270)}}}";
            break;
        }
    }
    auto encoded = json{{"query", query}}.dump();
    if (encoded.size() >= capacity) return false;
    std::memcpy(out, encoded.c_str(), encoded.size() + 1);
    return true;
} catch (...) { return false; }

static const json& member(const json& parent, const char* key) {
    static const json missing = nullptr;
    auto found = parent.find(key);
    return found == parent.end() ? missing : *found;
}

static void append_user(ChannelList* out, const json& user, const json& stream) {
    if (out->count >= CHANNEL_ROWS) return;
    Channel row{};
    if (!restricted(field(user, "login"), row.login, sizeof(row.login), "_")) return;
    restricted(field(user, "id"), row.id, sizeof(row.id), "0123456789", false);
    display_string(field(user, "displayName"), row.name, sizeof(row.name));
    row.kind = stream.is_object() ? ITEM_LIVE : ITEM_CHANNEL;
    row.status_known = true;
    display_string(field(stream, "title"), row.title, sizeof(row.title));
    display_string(field(stream, "previewImageURL"), row.thumbnail, sizeof(row.thumbnail));
    if (!*row.thumbnail && row.kind == ITEM_CHANNEL)
        display_string(field(user, "profileImageURL"), row.thumbnail, sizeof(row.thumbnail));
    row.viewers = number(stream, "viewersCount");
    out->items[out->count++] = row;
}

static void append_game(ChannelList* out, const json& game) {
    if (out->count >= CHANNEL_ROWS) return;
    Channel row{};
    if (!restricted(field(game, "id"), row.id, sizeof(row.id), "0123456789", false)) return;
    row.kind = ITEM_CATEGORY;
    display_string(field(game, "displayName"), row.name, sizeof(row.name));
    if (!*row.name) display_string(field(game, "name"), row.name, sizeof(row.name));
    display_string(field(game, "boxArtURL"), row.thumbnail, sizeof(row.thumbnail));
    row.viewers = number(game, "viewersCount");
    if (*row.name) out->items[out->count++] = row;
}

static void next_page(const json& connection, ChannelList* out) {
    out->next[0] = 0;
    const auto& page = member(connection, "pageInfo");
    if (!page.is_object() || page.value("hasNextPage", false) != true) return;
    auto cursor = field(page, "endCursor");
    const auto& edges = member(connection, "edges");
    if (cursor.empty() && edges.is_array() && !edges.empty()) cursor = field(edges.back(), "cursor");
    restricted(cursor, out->next, sizeof(out->next), "-._~+/=|:");
}

static void append_recording(const json& item, ItemKind kind, const json& owner, ChannelList* out) {
        if (out->count >= CHANNEL_ROWS) return;
        Channel row{};
        restricted(field(owner, "login"), row.login, sizeof(row.login), "_");
        display_string(field(owner, "displayName"), row.name, sizeof(row.name));
        auto value = field(item, kind == ITEM_CLIP ? "slug" : "id");
        if (!restricted(value, row.id, sizeof(row.id), kind == ITEM_CLIP ? "_-" : "0123456789", kind == ITEM_CLIP))
            return;
        display_string(field(item, "title"), row.title, sizeof(row.title));
        display_string(field(item, kind == ITEM_CLIP ? "thumbnailURL" : "previewThumbnailURL"), row.thumbnail, sizeof(row.thumbnail));
        const auto& length = member(item, kind == ITEM_CLIP ? "durationSeconds" : "lengthSeconds");
        double duration = length.is_number() ? length.get<double>() : 0;
        if (duration >= 1 && duration <= UINT32_MAX / 1000.0) row.duration_ms = static_cast<unsigned>(duration * 1000);
        row.kind = kind;
        out->items[out->count++] = row;
}

static void read_recordings(const json& root, ItemKind kind, const json& owner, ChannelList* out) {
    next_page(root, out);
    for (const auto& edge : member(root, "edges")) append_recording(member(edge, "node"), kind, owner, out);
}

bool parse_directory(const char* body, PublicFeed feed, ChannelList* out, ChannelProfile* profile) try {
    const auto root = json::parse(body, nullptr, false);
    const auto& data = member(root, "data");
    if (!data.is_object() || (member(root, "errors").is_array() && !member(root, "errors").empty())) return false;
    out->count = 0;
    out->next[0] = 0;
    if (feed == FEED_FAVORITES) {
        const auto& users = member(data, "users");
        if (!users.is_array()) return false;
        for (const auto& user : users) append_user(out, user, member(user, "stream"));
        return true;
    }
    if (feed == FEED_CHANNEL || feed == FEED_VIDEOS || feed == FEED_CLIPS) {
        const auto& user = member(data, "user");
        if (!user.is_object()) return false;
        if (feed != FEED_CHANNEL) {
            bool video = feed == FEED_VIDEOS;
            const auto& connection = member(user, video ? "videos" : "clips");
            if (!member(connection, "edges").is_array()) return false;
            read_recordings(connection, video ? ITEM_VIDEO : ITEM_CLIP, user, out);
            return true;
        }
        append_user(out, user, member(user, "stream"));
        if (!out->count || !profile) return false;
        profile->header = out->items[0];
        display_string(field(user, "description"), profile->description, sizeof(profile->description));
        profile->videos.count = profile->clips.count = 0;
        read_recordings(member(user, "videos"), ITEM_VIDEO, user, &profile->videos);
        read_recordings(member(user, "clips"), ITEM_CLIP, user, &profile->clips);
        return true;
    }
    if (feed == FEED_SEARCH) {
        const auto& search = member(data, "searchFor");
        if (!search.is_object()) return false;
        const auto& games = member(member(search, "games"), "items");
        const auto& channels = member(member(search, "channels"), "items");
        const auto& videos = member(member(search, "videos"), "items");
        size_t count = std::max({games.size(), channels.size(), videos.size()});
        for (size_t i = 0; i < count && out->count < CHANNEL_ROWS; ++i) {
            if (channels.is_array() && i < channels.size()) append_user(out, channels[i], member(channels[i], "stream"));
            if (games.is_array() && i < games.size()) append_game(out, games[i]);
            if (videos.is_array() && i < videos.size()) append_recording(videos[i], ITEM_VIDEO, member(videos[i], "owner"), out);
        }
        return true;
    }
    const auto& data_rows = feed == FEED_TRENDS ? member(data, "games") :
        member(feed == FEED_CATEGORY ? member(data, "game") : data, "streams");
    const auto& edges = member(data_rows, "edges");
    if (!edges.is_array()) return false;
    next_page(data_rows, out);
    for (const auto& edge : edges) {
        const auto& node = member(edge, "node");
        if (feed == FEED_TRENDS) append_game(out, node);
        else append_user(out, member(node, "broadcaster"), node);
    }
    if (feed == FEED_TRENDS)
        std::stable_sort(out->items, out->items + out->count, [](const Channel& a, const Channel& b) {
            return a.viewers > b.viewers;
        });
    return true;
} catch (...) { return false; }

unsigned parse_clip_qualities(const char* body, TwitchQuality out[TWITCH_QUALITIES]) try {
    const auto root = json::parse(body, nullptr, false);
    const auto& clip = member(member(root, "data"), "clip");
    const auto& access = member(clip, "playbackAccessToken");
    std::string_view token = field(access, "value"), signature = field(access, "signature");
    if (token.empty() || signature.empty()) return 0;
    std::string token_text(token), signature_text(signature);
    unsigned count = 0;
    for (const auto& row : member(clip, "videoQualities")) {
        std::string quality(field(row, "quality"));
        char* end;
        long height = std::strtol(quality.c_str(), &end, 10);
        auto url = field(row, "sourceURL");
        if (url.empty() || *end || height < 144 || height > TWITCH_MAX_HEIGHT) continue;
        if (std::any_of(out, out + count, [height](const TwitchQuality& choice) { return choice.height == height; })) continue;
        if (!twitch_sign_clip(std::string(url).c_str(), token_text.c_str(), signature_text.c_str(), out[count].url)) continue;
        out[count].height = static_cast<unsigned>(height);
        out[count].fps = 0;
        if (++count == TWITCH_QUALITIES) break;
    }
    return count;
} catch (...) { return 0; }
