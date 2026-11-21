/**
 * http_get.c — 最小 HTTP/1.1 GET 下载实现
 *
 * 只做 OTA 需要的事：GET 一个文件、只接受 200、写盘并 fsync。
 * 不做重定向/分块/chunked/超时后重试（由上层决定是否再次调用）。
 */
#include "http_get.h"

#include "log_svc.h"
#include "error_code.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_CONNECT_TIMEOUT_MS 3000
#define HTTP_IO_TIMEOUT_MS      10000   /* 单次收发超时（包体较大，取 10s） */
#define HTTP_HDR_MAX            4096

static char s_api_key[80];              /* T11：X-API-Key（空=不携带） */

void http_get_set_api_key(const char *key)
{
    snprintf(s_api_key, sizeof(s_api_key), "%s", (key && key[0]) ? key : "");
}

/* 拆 http://host[:port]/path；只接受 http（https 留到 T11） */
static int parse_url(const char *url, char *host, int hn,
                     char *path, int pn, int *port)
{
    if (strncmp(url, "http://", 7) != 0) {
        LOGE("HTTP", "unsupported url: %s", url);
        return -1;
    }

    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *hend = slash ? slash : p + strlen(p);

    const char *colon = NULL;
    for (const char *q = p; q < hend; q++) {
        if (*q == ':')
            colon = q;
    }

    int hl = (int)((colon ? colon : hend) - p);
    if (hl <= 0 || hl >= hn)
        return -1;

    memcpy(host, p, (size_t)hl);
    host[hl] = 0;

    *port = 80;
    if (colon)
        *port = atoi(colon + 1);

    if (slash)
        snprintf(path, (size_t)pn, "%s", slash);
    else
        snprintf(path, (size_t)pn, "/");
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 非阻塞 connect + select 超时；成功后恢复阻塞并设置收发超时 */
static int tcp_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    if (inet_aton(host, &sa.sin_addr) == 0) {
        struct hostent *he = gethostbyname(host);
        if (!he)
            return -1;
        memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(sa.sin_addr));
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        struct timeval ctv = { HTTP_CONNECT_TIMEOUT_MS / 1000,
                               (HTTP_CONNECT_TIMEOUT_MS % 1000) * 1000 };
        if (select(fd + 1, NULL, &wset, NULL, &ctv) <= 0) {
            close(fd);
            return -1;
        }
        int err = 0;
        socklen_t el = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);

    struct timeval tv = { HTTP_IO_TIMEOUT_MS / 1000,
                          (HTTP_IO_TIMEOUT_MS % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

int http_get_file(const char *url, const char *out_path)
{
    if (!url || !url[0] || !out_path || !out_path[0])
        return -1;

    char host[96], path[512];
    int port = 80;
    if (parse_url(url, host, sizeof(host), path, sizeof(path), &port) != 0)
        return -1;

    int fd = tcp_connect(host, port);
    if (fd < 0) {
        LOGE("HTTP", "connect %s:%d failed [%s]: %s",
             host, port, err_str(ERR_CLOUD_CONNECT), strerror(errno));
        return -1;
    }

    char req[768];
    int rn;
    if (s_api_key[0]) {
        rn = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "X-API-Key: %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, host, s_api_key);
    } else {
        rn = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, host);
    }
    if (rn <= 0 || send_all(fd, req, (size_t)rn) != 0) {
        LOGE("HTTP", "send request failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* 先收响应头，找到 \r\n\r\n；同一缓冲区里剩余的字节属于 body */
    char buf[HTTP_HDR_MAX];
    int used = 0, hdr_end = -1;
    while (used < (int)sizeof(buf) - 1) {
        ssize_t n = recv(fd, buf + used, sizeof(buf) - 1 - (size_t)used, 0);
        if (n <= 0)
            break;
        used += (int)n;
        buf[used] = 0;
        char *p = strstr(buf, "\r\n\r\n");
        if (p) {
            hdr_end = (int)(p - buf) + 4;
            break;
        }
    }
    if (hdr_end < 0) {
        LOGE("HTTP", "no response header from %s:%d", host, port);
        close(fd);
        return -1;
    }

    int status = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) {
        LOGE("HTTP", "status %d [%s]: %s", status, err_str(ERR_CLOUD_STATUS), url);
        close(fd);
        return -1;
    }

    FILE *fp = fopen(out_path, "wb");
    if (!fp) {
        LOGE("HTTP", "open %s failed: %s", out_path, strerror(errno));
        close(fd);
        return -1;
    }

    int fail = 0;
    if (used > hdr_end &&
        fwrite(buf + hdr_end, 1, (size_t)(used - hdr_end), fp) !=
            (size_t)(used - hdr_end))
        fail = 1;

    while (!fail) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0)
            break;                 /* 对端关闭 = 传输结束 */
        if (n < 0) {
            fail = 1;
            break;
        }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            fail = 1;
            break;
        }
    }

    if (!fail) {
        fflush(fp);
        fsync(fileno(fp));
    }
    fclose(fp);
    close(fd);

    if (fail) {
        LOGE("HTTP", "download failed: %s", strerror(errno));
        unlink(out_path);
        return -1;
    }

    LOGI("HTTP", "downloaded %s -> %s", url, out_path);
    return 0;
}
