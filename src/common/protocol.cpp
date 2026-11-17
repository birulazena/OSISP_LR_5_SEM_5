#include "protocol.hpp"

#include <cctype>
#include <stdexcept>

bool isValidMessageType(const std::string& type) {
    return type == "LOGIN" ||
           type == "SEND" ||
           type == "GET_PENDING" ||
           type == "LOGOUT" ||
           type == "ACK" ||
           type == "OK" ||
           type == "MSG" ||
           type == "ERROR";
}

bool parseIntStrict(const std::string& s, int& out) {
    if (s.empty()) return false;
    size_t idx = 0;
    try {
        int val = std::stoi(s, &idx);
        if (idx != s.size()) return false;
        out = val;
        return true;
    } catch (...) {
        return false;
    }
}

std::string serializeMessage(const Message& msg) {
    return std::to_string(msg.version) + "|" +
           msg.type + "|" +
           std::to_string(msg.reqId) + "|" +
           std::to_string(msg.senderId) + "|" +
           msg.body;
}

bool parseMessage(const std::string& raw, Message& out, std::string& errorDesc) {
    size_t p1 = raw.find('|');
    if (p1 == std::string::npos) { errorDesc = "Missing version delimiter"; return false; }
    size_t p2 = raw.find('|', p1 + 1);
    if (p2 == std::string::npos) { errorDesc = "Missing type delimiter"; return false; }
    size_t p3 = raw.find('|', p2 + 1);
    if (p3 == std::string::npos) { errorDesc = "Missing reqId delimiter"; return false; }
    size_t p4 = raw.find('|', p3 + 1);
    if (p4 == std::string::npos) { errorDesc = "Missing senderId delimiter"; return false; }

    std::string verStr = raw.substr(0, p1);
    std::string typeStr = raw.substr(p1 + 1, p2 - p1 - 1);
    std::string reqStr = raw.substr(p2 + 1, p3 - p2 - 1);
    std::string senderStr = raw.substr(p3 + 1, p4 - p3 - 1);
    std::string bodyStr = raw.substr(p4 + 1);

    int ver = 0, reqId = 0, senderId = 0;
    if (!parseIntStrict(verStr, ver) || ver != PROTOCOL_VERSION) {
        errorDesc = "Unsupported protocol version";
        return false;
    }
    if (!isValidMessageType(typeStr)) {
        errorDesc = "Unknown message type: " + typeStr;
        return false;
    }
    if (!parseIntStrict(reqStr, reqId) || reqId < 0) {
        errorDesc = "Invalid reqId";
        return false;
    }
    if (!parseIntStrict(senderStr, senderId) || senderId < 0) {
        errorDesc = "Invalid senderId";
        return false;
    }

    out.version = ver;
    out.type = typeStr;
    out.reqId = reqId;
    out.senderId = senderId;
    out.body = bodyStr;
    return true;
}