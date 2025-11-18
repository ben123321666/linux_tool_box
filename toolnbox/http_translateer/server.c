// server_zmq_thread.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <zmq.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define BUF_SIZE 8192
#define ZMQ_REPLY_TIMEOUT_MS 5000  // zmq recv timeout

enum error
{
    SUCCEED = 0,
    ZMQ_CTX_ERR,
    ZMQ_SOCK_ERR,
    ZMQ_BIND_ERR,
    ZMQ_CONNECT_ERR,
    CJSON_PARSE_ERR,
};

int parse_http_request(const char *buffer, int total_len, char **body, size_t *body_len)
{
    char *body_start = strstr(buffer, "\r\n\r\n");
    if (!body_start)
        return -1;

    body_start += 4; // 跳过 \r\n\r\n
    char *content_length_ptr = strstr(buffer, "Content-Length:");
    if (content_length_ptr)
    {
        content_length_ptr += strlen("Content-Length:");
        while (*content_length_ptr == ' ')
            content_length_ptr++;

        int content_length = atoi(content_length_ptr);
        if (content_length > 0)
        {
            *body = body_start;
            *body_len = content_length;
            if (body_start + content_length > buffer + total_len)
            {
                *body_len = (buffer + total_len) - body_start;
            }
            return 0;
        }
    }

    *body = body_start;
    *body_len = total_len - (body_start - buffer);
    return 0;
}

typedef struct {
    int client_fd;
    void *zmq_context;
} thread_arg_t;

