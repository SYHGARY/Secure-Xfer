// server.cpp  (Windows / WinSock2 / UDP TFTP-like)
// 功能：服务端只存储压缩文件：filename + ".hfm"
// 下载请求 filename -> 实际发送 filename + ".hfm"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sys/stat.h>

#pragma comment(lib, "Ws2_32.lib")

#define LISTEN_PORT 69
#define BUFFER_SIZE 516          // 2(op) + 2(block) + 512(data)
#define DATA_SIZE 512
#define MAX_FILENAME 256

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

#define ERROR_NOT_FOUND        1
#define ERROR_ACCESS_VIOLATION 2
#define ERROR_ILLEGAL_OP       4
#define ERROR_UNKNOWN_TID      5

#define TFTP_ROOT_DIR "tftp_root"

static void print_peer(const char* tag, const sockaddr_in* addr) {
    char ip[64] = { 0 };
    inet_ntop(AF_INET, (void*)&addr->sin_addr, ip, sizeof(ip));
    printf("%s %s:%d\n", tag, ip, ntohs(addr->sin_port));
}

static void send_error_packet(SOCKET sock, const sockaddr_in* client_addr, int error_code, const char* error_msg) {
    char pkt[BUFFER_SIZE];
    short op = htons(OP_ERROR);
    short ec = htons((short)error_code);
    int msg_len = (int)strlen(error_msg);

    memcpy(pkt, &op, 2);
    memcpy(pkt + 2, &ec, 2);
    memcpy(pkt + 4, error_msg, msg_len);
    pkt[4 + msg_len] = '\0';

    sendto(sock, pkt, 5 + msg_len, 0, (const sockaddr*)client_addr, sizeof(*client_addr));
}

static int parse_rrq_wrq(const char* buf, int n, char* out_filename, size_t out_sz) {
    if (n < 4) return 0;
    const char* p = buf + 2;
    const char* end = buf + n;

    const char* fn_end = (const char*)memchr(p, '\0', end - p);
    if (!fn_end) return 0;
    size_t fn_len = (size_t)(fn_end - p);
    if (fn_len == 0 || fn_len >= out_sz) return 0;
    memcpy(out_filename, p, fn_len);
    out_filename[fn_len] = '\0';

    const char* mode = fn_end + 1;
    if (mode >= end) return 0;
    const char* mode_end = (const char*)memchr(mode, '\0', end - mode);
    if (!mode_end) return 0;
    if (mode_end == mode) return 0;

    return 1;
}

