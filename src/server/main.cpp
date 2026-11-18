#include "../common/net.hpp"
#include "../common/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <iostream>
#include <map>
#include <string>
#include <vector>

struct PendingMsg {
    int seq = 0;
    int senderId = 0;
    std::string text;
};

struct ClientSession {
    SOCKET sock = INVALID_SOCKET;
    std::string peer;
    int userId = 0;
};

static std::atomic<bool> g_running{true};
static SOCKET g_listenSock = INVALID_SOCKET;

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        if (g_listenSock != INVALID_SOCKET) {
            closesocket(g_listenSock);
            g_listenSock = INVALID_SOCKET;
        }
        return TRUE;
    }
    return FALSE;
}

static std::string fmtMs(double ms) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", ms);
    return std::string(buf);
}

static void sendError(SOCKET sock, int reqId, const std::string& code, const std::string& desc) {
    Message err{PROTOCOL_VERSION, "ERROR", reqId, 0, code + "|" + desc};
    sendFrame(sock, serializeMessage(err));
}

int main(int argc, char* argv[]) {
    std::string host = DEFAULT_HOST;
    uint16_t port = DEFAULT_PORT;
    int timeoutMs = 5000;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeoutMs = std::stoi(argv[++i]);
        }
    }

    if (!initWinsock()) return 1;
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    g_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listenSock == INVALID_SOCKET) {
        printWsaError("socket", WSAGetLastError());
        cleanupWinsock();
        return 1;
    }

    int opt = 1;
    setsockopt(g_listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[ERROR] Invalid host address: " << host << "\n";
        closesocket(g_listenSock);
        cleanupWinsock();
        return 1;
    }

    if (bind(g_listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        printWsaError("bind", WSAGetLastError());
        closesocket(g_listenSock);
        cleanupWinsock();
        return 1;
    }

    if (listen(g_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        printWsaError("listen", WSAGetLastError());
        closesocket(g_listenSock);
        cleanupWinsock();
        return 1;
    }

    logEvent("SERVER", 0, host + ":" + std::to_string(port), "START", 0, "OK");

    std::vector<ClientSession> sessions;
    std::map<int, std::deque<PendingMsg>> pendingQueues;
    int globalSeq = 1;

    auto findOnline = [&](int uid) -> ClientSession* {
        for (auto& s : sessions) {
            if (s.userId == uid) return &s;
        }
        return nullptr;
    };

    while (g_running) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(g_listenSock, &readSet);
        for (const auto& s : sessions) {
            FD_SET(s.sock, &readSet);
        }

        timeval tv{1, 0};
        int ready = select(0, &readSet, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) {
            if (g_running) {
                printWsaError("select", WSAGetLastError());
            }
            break;
        }
        if (ready == 0) continue;

        if (FD_ISSET(g_listenSock, &readSet)) {
            sockaddr_in clientAddr{};
            int clientLen = sizeof(clientAddr);
            SOCKET clientSock = accept(g_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
            if (clientSock == INVALID_SOCKET) {
                if (g_running) {
                    printWsaError("accept", WSAGetLastError());
                }
            } else {
                if (sessions.size() + 1 >= FD_SETSIZE) {
                    sendError(clientSock, 0, "SERVER_BUSY", "Too many connections");
                    shutdown(clientSock, SD_BOTH);
                    closesocket(clientSock);
                } else {
                    setSocketTimeout(clientSock, timeoutMs);
                    std::string peer = formatPeer(clientAddr);
                    sessions.push_back({clientSock, peer, 0});
                    logEvent("SERVER", 0, peer, "CLIENT_CONNECTED", 0, "OK");
                }
            }
        }

        for (size_t i = 0; i < sessions.size(); ) {
            ClientSession& s = sessions[i];
            if (!FD_ISSET(s.sock, &readSet)) {
                ++i;
                continue;
            }

            std::string raw;
            int r = recvFrame(s.sock, raw);
            if (r == 0) {
                logEvent("SERVER", s.userId, s.peer, "CLIENT_DISCONNECTED", 0, "CLOSED");
                shutdown(s.sock, SD_BOTH);
                closesocket(s.sock);
                sessions.erase(sessions.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            if (r < 0) {
                int err = WSAGetLastError();
                printWsaError("recvFrame", err);
                logEvent("SERVER", s.userId, s.peer, "CLIENT_DISCONNECTED", 0, "ERR_" + std::to_string(err));
                shutdown(s.sock, SD_BOTH);
                closesocket(s.sock);
                sessions.erase(sessions.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }

            LARGE_INTEGER t0 = qpcNow();
            Message req;
            std::string errDesc;
            if (!parseMessage(raw, req, errDesc)) {
                logEvent("SERVER", s.userId, s.peer, "RECV_INVALID", 0, errDesc);
                sendError(s.sock, 0, "BAD_FORMAT", errDesc);
                ++i;
                continue;
            }

            logEvent("SERVER", req.senderId, s.peer, "RECV_" + req.type, req.reqId,
                     "size=" + std::to_string(raw.size()));

            bool closeThisClient = false;

            if (req.type == "LOGIN") {
                if (req.senderId <= 0) {
                    sendError(s.sock, req.reqId, "INVALID_ID", "User ID must be positive");
                    logEvent("SERVER", req.senderId, s.peer, "LOGIN", req.reqId, "ERROR_INVALID_ID");
                } else if (s.userId != 0) {
                    sendError(s.sock, req.reqId, "ALREADY_LOGGED_IN", "Session already authenticated");
                    logEvent("SERVER", req.senderId, s.peer, "LOGIN", req.reqId, "ERROR_ALREADY_LOGGED_IN");
                } else if (findOnline(req.senderId) != nullptr) {
                    sendError(s.sock, req.reqId, "USER_ONLINE", "User is already connected");
                    logEvent("SERVER", req.senderId, s.peer, "LOGIN", req.reqId, "ERROR_USER_ONLINE");
                } else {
                    s.userId = req.senderId;
                    size_t pendingCount = pendingQueues[s.userId].size();
                    double srvMs = qpcDiffMs(t0, qpcNow());
                    Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0,
                                 "pending=" + std::to_string(pendingCount) + ";srv_ms=" + fmtMs(srvMs)};
                    sendFrame(s.sock, serializeMessage(resp));
                    logEvent("SERVER", s.userId, s.peer, "LOGIN", req.reqId,
                             "OK pending=" + std::to_string(pendingCount));
                }
            } else if (s.userId == 0) {
                sendError(s.sock, req.reqId, "NOT_LOGGED_IN", "LOGIN required before commands");
                logEvent("SERVER", req.senderId, s.peer, req.type, req.reqId, "ERROR_NOT_LOGGED_IN");
            } else if (req.senderId != s.userId) {
                sendError(s.sock, req.reqId, "SENDER_MISMATCH", "senderId does not match logged-in user");
                logEvent("SERVER", s.userId, s.peer, req.type, req.reqId, "ERROR_SENDER_MISMATCH");
            } else if (req.type == "SEND") {
                size_t sep = req.body.find('|');
                int targetId = 0;
                if (sep == std::string::npos ||
                    !parseIntStrict(req.body.substr(0, sep), targetId) ||
                    targetId <= 0 ||
                    sep + 1 >= req.body.size()) {
                    sendError(s.sock, req.reqId, "INVALID_PARAMS", "SEND body must be <targetId>|<non-empty text>");
                    logEvent("SERVER", s.userId, s.peer, "SEND", req.reqId, "ERROR_INVALID_PARAMS");
                } else {
                    std::string text = req.body.substr(sep + 1);
                    int seq = globalSeq++;
                    ClientSession* targetSession = findOnline(targetId);
                    if (targetSession != nullptr) {
                        Message pushMsg{PROTOCOL_VERSION, "MSG", 0, 0,
                                        std::to_string(s.userId) + "|" + std::to_string(seq) + "|" + text};
                        if (sendFrame(targetSession->sock, serializeMessage(pushMsg))) {
                            double srvMs = qpcDiffMs(t0, qpcNow());
                            Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0,
                                         "status=DELIVERED;seq=" + std::to_string(seq) + ";srv_ms=" + fmtMs(srvMs)};
                            sendFrame(s.sock, serializeMessage(resp));
                            logEvent("SERVER", s.userId, s.peer, "SEND", req.reqId,
                                     "DELIVERED to=" + std::to_string(targetId));
                        } else {
                            // Адресат оборвался во время отправки — сохраняем в очередь
                            auto& q = pendingQueues[targetId];
                            if (static_cast<int>(q.size()) >= MAX_PENDING_MESSAGES) {
                                sendError(s.sock, req.reqId, "QUEUE_FULL",
                                          "Pending queue limit (20) reached for user " + std::to_string(targetId));
                                logEvent("SERVER", s.userId, s.peer, "SEND", req.reqId,
                                         "QUEUE_FULL target=" + std::to_string(targetId));
                            } else {
                                q.push_back({seq, s.userId, text});
                                double srvMs = qpcDiffMs(t0, qpcNow());
                                Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0,
                                             "status=QUEUED;queueSize=" + std::to_string(q.size()) +
                                                 ";srv_ms=" + fmtMs(srvMs)};
                                sendFrame(s.sock, serializeMessage(resp));
                                logEvent("SERVER", s.userId, s.peer, "SEND", req.reqId,
                                         "QUEUED target=" + std::to_string(targetId) +
                                             " qsize=" + std::to_string(q.size()));
                            }
                        }
                    } else {
                        auto& q = pendingQueues[targetId];
                        if (static_cast<int>(q.size()) >= MAX_PENDING_MESSAGES) {
                            sendError(s.sock, req.reqId, "QUEUE_FULL",
                                      "Pending queue limit (20) reached for user " + std::to_string(targetId));
                            logEvent("SERVER", s.userId, s.peer, "SEND", req.reqId,
                                     "QUEUE_FULL target=" + std::to_string(targetId));
                        } else {
                            q.push_back({seq, s.userId, text});
                            double srvMs = qpcDiffMs(t0, qpcNow());
                            Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0,
                                         "status=QUEUED;queueSize=" + std::to_string(q.size()) +
                                             ";srv_ms=" + fmtMs(srvMs)};
                            sendFrame(s.sock, serializeMessage(resp));
                            logEvent("SERVER", s.userId, s.peer, "SEND", req.reqId,
                                     "QUEUED target=" + std::to_string(targetId) +
                                         " qsize=" + std::to_string(q.size()));
                        }
                    }
                }
            } else if (req.type == "GET_PENDING") {
                const auto& q = pendingQueues[s.userId];
                for (const auto& pm : q) {
                    Message m{PROTOCOL_VERSION, "MSG", req.reqId, 0,
                              std::to_string(pm.senderId) + "|" + std::to_string(pm.seq) + "|" + pm.text};
                    sendFrame(s.sock, serializeMessage(m));
                }
                double srvMs = qpcDiffMs(t0, qpcNow());
                Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0,
                             "count=" + std::to_string(q.size()) + ";srv_ms=" + fmtMs(srvMs)};
                sendFrame(s.sock, serializeMessage(resp));
                logEvent("SERVER", s.userId, s.peer, "GET_PENDING", req.reqId,
                         "sent=" + std::to_string(q.size()));
            } else if (req.type == "ACK") {
                int ackCount = -1;
                if (req.body.rfind("count=", 0) != 0 ||
                    !parseIntStrict(req.body.substr(6), ackCount) ||
                    ackCount < 0) {
                    sendError(s.sock, req.reqId, "INVALID_PARAMS", "ACK body must be count=<N>");
                    logEvent("SERVER", s.userId, s.peer, "ACK", req.reqId, "ERROR_INVALID_PARAMS");
                } else {
                    auto& q = pendingQueues[s.userId];
                    size_t toRemove = std::min(static_cast<size_t>(ackCount), q.size());
                    q.erase(q.begin(), q.begin() + static_cast<ptrdiff_t>(toRemove));
                    double srvMs = qpcDiffMs(t0, qpcNow());
                    Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0,
                                 "cleared=" + std::to_string(toRemove) +
                                     ";remaining=" + std::to_string(q.size()) +
                                     ";srv_ms=" + fmtMs(srvMs)};
                    sendFrame(s.sock, serializeMessage(resp));
                    logEvent("SERVER", s.userId, s.peer, "ACK", req.reqId,
                             "cleared=" + std::to_string(toRemove));
                }
            } else if (req.type == "LOGOUT") {
                double srvMs = qpcDiffMs(t0, qpcNow());
                Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0, "bye;srv_ms=" + fmtMs(srvMs)};
                sendFrame(s.sock, serializeMessage(resp));
                logEvent("SERVER", s.userId, s.peer, "LOGOUT", req.reqId, "OK");
                closeThisClient = true;
            } else {
                sendError(s.sock, req.reqId, "UNKNOWN_COMMAND", "Unsupported command: " + req.type);
                logEvent("SERVER", s.userId, s.peer, "UNKNOWN", req.reqId, req.type);
            }

            if (closeThisClient) {
                shutdown(s.sock, SD_BOTH);
                closesocket(s.sock);
                sessions.erase(sessions.begin() + static_cast<ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    for (auto& s : sessions) {
        shutdown(s.sock, SD_BOTH);
        closesocket(s.sock);
    }
    if (g_listenSock != INVALID_SOCKET) {
        closesocket(g_listenSock);
    }
    cleanupWinsock();
    logEvent("SERVER", 0, "-", "STOP", 0, "OK");
    return 0;
}