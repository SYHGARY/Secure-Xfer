#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sys/stat.h>

#pragma comment(lib, "Ws2_32.lib")

#define PORT 69
#define BUFFER_SIZE 516
#define DATA_SIZE 512
#define MAX_FILENAME 256

#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

static int set_recv_timeout(SOCKET s, int timeout_ms) {
    return setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
}

static const char* basename_win(const char* path) {
    const char* p1 = strrchr(path, '\\');
    const char* p2 = strrchr(path, '/');
    const char* p = p1 > p2 ? p1 : p2;
    return p ? (p + 1) : path;
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

static int build_rrq_wrq(char* buf, int bufsz, unsigned short opcode, const char* filename) {
    // [2 opcode][filename]\0[octet]\0
    const char* mode = "octet";
    int need = 2 + (int)strlen(filename) + 1 + (int)strlen(mode) + 1;
    if (need > bufsz) return 0;

    short op = htons((short)opcode);
    memcpy(buf, &op, 2);

    char* p = buf + 2;
    strcpy_s(p, bufsz - 2, filename);
    p += (int)strlen(filename) + 1;
    strcpy_s(p, bufsz - (int)(p - buf), mode);
    p += (int)strlen(mode) + 1;

    return (int)(p - buf);
}

static int send_ack(SOCKET sock, const sockaddr_in* server, unsigned short block) {
    char pkt[4];
    short op = htons(OP_ACK);
    short bn = htons((short)block);
    memcpy(pkt, &op, 2);
    memcpy(pkt + 2, &bn, 2);
    return sendto(sock, pkt, 4, 0, (const sockaddr*)server, sizeof(*server));
}

static void tftp_rrq(const char* remote_filename, const char* save_as_local_path, const char* server_ip) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed\n");
        return;
    }
    set_recv_timeout(sock, 2000);

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    server_addr.sin_port = htons(PORT);

    char req[BUFFER_SIZE];
    int req_len = build_rrq_wrq(req, BUFFER_SIZE, OP_RRQ, remote_filename);
    if (!req_len) {
        printf("RRQ request too long\n");
        closesocket(sock);
        return;
    }

    printf("连接服务器 %s:%d，请求下载：%s\n", server_ip, PORT, remote_filename);
    sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));

    FILE* file = NULL;
    fopen_s(&file, save_as_local_path, "wb");
    if (!file) {
        printf("无法打开本地文件用于写入：%s\n", save_as_local_path);
        closesocket(sock);
        return;
    }

    char buf[BUFFER_SIZE];
    unsigned short expected = 1;
    int retries = 0;
    long long received_bytes = 0;

    DWORD startTick = GetTickCount();
    int got_first_data = 0;

    while (1) {
        sockaddr_in from;
        int from_len = sizeof(from);
        int n = recvfrom(sock, buf, BUFFER_SIZE, 0, (sockaddr*)&from, &from_len);
        if (n == SOCKET_ERROR) {
            if (++retries >= 5) {
                printf("\n超时次数过多，终止下载\n");
                break;
            }
            // 还没收到任何数据，重发RRQ；否则重发上一个ACK
            if (!got_first_data) {
                sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));
            }
            else {
                send_ack(sock, &server_addr, (unsigned short)(expected - 1));
            }
            continue;
        }

        // 第一次收到响应后锁定服务端TID（端口会变）
        server_addr = from;

        if (n < 4) continue;
        short op = ntohs(*(short*)buf);

        if (op == OP_DATA) {
            got_first_data = 1;
            unsigned short block = (unsigned short)ntohs(*(short*)(buf + 2));

            if (block == expected) {
                int data_len = n - 4;
                if (data_len > 0) fwrite(buf + 4, 1, data_len, file);
                received_bytes += data_len;

                send_ack(sock, &server_addr, block);
                expected++;
                retries = 0;

                DWORD now = GetTickCount();
                double sec = (now - startTick) / 1000.0;
                double kbps = (sec > 0) ? (received_bytes / 1024.0 / sec) : 0.0;
                printf("\r下载中：%lld bytes (%.1f KB/s) block=%u   ", received_bytes, kbps, block);
                fflush(stdout);

                if (data_len < DATA_SIZE) {
                    printf("\n下载完成，保存为：%s\n", save_as_local_path);
                    break;
                }
            }
            else if (block == (unsigned short)(expected - 1)) {
                // duplicate block -> ack again
                send_ack(sock, &server_addr, block);
            }
        }
        else if (op == OP_ERROR) {
            unsigned short err = (unsigned short)ntohs(*(short*)(buf + 2));
            printf("\n服务器返回错误: %u\n", err);
            break;
        }
    }

    fclose(file);
    closesocket(sock);
}

