#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    int tty = -1;
    char *tty_name = NULL;
    int log_fd;

    // 打开日志文件（写入 /mnt/nfs/output.log）
    log_fd = open("/mnt/nfs/output.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0)
    {
        perror("open /mnt/nfs/output.log failed");
        return -1;
    }

    // 将标准输出(stdout)与标准错误(stderr)都重定向到文件
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd); // 关闭原文件描述符（stdout/stderr 已经复制了）

    if (argc < 2)
    {
        printf("miss argument\n");
        return 0;
    }

    // 获取当前tty名称
    tty_name = ttyname(STDOUT_FILENO);
    printf("tty_name: %s\n", tty_name ? tty_name : "unknown");

    if (!strcmp(argv[1], "on"))
    {
        // 重定向 console 到当前 tty
        tty = open(tty_name, O_RDONLY | O_WRONLY);
        if (tty < 0)
        {
            perror("open tty failed");
            return -1;
        }
        ioctl(tty, TIOCCONS);
        perror("ioctl TIOCCONS");
    }
    else if (!strcmp(argv[1], "off"))
    {
        // 恢复 console
        tty = open("/dev/console", O_RDONLY | O_WRONLY);
        if (tty < 0)
        {
            perror("open /dev/console failed");
            return -1;
        }
        ioctl(tty, TIOCCONS);
        perror("ioctl TIOCCONS");
    }
    else
    {
        printf("error argument\n");
        return 0;
    }

    close(tty);
    return 0;
}
