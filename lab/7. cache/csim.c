/* csim.c -- Cache simulator for CS:APP CacheLab
 *
 * 说明（要点）：
 * - 使用 LRU 替换策略
 * - 支持命令行参数：-s <s> -E <E> -b <b> -t <tracefile> [-v]
 * - 对 'M' 操作执行两次访问（load + store）
 * - 在没有课程提供的 printSummary 时提供一个弱（weak）实现，便于单文件编译测试；当把它与课程的库链接时，课程的强实现会覆盖这个弱实现。
 */
#define _POSIX_C_SOURCE 200809L /* for getopt */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include "cachelab.h"

/* ----------------------------- 类型与数据结构 ----------------------------- */

typedef unsigned long long ull;

/*
 * 每条 cache line（缓存行）的结构
 * - valid: 该行是否包含有效数据（0 = invalid, 1 = valid）
 * - tag:   地址的 tag 部分（用于比较是否是同一内存块）
 * - last_used: 用于实现 LRU 的时间戳/计数器；值越大表示最近越被访问
 */
typedef struct {
    int valid;
    uint64_t tag;
    unsigned long long last_used;
} line_t;

/*
 * 每个 set 包含 E 条 line。我们用动态数组表示。
 */
typedef struct {
    line_t *lines; /* 指向长度为 E 的 line 数组 */
} set_t;

/*
 * 整个 cache 的表示
 * - sets: 指向长度为 S 的 set 数组（S = 1 << s）
 * - S:    set 的数量
 * - E:    每个 set 中 line 的数量（associativity）
 */
typedef struct {
    set_t *sets;
    size_t S;
    int E;
} cache_t;

/* ----------------------------- 全局变量 ----------------------------- */

int s = 0;                /* set index 位数 */
int E = 0;                /* 每个 set 的行数 */
int b = 0;                /* block offset 位数 */
cache_t cache;            /* 模拟的 cache 实例 */

/* 计数器（供最终输出 grading 使用） */
int hit_count = 0;
int miss_count = 0;
int eviction_count = 0;

int verbose = 0;          /* -v 标志 */
unsigned long long use_counter = 0; /* 单调递增的计数器，用于 LRU 时间戳 */

/* ----------------------------- 函数声明 ----------------------------- */
void init_cache(void);
void free_cache(void);
int access_data(uint64_t addr);
void replay_trace(const char *tracefile);

/* ----------------------------- 函数实现 ----------------------------- */

/* 初始化 cache：分配 sets 与 lines，并将所有行置为 invalid */
void init_cache(void) {
    if (s >= (int)(8 * sizeof(size_t))) {
        /* 防止 1ULL<<s 溢出（不大可能在作业中出现，但加个保险） */
        fprintf(stderr, "Error: s is too large");
        exit(EXIT_FAILURE);
    }

    size_t S = 1ULL << s; /* set 的数量 S = 2^s */
    cache.S = S;
    cache.E = E;
    cache.sets = (set_t *)malloc(sizeof(set_t) * S);
    if (!cache.sets) {
        perror("malloc sets");
        exit(EXIT_FAILURE);
    }

    for (size_t i = 0; i < S; i++) {
        cache.sets[i].lines = (line_t *)malloc(sizeof(line_t) * E);
        if (!cache.sets[i].lines) {
            perror("malloc lines");
            exit(EXIT_FAILURE);
        }
        /* 初始化每个 line */
        for (int j = 0; j < E; j++) {
            cache.sets[i].lines[j].valid = 0;
            cache.sets[i].lines[j].tag = 0;
            cache.sets[i].lines[j].last_used = 0;
        }
    }
}

/* 释放初始化时分配的内存 */
void free_cache(void) {
    if (!cache.sets) return;
    for (size_t i = 0; i < cache.S; i++) {
        free(cache.sets[i].lines);
    }
    free(cache.sets);
    cache.sets = NULL;
}

/*
 * access_data: 模拟一次对给定地址的内存访问
 * 返回值：0 = hit, 1 = miss (no eviction), 2 = miss + eviction
 *
 * 处理步骤：
 * 1) 根据 b 和 s 提取 set index 与 tag
 * 2) 在对应 set 中查找 tag 相同且 valid 的 line -> hit
 * 3) 若未命中 -> miss。查找是否存在 invalid line 可填充；若没有则进行 LRU 替换（eviction）
 * 4) 更新 last_used（使用全局单调 use_counter 模拟时间）
 */
