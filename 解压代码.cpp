#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_TREE_HT 256
#define BUFFER_SIZE 4096

typedef struct HuffmanNode {
    uint8_t data;
    unsigned freq;
    struct HuffmanNode* left, * right;
} HuffmanNode;

typedef struct CodeTable {
    uint8_t data;
    char code[MAX_TREE_HT];
    int codeLen;
} CodeTable;

static HuffmanNode* newNode(uint8_t data, unsigned freq) {
    HuffmanNode* node = (HuffmanNode*)malloc(sizeof(HuffmanNode));
    if (!node) {
        perror("内存分配失败");
        exit(EXIT_FAILURE);
    }
    node->left = node->right = NULL;
    node->data = data;
    node->freq = freq;
    return node;
}

static void freeHuffmanTree(HuffmanNode* root) {
    if (!root) return;
    freeHuffmanTree(root->left);
    freeHuffmanTree(root->right);
    free(root);
}

// 从字节数组还原二进制字符串（高位在前）
static void bytesToBinary(const uint8_t* bytes, int numBytes, int codeLen, char* binary) {
    int idx = 0;
    for (int i = 0; i < numBytes; i++) {
        for (int j = 7; j >= 0; j--) {
            if (idx < codeLen) {
                binary[idx++] = ((bytes[i] >> j) & 1) ? '1' : '0';
            }
        }
    }
    binary[idx] = '\0';
}

// 根据编码表重建哈夫曼树
static HuffmanNode* buildHuffmanTreeFromCodeTable(const CodeTable* table) {
    HuffmanNode* root = newNode((uint8_t)'$', 0);

    for (int i = 0; i < 256; i++) {
        if (table[i].codeLen > 0) {
            HuffmanNode* cur = root;
            for (int j = 0; j < table[i].codeLen; j++) {
                if (table[i].code[j] == '0') {
                    if (!cur->left) cur->left = newNode((uint8_t)'$', 0);
                    cur = cur->left;
                }
                else {
                    if (!cur->right) cur->right = newNode((uint8_t)'$', 0);
                    cur = cur->right;
                }
            }
            cur->data = table[i].data;
        }
    }
    return root;
}

static void decompressFile(const char* inputFile, const char* outputFile) {
    FILE* in = fopen(inputFile, "rb");
    if (!in) {
        perror("无法打开压缩文件");
        exit(EXIT_FAILURE);
    }
    FILE* out = fopen(outputFile, "wb");
    if (!out) {
        perror("无法创建输出文件");
        fclose(in);
        exit(EXIT_FAILURE);
    }

    uint64_t totalDecompressed = 0;

    while (1) {
        uint64_t rawSize = 0;
        size_t r1 = fread(&rawSize, sizeof(rawSize), 1, in);
        if (r1 != 1) {
            // EOF 正常结束
            break;
        }

        uint16_t entryCount = 0;
        if (fread(&entryCount, sizeof(entryCount), 1, in) != 1) {
            perror("读取编码表条目数失败");
            break;
        }

        CodeTable table[256] = { 0 };
        for (uint16_t k = 0; k < entryCount; k++) {
            uint8_t byteVal = 0, codeLen = 0;

            if (fread(&byteVal, 1, 1, in) != 1 ||
                fread(&codeLen, 1, 1, in) != 1) {
                perror("读取编码表项失败");
                goto cleanup;
            }

            int numBytes = (codeLen + 7) / 8;
            if (numBytes <= 0 || numBytes > (MAX_TREE_HT / 8)) {
                fprintf(stderr, "编码长度异常: %u\n", (unsigned)codeLen);
                goto cleanup;
            }

            uint8_t codeBytes[MAX_TREE_HT / 8];
            if (fread(codeBytes, 1, (size_t)numBytes, in) != (size_t)numBytes) {
                perror("读取编码字节失败");
                goto cleanup;
            }

            char binary[MAX_TREE_HT];
            bytesToBinary(codeBytes, numBytes, codeLen, binary);

            table[byteVal].data = byteVal;
            strcpy(table[byteVal].code, binary);
            table[byteVal].codeLen = (int)codeLen;
        }

        HuffmanNode* root = buildHuffmanTreeFromCodeTable(table);
        if (!root) {
            perror("构建哈夫曼树失败");
            goto cleanup;
        }

        uint64_t compBytes = 0;
        if (fread(&compBytes, sizeof(compBytes), 1, in) != 1) {
            perror("读取压缩数据长度失败");
            freeHuffmanTree(root);
            goto cleanup;
        }

        uint8_t* compBuf = NULL;
        if (compBytes > 0) {
            compBuf = (uint8_t*)malloc((size_t)compBytes);
            if (!compBuf) {
                perror("分配压缩缓冲区失败");
                freeHuffmanTree(root);
                goto cleanup;
            }
            if (fread(compBuf, 1, (size_t)compBytes, in) != (size_t)compBytes) {
                perror("读取压缩数据失败");
                free(compBuf);
                freeHuffmanTree(root);
                goto cleanup;
            }
        }

        // 解压该块
        HuffmanNode* cur = root;
        uint64_t bytesWritten = 0;
        uint8_t outputBuf[BUFFER_SIZE];
        size_t outPos = 0;

        for (uint64_t i = 0; i < compBytes; i++) {
            for (int j = 7; j >= 0; j--) {
                int bit = (compBuf[i] >> j) & 1;
                cur = (bit == 0) ? cur->left : cur->right;

                if (!cur) {
                    fprintf(stderr, "解码失败：遇到空指针（编码表或数据损坏）\n");
                    free(compBuf);
                    freeHuffmanTree(root);
                    goto cleanup;
                }

                if (!cur->left && !cur->right) {
                    outputBuf[outPos++] = cur->data;
                    bytesWritten++;
                    cur = root;

                    if (outPos >= BUFFER_SIZE) {
                        fwrite(outputBuf, 1, outPos, out);
                        outPos = 0;
                    }
                    if (bytesWritten == rawSize) break;
                }
            }
            if (bytesWritten == rawSize) break;
        }

        if (outPos > 0) fwrite(outputBuf, 1, outPos, out);

        if (bytesWritten != rawSize) {
            fprintf(stderr, "警告：块解压不完整，预期 %llu 字节，实际 %llu 字节\n",
                (unsigned long long)rawSize, (unsigned long long)bytesWritten);
        }

        totalDecompressed += bytesWritten;

        free(compBuf);
        freeHuffmanTree(root);
    }

    printf("解压完成！共解压 %llu 字节。输出文件: %s\n",
        (unsigned long long)totalDecompressed, outputFile);

cleanup:
    fclose(in);
    fclose(out);
}

int main() {
    char inputFile[256];
    char outputFile[256];

    printf("请输入压缩文件名（.hfm）: ");
    scanf("%255s", inputFile);

    printf("请输入解压后输出的文件名: ");
    scanf("%255s", outputFile);

    decompressFile(inputFile, outputFile);
    return 0;
}