// client.cpp (Windows / WinSock2 / UDP TFTP-like)
// 上传：先压缩为临时 .hfm，再上传到服务器（远程名用原文件名）
// 下载：请求原文件名，下载到临时 .hfm，再解压成目标文件

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sys/stat.h>

#pragma comment(lib, "Ws2_32.lib")

// ======================= TFTP 部分 =======================
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

static int tftp_rrq(const char* remote_filename, const char* save_as_local_path, const char* server_ip) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed\n");
        return 0;
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
        return 0;
    }

    printf("连接服务器 %s:%d，请求下载：%s\n", server_ip, PORT, remote_filename);
    sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));

    FILE* file = NULL;
    fopen_s(&file, save_as_local_path, "wb");
    if (!file) {
        printf("无法打开本地文件用于写入：%s\n", save_as_local_path);
        closesocket(sock);
        return 0;
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
            if (!got_first_data) {
                sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));
            }
            else {
                send_ack(sock, &server_addr, (unsigned short)(expected - 1));
            }
            continue;
        }

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
                printf("\r下载中(hfm)：%lld bytes (%.1f KB/s) block=%u   ", received_bytes, kbps, block);
                fflush(stdout);

                if (data_len < DATA_SIZE) {
                    printf("\n下载完成（压缩文件），保存为：%s\n", save_as_local_path);
                    break;
                }
            }
            else if (block == (unsigned short)(expected - 1)) {
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
    return got_first_data ? 1 : 0;
}

static int tftp_wrq_with_remote(const char* local_path, const char* remote_filename, const char* server_ip) {
    FILE* file = NULL;
    fopen_s(&file, local_path, "rb");
    if (!file) {
        printf("无法打开本地文件：%s\n", local_path);
        return 0;
    }

    long long total = file_size_64(local_path);
    if (!remote_filename || !remote_filename[0]) {
        printf("远程文件名非法\n");
        fclose(file);
        return 0;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed\n");
        fclose(file);
        return 0;
    }
    set_recv_timeout(sock, 2000);

    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, server_ip, &server_addr.sin_addr);
    server_addr.sin_port = htons(PORT);

    char req[BUFFER_SIZE];
    int req_len = build_rrq_wrq(req, BUFFER_SIZE, OP_WRQ, remote_filename);
    if (!req_len) {
        printf("WRQ request too long\n");
        closesocket(sock);
        fclose(file);
        return 0;
    }

    printf("连接服务器 %s:%d，准备上传（压缩文件流）:\n", server_ip, PORT);
    printf("  本地(压缩临时): %s\n  远程(原名): %s\n", local_path, remote_filename);

    sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));

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
                return 0;
            }
            printf("等待ACK0超时，重发WRQ...\n");
            sendto(sock, req, req_len, 0, (sockaddr*)&server_addr, sizeof(server_addr));
            continue;
        }

        server_addr = from;

        if (n >= 4 && ntohs(*(short*)buf) == OP_ACK && ntohs(*(short*)(buf + 2)) == 0) {
            printf("服务器接受上传，开始发送数据...\n");
            break;
        }
        if (n >= 4 && ntohs(*(short*)buf) == OP_ERROR) {
            printf("服务器拒绝上传，错误码: %u\n", (unsigned short)ntohs(*(short*)(buf + 2)));
            closesocket(sock);
            fclose(file);
            return 0;
        }
    }

    unsigned short block = 1;
    long long sent_bytes = 0;

    while (1) {
        size_t bytes_read = fread(buf + 4, 1, DATA_SIZE, file);
        short op = htons(OP_DATA);
        short bn = htons((short)block);
        memcpy(buf, &op, 2);
        memcpy(buf + 2, &bn, 2);

        int pkt_len = (int)bytes_read + 4;

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
                    return 0;
                }
                continue;
            }

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
                return 0;
            }
        }

        sent_bytes += (long long)bytes_read;
        print_progress_bar("上传中(hfm)", sent_bytes, total);

        if (bytes_read < DATA_SIZE) {
            printf("\n上传完成（压缩文件）\n");
            break;
        }

        block++;
        if (block == 0) block = 1;
    }

    closesocket(sock);
    fclose(file);
    return 1;
}

// ======================= Huffman 压缩/解压 =======================
#define MAX_TREE_HT 256
#define HFM_BLOCK_SIZE (1024 * 1024)  // 1MB
#define HFM_BUFFER_SIZE 4096

typedef struct HuffmanNode {
    uint8_t data;
    unsigned freq;
    struct HuffmanNode* left, * right;
} HuffmanNode;

