#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <zmq.h>

#define PORT 8080
#define BUF_SIZE 8192

enum error
{
    SUCCEED = 0,
    ZMQ_CTX_ERR,
    ZMQ_SOCK_ERR,
    ZMQ_BIND_ERR,
    ZMQ_CONNECT_ERR,
    CJSON_PARSE_ERR,
};

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len;
    char buffer[BUF_SIZE];

    // 1. 创建 socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    // 2. 允许端口复用
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 3. 绑定到所有网卡（0.0.0.0 表示本机所有IP，包括局域网IP）
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;  // 关键点！
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    // 4. 开始监听
    if (listen(server_fd, 5) == -1) {
        perror("listen");
        close(server_fd);
        exit(1);
    }

    printf("HTTP server is running , listing on port: %d\n", PORT);
    //printf("可从浏览器访问： http://<你的局域网IP>:%d\n", PORT);


    // 5. 主循环
    while (1) {
        client_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        memset(buffer, 0, sizeof(buffer));
        int len = read(client_fd, buffer, sizeof(buffer) - 1);
        if (len <= 0) {
            close(client_fd);
            continue;
        }

        printf("\n收到请求:\n%s\n", buffer);

        // ---- 处理 OPTIONS 预检请求 ----
        if (strncmp(buffer, "OPTIONS", 7) == 0) {
            const char *options_response =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Access-Control-Max-Age: 86400\r\n"
                "\r\n";
            write(client_fd, options_response, strlen(options_response));
            close(client_fd);
            continue;
        }

        // ---- 解析 POST 请求体 ----
        char *body = strstr(buffer, "\r\n\r\n");
        if (body)
            body += 4;
        else
            body = "{}";

        printf("请求体内容: %s\n", body);

        // // ---- 构造 JSON 响应 ----
        char response_body[1024];
        snprintf(response_body, sizeof(response_body),
                 "{ \"status\": \"ok\", \"received\": %s }", body);

        // ---- 构造 HTTP 响应 ----
        char response[2048];
        snprintf(response, sizeof(response),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Content-Length: %zu\r\n"
                 "\r\n"
                 "%s",
                 strlen(response_body), response_body);

        // ---- 发送响应 ----
        write(client_fd, response, strlen(response));
        close(client_fd);
    }

    close(server_fd);
    return 0;
}
