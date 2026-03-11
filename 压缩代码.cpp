#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_TREE_HT 256
#define BLOCK_SIZE (1024 * 1024)  // 1 MB

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

static HuffmanNode* newNode(uint8_t data, unsigned freq) {
    HuffmanNode* node = (HuffmanNode*)malloc(sizeof(HuffmanNode));
    if (!node) { perror("malloc"); exit(EXIT_FAILURE); }
    node->left = node->right = NULL;
    node->data = data;
    node->freq = freq;
    return node;
}

static MinHeap* createMinHeap(unsigned capacity) {
    MinHeap* heap = (MinHeap*)malloc(sizeof(MinHeap));
    if (!heap) { perror("malloc"); exit(EXIT_FAILURE); }
    heap->size = 0;
    heap->capacity = capacity;
    heap->array = (HuffmanNode**)malloc(capacity * sizeof(HuffmanNode*));
    if (!heap->array) { perror("malloc"); free(heap); exit(EXIT_FAILURE); }
    return heap;
}

static void minHeapify(MinHeap* heap, int idx) {
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
        minHeapify(heap, smallest);
    }
}

static HuffmanNode* extractMin(MinHeap* heap) {
    HuffmanNode* temp = heap->array[0];
    heap->array[0] = heap->array[heap->size - 1];
    --heap->size;
    if (heap->size > 0) minHeapify(heap, 0);
    return temp;
}

