#ifndef JCC_SHELL_CARDPOOL_H
#define JCC_SHELL_CARDPOOL_H

// 兼容旧入口：server + worker
void cardpool_start(const char *game_data_dir);

// 拆分：先 TCP（证明注入），再 worker（延后碰游戏）
void cardpool_start_server_only();
void cardpool_start_worker();

#endif
