#ifndef JCC_SHELL_CARDPOOL_H
#define JCC_SHELL_CARDPOOL_H

// 在已 attach 的 il2cpp 线程中启动：扫牌库 + 31338 协议 + 落盘
void cardpool_start(const char *game_data_dir);

#endif