static void tftp_wrq(const char* local_path, const char* server_ip) {
    FILE* file = NULL;
    fopen_s(&file, local_path, "rb");
    if (!file) {
        printf("无法打开本地文件：%s\n", local_path);
        return;
    }

    long long total = file_size_64(local_path);
    const char* remote_name = basename_win(local_path);
    if (!remote_name || remote_name[0] == '\0') {
        printf("本地路径解析失败：%s\n", local_path);
        fclose(file);
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed\n");
        fclose(file);
        return;
    }
    set_recv_timeout(sock, 2000);

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    server_addr.sin_port = htons(PORT);

    char req[BUFFER_SIZE];
    int req_len = build_rrq_wrq(req, BUFFER_SIZE, OP_WRQ, remote_name);
    if (!req_len) {
        printf("WRQ request too long\n");
        closesocket(sock);
        fclose(file);
        return;
    }

    printf("连接服务器 %s:%d，准备上传：\n", server_ip, PORT);
    printf("  本地文件: %s\n  远程文件名: %s\n", local_path, remote_name);

    // 发送WRQ到69
    sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));

    // 等ACK(0) 并锁定TID
    char buf[BUFFER_SIZE];
    int retries = 0;
    while (1) {
        sockaddr_in from;
        int from_len = sizeof(from);
        int n = recvfrom(sock, buf, BUFFER_SIZE, 0, (sockaddr*)&from, &from_len);
        if (n == SOCKET_ERROR) {
            if (++retries >= 5) {
                printf("等待ACK0超时，终止上传\n");
                closesocket(sock);
                fclose(file);
                return;
            }
            printf("等待ACK0超时，重发WRQ...\n");
            sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));
            continue;
        }

        // 锁定服务端TID（端口会变）
        server_addr = from;

        if (n >= 4 && ntohs(*(short*)buf) == OP_ACK && ntohs(*(short*)(buf + 2)) == 0) {
            printf("服务器接受上传，开始发送数据...\n");
            break;
        }
        if (n >= 4 && ntohs(*(short*)buf) == OP_ERROR) {
            printf("服务器拒绝上传，错误码: %u\n", (unsigned short)ntohs(*(short*)(buf + 2)));
            closesocket(sock);
            fclose(file);
            return;
        }
    }

    unsigned short block = 1;
    long long sent_bytes = 0;

    while (1) {
        // build DATA
        size_t bytes_read = fread(buf + 4, 1, DATA_SIZE, file);
        short op = htons(OP_DATA);
        short bn = htons((short)block);
        memcpy(buf, &op, 2);
        memcpy(buf + 2, &bn, 2);

        int pkt_len = (int)bytes_read + 4;

        // send and wait ack
        int send_retries = 0;
        for (;;) {
            sendto(sock, buf, pkt_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));

            char rbuf[BUFFER_SIZE];
            sockaddr_in from;
            int from_len = sizeof(from);
            int n = recvfrom(sock, rbuf, BUFFER_SIZE, 0, (sockaddr*)&from, &from_len);
            if (n == SOCKET_ERROR) {
                if (++send_retries >= 5) {
                    printf("\n等待ACK超时过多，终止上传\n");
                    closesocket(sock);
                    fclose(file);
                    return;
                }
                continue;
            }

            // accept only same TID
            if (from.sin_addr.s_addr != server_addr.sin_addr.s_addr || from.sin_port != server_addr.sin_port) {
                continue;
            }

            if (n >= 4 && ntohs(*(short*)rbuf) == OP_ACK && (unsigned short)ntohs(*(short*)(rbuf + 2)) == block) {
                break;
            }
            if (n >= 4 && ntohs(*(short*)rbuf) == OP_ERROR) {
                printf("\n服务器返回错误码: %u\n", (unsigned short)ntohs(*(short*)(rbuf + 2)));
                closesocket(sock);
                fclose(file);
                return;
            }
        }

        sent_bytes += (long long)bytes_read;
        print_progress_bar("上传中", sent_bytes, total);

        if (bytes_read < DATA_SIZE) {
            printf("\n上传完成\n");
            break;
        }

        block++;
        if (block == 0) block = 1;
    }

    closesocket(sock);
    fclose(file);
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("Winsock initialization failed\n");
        return 1;
    }

    char server_ip[INET_ADDRSTRLEN];
    int choice;

    printf("请输入 TFTP 服务器的 IP 地址（如 127.0.0.1）：");
    scanf_s("%s", server_ip, (unsigned)_countof(server_ip));

    while (1) {
        printf("\nTFTP 客户端菜单:\n");
        printf("1. 下载文件\n");
        printf("2. 上传文件\n");
        printf("3. 退出\n");
        printf("请输入您的选择: ");
        if (scanf_s("%d", &choice) != 1) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {}
            continue;
        }

        if (choice == 1) {
            char remote[MAX_FILENAME];
            char save_as[MAX_FILENAME * 2];

            printf("请输入要下载的远程文件名（只写文件名，如 a.jpg）：");
            scanf_s("%s", remote, (unsigned)_countof(remote));

            printf("请输入保存到本地的路径（如 C:\\temp\\a.jpg）：");
            scanf_s("%s", save_as, (unsigned)_countof(save_as));

            tftp_rrq(remote, save_as, server_ip);
        }
        else if (choice == 2) {
            char local_path[MAX_FILENAME * 2];
            printf("请输入要上传的本地文件路径（可带完整路径）：");
            scanf_s("%s", local_path, (unsigned)_countof(local_path));
            tftp_wrq(local_path, server_ip);
        }
        else if (choice == 3) {
            break;
        }
    }

    WSACleanup();
    return 0;
}