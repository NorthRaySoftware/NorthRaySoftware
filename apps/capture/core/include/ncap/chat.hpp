// Twitch chat, read-only and without signing in: Twitch lets anyone read a channel's chat as an
// anonymous "justinfan" user over its IRC WebSocket (wss://irc-ws.chat.twitch.tv). This is the portable
// part: reading IRC lines (with IRCv3 tags), turning them into chat events, and keeping the recent
// messages (removing ones a moderator deletes or a user who is timed out or banned). The connection
// lives in src/net/twitch_chat.
#pragma once

#include "ncap/colour.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ncap::chat {

using sourceutil::Rgba;

struct IrcLine {
    std::map<std::string, std::string> tags;   // unescaped
    std::string prefix;                        // without the ':'
    std::string command;                       // PRIVMSG, PING, 001...
    std::vector<std::string> params;           // the last may contain spaces (the trailing part)
};
// False for an empty or broken line.
bool parseLine(const std::string& raw, IrcLine& out);
std::string unescapeTag(const std::string& v);   // \s space, \: ;, \\ \, \r, \n

struct Message {
    std::string id;          // Twitch's message id (for deletions)
    std::string userId;      // for timeouts and bans
    std::string login;       // lower case
    std::string name;        // display name (the login if none)
    Rgba colour{0, 0, 0, 0}; // the user's chosen colour (alpha 0: none chosen)
    std::string text;
    bool action = false;     // "/me" messages
    std::vector<std::string> badges;   // e.g. "broadcaster", "moderator", "subscriber", "vip"
    std::int64_t atMs = 0;
    bool operator==(const Message&) const = default;
};

struct Event {
    enum Type { None, Chat, Ping, ClearAll, ClearUser, Delete, Joined, Notice, Reconnect } type = None;
    Message message;     // Chat
    std::string arg;     // Ping: what to answer; ClearUser: the user id; Delete: the message id; Notice: its text
};
Event interpret(const IrcLine& l, std::int64_t nowMs);

// The channel from what someone typed for Twitch: "nightowl", "@NightOwl", "twitch.tv/nightowl",
// "https://www.twitch.tv/nightowl/" all give "nightowl". "" if it isn't a possible Twitch name
// (4 to 25 letters, digits or underscores; Twitch's own older names can be shorter, so 1 to 25).
std::string channelFrom(const std::string& typed);

// The lines to send after connecting: ask for tags and commands, sign in anonymously, join.
std::vector<std::string> loginLines(const std::string& channel, unsigned anonNumber);

// A colour for a name with none chosen (Twitch's default colours, picked by the name).
Rgba defaultColour(const std::string& login);

// The recent messages.
class Log {
public:
    explicit Log(std::size_t keep = 100) : keep_(keep) {}
    // Applies an event; true if what's shown changed.
    bool apply(const Event& e);
    const std::vector<Message>& messages() const { return msgs_; }   // oldest first
    unsigned version() const { return version_; }
    void clear();

private:
    std::size_t keep_;
    std::vector<Message> msgs_;
    unsigned version_ = 0;
};

// Messages to show: the newest `count`, without ones older than `fadeSec` seconds (0: never fade).
std::vector<Message> recent(const std::vector<Message>& all, std::size_t count, int fadeSec, std::int64_t nowMs);

// Sample messages for the designer while nothing is connected (clearly made up: "Sample" names).
std::vector<Message> samples(std::int64_t nowMs);

}  // namespace ncap::chat