int access_data(uint64_t addr) {
    /* 每次访问都把全局计数器增加，以便 last_used 总是单调递增 */
    use_counter++;

    /* 计算 set index 和 tag
     * 例如：地址位分布为 [tag][s位 set][b位 offset]
     * 先右移 b 位去掉 block offset，再取低 s 位作为 set index，剩下高位作为 tag
     */
    uint64_t set_mask = 0;
    if (s > 0) set_mask = ((1ULL << s) - 1ULL);
    uint64_t set_index = (addr >> b) & set_mask;
    uint64_t tag = addr >> (b + s);

    set_t *set = &cache.sets[set_index];

    /* 1) 检查是否命中 */
    for (int i = 0; i < cache.E; i++) {
        line_t *line = &set->lines[i];
        if (line->valid && line->tag == tag) {  //行有效并且tag匹配
            /* 命中：更新 LRU 时间戳并返回 */
            hit_count++;
            line->last_used = use_counter;
            return 0; /* hit */
        }
    }

    /* 2) Miss（未命中） */
    miss_count++;

    /* 2a) 找空闲行填充（不需要替换） */
    for (int i = 0; i < cache.E; i++) {
        line_t *line = &set->lines[i];
        if (!line->valid) {
            line->valid = 1;
            line->tag = tag;
            line->last_used = use_counter;
            return 1; /* miss, no eviction */
        }
    }

    /* 2b) 没有空闲行，必须进行替换 —— LRU 策略：选择 last_used 最小的那一行 */
    eviction_count++;
    unsigned long long min_used = ULLONG_MAX;
    int lru_index = 0;
    for (int i = 0; i < cache.E; i++) {
        if (set->lines[i].last_used < min_used) {
            min_used = set->lines[i].last_used;
            lru_index = i;
        }
    }
    /* 替换：更新 tag 与 last_used（valid 已经为 1） */
    set->lines[lru_index].tag = tag;
    set->lines[lru_index].last_used = use_counter;
    return 2; /* miss + eviction */
}

/* 读取 trace 文件并逐行重放操作 */
void replay_trace(const char *tracefile) {
    FILE *fp = fopen(tracefile, "r");
    if (!fp) {
        perror("fopen tracefile");
        exit(EXIT_FAILURE);
    }

    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        char op;
        unsigned long long addr = 0;
        int size = 0;
        /* 跳过行首空白，格式化字符串为：OP ADDRESS,SIZE
         * 示例行：" L 10,1" 或 "M 20,1"
         */
        if (sscanf(buf, " %c %llx,%d", &op, &addr, &size) != 3) continue;
        if (op == 'I') continue; /* 忽略指令访存 */

        if (verbose) printf("%c %llx,%d", op, addr, size);

        if (op == 'M') {
            /* Modify = load + store：先访问一次，再访问一次（第二次通常命中） */
            int r1 = access_data((uint64_t)addr);
            if (verbose) {
                if (r1 == 0) printf(" hit");
                else if (r1 == 1) printf(" miss");
                else printf(" miss eviction");
            }
            int r2 = access_data((uint64_t)addr);
            if (verbose) {
                if (r2 == 0) printf(" hit");
                else if (r2 == 1) printf(" miss");
                else printf(" miss eviction");
            }
        } else if (op == 'L' || op == 'S') {
            int r = access_data((uint64_t)addr);
            if (verbose) {
                if (r == 0) printf(" hit");
                else if (r == 1) printf(" miss");
                else printf(" miss eviction");
            }
        }

        if (verbose) printf("\n");
    }

    fclose(fp);
}

/* ----------------------------- main ----------------------------- */
int main(int argc, char **argv) {
    char *tracefile = NULL;
    int opt;

    /* 使用 getopt 解析参数：-s <s> -E <E> -b <b> -t <tracefile> [-v] */
    while ((opt = getopt(argc, argv, "s:E:b:t:vh")) != -1) {
        switch (opt) {
            case 's': s = atoi(optarg); break;
            case 'E': E = atoi(optarg); break;
            case 'b': b = atoi(optarg); break;
            case 't': tracefile = strdup(optarg); break;
            case 'v': verbose = 1; break;
            case 'h':
            default:
                fprintf(stderr, "Usage: %s -s <s> -E <E> -b <b> -t <tracefile> [-v]", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    /* 参数基本检测：s >= 0, E > 0, b >= 0, tracefile 不为空 */
    if (s < 0 || E <= 0 || b < 0 || tracefile == NULL) {
        fprintf(stderr, "Missing required args");
        fprintf(stderr, "Usage: %s -s <s> -E <E> -b <b> -t <tracefile> [-v]", argv[0]);
        exit(EXIT_FAILURE);
    }

    /* 初始化 cache, 重放 trace, 输出 summary, 清理内存 */
    init_cache();
    replay_trace(tracefile);
    printSummary(hit_count, miss_count, eviction_count);
    free_cache();
    free(tracefile);

    return 0;
}