static int same_tid(const sockaddr_in* a, const sockaddr_in* b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static SOCKET create_transfer_socket() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(s, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    int timeout_ms = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
    return s;
}

static int send_ack(SOCKET sock, const sockaddr_in* peer, unsigned short block) {
    char pkt[4];
    short op = htons(OP_ACK);
    short bn = htons((short)block);
    memcpy(pkt, &op, 2);
    memcpy(pkt + 2, &bn, 2);
    return sendto(sock, pkt, 4, 0, (const sockaddr*)peer, sizeof(*peer));
}

static int recv_packet(SOCKET sock, char* buf, int bufsz, sockaddr_in* from, int* out_n) {
    int from_len = sizeof(*from);
    int n = recvfrom(sock, buf, bufsz, 0, (sockaddr*)from, &from_len);
    if (n == SOCKET_ERROR) return 0;
    *out_n = n;
    return 1;
}

static int is_safe_filename(const char* name) {
    if (!name || !name[0]) return 0;
    if (strstr(name, "..")) return 0;
    for (const char* p = name; *p; ++p) {
        char c = *p;
        if (c == '/' || c == '\\' || c == ':') return 0;
    }
    return 1;
}

static int ends_with(const char* s, const char* suffix) {
    size_t sl = strlen(s), su = strlen(suffix);
    if (sl < su) return 0;
    return strcmp(s + (sl - su), suffix) == 0;
}

static int build_server_path_compressed(const char* filename, char* out, int out_sz) {
    if (!is_safe_filename(filename)) return 0;
    CreateDirectoryA(TFTP_ROOT_DIR, NULL);

    const char* ext = ".hfm";
    char namebuf[MAX_FILENAME * 2] = { 0 };

    if (ends_with(filename, ext)) {
        strncpy_s(namebuf, filename, _TRUNCATE);
    }
    else {
        sprintf_s(namebuf, sizeof(namebuf), "%s%s", filename, ext);
    }

    int need = (int)strlen(TFTP_ROOT_DIR) + 1 + (int)strlen(namebuf) + 1;
    if (need > out_sz) return 0;
    sprintf_s(out, out_sz, "%s\\%s", TFTP_ROOT_DIR, namebuf);
    return 1;
}

static long long file_size_64(const char* path) {
    struct _stat64 st;
    if (_stat64(path, &st) != 0) return -1;
    return (long long)st.st_size;
}

static void print_progress_bar(const char* prefix, long long done, long long total) {
    const int width = 30;
    if (total <= 0) {
        printf("\r%s %lld bytes", prefix, done);
        fflush(stdout);
        return;
    }
    double pct = (double)done * 100.0 / (double)total;
    int filled = (int)(pct / 100.0 * width);
    if (filled < 0) filled = 0;
    if (filled > width) filled = width;

    printf("\r%s [", prefix);
    for (int i = 0; i < width; i++) putchar(i < filled ? '#' : '-');
    printf("] %6.2f%% (%lld/%lld bytes)", pct, done, total);
    fflush(stdout);
}

static void handle_rrq(const sockaddr_in* client_addr, const char* filename) {
    char path[512];
    if (!build_server_path_compressed(filename, path, (int)sizeof(path))) {
        SOCKET tmp = create_transfer_socket();
        if (tmp != INVALID_SOCKET) {
            send_error_packet(tmp, client_addr, ERROR_ACCESS_VIOLATION, "Access violation (bad filename)");
            closesocket(tmp);
        }
        printf("[RRQ] reject bad filename: %s\n", filename);
        return;
    }

    FILE* file = NULL;
    fopen_s(&file, path, "rb");
    if (!file) {
        SOCKET tmp = create_transfer_socket();
        if (tmp != INVALID_SOCKET) {
            send_error_packet(tmp, client_addr, ERROR_NOT_FOUND, "File not found (compressed missing)");
            closesocket(tmp);
        }
        printf("[RRQ] file not found: %s\n", path);
        return;
    }

    long long total = file_size_64(path);
    SOCKET tsock = create_transfer_socket();
    if (tsock == INVALID_SOCKET) {
        fclose(file);
        return;
    }

    printf("[RRQ] start send (compressed): %s\n", path);
    print_peer("[RRQ] client:", client_addr);

    unsigned short block = 1;
    long long sent_bytes = 0;
    char pkt[BUFFER_SIZE];
    char rbuf[BUFFER_SIZE];

    int needs_final_zero = (total >= 0 && (total % DATA_SIZE == 0));

    while (1) {
        size_t bytes_read = fread(pkt + 4, 1, DATA_SIZE, file);

        short op = htons(OP_DATA);
        short bn = htons((short)block);
        memcpy(pkt, &op, 2);
        memcpy(pkt + 2, &bn, 2);

        int pkt_len = (int)bytes_read + 4;

        int retries = 0;
        for (;;) {
            sendto(tsock, pkt, pkt_len, 0, (const sockaddr*)client_addr, sizeof(*client_addr));

            sockaddr_in from;
            int n = 0;
            if (!recv_packet(tsock, rbuf, BUFFER_SIZE, &from, &n)) {
                if (++retries >= 5) {
                    printf("\n[RRQ] too many timeouts, abort\n");
                    fclose(file);
                    closesocket(tsock);
                    return;
                }
                continue;
            }

            if (!same_tid(&from, client_addr)) {
                send_error_packet(tsock, &from, ERROR_UNKNOWN_TID, "Unknown transfer ID");
                continue;
            }

            if (n >= 4 && ntohs(*(short*)rbuf) == OP_ACK && ntohs(*(short*)(rbuf + 2)) == block) {
                break;
            }
        }

        sent_bytes += (long long)bytes_read;
        print_progress_bar("[RRQ] sending(hfm)", sent_bytes, total);

        if (bytes_read < DATA_SIZE) {
            needs_final_zero = 0;
            break;
        }

        block++;
        if (block == 0) block = 1;
    }

    if (needs_final_zero) {
        short op = htons(OP_DATA);
        short bn = htons((short)block);
        memcpy(pkt, &op, 2);
        memcpy(pkt + 2, &bn, 2);
        int pkt_len = 4;

        int retries = 0;
        for (;;) {
            sendto(tsock, pkt, pkt_len, 0, (const sockaddr*)client_addr, sizeof(*client_addr));
            sockaddr_in from;
            int n = 0;
            if (!recv_packet(tsock, rbuf, BUFFER_SIZE, &from, &n)) {
                if (++retries >= 5) break;
                continue;
            }
            if (!same_tid(&from, client_addr)) {
                send_error_packet(tsock, &from, ERROR_UNKNOWN_TID, "Unknown transfer ID");
                continue;
            }
            if (n >= 4 && ntohs(*(short*)rbuf) == OP_ACK && ntohs(*(short*)(rbuf + 2)) == block) {
                break;
            }
        }
    }

    printf("\n[RRQ] done\n");
    fclose(file);
    closesocket(tsock);
}

static void handle_wrq(const sockaddr_in* client_addr, const char* filename) {
    char save_path[512];
    if (!build_server_path_compressed(filename, save_path, (int)sizeof(save_path))) {
        SOCKET tmp = create_transfer_socket();
        if (tmp != INVALID_SOCKET) {
            send_error_packet(tmp, client_addr, ERROR_ACCESS_VIOLATION, "Access violation (bad filename)");
            closesocket(tmp);
        }
        printf("[WRQ] reject bad filename: %s\n", filename);
        return;
    }

    FILE* file = NULL;
    fopen_s(&file, save_path, "wb");
    if (!file) {
        SOCKET tmp = create_transfer_socket();
        if (tmp != INVALID_SOCKET) {
            send_error_packet(tmp, client_addr, ERROR_ACCESS_VIOLATION, "Access violation (cannot open file)");
            closesocket(tmp);
        }
        printf("[WRQ] cannot open for write: %s\n", save_path);
        return;
    }

    SOCKET tsock = create_transfer_socket();
    if (tsock == INVALID_SOCKET) {
        fclose(file);
        return;
    }

    printf("[WRQ] start receive (compressed): %s\n", save_path);
    print_peer("[WRQ] client:", client_addr);

    send_ack(tsock, client_addr, 0);
    printf("[WRQ] sent ACK(0), waiting DATA...\n");

    unsigned short expected = 1;
    long long received_bytes = 0;
    char buf[BUFFER_SIZE];

    DWORD startTick = GetTickCount();

    while (1) {
        sockaddr_in from;
        int n = 0;
        if (!recv_packet(tsock, buf, BUFFER_SIZE, &from, &n)) {
            send_ack(tsock, client_addr, (unsigned short)(expected - 1));
            continue;
        }

        if (!same_tid(&from, client_addr)) {
            send_error_packet(tsock, &from, ERROR_UNKNOWN_TID, "Unknown transfer ID");
            continue;
        }

        if (n < 4) continue;

        short op = ntohs(*(short*)buf);
        if (op == OP_DATA) {
            unsigned short block = (unsigned short)ntohs(*(short*)(buf + 2));
            int data_len = n - 4;

            if (block == expected) {
                if (data_len > 0) fwrite(buf + 4, 1, data_len, file);
                received_bytes += data_len;

                send_ack(tsock, client_addr, block);
                expected++;

                DWORD now = GetTickCount();
                double sec = (now - startTick) / 1000.0;
                double kbps = (sec > 0) ? (received_bytes / 1024.0 / sec) : 0.0;
                printf("\r[WRQ] received(hfm) %lld bytes (%.1f KB/s) last block=%u   ",
                    received_bytes, kbps, block);
                fflush(stdout);

                if (data_len < DATA_SIZE) {
                    break;
                }
            }
            else if (block == (unsigned short)(expected - 1)) {
                send_ack(tsock, client_addr, block);
            }
        }
        else if (op == OP_ERROR) {
            printf("\n[WRQ] client sent ERROR, abort\n");
            break;
        }
        else {
            send_error_packet(tsock, client_addr, ERROR_ILLEGAL_OP, "Illegal operation");
            printf("\n[WRQ] illegal op, abort\n");
            break;
        }
    }

    printf("\n[WRQ] done, saved compressed: %s\n", save_path);
    fclose(file);
    closesocket(tsock);
}

static void start_tftp_server() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("Failed to initialize WinSock.\n");
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed\n");
        WSACleanup();
        return;
    }

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(LISTEN_PORT);

    if (bind(sock, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        printf("Bind failed (port %d). Maybe occupied?\n", LISTEN_PORT);
        closesocket(sock);
        WSACleanup();
        return;
    }

    CreateDirectoryA(TFTP_ROOT_DIR, NULL);

    printf("TFTP server started on port %d\n", LISTEN_PORT);
    printf("Root dir: .\\%s\n", TFTP_ROOT_DIR);
    printf("Storage rule: server stores only compressed: <filename>.hfm\n");
    printf("Download rule: RRQ <filename> -> server sends <filename>.hfm\n\n");

    while (1) {
        sockaddr_in client_addr;
        int client_len = sizeof(client_addr);
        char buffer[BUFFER_SIZE];
        int n = recvfrom(sock, buffer, BUFFER_SIZE, 0, (sockaddr*)&client_addr, &client_len);
        if (n <= 0) continue;

        short opcode = ntohs(*(short*)buffer);
        char filename[MAX_FILENAME] = { 0 };

        if (opcode == OP_RRQ) {
            if (!parse_rrq_wrq(buffer, n, filename, sizeof(filename))) {
                send_error_packet(sock, &client_addr, ERROR_ILLEGAL_OP, "Bad RRQ format");
                continue;
            }
            printf("\n[REQ] RRQ filename=%s\n", filename);
            handle_rrq(&client_addr, filename);
        }
        else if (opcode == OP_WRQ) {
            if (!parse_rrq_wrq(buffer, n, filename, sizeof(filename))) {
                send_error_packet(sock, &client_addr, ERROR_ILLEGAL_OP, "Bad WRQ format");
                continue;
            }
            printf("\n[REQ] WRQ filename=%s\n", filename);
            handle_wrq(&client_addr, filename);
        }
        else {
            send_error_packet(sock, &client_addr, ERROR_ILLEGAL_OP, "Illegal operation");
        }
    }

    closesocket(sock);
    WSACleanup();
}

int main() {
    int choice;
    while (1) {
        printf("TFTP 服务器菜单:\n");
        printf("1. 启动服务器\n");
        printf("2. 退出\n");
        printf("请输入您的选择: ");
        if (scanf_s("%d", &choice) != 1) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            continue;
        }

        if (choice == 1) start_tftp_server();
        else if (choice == 2) break;
    }
    return 0;
}