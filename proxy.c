#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define THREADNUM 8
#define SBUFSIZE 16
#define CACHE_BLOCK_NUM 8

/* User-Agent header */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

/* URI 结构体 */
struct Uri {
    char host[MAXLINE]; // 主机名
    char port[MAXLINE]; // 端口号
    char path[MAXLINE]; // 路径
};

/* 缓冲区结构体 */
typedef struct {
    int *buf;          /* 缓冲区数组 */
    int n;             /* 最大槽数 */
    int front;         /* buf[(front+1)%n] 是第一个元素 */
    int rear;          /* buf[rear%n] 是最后一个元素 */
    sem_t mutex;       /* 保护缓冲区访问的互斥锁 */
    sem_t slots;       /* 可用槽计数 */
    sem_t items;       /* 可用项计数 */
} sbuf_t;

// Cache结构
typedef struct
{
    char obj[MAX_OBJECT_SIZE];
    char uri[MAXLINE];
    int LRU;
    int isEmpty;

    int read_cnt; //读者数量
    sem_t w;      //Cache信号量
    sem_t mutex;  //read_cnt信号量

} block;

typedef struct
{
    block data[CACHE_BLOCK_NUM];
    int num;
} Cache;

/* 全局变量 */
sbuf_t sbuf;                     // 生产者-消费者缓冲区
Cache cache;                 // 缓存

/* 函数声明 */
void sbuf_init(sbuf_t *sp, int n);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);
void doit(int connfd);
void parse_uri(char *uri, struct Uri *uri_data);
void build_header(char *server, struct Uri *uri_data, rio_t *rio);
void *thread(void *vargp);
int get_Cache(char *uri);
void write_Cache(char *uri, char *buf);
void init_Cache(Cache *cache);


int main(int argc, char **argv) {

    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    // 检查命令行参数
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    init_Cache(&cache); // 初始化缓存
    // 初始化缓冲区
    sbuf_init(&sbuf, SBUFSIZE);

    // 创建线程池
    for (int i = 0; i < THREADNUM; i++) {
        Pthread_create(&tid, NULL, thread, NULL);
    }

    // 启动监听
    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        sbuf_insert(&sbuf, connfd); // 插入连接描述符到缓冲区
    }
    return 0;
}

/* 处理客户端请求 */
void doit(int connfd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char server[MAXLINE];
    rio_t rio, rio_server;

    char cache_tag[MAXLINE];

    /* 初始化与客户端通信的 rio */
    Rio_readinitb(&rio, connfd);

    /* 读请求行 */
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        return;
    }
    sscanf(buf, "%s %s %s", method, uri, version);
    strcpy(cache_tag, uri);

    /* 只实现 GET 方法 */
    if (strcasecmp(method, "GET") != 0) {
        printf("Proxy does not implement the method\n");
        return;
    }

    /* 如果在缓存中命中 */
    int i;
    if ((i = get_Cache(cache_tag)) != -1) {
        P(&cache.data[i].mutex);
        cache.data[i].read_cnt++;
        if (cache.data[i].read_cnt == 1) {
            P(&cache.data[i].w);   // 第一个读者阻塞写
        }
        V(&cache.data[i].mutex);

        /* 返回缓存数据给客户端 */
        Rio_writen(connfd, cache.data[i].obj, strlen(cache.data[i].obj));

        /* 更新读者计数 */
        P(&cache.data[i].mutex);
        cache.data[i].read_cnt--;
        if (cache.data[i].read_cnt == 0) {
            V(&cache.data[i].w);   // 最后一个读者释放写锁
        }
        V(&cache.data[i].mutex);

        return;
    }

    /* 没命中缓存：解析 URI 并向服务器请求 */
    struct Uri *uri_data = (struct Uri *)Malloc(sizeof(struct Uri));
    parse_uri(uri, uri_data);
    build_header(server, uri_data, &rio);

    int serverfd = Open_clientfd(uri_data->host, uri_data->port);
    if (serverfd < 0) {
        printf("connection failed\n");
        free(uri_data);
        return;
    }

    /* 和目标服务器通信 */
    Rio_readinitb(&rio_server, serverfd);
    Rio_writen(serverfd, server, strlen(server));

    char cache_buf[MAX_OBJECT_SIZE];
    int size_buf = 0;
    size_t n;

    /* 读取目标服务器响应并转发给客户端 */
    while ((n = Rio_readlineb(&rio_server, buf, MAXLINE)) > 0) {
        if (size_buf + n < MAX_OBJECT_SIZE) {
            memcpy(cache_buf + size_buf, buf, n);
            size_buf += n;
        }
        Rio_writen(connfd, buf, n);
    }

    Close(serverfd);

    /* 将响应写入缓存 */
    if (size_buf < MAX_OBJECT_SIZE) {
        cache_buf[size_buf] = '\0';   // 确保字符串结束符
        write_Cache(cache_tag, cache_buf);
    }

    free(uri_data);  // 释放 URI 内存
}


