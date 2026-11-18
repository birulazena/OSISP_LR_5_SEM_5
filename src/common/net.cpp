#include "net.hpp"

#include <cstdio>
#include <iostream>
#include <vector>

bool initWinsock() {
    WSADATA wsaData{};
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        printWsaError("WSAStartup", res);
        return false;
    }
    return true;
}

void cleanupWinsock() {
    WSACleanup();
}

std::string getWsaErrorText(int err) {
    char* msgBuf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageA(flags, nullptr, static_cast<DWORD>(err),
                               MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
                               reinterpret_cast<LPSTR>(&msgBuf), 0, nullptr);
    if (len == 0 || msgBuf == nullptr) {
        return "Unknown error";
    }
    std::string result(msgBuf, len);
    LocalFree(msgBuf);
    while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' ')) {
        result.pop_back();
    }
    return result;
}

void printWsaError(const char* funcName, int err) {
    std::cerr << "[ERROR] " << funcName << " failed: code=" << err
              << " (" << getWsaErrorText(err) << ")\n";
}

void logEvent(const std::string& role,
              int participantId,
              const std::string& peer,
              const std::string& operation,
              int reqId,
              const std::string& result) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char timeBuf[32];
    std::snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u.%03u",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::cout << "[" << timeBuf << "] " << role
              << " id=" << participantId
              << " peer=" << (peer.empty() ? "-" : peer)
              << " op=" << operation
              << " reqId=" << reqId
              << " result=" << result << "\n";
}

std::string formatPeer(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

bool setSocketTimeout(SOCKET sock, int timeoutMs) {
    if (timeoutMs <= 0) return true;
    DWORD tv = static_cast<DWORD>(timeoutMs);
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv)) == SOCKET_ERROR) {
        printWsaError("setsockopt(SO_RCVTIMEO)", WSAGetLastError());
        return false;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv)) == SOCKET_ERROR) {
        printWsaError("setsockopt(SO_SNDTIMEO)", WSAGetLastError());
        return false;
    }
    return true;
}

LARGE_INTEGER qpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t;
}

double qpcDiffMs(LARGE_INTEGER start, LARGE_INTEGER end) {
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart == 0) return 0.0;
    return static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

int sendAll(SOCKET sock, const char* data, int len) {
    int total = 0;
    while (total < len) {
        int sent = send(sock, data + total, len - total, 0);
        if (sent == SOCKET_ERROR) {
            return -1;
        }
        if (sent == 0) {
            return 0;
        }
        total += sent;
    }
    return 1;
}

int recvAll(SOCKET sock, char* data, int len) {
    int total = 0;
    while (total < len) {
        int recvd = recv(sock, data + total, len - total, 0);
        if (recvd == SOCKET_ERROR) {
            return -1;
        }
        if (recvd == 0) {
            return 0;
        }
        total += recvd;
    }
    return 1;
}

bool sendFrame(SOCKET sock, const std::string& payload) {
    if (payload.size() > MAX_PAYLOAD_SIZE) {
        std::cerr << "[ERROR] sendFrame: payload size exceeds " << MAX_PAYLOAD_SIZE << "\n";
        return false;
    }
    uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
    if (sendAll(sock, reinterpret_cast<const char*>(&netLen), sizeof(netLen)) != 1) {
        return false;
    }
    if (!payload.empty()) {
        if (sendAll(sock, payload.data(), static_cast<int>(payload.size())) != 1) {
            return false;
        }
    }
    return true;
}

int recvFrame(SOCKET sock, std::string& payload) {
    uint32_t netLen = 0;
    int status = recvAll(sock, reinterpret_cast<char*>(&netLen), sizeof(netLen));
    if (status <= 0) {
        return status;
    }
    uint32_t len = ntohl(netLen);
    if (len == 0 || len > MAX_PAYLOAD_SIZE) {
        std::cerr << "[ERROR] recvFrame: invalid frame length=" << len << "\n";
        return -1;
    }
    std::vector<char> buf(len);
    status = recvAll(sock, buf.data(), static_cast<int>(len));
    if (status <= 0) {
        return status;
    }
    payload.assign(buf.data(), len);
    return 1;
}