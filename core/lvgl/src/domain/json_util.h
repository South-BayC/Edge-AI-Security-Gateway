/**
 * json_util.h — 极简 JSON 取值/转义工具（domain 层纯函数）
 *
 * 仅服务于云端下发的扁平 JSON（设备配置 / 命令 / 事件上报），
 * 不做完整语法解析：定位 "key" 后取值，兼容冒号前后空格。
 *
 * 依赖：仅 C 标准库。无网络、无 UI、无硬件。
 */
#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

/** 跳过空白（空格/制表/换行/回车） */
const char *json_skip_ws(const char *p);

/** 定位 "key" 之后、冒号与空白之后的值起点；找不到返回 NULL */
const char *json_value_of(const char *json, const char *key);

/** 读取整数字段；成功返回 1，缺字段返回 0 */
int json_get_int(const char *json, const char *key, int *out);

/** 读取字符串字段（不带转义还原）；成功返回 1 */
int json_get_str(const char *json, const char *key, char *out, int n);

/** 把字符串转义后写入 out（引号/反斜杠转义，控制字符替换为空格） */
void json_escape(const char *in, char *out, int n);

#ifdef __cplusplus
}
#endif

#endif /* JSON_UTIL_H */
