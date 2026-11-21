/**
 * event_store.h — 本地事件缓存（断网补传，T6）
 *
 * 存储：<dir>/events.jsonl，每行一个事件 JSON。
 * 策略：
 *   - 事件产生时先落盘（掉电不丢）；
 *   - MQTT 发送成功后按 id 删除；
 *   - 最多保留 EVENT_STORE_MAX 条，满时丢最旧；
 *   - 写入用 tmp + fsync + rename，避免半截文件。
 *
 * 线程安全：所有接口内部加锁，可从 UI/网络线程调用。
 */
#ifndef EVENT_STORE_H
#define EVENT_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#define EVENT_STORE_MAX 256

/** 初始化并加载已有事件（config_init 之后、网络线程之前调用） */
int event_store_init(const char *dir);

/** 追加一条事件 JSON（按 "id":"..." 去重；满时丢最旧） */
int event_store_append(const char *json_line);

/** 删除指定 id 的事件并重写文件 */
int event_store_remove(const char *id);

/** 当前缓存条数 */
int event_store_count(void);

/** 取第 idx 条事件（0 为最旧），成功返回 0 */
int event_store_get(int idx, char *out, int n);

/** 当前文件路径（只读） */
const char *event_store_path(void);

/** 停止 writer 线程并落盘剩余事件（退出时调用） */
void event_store_close(void);

#ifdef __cplusplus
}
#endif

#endif /* EVENT_STORE_H */
