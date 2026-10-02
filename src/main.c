/*
 * main.c -- lansync 入口与自检
 */
#include <stdio.h>
#include <string.h>


int server_main(int argc, char **argv);
int client_main(int argc, char **argv);
int discover_main(int argc, char **argv);

static void usage(void)
{
    printf(
        "lansync -- 局域网文件夹同步\n"
        "\n"
        "用法: lansync <命令> [选项]\n"
        "\n"
        "命令\n"
        "  server [-d DIR] [选项]    常驻服务端，让客户端成为本目录的副本\n"
        "  client [-d DIR] [选项]    连接服务端，同步一次后退出（默认目录：当前目录）\n"
        "  client update [选项]      从服务端取回最新程序并替换自己\n"
        "  discover [选项]           搜索局域网内的服务端\n"
        "\n"
        "示例\n"
        "  lansync server -d /srv/share          服务端；默认只读，客户端改不了它\n"
        "  lansync client -d ~/share             客户端；自动发现服务端并同步一次\n"
        "  lansync client -d ~/share -s 10.0.0.2 直接指定服务端地址\n"
        "  lansync client update                 把自己更新成服务端上的版本\n"
        "\n"
        "  lansync server -h | client -h         查看各自选项\n"
        "  lansync server --help-debug           含调试选项\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "server") == 0) {
        return server_main(argc - 1, argv + 1);
    }
    if (strcmp(cmd, "client") == 0) {
        return client_main(argc - 1, argv + 1);
    }
    if (strcmp(cmd, "discover") == 0) {
        return discover_main(argc - 1, argv + 1);
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage();
        return 0;
    }
    if (strcmp(cmd, "-V") == 0 || strcmp(cmd, "--version") == 0) {
        printf("lansync %s\n", "1.0");
        return 0;
    }

    fprintf(stderr, "未知子命令：%s\n\n", cmd);
    usage();
    return 2;
}
