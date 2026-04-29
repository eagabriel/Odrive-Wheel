// Stub CommandHandler.h — satisfaz APIs usadas por EffectsCalculator/HidFFB.
#ifndef COMMANDHANDLER_H_
#define COMMANDHANDLER_H_

#ifdef __cplusplus
#include <vector>
#include <string>
#include <cstdint>

enum class CommandStatus : uint8_t { NOT_FOUND, OK, ERR, NO_REPLY, BROADCAST };
enum class CommandReplyType : uint8_t { NONE, ACK, INT, STRING, STRING_OR_INT, STRING_OR_DOUBLEINT, DOUBLEINTS, ERR };
enum class CMDtype : uint8_t { none, info, get, set, setget, err, getat, setat };

// Forward declaration of CommandInterface — the broadcastCommandReplyAsync
// stub is defined after CommandReply below.

namespace {
enum class ClassVisibility { Normal, Hidden, hidden = Hidden };
}

struct ClassIdentifier {
    const char *name;
    uint32_t id;
    ClassVisibility visibility = ClassVisibility::Normal;
};

struct ParsedCommand {
    uint32_t cmdId = 0;
    CMDtype type = CMDtype::none;
    int64_t val = 0;
    int64_t adr = 0;
    std::string string;
};

class CommandReply {
public:
    CommandReply() {}
    CommandReply(int64_t v) : val(v), type(CommandReplyType::INT) {}
    CommandReply(const std::string &s) : reply(s), type(CommandReplyType::STRING) {}
    CommandReply(int64_t v, int64_t a) : val(v), adr(a), type(CommandReplyType::DOUBLEINTS) {}
    int64_t val = 0;
    int64_t adr = 0;
    std::string reply;
    CommandReplyType type = CommandReplyType::ACK;
};

#define CMDFLAG_NONE        0x00
#define CMDFLAG_GET         0x01
#define CMDFLAG_SET         0x02
#define CMDFLAG_INFO        0x04
#define CMDFLAG_INFOSTRING  0x08
#define CMDFLAG_STR_ONLY    0x10
#define CMDFLAG_GETADR      0x20
#define CMDFLAG_SETADR      0x40
#define CMDFLAG_STR         0x80

class CommandHandler {
public:
    CommandHandler() {}
    CommandHandler(const char *cls, int id) { (void)cls; (void)id; }
    CommandHandler(const char *cls, int id, int inst) { (void)cls; (void)id; (void)inst; }
    CommandHandler(const char *cls, uint32_t id) { (void)cls; (void)id; }
    CommandHandler(const char *cls, uint32_t id, uint8_t inst) { (void)cls; (void)id; (void)inst; }
    virtual ~CommandHandler() {}

    virtual void registerCommands() {}
    template<typename IdType>
    void registerCommand(const char *name, IdType id, const char *help, uint32_t flags = 0) {
        (void)name; (void)id; (void)help; (void)flags;
    }
    virtual CommandStatus command(const ParsedCommand &cmd, std::vector<CommandReply> &replies) {
        (void)cmd; (void)replies;
        return CommandStatus::NOT_FOUND;
    }

    template<typename T>
    CommandStatus handleGetSet(const ParsedCommand &cmd, std::vector<CommandReply> &replies, T &var) {
        if (cmd.type == CMDtype::get) { replies.emplace_back((int64_t)var); return CommandStatus::OK; }
        if (cmd.type == CMDtype::set) { var = (T)cmd.val; return CommandStatus::OK; }
        return CommandStatus::ERR;
    }

    template<typename T, typename F>
    CommandStatus handleGetSetFunc(const ParsedCommand &cmd, std::vector<CommandReply> &replies, T val, F setter) {
        if (cmd.type == CMDtype::get) { replies.emplace_back((int64_t)val); return CommandStatus::OK; }
        if (cmd.type == CMDtype::set) { setter((T)cmd.val); return CommandStatus::OK; }
        return CommandStatus::ERR;
    }

    std::string getHelpstring() { return ""; }
};

class CommandInterface {
public:
    static void broadcastCommandReplyAsync(std::vector<CommandReply>& replies, void *src,
                                            uint32_t cmdId, CMDtype type) {
        (void)replies; (void)src; (void)cmdId; (void)type;
    }
};

#endif
#endif
