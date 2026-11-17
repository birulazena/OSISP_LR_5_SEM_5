#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>

constexpr const char* DEFAULT_HOST = "127.0.0.1";
constexpr uint16_t DEFAULT_PORT = 20016; // 20000 + STUDENT_ID (16)
constexpr int DEFAULT_SEED = 2552;       // 1000 + 16 * 97
constexpr uint32_t MAX_PAYLOAD_SIZE = 65536;

bool initWinsock();
void cleanupWinsock();

std::string getWsaErrorText(int err);
void printWsaError(const char* funcName, int err);

void logEvent(const std::string& role,
              int participantId,
              const std::string& peer,
              const std::string& operation,
              int reqId,
              const std::string& result);

std::string formatPeer(const sockaddr_in& addr);
bool setSocketTimeout(SOCKET sock, int timeoutMs);

// Возвращают: 1 = успех, 0 = штатное закрытие соединения, -1 = ошибка/тайм-аут
int sendAll(SOCKET sock, const char* data, int len);
int recvAll(SOCKET sock, char* data, int len);

bool sendFrame(SOCKET sock, const std::string& payload);
int recvFrame(SOCKET sock, std::string& payload);