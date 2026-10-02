/*
 * 进度条演示：不传任何数据，只模拟推进，用来肉眼检查渲染效果。
 * 编译运行:  make demo && ./build/progress-demo
 */
#include <unistd.h>

#include "common.h"

int main(void)
{
    const uint64_t total = 1200ull * 1024 * 1024; /* 1.2 GiB */
    const int steps = 120;

    printf("模拟推送 1.2 GiB / 42 个文件，共 %d 步，每步 45 ms\n", steps);
    printf("中途会插两条普通日志，看进度条会不会被弄乱\n\n");

    progress_begin("推送", total, 42);
    for (int i = 1; i <= steps; i++) {
        usleep(45000);
        progress_add(total / steps, (i % 3 == 0) ? 1 : 0);
        if (i == 35) {
            ls_log("（演示）普通日志插入：这一行应该完整显示，进度条重画在下一行");
        }
        if (i == 80) {
            ls_log("（演示）再来一条，中间不该出现乱码或残影");
        }
    }
    progress_end("推送完成：42 个文件，1.2 GiB");

    printf("\n结束。如果把 stderr 重定向到文件，会自动退化成每 10%% 一行：\n");
    printf("  ./build/progress-demo 2> /tmp/p.log && head /tmp/p.log\n");
    return 0;
}