static void insertMinHeap(MinHeap* heap, HuffmanNode* node) {
    ++heap->size;
    int i = (int)heap->size - 1;
    while (i && node->freq < heap->array[(i - 1) / 2]->freq) {
        heap->array[i] = heap->array[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap->array[i] = node;
}

static HuffmanNode* buildHuffmanTree(const uint8_t data[], const unsigned freq[], int size) {
    // size >= 1
    MinHeap* heap = createMinHeap((unsigned)size);
    for (int i = 0; i < size; ++i)
        heap->array[i] = newNode(data[i], freq[i]);
    heap->size = (unsigned)size;

    for (int i = (size - 1) / 2; i >= 0; --i) {
        minHeapify(heap, i);
        if (i == 0) break; // 防止 i-- 变负后死循环（虽然这里一般不会，但更安全）
    }

    while (heap->size != 1) {
        HuffmanNode* left = extractMin(heap);
        HuffmanNode* right = extractMin(heap);
        HuffmanNode* top = newNode((uint8_t)'$', left->freq + right->freq);
        top->left = left;
        top->right = right;
        insertMinHeap(heap, top);
    }

    HuffmanNode* root = extractMin(heap);
    free(heap->array);
    free(heap);
    return root;
}

static void freeHuffmanTree(HuffmanNode* root) {
    if (!root) return;
    freeHuffmanTree(root->left);
    freeHuffmanTree(root->right);
    free(root);
}

// 将长度为len的二进制字符串（'0'/'1'）转换为字节数组（高位在前）
static void binaryToBytes(const char* binary, int len, uint8_t* bytes, int* numBytes) {
    *numBytes = (len + 7) / 8;
    memset(bytes, 0, (size_t)*numBytes);
    for (int i = 0; i < len; i++) {
        int byteIndex = i / 8;
        int bitIndex = 7 - (i % 8);
        if (binary[i] == '1')
            bytes[byteIndex] |= (uint8_t)(1u << bitIndex);
    }
}

// ✅ 修复点：当哈夫曼树只有一个叶子时，top==0，需要给它一个非空编码，比如 "0"
static void generateCodes(HuffmanNode* root, CodeTable* table, char code[], int top) {
    if (!root) return;

    if (root->left) {
        code[top] = '0';
        generateCodes(root->left, table, code, top + 1);
    }
    if (root->right) {
        code[top] = '1';
        generateCodes(root->right, table, code, top + 1);
    }

    if (!root->left && !root->right) {
        int realLen = top;

        // 单节点树：给它强制编码 "0"
        if (realLen == 0) {
            code[0] = '0';
            realLen = 1;
        }

        table[root->data].data = root->data;
        memcpy(table[root->data].code, code, (size_t)realLen);
        table[root->data].code[realLen] = '\0';
        table[root->data].codeLen = realLen;
    }
}

static void compressBlock(FILE* out, const uint8_t* data, size_t dataSize) {
    if (dataSize == 0) return;

    // 1) 统计频率
    unsigned freq[256] = { 0 };
    for (size_t i = 0; i < dataSize; i++)
        freq[data[i]]++;

    // 2) 收集出现过的符号
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

    // 3) 构建哈夫曼树
    HuffmanNode* root = NULL;
    if (symCount == 1) {
        // 单符号块：依然构造一个只有一个叶子的树
        root = newNode(symbols[0], symFreq[0]);
    }
    else {
        root = buildHuffmanTree(symbols, symFreq, symCount);
    }

    // 4) 生成编码表
    CodeTable table[256] = { 0 };
    char code[MAX_TREE_HT];
    generateCodes(root, table, code, 0);

    // 5) 写入块头：原始大小、编码表条目数、编码表
    uint64_t rawSize = (uint64_t)dataSize;
    if (fwrite(&rawSize, sizeof(rawSize), 1, out) != 1) { perror("write rawSize"); exit(EXIT_FAILURE); }

    uint16_t entryCount = 0;
    for (int i = 0; i < 256; i++)
        if (table[i].codeLen > 0) entryCount++;
    if (fwrite(&entryCount, sizeof(entryCount), 1, out) != 1) { perror("write entryCount"); exit(EXIT_FAILURE); }

    for (int i = 0; i < 256; i++) {
        if (table[i].codeLen > 0) {
            uint8_t byteVal = (uint8_t)i;
            uint8_t codeLen = (uint8_t)table[i].codeLen;  // <= 255 一般足够
            uint8_t codeBytes[MAX_TREE_HT / 8];
            int numBytes = 0;
            binaryToBytes(table[i].code, table[i].codeLen, codeBytes, &numBytes);

            if (fwrite(&byteVal, 1, 1, out) != 1) { perror("write byteVal"); exit(EXIT_FAILURE); }
            if (fwrite(&codeLen, 1, 1, out) != 1) { perror("write codeLen"); exit(EXIT_FAILURE); }
            if (fwrite(codeBytes, 1, (size_t)numBytes, out) != (size_t)numBytes) { perror("write codeBytes"); exit(EXIT_FAILURE); }
        }
    }

    // 6) 压缩数据：将 data 转换为比特流
    uint64_t totalBits = 0;
    for (size_t i = 0; i < dataSize; i++)
        totalBits += (uint64_t)table[data[i]].codeLen;

    uint64_t compBytes = (totalBits + 7) / 8;
    uint8_t* compBuf = NULL;

    if (compBytes == 0) {
        // 理论上经过“单符号块编码修复”后不会再出现 compBytes==0 且 rawSize>0
        compBuf = NULL;
    }
    else {
        compBuf = (uint8_t*)calloc((size_t)compBytes, 1);
        if (!compBuf) { perror("calloc"); exit(EXIT_FAILURE); }

        int bitPos = 7;
        uint8_t* curByte = compBuf;

        for (size_t i = 0; i < dataSize; i++) {
            const char* bits = table[data[i]].code;
            int len = table[data[i]].codeLen;
            for (int j = 0; j < len; j++) {
                if (bits[j] == '1')
                    *curByte |= (uint8_t)(1u << bitPos);

                bitPos--;
                if (bitPos < 0) {
                    curByte++;
                    bitPos = 7;
                }
            }
        }
    }

    // 7) 写入压缩数据长度和压缩数据
    if (fwrite(&compBytes, sizeof(compBytes), 1, out) != 1) { perror("write compBytes"); exit(EXIT_FAILURE); }
    if (compBytes > 0) {
        if (fwrite(compBuf, 1, (size_t)compBytes, out) != (size_t)compBytes) { perror("write compBuf"); exit(EXIT_FAILURE); }
    }

    free(compBuf);
    freeHuffmanTree(root);
}

static void compressFile(const char* inputFile, const char* outputFile) {
    FILE* in = NULL;
    FILE* out = NULL;
    errno_t err;

    err = fopen_s(&in, inputFile, "rb");
    if (err != 0 || in == NULL) {
        perror("打开输入文件失败");
        exit(EXIT_FAILURE);
    }

    err = fopen_s(&out, outputFile, "wb");
    if (err != 0 || out == NULL) {
        perror("打开输出文件失败");
        fclose(in);
        exit(EXIT_FAILURE);
    }

    uint8_t* blockBuf = (uint8_t*)malloc(BLOCK_SIZE);
    if (!blockBuf) {
        perror("分配缓冲区失败");
        fclose(in);
        fclose(out);
        exit(EXIT_FAILURE);
    }

    size_t bytesRead;
    while ((bytesRead = fread(blockBuf, 1, BLOCK_SIZE, in)) > 0) {
        compressBlock(out, blockBuf, bytesRead);
    }

    free(blockBuf);
    fclose(in);
    fclose(out);
    printf("压缩完成！输出文件: %s\n", outputFile);
}

int main() {
    char inputFile[256];
    char outputFile[256];

    printf("请输入要压缩的文件名: ");
    scanf_s("%255s", inputFile, (unsigned)sizeof(inputFile));

    printf("请输入压缩后输出的文件名: ");
    scanf_s("%255s", outputFile, (unsigned)sizeof(outputFile));

    compressFile(inputFile, outputFile);
    return 0;
}