typedef struct MinHeap {
    unsigned size;
    unsigned capacity;
    HuffmanNode** array;
} MinHeap;

typedef struct CodeTable {
    uint8_t data;
    char code[MAX_TREE_HT];
    int codeLen;
} CodeTable;

static HuffmanNode* hfm_newNode(uint8_t data, unsigned freq) {
    HuffmanNode* node = (HuffmanNode*)malloc(sizeof(HuffmanNode));
    if (!node) { perror("malloc"); exit(EXIT_FAILURE); }
    node->left = node->right = NULL;
    node->data = data;
    node->freq = freq;
    return node;
}

static MinHeap* hfm_createMinHeap(unsigned capacity) {
    MinHeap* heap = (MinHeap*)malloc(sizeof(MinHeap));
    if (!heap) { perror("malloc"); exit(EXIT_FAILURE); }
    heap->size = 0;
    heap->capacity = capacity;
    heap->array = (HuffmanNode**)malloc(capacity * sizeof(HuffmanNode*));
    if (!heap->array) { perror("malloc"); free(heap); exit(EXIT_FAILURE); }
    return heap;
}

static void hfm_minHeapify(MinHeap* heap, int idx) {
    int smallest = idx;
    int left = 2 * idx + 1;
    int right = 2 * idx + 2;

    if (left < (int)heap->size && heap->array[left]->freq < heap->array[smallest]->freq)
        smallest = left;
    if (right < (int)heap->size && heap->array[right]->freq < heap->array[smallest]->freq)
        smallest = right;

    if (smallest != idx) {
        HuffmanNode* temp = heap->array[idx];
        heap->array[idx] = heap->array[smallest];
        heap->array[smallest] = temp;
        hfm_minHeapify(heap, smallest);
    }
}

static HuffmanNode* hfm_extractMin(MinHeap* heap) {
    HuffmanNode* temp = heap->array[0];
    heap->array[0] = heap->array[heap->size - 1];
    --heap->size;
    if (heap->size > 0) hfm_minHeapify(heap, 0);
    return temp;
}

