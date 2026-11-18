#include "../common/net.hpp"
#include "../common/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

struct IncomingMsg {
    int origSender = 0;
    int seq = 0;
    std::string text;
};

static bool parseIncomingBody(const std::string& body, IncomingMsg& out) {
    size_t p1 = body.find('|');
    if (p1 == std::string::npos) return false;
    size_t p2 = body.find('|', p1 + 1);
    if (p2 == std::string::npos) return false;
    if (!parseIntStrict(body.substr(0, p1), out.origSender)) return false;
    if (!parseIntStrict(body.substr(p1 + 1, p2 - p1 - 1), out.seq)) return false;
    out.text = body.substr(p2 + 1);
    return true;
}

static double extractSrvMs(const std::string& body) {
    size_t pos = body.find("srv_ms=");
    if (pos == std::string::npos) return 0.0;
    size_t start = pos + 7;
    size_t end = body.find(';', start);
    std::string val = (end == std::string::npos) ? body.substr(start) : body.substr(start, end - start);
    try {
        return std::stod(val);
    } catch (...) {
        return 0.0;
    }
}

static int extractIntField(const std::string& body, const std::string& key, int defVal = -1) {
    size_t pos = body.find(key + "=");
    if (pos == std::string::npos) return defVal;
    size_t start = pos + key.size() + 1;
    size_t end = body.find(';', start);
    std::string val = (end == std::string::npos) ? body.substr(start) : body.substr(start, end - start);
    int out = defVal;
    parseIntStrict(val, out);
    return out;
}

static bool sendAndWait(SOCKET sock,
                        int clientId,
                        const std::string& peer,
                        const Message& req,
                        Message& resp,
                        std::vector<IncomingMsg>* pendingOut,
                        uint64_t& bytesSent,
                        uint64_t& bytesRecv,
                        double& rttMs) {
    std::string rawReq = serializeMessage(req);
    LARGE_INTEGER t0 = qpcNow();
    if (!sendFrame(sock, rawReq)) {
        printWsaError("sendFrame", WSAGetLastError());
        return false;
    }
    bytesSent += 4 + rawReq.size();

    while (true) {
        std::string rawResp;
        int r = recvFrame(sock, rawResp);
        if (r <= 0) {
            if (r == 0) {
                logEvent("CLIENT", clientId, peer, "RECV", req.reqId, "SERVER_CLOSED");
            } else {
                printWsaError("recvFrame", WSAGetLastError());
            }
            return false;
        }
        bytesRecv += 4 + rawResp.size();

        Message msg;
        std::string err;
        if (!parseMessage(rawResp, msg, err)) {
            logEvent("CLIENT", clientId, peer, "PARSE_ERROR", req.reqId, err);
            return false;
        }

        if (msg.type == "MSG") {
            IncomingMsg inc;
            if (parseIncomingBody(msg.body, inc)) {
                if (pendingOut != nullptr) {
                    pendingOut->push_back(inc);
                } else {
                    std::cout << "[MSG] from=" << inc.origSender
                              << " seq=" << inc.seq
                              << " text=\"" << inc.text << "\"\n";
                }
            }
            continue;
        }

        rttMs = qpcDiffMs(t0, qpcNow());
        resp = msg;
        return true;
    }
}

static void pollSocketMessages(SOCKET sock, uint64_t& bytesRecv) {
    while (true) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        timeval tv{0, 0};
        int ready = select(0, &rd, nullptr, nullptr, &tv);
        if (ready <= 0) break;

        std::string raw;
        if (recvFrame(sock, raw) <= 0) break;
        bytesRecv += 4 + raw.size();
        Message msg;
        std::string err;
        if (parseMessage(raw, msg, err) && msg.type == "MSG") {
            IncomingMsg inc;
            if (parseIncomingBody(msg.body, inc)) {
                std::cout << "[MSG] from=" << inc.origSender
                          << " seq=" << inc.seq
                          << " text=\"" << inc.text << "\"\n";
            }
        }
    }
}

static std::string makeRandomText(std::mt19937& rng, int index) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<int> dist(0, static_cast<int>(sizeof(alphabet) - 2));
    std::string suffix(16, 'a');
    for (char& c : suffix) c = alphabet[dist(rng)];
    return "msg_" + std::to_string(index) + "_" + suffix;
}