/* 解析 URI */
void parse_uri(char *uri, struct Uri *uri_data) {
    char *host_start = strstr(uri, "http://");
    if (host_start) {
        host_start += 7; // 跳过 "http://"
    } else {
        host_start = uri;
    }

    char *path_start = strchr(host_start, '/');
    if (path_start) {
        strcpy(uri_data->path, path_start + 1);
        *path_start = '\0';
    } else {
        uri_data->path[0] = '\0';
    }

    char *port_start = strchr(host_start, ':');
    if (port_start) {
        strcpy(uri_data->port, port_start + 1);
        *port_start = '\0';
    } else {
        strcpy(uri_data->port, "80"); // 默认 HTTP 端口
    }

    strcpy(uri_data->host, host_start);
}

/* 构建请求头 */
void build_header(char *server, struct Uri *uri_data, rio_t *rio) {
    char buf[MAXLINE], host_header[MAXLINE], other_headers[MAXLINE];
    int host_header_found = 0;

    // 初始化
    strcpy(host_header, "");
    strcpy(other_headers, "");

    // 读取请求头
    while (Rio_readlineb(rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) break; // 空行表示头部结束

        if (strncasecmp(buf, "Host:", 5) == 0) {
            strcpy(host_header, buf);
            host_header_found = 1;
        } else if (strncasecmp(buf, "User-Agent:", 11) == 0) {
            continue; // 跳过 User-Agent
        } else if (strncasecmp(buf, "Connection:", 11) == 0 ||
                   strncasecmp(buf, "Proxy-Connection:", 17) == 0) {
            continue; // 跳过 Connection 和 Proxy-Connection
        } else {
            strcat(other_headers, buf);
        }
    }

    if (!host_header_found) {
        snprintf(host_header, sizeof(host_header), "Host: %.255s\r\n", uri_data->host);
    }

    // 构建完整请求头
    sprintf(server, "GET /%s HTTP/1.0\r\n", uri_data->path);
    strcat(server, host_header);
    strcat(server, "Connection: close\r\n");
    strcat(server, "Proxy-Connection: close\r\n");
    strcat(server, user_agent_hdr);
    strcat(server, other_headers);
    strcat(server, "\r\n");
}

/* 线程函数 */
void *thread(void *vargp) {
    Pthread_detach(pthread_self());
    while (1) {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
    }
}

/* 缓冲区初始化 */
void sbuf_init(sbuf_t *sp, int n) {
    sp->buf = calloc(n, sizeof(int));
    sp->n = n;
    sp->front = sp->rear = 0;
    Sem_init(&sp->mutex, 0, 1);
    Sem_init(&sp->slots, 0, n);
    Sem_init(&sp->items, 0, 0);
}

/* 缓冲区插入 */
void sbuf_insert(sbuf_t *sp, int item) {
    P(&sp->slots);
    P(&sp->mutex);
    sp->buf[(++sp->rear) % (sp->n)] = item;
    V(&sp->mutex);
    V(&sp->items);
}

/* 缓冲区移除 */
int sbuf_remove(sbuf_t *sp) {
    int item;
    P(&sp->items);
    P(&sp->mutex);
    item = sp->buf[(++sp->front) % (sp->n)];
    V(&sp->mutex);
    V(&sp->slots);
    return item;
}

/* 如果命中缓存，返回块下标；没命中返回 -1 */
int get_Cache(char *uri) {
    for (int i = 0; i < cache.num; i++) {
        if (!cache.data[i].isEmpty && (strcmp(uri, cache.data[i].uri) == 0)) {
            return i;  // 命中
        }
    }
    return -1; // 未命中
}

/* 将对象写入缓存，如果缓存已满，采用 LRU 替换 */
void write_Cache(char *uri, char *buf) {
    int victim = -1;

    // 1. 先找有没有空闲块
    for (int i = 0; i < cache.num; i++) {
        if (cache.data[i].isEmpty) {
            victim = i;
            break;
        }
    }

    // 2. 如果没有空闲块，按 LRU 替换
    if (victim == -1) {
        int maxLRU = -1;
        for (int i = 0; i < cache.num; i++) {
            if (cache.data[i].LRU > maxLRU) {
                maxLRU = cache.data[i].LRU;
                victim = i;
            }
        }
    }

    // 3. 写入缓存
    P(&cache.data[victim].w);   // 写锁
    strcpy(cache.data[victim].obj, buf);
    strcpy(cache.data[victim].uri, uri);
    cache.data[victim].isEmpty = 0;
    cache.data[victim].LRU = 0;  // 新写入，置 0
    V(&cache.data[victim].w);

    // 4. 更新其他块的 LRU
    for (int i = 0; i < cache.num; i++) {
        if (i != victim && !cache.data[i].isEmpty) {
            cache.data[i].LRU++;
        }
    }
}

void init_Cache(Cache *c) {
    c->num = CACHE_BLOCK_NUM;  // 缓存块数，固定为 CACHE_BLOCK_NUM
    for (int i = 0; i < c->num; i++) {
        c->data[i].isEmpty = 1;    // 初始为空
        c->data[i].LRU = 0;        // LRU 值初始化
        c->data[i].read_cnt = 0;   // 还没人读
        Sem_init(&c->data[i].w, 0, 1);      // 写锁
        Sem_init(&c->data[i].mutex, 0, 1);  // 保护 read_cnt 的锁
    }
}