static void hfm_insertMinHeap(MinHeap* heap, HuffmanNode* node) {
    ++heap->size;
    int i = (int)heap->size - 1;
    while (i && node->freq < heap->array[(i - 1) / 2]->freq) {
        heap->array[i] = heap->array[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap->array[i] = node;
}

static HuffmanNode* hfm_buildHuffmanTree(const uint8_t data[], const unsigned freq[], int size) {
    MinHeap* heap = hfm_createMinHeap((unsigned)size);
    for (int i = 0; i < size; ++i)
        heap->array[i] = hfm_newNode(data[i], freq[i]);
    heap->size = (unsigned)size;

    for (int i = (size - 1) / 2; i >= 0; --i) {
        hfm_minHeapify(heap, i);
        if (i == 0) break;
    }

    while (heap->size != 1) {
        HuffmanNode* left = hfm_extractMin(heap);
        HuffmanNode* right = hfm_extractMin(heap);
        HuffmanNode* top = hfm_newNode((uint8_t)'$', left->freq + right->freq);
        top->left = left;
        top->right = right;
        hfm_insertMinHeap(heap, top);
    }

    HuffmanNode* root = hfm_extractMin(heap);
    free(heap->array);
    free(heap);
    return root;
}

static void hfm_freeTree(HuffmanNode* root) {
    if (!root) return;
    hfm_freeTree(root->left);
    hfm_freeTree(root->right);
    free(root);
}

static void hfm_binaryToBytes(const char* binary, int len, uint8_t* bytes, int* numBytes) {
    *numBytes = (len + 7) / 8;
    memset(bytes, 0, (size_t)*numBytes);
    for (int i = 0; i < len; i++) {
        int byteIndex = i / 8;
        int bitIndex = 7 - (i % 8);
        if (binary[i] == '1')
            bytes[byteIndex] |= (uint8_t)(1u << bitIndex);
    }
}

static void hfm_generateCodes(HuffmanNode* root, CodeTable* table, char code[], int top) {
    if (!root) return;

    if (root->left) {
        code[top] = '0';
        hfm_generateCodes(root->left, table, code, top + 1);
    }
    if (root->right) {
        code[top] = '1';
        hfm_generateCodes(root->right, table, code, top + 1);
    }

    if (!root->left && !root->right) {
        int realLen = top;
        if (realLen == 0) { // 单节点树修复
            code[0] = '0';
            realLen = 1;
        }
        table[root->data].data = root->data;
        memcpy(table[root->data].code, code, (size_t)realLen);
        table[root->data].code[realLen] = '\0';
        table[root->data].codeLen = realLen;
    }
}

static void hfm_compressBlock(FILE* out, const uint8_t* data, size_t dataSize) {
    if (dataSize == 0) return;

    unsigned freq[256] = { 0 };
    for (size_t i = 0; i < dataSize; i++) freq[data[i]]++;

    uint8_t symbols[256];
    unsigned symFreq[256];
    int symCount = 0;
    for (int i = 0; i < 256; i++) {
        if (freq[i] > 0) {
            symbols[symCount] = (uint8_t)i;
            symFreq[symCount] = freq[i];
            symCount++;
        }
    }
    if (symCount <= 0) return;

    HuffmanNode* root = (symCount == 1) ? hfm_newNode(symbols[0], symFreq[0])
        : hfm_buildHuffmanTree(symbols, symFreq, symCount);

    CodeTable table[256] = { 0 };
    char code[MAX_TREE_HT];
    hfm_generateCodes(root, table, code, 0);

    uint64_t rawSize = (uint64_t)dataSize;
    fwrite(&rawSize, sizeof(rawSize), 1, out);

    uint16_t entryCount = 0;
    for (int i = 0; i < 256; i++) if (table[i].codeLen > 0) entryCount++;
    fwrite(&entryCount, sizeof(entryCount), 1, out);

    for (int i = 0; i < 256; i++) {
        if (table[i].codeLen > 0) {
            uint8_t byteVal = (uint8_t)i;
            uint8_t codeLen = (uint8_t)table[i].codeLen;
            uint8_t codeBytes[MAX_TREE_HT / 8];
            int numBytes = 0;
            hfm_binaryToBytes(table[i].code, table[i].codeLen, codeBytes, &numBytes);

            fwrite(&byteVal, 1, 1, out);
            fwrite(&codeLen, 1, 1, out);
            fwrite(codeBytes, 1, (size_t)numBytes, out);
        }
    }

    uint64_t totalBits = 0;
    for (size_t i = 0; i < dataSize; i++) totalBits += (uint64_t)table[data[i]].codeLen;

    uint64_t compBytes = (totalBits + 7) / 8;
    uint8_t* compBuf = NULL;

    if (compBytes > 0) {
        compBuf = (uint8_t*)calloc((size_t)compBytes, 1);
        if (!compBuf) { perror("calloc"); exit(EXIT_FAILURE); }

        int bitPos = 7;
        uint8_t* curByte = compBuf;

        for (size_t i = 0; i < dataSize; i++) {
            const char* bits = table[data[i]].code;
            int len = table[data[i]].codeLen;
            for (int j = 0; j < len; j++) {
                if (bits[j] == '1') *curByte |= (uint8_t)(1u << bitPos);
                bitPos--;
                if (bitPos < 0) {
                    curByte++;
                    bitPos = 7;
                }
            }
        }
    }

    fwrite(&compBytes, sizeof(compBytes), 1, out);
    if (compBytes > 0) fwrite(compBuf, 1, (size_t)compBytes, out);

    free(compBuf);
    hfm_freeTree(root);
}

static int hfm_compressFile(const char* inputFile, const char* outputFile) {
    FILE* in = NULL;
    FILE* out = NULL;

    if (fopen_s(&in, inputFile, "rb") != 0 || !in) { perror("打开输入文件失败"); return 0; }
    if (fopen_s(&out, outputFile, "wb") != 0 || !out) { perror("打开输出文件失败"); fclose(in); return 0; }

    uint8_t* blockBuf = (uint8_t*)malloc(HFM_BLOCK_SIZE);
    if (!blockBuf) { perror("分配缓冲区失败"); fclose(in); fclose(out); return 0; }

    size_t bytesRead;
    while ((bytesRead = fread(blockBuf, 1, HFM_BLOCK_SIZE, in)) > 0) {
        hfm_compressBlock(out, blockBuf, bytesRead);
    }

    free(blockBuf);
    fclose(in);
    fclose(out);
    return 1;
}

static void hfm_bytesToBinary(const uint8_t* bytes, int numBytes, int codeLen, char* binary) {
    int idx = 0;
    for (int i = 0; i < numBytes; i++) {
        for (int j = 7; j >= 0; j--) {
            if (idx < codeLen) binary[idx++] = ((bytes[i] >> j) & 1) ? '1' : '0';
        }
    }
    binary[idx] = '\0';
}

static HuffmanNode* hfm_buildTreeFromTable(const CodeTable* table) {
    HuffmanNode* root = hfm_newNode((uint8_t)'$', 0);
    for (int i = 0; i < 256; i++) {
        if (table[i].codeLen > 0) {
            HuffmanNode* cur = root;
            for (int j = 0; j < table[i].codeLen; j++) {
                if (table[i].code[j] == '0') {
                    if (!cur->left) cur->left = hfm_newNode((uint8_t)'$', 0);
                    cur = cur->left;
                }
                else {
                    if (!cur->right) cur->right = hfm_newNode((uint8_t)'$', 0);
                    cur = cur->right;
                }
            }
            cur->data = table[i].data;
        }
    }
    return root;
}

static int hfm_decompressFile(const char* inputFile, const char* outputFile) {
    FILE* in = fopen(inputFile, "rb");
    if (!in) { perror("无法打开压缩文件"); return 0; }

    FILE* out = fopen(outputFile, "wb");
    if (!out) { perror("无法创建输出文件"); fclose(in); return 0; }

    while (1) {
        uint64_t rawSize = 0;
        if (fread(&rawSize, sizeof(rawSize), 1, in) != 1) break;

        uint16_t entryCount = 0;
        if (fread(&entryCount, sizeof(entryCount), 1, in) != 1) { perror("读取编码表条目数失败"); fclose(in); fclose(out); return 0; }

        CodeTable table[256] = { 0 };
        for (uint16_t k = 0; k < entryCount; k++) {
            uint8_t byteVal = 0, codeLen = 0;
            if (fread(&byteVal, 1, 1, in) != 1 || fread(&codeLen, 1, 1, in) != 1) { perror("读取编码表项失败"); fclose(in); fclose(out); return 0; }

            int numBytes = (codeLen + 7) / 8;
            if (numBytes <= 0 || numBytes > (MAX_TREE_HT / 8)) { fprintf(stderr, "编码长度异常: %u\n", (unsigned)codeLen); fclose(in); fclose(out); return 0; }

            uint8_t codeBytes[MAX_TREE_HT / 8];
            if (fread(codeBytes, 1, (size_t)numBytes, in) != (size_t)numBytes) { perror("读取编码字节失败"); fclose(in); fclose(out); return 0; }

            char binary[MAX_TREE_HT];
            hfm_bytesToBinary(codeBytes, numBytes, codeLen, binary);

            table[byteVal].data = byteVal;
            strcpy(table[byteVal].code, binary);
            table[byteVal].codeLen = (int)codeLen;
        }

        HuffmanNode* root = hfm_buildTreeFromTable(table);

        uint64_t compBytes = 0;
        if (fread(&compBytes, sizeof(compBytes), 1, in) != 1) { perror("读取压缩数据长度失败"); hfm_freeTree(root); fclose(in); fclose(out); return 0; }

        uint8_t* compBuf = NULL;
        if (compBytes > 0) {
            compBuf = (uint8_t*)malloc((size_t)compBytes);
            if (!compBuf) { perror("分配压缩缓冲区失败"); hfm_freeTree(root); fclose(in); fclose(out); return 0; }
            if (fread(compBuf, 1, (size_t)compBytes, in) != (size_t)compBytes) { perror("读取压缩数据失败"); free(compBuf); hfm_freeTree(root); fclose(in); fclose(out); return 0; }
        }

        HuffmanNode* cur = root;
        uint64_t bytesWritten = 0;
        uint8_t outputBuf[HFM_BUFFER_SIZE];
        size_t outPos = 0;

        for (uint64_t i = 0; i < compBytes; i++) {
            for (int j = 7; j >= 0; j--) {
                int bit = (compBuf[i] >> j) & 1;
                cur = (bit == 0) ? cur->left : cur->right;

                if (!cur) {
                    fprintf(stderr, "解码失败：遇到空指针（编码表或数据损坏）\n");
                    free(compBuf);
                    hfm_freeTree(root);
                    fclose(in);
                    fclose(out);
                    return 0;
                }

                if (!cur->left && !cur->right) {
                    outputBuf[outPos++] = cur->data;
                    bytesWritten++;
                    cur = root;

                    if (outPos >= HFM_BUFFER_SIZE) {
                        fwrite(outputBuf, 1, outPos, out);
                        outPos = 0;
                    }
                    if (bytesWritten == rawSize) break;
                }
            }
            if (bytesWritten == rawSize) break;
        }

        if (outPos > 0) fwrite(outputBuf, 1, outPos, out);

        free(compBuf);
        hfm_freeTree(root);
    }

    fclose(in);
    fclose(out);
    return 1;
}

// ======================= 临时文件工具（已修复） =======================
// 逻辑：优先 C:\Temp\ （自动创建），兜底 C:\Windows\Temp\、.\
// 并尝试实际 fopen("wb") 确保可写；否则换目录/换名字重试。
static int ensure_dir_exists(const char* dir) {
    DWORD attr = GetFileAttributesA(dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    return CreateDirectoryA(dir, NULL) ? 1 : 0;
}

static int try_make_temp_in_dir(const char* dir, const char* suffix, char* outPath, size_t outSize) {
    if (!dir || !dir[0]) return 0;

    // 如果是 C:\Temp\ 这种，我们尝试创建
    ensure_dir_exists(dir);

    for (int i = 0; i < 50; i++) {
        unsigned long long t = (unsigned long long)GetTickCount64();
        unsigned long long pid = (unsigned long long)GetCurrentProcessId();
        unsigned long long r = (unsigned long long)rand();

        int n = snprintf(outPath, outSize, "%shfm_%llu_%llu_%llu%s", dir, t, pid, r, suffix);
        if (n <= 0 || (size_t)n >= outSize) return 0;

        FILE* f = NULL;
        if (fopen_s(&f, outPath, "wb") == 0 && f) {
            fclose(f);
            DeleteFileA(outPath); // 这里只是探测可写；真正写入由压缩函数完成
            return 1;
        }
    }
    return 0;
}

static int make_temp_path(const char* suffix, char* outPath, size_t outSize) {
    // 固定 ASCII 临时目录优先（避免中文环境变量导致 A 版 API 问题）
    const char* candidates[] = {
        "C:\\Temp\\",
        "C:\\Windows\\Temp\\",
        ".\\"
    };

    // 让 rand() 每次运行更不一样一点
    srand((unsigned)GetTickCount());

    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (try_make_temp_in_dir(candidates[i], suffix, outPath, outSize)) return 1;
    }

    // 最后：仍然失败
    return 0;
}

// ======================= 业务流程：上传/下载 =======================
static void upload_file_compressed(const char* local_path, const char* server_ip) {
    const char* remote_name = basename_win(local_path);
    if (!remote_name || !remote_name[0]) {
        printf("无法解析远程文件名\n");
        return;
    }

    char tmpHfm[MAX_PATH];
    if (!make_temp_path(".hfm", tmpHfm, sizeof(tmpHfm))) {
        printf("创建临时文件失败（建议手动创建 C:\\Temp\\ 并确保可写）\n");
        return;
    }

    printf("开始压缩：%s -> %s\n", local_path, tmpHfm);
    if (!hfm_compressFile(local_path, tmpHfm)) {
        printf("压缩失败\n");
        DeleteFileA(tmpHfm);
        return;
    }

    printf("开始上传压缩文件（服务端将存储为 %s.hfm）...\n", remote_name);
    int ok = tftp_wrq_with_remote(tmpHfm, remote_name, server_ip);

    DeleteFileA(tmpHfm);

    if (ok) printf("上传成功（服务端仅保存压缩文件）\n");
    else printf("上传失败\n");
}

static void download_file_decompressed(const char* remote_filename, const char* save_as_local_path, const char* server_ip) {
    char tmpHfm[MAX_PATH];
    if (!make_temp_path(".hfm", tmpHfm, sizeof(tmpHfm))) {
        printf("创建临时文件失败（建议手动创建 C:\\Temp\\ 并确保可写）\n");
        return;
    }

    printf("开始下载压缩文件到临时：%s\n", tmpHfm);
    int ok = tftp_rrq(remote_filename, tmpHfm, server_ip);
    if (!ok) {
        printf("下载失败\n");
        DeleteFileA(tmpHfm);
        return;
    }

    printf("开始解压：%s -> %s\n", tmpHfm, save_as_local_path);
    if (!hfm_decompressFile(tmpHfm, save_as_local_path)) {
        printf("解压失败（压缩文件损坏或格式不匹配）\n");
        DeleteFileA(tmpHfm);
        return;
    }

    DeleteFileA(tmpHfm);
    printf("下载并解压完成：%s\n", save_as_local_path);
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
        printf("\n客户端菜单（上传存压缩，下载解压）:\n");
        printf("1. 下载文件（自动解压）\n");
        printf("2. 上传文件（自动压缩）\n");
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

            download_file_decompressed(remote, save_as, server_ip);
        }
        else if (choice == 2) {
            char local_path[MAX_FILENAME * 2];
            printf("请输入要上传的本地文件路径（可带完整路径）：");
            scanf_s("%s", local_path, (unsigned)_countof(local_path));

            upload_file_compressed(local_path, server_ip);
        }
        else if (choice == 3) {
            break;
        }
    }

    WSACleanup();
    return 0;
}