int main(int argc, char* argv[]) {
    std::string host = DEFAULT_HOST;
    uint16_t port = DEFAULT_PORT;
    int clientId = 1;
    int targetId = 2;
    int timeoutMs = 5000;
    int count = 5;
    int seed = DEFAULT_SEED;
    int durationMs = 3000;
    int runId = 1;
    std::string mode = "interactive";
    std::string csvPath;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--id" && i + 1 < argc) clientId = std::stoi(argv[++i]);
        else if (arg == "--target" && i + 1 < argc) targetId = std::stoi(argv[++i]);
        else if (arg == "--timeout" && i + 1 < argc) timeoutMs = std::stoi(argv[++i]);
        else if (arg == "--count" && i + 1 < argc) count = std::stoi(argv[++i]);
        else if (arg == "--seed" && i + 1 < argc) seed = std::stoi(argv[++i]);
        else if (arg == "--duration" && i + 1 < argc) durationMs = std::stoi(argv[++i]);
        else if (arg == "--run" && i + 1 < argc) runId = std::stoi(argv[++i]);
        else if (arg == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (arg == "--csv" && i + 1 < argc) csvPath = argv[++i];
    }

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
    LARGE_INTEGER tConn0 = qpcNow();
    if (connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        printWsaError("connect", WSAGetLastError());
        closesocket(sock);
        cleanupWinsock();
        return 1;
    }
    double connectMs = qpcDiffMs(tConn0, qpcNow());
    logEvent("CLIENT", clientId, peer, "CONNECT", 0, "OK ms=" + std::to_string(connectMs));

    uint64_t bytesSent = 0;
    uint64_t bytesRecv = 0;
    int reqId = 1;
    int exitCode = 0;

    auto doLogin = [&](Message& resp, double& rtt) -> bool {
        Message req{PROTOCOL_VERSION, "LOGIN", reqId++, clientId, ""};
        if (!sendAndWait(sock, clientId, peer, req, resp, nullptr, bytesSent, bytesRecv, rtt)) return false;
        logEvent("CLIENT", clientId, peer, "LOGIN", req.reqId, resp.type + " " + resp.body);
        return resp.type == "OK";
    };

    auto doLogout = [&]() {
        Message req{PROTOCOL_VERSION, "LOGOUT", reqId++, clientId, ""};
        Message resp;
        double rtt = 0.0;
        if (sendAndWait(sock, clientId, peer, req, resp, nullptr, bytesSent, bytesRecv, rtt)) {
            logEvent("CLIENT", clientId, peer, "LOGOUT", req.reqId, resp.type + " " + resp.body);
        }
    };

    if (mode == "send") {
        Message loginResp;
        double loginRtt = 0.0;
        if (!doLogin(loginResp, loginRtt)) {
            exitCode = 1;
        } else {
            std::mt19937 rng(static_cast<unsigned int>(seed));
            int okCount = 0;
            int errCount = 0;
            double sumRtt = 0.0;
            double sumSrv = 0.0;

            LARGE_INTEGER tStart = qpcNow();
            for (int k = 1; k <= count; ++k) {
                std::string text = makeRandomText(rng, k);
                Message req{PROTOCOL_VERSION, "SEND", reqId++, clientId,
                            std::to_string(targetId) + "|" + text};
                Message resp;
                double rtt = 0.0;
                if (!sendAndWait(sock, clientId, peer, req, resp, nullptr, bytesSent, bytesRecv, rtt)) {
                    errCount++;
                    break;
                }
                sumRtt += rtt;
                if (resp.type == "OK") {
                    okCount++;
                    sumSrv += extractSrvMs(resp.body);
                } else {
                    errCount++;
                    logEvent("CLIENT", clientId, peer, "SEND_ERR", req.reqId, resp.body);
                }
            }
            double totalMs = qpcDiffMs(tStart, qpcNow());
            double avgRtt = (okCount + errCount > 0) ? sumRtt / (okCount + errCount) : 0.0;
            double avgSrv = (okCount > 0) ? sumSrv / okCount : 0.0;
            double throughput = (totalMs > 0.0) ? (okCount * 1000.0 / totalMs) : 0.0;

            std::printf("SUMMARY mode=send count=%d ok=%d err=%d connect_ms=%.3f total_ms=%.3f avg_rtt_ms=%.3f avg_srv_ms=%.3f throughput_msg_s=%.2f bytes_sent=%llu bytes_recv=%llu\n",
                        count, okCount, errCount, connectMs, totalMs, avgRtt, avgSrv, throughput,
                        static_cast<unsigned long long>(bytesSent),
                        static_cast<unsigned long long>(bytesRecv));

            if (!csvPath.empty()) {
                bool writeHeader = false;
                {
                    std::ifstream chk(csvPath);
                    writeHeader = !chk.good() || chk.peek() == std::ifstream::traits_type::eof();
                }
                std::ofstream out(csvPath, std::ios::app);
                if (writeHeader) {
                    out << "phase,config_count,run_id,connect_ms,total_ms,avg_rtt_ms,avg_srv_ms,ok_ops,err_ops,bytes_sent,bytes_recv,throughput_msg_s\n";
                }
                out << "send," << count << "," << runId << ","
                    << connectMs << "," << totalMs << "," << avgRtt << "," << avgSrv << ","
                    << okCount << "," << errCount << "," << bytesSent << "," << bytesRecv << "," << throughput << "\n";
            }
            doLogout();
        }
    } else if (mode == "recv") {
        Message loginResp;
        double loginRtt = 0.0;
        if (!doLogin(loginResp, loginRtt)) {
            exitCode = 1;
        } else {
            LARGE_INTEGER tStart = qpcNow();
            std::vector<IncomingMsg> msgs;
            Message getResp;
            double getRtt = 0.0;
            Message getReq{PROTOCOL_VERSION, "GET_PENDING", reqId++, clientId, ""};
            if (!sendAndWait(sock, clientId, peer, getReq, getResp, &msgs, bytesSent, bytesRecv, getRtt) ||
                getResp.type != "OK") {
                exitCode = 1;
            } else {
                bool orderOk = true;
                for (size_t i = 1; i < msgs.size(); ++i) {
                    if (msgs[i].seq <= msgs[i - 1].seq) {
                        orderOk = false;
                        break;
                    }
                }
                double srvGetMs = extractSrvMs(getResp.body);

                // Подтверждаем полученные сообщения
                Message ackReq{PROTOCOL_VERSION, "ACK", reqId++, clientId,
                               "count=" + std::to_string(msgs.size())};
                Message ackResp;
                double ackRtt = 0.0;
                sendAndWait(sock, clientId, peer, ackReq, ackResp, nullptr, bytesSent, bytesRecv, ackRtt);

                // Проверяем, что подтверждённые сообщения повторно не выдаются
                std::vector<IncomingMsg> secondMsgs;
                Message checkResp;
                double checkRtt = 0.0;
                Message checkReq{PROTOCOL_VERSION, "GET_PENDING", reqId++, clientId, ""};
                sendAndWait(sock, clientId, peer, checkReq, checkResp, &secondMsgs, bytesSent, bytesRecv, checkRtt);
                int remainingAfterAck = extractIntField(checkResp.body, "count", -1);
                bool noRedelivery = (remainingAfterAck == 0 && secondMsgs.empty());

                double totalMs = qpcDiffMs(tStart, qpcNow());
                double throughput = (totalMs > 0.0) ? (msgs.size() * 1000.0 / totalMs) : 0.0;

                std::printf("SUMMARY mode=recv delivered=%zu order_ok=%d no_redelivery=%d connect_ms=%.3f get_rtt_ms=%.3f ack_rtt_ms=%.3f srv_get_ms=%.3f total_ms=%.3f bytes_sent=%llu bytes_recv=%llu\n",
                            msgs.size(), orderOk ? 1 : 0, noRedelivery ? 1 : 0,
                            connectMs, getRtt, ackRtt, srvGetMs, totalMs,
                            static_cast<unsigned long long>(bytesSent),
                            static_cast<unsigned long long>(bytesRecv));

                if (!csvPath.empty()) {
                    bool writeHeader = false;
                    {
                        std::ifstream chk(csvPath);
                        writeHeader = !chk.good() || chk.peek() == std::ifstream::traits_type::eof();
                    }
                    std::ofstream out(csvPath, std::ios::app);
                    if (writeHeader) {
                        out << "phase,config_count,run_id,connect_ms,total_ms,avg_rtt_ms,avg_srv_ms,ok_ops,err_ops,bytes_sent,bytes_recv,throughput_msg_s\n";
                    }
                    out << "recv," << msgs.size() << "," << runId << ","
                        << connectMs << "," << totalMs << "," << getRtt << "," << srvGetMs << ","
                        << msgs.size() << "," << ((orderOk && noRedelivery) ? 0 : 1) << ","
                        << bytesSent << "," << bytesRecv << "," << throughput << "\n";
                }
            }
            doLogout();
        }
    } else if (mode == "hold") {
        Message loginResp;
        double loginRtt = 0.0;
        if (!doLogin(loginResp, loginRtt)) {
            exitCode = 1;
        } else {
            LARGE_INTEGER t0 = qpcNow();
            while (qpcDiffMs(t0, qpcNow()) < durationMs) {
                pollSocketMessages(sock, bytesRecv);
                Sleep(50);
            }
            doLogout();
        }
    } else {
        // interactive mode
        std::cout << "Commands: LOGIN | SEND <targetId> <text> | GET_PENDING | LOGOUT | CHECK | RAW <payload> | QUIT\n";
        std::string line;
        while (true) {
            pollSocketMessages(sock, bytesRecv);
            std::cout << "> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            for (char& c : cmd) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

            if (cmd == "QUIT" || cmd == "EXIT") {
                break;
            } else if (cmd == "CHECK") {
                pollSocketMessages(sock, bytesRecv);
            } else if (cmd == "LOGIN") {
                Message resp;
                double rtt = 0.0;
                doLogin(resp, rtt);
            } else if (cmd == "SEND") {
                int tgt = 0;
                if (!(iss >> tgt)) {
                    std::cout << "Usage: SEND <targetId> <text>\n";
                    continue;
                }
                std::string text;
                std::getline(iss, text);
                if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                Message req{PROTOCOL_VERSION, "SEND", reqId++, clientId, std::to_string(tgt) + "|" + text};
                Message resp;
                double rtt = 0.0;
                if (sendAndWait(sock, clientId, peer, req, resp, nullptr, bytesSent, bytesRecv, rtt)) {
                    logEvent("CLIENT", clientId, peer, "SEND", req.reqId,
                             resp.type + " " + resp.body + " rtt_ms=" + std::to_string(rtt));
                }
            } else if (cmd == "GET_PENDING") {
                std::vector<IncomingMsg> msgs;
                Message req{PROTOCOL_VERSION, "GET_PENDING", reqId++, clientId, ""};
                Message resp;
                double rtt = 0.0;
                if (sendAndWait(sock, clientId, peer, req, resp, &msgs, bytesSent, bytesRecv, rtt)) {
                    for (const auto& m : msgs) {
                        std::cout << "  [PENDING] from=" << m.origSender
                                  << " seq=" << m.seq
                                  << " text=\"" << m.text << "\"\n";
                    }
                    logEvent("CLIENT", clientId, peer, "GET_PENDING", req.reqId, resp.type + " " + resp.body);
                    if (resp.type == "OK" && !msgs.empty()) {
                        Message ack{PROTOCOL_VERSION, "ACK", reqId++, clientId,
                                    "count=" + std::to_string(msgs.size())};
                        Message ackResp;
                        double ackRtt = 0.0;
                        if (sendAndWait(sock, clientId, peer, ack, ackResp, nullptr, bytesSent, bytesRecv, ackRtt)) {
                            logEvent("CLIENT", clientId, peer, "ACK", ack.reqId, ackResp.type + " " + ackResp.body);
                        }
                    }
                }
            } else if (cmd == "LOGOUT") {
                doLogout();
                break;
            } else if (cmd == "RAW") {
                std::string rawPayload;
                std::getline(iss, rawPayload);
                if (!rawPayload.empty() && rawPayload[0] == ' ') rawPayload.erase(0, 1);
                sendFrame(sock, rawPayload);
                std::string rawResp;
                if (recvFrame(sock, rawResp) > 0) {
                    std::cout << "[RAW RESP] " << rawResp << "\n";
                }
            } else {
                Message req{PROTOCOL_VERSION, cmd, reqId++, clientId, ""};
                Message resp;
                double rtt = 0.0;
                if (sendAndWait(sock, clientId, peer, req, resp, nullptr, bytesSent, bytesRecv, rtt)) {
                    logEvent("CLIENT", clientId, peer, cmd, req.reqId, resp.type + " " + resp.body);
                }
            }
        }
    }

    shutdown(sock, SD_BOTH);
    closesocket(sock);
    cleanupWinsock();
    logEvent("CLIENT", clientId, peer, "DISCONNECT", 0, "OK");
    return exitCode;
}