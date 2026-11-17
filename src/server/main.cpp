#include "../common/net.hpp"
#include "../common/protocol.hpp"

#include <atomic>
#include <iostream>
#include <string>

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

    while (g_running) {
        sockaddr_in clientAddr{};
        int clientLen = sizeof(clientAddr);
        SOCKET clientSock = accept(g_listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientSock == INVALID_SOCKET) {
            if (g_running) {
                printWsaError("accept", WSAGetLastError());
            }
            break;
        }

        setSocketTimeout(clientSock, timeoutMs);
        std::string peer = formatPeer(clientAddr);
        logEvent("SERVER", 0, peer, "CLIENT_CONNECTED", 0, "OK");

        int currentUserId = 0;
        std::string raw;
        while (g_running) {
            int r = recvFrame(clientSock, raw);
            if (r == 0) {
                logEvent("SERVER", 0, peer, "CLIENT_DISCONNECTED", 0, "CLOSED");
                break;
            }
            if (r < 0) {
                printWsaError("recvFrame", WSAGetLastError());
                break;
            }

            Message req;
            std::string errDesc;
            if (!parseMessage(raw, req, errDesc)) {
                logEvent("SERVER", 0, peer, "RECV_INVALID", 0, errDesc);
                Message errMsg{PROTOCOL_VERSION, "ERROR", 0, 0, "BAD_FORMAT|" + errDesc};
                sendFrame(clientSock, serializeMessage(errMsg));
                continue;
            }

            logEvent("SERVER", 0, peer, "RECV_" + req.type, req.reqId, "senderId=" + std::to_string(req.senderId));

            if (req.type == "LOGIN") {
                currentUserId = req.senderId;
                Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0, "pending=0"};
                sendFrame(clientSock, serializeMessage(resp));
                logEvent("SERVER", 0, peer, "SEND_OK", req.reqId, "logged_in=" + std::to_string(currentUserId));
            } else if (req.type == "LOGOUT") {
                Message resp{PROTOCOL_VERSION, "OK", req.reqId, 0, "bye"};
                sendFrame(clientSock, serializeMessage(resp));
                logEvent("SERVER", 0, peer, "LOGOUT", req.reqId, "OK");
                break;
            } else {
                Message resp{PROTOCOL_VERSION, "ERROR", req.reqId, 0, "NOT_IMPLEMENTED|Stage 2 stub"};
                sendFrame(clientSock, serializeMessage(resp));
            }
        }

        shutdown(clientSock, SD_BOTH);
        closesocket(clientSock);
    }

    if (g_listenSock != INVALID_SOCKET) {
        closesocket(g_listenSock);
    }
    cleanupWinsock();
    logEvent("SERVER", 0, "-", "STOP", 0, "OK");
    return 0;
}