void *connection_handler(void *arg)
{
    thread_arg_t *targ = (thread_arg_t *)arg;
    int client_fd = targ->client_fd;
    void *context = targ->zmq_context;
    free(targ); // 已经复制需要的数据，释放结构体

    char buffer[BUF_SIZE];
    memset(buffer, 0, sizeof(buffer));

    // 读取请求（简单实现：一次 read）
    ssize_t len = read(client_fd, buffer, sizeof(buffer) - 1);
    if (len <= 0) {
        close(client_fd);
        pthread_exit(NULL);
    }

    printf("\n收到请求 (fd=%d):\n%.*s\n", client_fd, (int)len, buffer);

    // 处理 OPTIONS 预检请求
    if (strncmp(buffer, "OPTIONS", 7) == 0)
    {
        const char *options_response =
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: POST, GET, OPTIONS, PUT\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "\r\n";
        write(client_fd, options_response, strlen(options_response));
        close(client_fd);
        pthread_exit(NULL);
    }

    // 解析请求体
    char *body = NULL;
    size_t body_length = 0;
    if (parse_http_request(buffer, len, &body, &body_length) != 0) {
        body = "{}";
        body_length = 2;
    }

    printf("请求体内容: %.*s\n", (int)body_length, body);

    /* 如果请求体为空，则立即返回 400 并结束处理线程，不再向后端 ZMQ 发送 */
    if (body_length == 0) {
        const char *empty_body_resp = "{\"error\":\"empty request body\"}";
        char empty_resp[512];
        snprintf(empty_resp, sizeof(empty_resp),
                 "HTTP/1.1 400 Bad Request\r\n"
                 "Content-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n"
                 "%s",
                 strlen(empty_body_resp), empty_body_resp);
        write(client_fd, empty_resp, strlen(empty_resp));
        close(client_fd);
        pthread_exit(NULL);
    }

    // 每个线程创建自己的 ZMQ REQ socket（同一 context 下）
    void *socket_zmq = zmq_socket(context, ZMQ_REQ);
    if (!socket_zmq) {
        fprintf(stderr, "thread: zmq_socket failed\n");
        // 给客户端返回 500
        const char *err_resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        write(client_fd, err_resp, strlen(err_resp));
        close(client_fd);
        pthread_exit(NULL);
    }

    // 设置接收超时，防止无限阻塞
    int rcv_timeout = ZMQ_REPLY_TIMEOUT_MS;
    zmq_setsockopt(socket_zmq, ZMQ_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));
    // 设置 linger 为 0，close 时不阻塞
    int linger = 0;
    zmq_setsockopt(socket_zmq, ZMQ_LINGER, &linger, sizeof(linger));

    // 这里连接到本地 5555（同原程序）
    if (zmq_connect(socket_zmq, "tcp://localhost:5555") != 0) {
        fprintf(stderr, "thread: zmq_connect failed: %s\n", zmq_strerror(zmq_errno()));
        zmq_close(socket_zmq);
        const char *err_resp = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        write(client_fd, err_resp, strlen(err_resp));
        close(client_fd);
        pthread_exit(NULL);
    }

    // 发送到 ZMQ
    int send_flags = 0;
    ssize_t sret = zmq_send(socket_zmq, body, body_length, send_flags);
    if (sret < 0) {
        fprintf(stderr, "thread: zmq_send fail: %s\n", zmq_strerror(zmq_errno()));
        zmq_close(socket_zmq);
        const char *err_resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
        write(client_fd, err_resp, strlen(err_resp));
        close(client_fd);
        pthread_exit(NULL);
    }

    printf("zmq_send success (thread for fd=%d)\n", client_fd);

    // 接收来自 ZMQ 的回复（有超时）
    char recv_buffer[4096];
    memset(recv_buffer, 0, sizeof(recv_buffer));
    ssize_t rret = zmq_recv(socket_zmq, recv_buffer, sizeof(recv_buffer) - 1, 0);
    if (rret < 0) {
        int zerr = zmq_errno();
        if (zerr == EAGAIN) {
            // 接收超时
            fprintf(stderr, "thread: zmq_recv timeout for fd=%d\n", client_fd);
            const char *timeout_body = "{\"error\":\"timeout from backend\"}";
            char response[512];
            snprintf(response, sizeof(response),
                     "HTTP/1.1 504 Gateway Timeout\r\n"
                     "Content-Type: application/json\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %zu\r\n"
                     "\r\n"
                     "%s",
                     strlen(timeout_body), timeout_body);
            write(client_fd, response, strlen(response));
        } else {
            fprintf(stderr, "thread: zmq_recv fail: %s\n", zmq_strerror(zerr));
            const char *err_resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
            write(client_fd, err_resp, strlen(err_resp));
        }
        zmq_close(socket_zmq);
        close(client_fd);
        pthread_exit(NULL);
    }

    // 成功收到回复
    recv_buffer[rret] = '\0';
    printf("Received from ZMQ (fd=%d): %s\n", client_fd, recv_buffer);

    // 构造 HTTP 响应并发送给客户端
    char response[8192];
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: application/json\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Content-Length: %zu\r\n"
             "\r\n"
             "%s",
             strlen(recv_buffer), recv_buffer);

    write(client_fd, response, strlen(response));

    // 清理
    zmq_close(socket_zmq);
    close(client_fd);

    pthread_exit(NULL);
}

int main()
{
    int server_fd;
    struct sockaddr_in server_addr;
    int opt = 1;

    // 创建 ZMQ 上下文（全局共享）
    void *context = zmq_ctx_new();
    if (!context) {
        fprintf(stderr, "zmq_ctx_new failed\n");
        return ZMQ_CTX_ERR;
    }

    // 创建服务器 socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        zmq_ctx_destroy(context);
        exit(1);
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        zmq_ctx_destroy(context);
        exit(1);
    }

    if (listen(server_fd, 128) == -1) {
        perror("listen");
        close(server_fd);
        zmq_ctx_destroy(context);
        exit(1);
    }

    printf("HTTP server is running on port: %d\n", PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        printf("New connection (fd=%d)\n", client_fd);

        // 创建线程参数并传入
        thread_arg_t *targ = malloc(sizeof(thread_arg_t));
        if (!targ) {
            fprintf(stderr, "malloc fail\n");
            close(client_fd);
            continue;
        }
        targ->client_fd = client_fd;
        targ->zmq_context = context;

        pthread_t tid;
        if (pthread_create(&tid, NULL, connection_handler, targ) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(targ);
            continue;
        }
        pthread_detach(tid);
    }

    // 永远不会到这里，但写上清理以示完整性
    close(server_fd);
    zmq_ctx_destroy(context);
    return 0;
}
