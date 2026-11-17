#pragma once

#include <string>

constexpr int PROTOCOL_VERSION = 1;
constexpr int MAX_PENDING_MESSAGES = 20;

struct Message {
    int version = PROTOCOL_VERSION;
    std::string type;
    int reqId = 0;
    int senderId = 0;
    std::string body;
};

bool isValidMessageType(const std::string& type);
std::string serializeMessage(const Message& msg);
bool parseMessage(const std::string& raw, Message& out, std::string& errorDesc);
bool parseIntStrict(const std::string& s, int& out);