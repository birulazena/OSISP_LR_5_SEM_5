#include "../common/net.hpp"
#include "../common/protocol.hpp"

#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string host = DEFAULT_HOST;
    uint16_t port = DEFAULT_PORT;
    int clientId = 1;
    int timeoutMs = 5000;
    int count = 5;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--id" && i + 1 < argc) {
            clientId = std::stoi(argv[++i]);
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeoutMs = std::stoi(argv[++i]);
        } else if (arg == "--count" && i + 1 < argc) {
            count = std::stoi(argv[++i]);
        }
    }
    (void)count;

    if (!initWinsock()) return 1;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        printWsaError("socket", WSAGetLastError());
        cleanupWinsock();
        return 1;
    }

    setSocketTimeout(sock, timeoutMs);

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr) != 1) {
        std::cerr << "[ERROR] Invalid server address: " << host << "\n";
        closesocket(sock);
        cleanupWinsock();
        return 1;
    }

    std::string peer = host + ":" + std::to_string(port);
    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        printWsaError("connect", WSAGetLastError());
        closesocket(sock);
        cleanupWinsock();
        return 1;
    }
    logEvent("CLIENT", clientId, peer, "CONNECT", 0, "OK");

    Message loginReq{PROTOCOL_VERSION, "LOGIN", 1, clientId, ""};
    if (!sendFrame(sock, serializeMessage(loginReq))) {
        printWsaError("sendFrame(LOGIN)", WSAGetLastError());
        closesocket(sock);
        cleanupWinsock();
        return 1;
    }

    std::string rawResp;
    if (recvFrame(sock, rawResp) <= 0) {
        printWsaError("recvFrame(LOGIN)", WSAGetLastError());
        closesocket(sock);
        cleanupWinsock();
        return 1;
    }

    Message resp;
    std::string err;
    if (parseMessage(rawResp, resp, err)) {
        logEvent("CLIENT", clientId, peer, "RECV_" + resp.type, resp.reqId, resp.body);
    }

    Message logoutReq{PROTOCOL_VERSION, "LOGOUT", 2, clientId, ""};
    sendFrame(sock, serializeMessage(logoutReq));
    if (recvFrame(sock, rawResp) > 0 && parseMessage(rawResp, resp, err)) {
        logEvent("CLIENT", clientId, peer, "RECV_" + resp.type, resp.reqId, resp.body);
    }

    shutdown(sock, SD_BOTH);
    closesocket(sock);
    cleanupWinsock();
    logEvent("CLIENT", clientId, peer, "DISCONNECT", 0, "OK");
    return 0;
}