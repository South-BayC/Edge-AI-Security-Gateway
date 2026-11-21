/**
 * detector.cpp — 入侵精判框架实现（统一两级管线：本地快响应 + 云端复核，独立工作线程）
 *
 * 判定实现（worker 串行两段，同一帧）：
 *   段1 本地: ncnn 运行 SCRFD-500M 人脸检测（模型 /root/face.param|bin，来自
 *             ncnn-assets；缺失时优雅回退"有运动"告警语义）。
 *             检出人脸 → FACE_UNKNOWN / 未检出 → NO_FACE，以 INITIAL 阶段发布。
 *   段2 云端: 设置→云端复核 开关开时，cloud_detect_query() 上传同一 RGB565 帧
 *             （云服务器两级判定：YOLOv8 人员/类型 + face_recognition 白名单比对，
 *             白名单=cloud/whitelist/<姓名>.jpg）。
 *             白名单命中 → FACE_KNOWN / 陌生人 → FACE_UNKNOWN /
 *             无人 → OTHER / 不可达 → NONE，以 FINAL 阶段发布（附入侵类型词）。
 *             开关关闭 → 只跑段1，初步结论即最终。
 *
 * 线程模型: worker 为独立低优先级线程（nice+10：显示/采集优先抢 CPU，
 * 推理用碎片算力）；submit 由 dev_bridge 周期线程调用（抓帧+唤醒后立即返回，
 * 显示线程从不等待推理）；结论经原子区由 dev_bridge 200ms 周期轮询分发
 * （UI/告警调用路径与现有告警完全一致，避免跨线程 LVGL 调用）。
 * 两段竞态防护：段1 发布后轮询等待 pending 清零（≤1s）再上云，防初步结论被最终结论覆盖。
 */
#include "detector.h"
#include "cam_feed.h"
#include "cloud_detect.h"        /* 云端复核客户端（段2 帧上传） */
#include "log_svc.h"             /* 分级异步日志 */
#include "config_mgr.h"
#include "thread_util.h"
#include <ncnn/net.h>            /* ncnn（-I third_party/ncnn-armhf/include） */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <sys/resource.h>

#define DET_W 630
#define DET_H 340

static detector_verifier_fn s_verifier = NULL;  /* P3 人脸识别引擎接入点（预留） */

/* ---- T5-B：精判任务有界队列 ----
 * 每个任务自带一份帧快照（628KB），避免"后一次提交覆盖前一任务帧"的错配。
 * 深度 2：告警冷却已做节流，再多只会加剧单核排队；满时丢弃新任务并计数。 */
#define DET_QUEUE_DEPTH 2
static uint8_t s_job_frames[DET_QUEUE_DEPTH][DET_W * DET_H * 2];
static int s_job_zone[DET_QUEUE_DEPTH];

static int s_free_slots[DET_QUEUE_DEPTH];   /* 空闲槽位栈 */
static int s_free_top = 0;
static int s_job_ids[DET_QUEUE_DEPTH];      /* 待处理槽位环形队列 */
static int s_job_head = 0;
static int s_job_count = 0;
static pthread_mutex_t s_job_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t s_job_sem;

static volatile int s_quit = 0;
static pthread_t s_worker;
static int s_worker_started = 0;
static int s_drop_count = 0;                /* 队列满丢弃次数（诊断） */
static int s_hits = 0;                      /* 命中计数（__atomic 访问） */

/* ---- 待消费结论（worker 写，dev_bridge 轮询读；多字段用锁交接） ---- */
static pthread_mutex_t s_result_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_result_pending = 0;
static int s_result_zone = -1;
static int s_result = DET_RESULT_NONE;
static int s_result_stage = DET_STAGE_INITIAL;
static char s_result_name[32];

/* SCRFD 检测输入/结果（BGR 三通道，行宽 640=32 对齐） */
#define DET_BGR_W 640
#define DET_BGR_H 340
static unsigned char s_det_bgr[DET_BGR_W * DET_BGR_H * 3];   /* BGR 检测输入（~640KB） */

/* 检出框发布（UI 画布叠加用；worker 写，UI 读，单写多读快照语义） */
#define DET_PUB_BOX_MAX 8
#define DET_PUB_HOLD_MS 3000                 /* 检出框保持窗（超时不再叠加显示） */
static detector_box_t s_pub_boxes[DET_PUB_BOX_MAX];
static volatile int s_pub_count = 0;
static volatile long s_pub_tick_ms = 0;

static long det_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ncnn 网络（加载一次） */
static ncnn::Net *s_scrfd = NULL;
static int s_ncnn_ok = 0;

static int ncnn_load_once(void)
{
    if (s_ncnn_ok) return 0;
    s_scrfd = new ncnn::Net();
    s_scrfd->opt.use_vulkan_compute = false;
    s_scrfd->opt.num_threads = 1;   /* 单核 SoC：ncnn 内部不再开线程（与渲染/采集调度） */
    if (s_scrfd->load_param("/root/face.param") != 0) {
        LOGW("DET", "load /root/face.param failed -> 回退移动告警语义");
        delete s_scrfd; s_scrfd = NULL;
        return -1;
    }
    if (s_scrfd->load_model("/root/face.bin") != 0) {
        LOGW("DET", "load /root/face.bin failed -> 回退移动告警语义");
        delete s_scrfd; s_scrfd = NULL;
        return -1;
    }
    s_ncnn_ok = 1;
    LOGI("DET", "ncnn SCRFD ready");
    return 0;
}

/* ---- 精判结果对象 + 排序/NMS（移植 ncnn 官方 examples/scrfd.cpp，去 OpenCV 化） ---- */
struct DetObject
{
    float x, y, w, h;
    float prob;
};

static inline float det_intersection_area(const DetObject &a, const DetObject &b)
{
    float ix0 = a.x > b.x ? a.x : b.x;
    float iy0 = a.y > b.y ? a.y : b.y;
    float ix1 = (a.x + a.w) < (b.x + b.w) ? (a.x + a.w) : (b.x + b.w);
    float iy1 = (a.y + a.h) < (b.y + b.h) ? (a.y + a.h) : (b.y + b.h);
    float iw = ix1 - ix0 > 0 ? ix1 - ix0 : 0;
    float ih = iy1 - iy0 > 0 ? iy1 - iy0 : 0;
    return iw * ih;
}

static void det_qsort_descent(std::vector<DetObject> &objs, int left, int right)
{
    if (left >= right) return;   /* 空区间/单元素保护（官方示例的 empty 检查等价物，移植时不可漏） */
    int i = left, j = right;
    float p = objs[(left + right) / 2].prob;
    while (i <= j) {
        while (objs[i].prob > p) i++;
        while (objs[j].prob < p) j--;
        if (i <= j) {
            DetObject t = objs[i]; objs[i] = objs[j]; objs[j] = t;
            i++; j--;
        }
    }
    if (left < j) det_qsort_descent(objs, left, j);
    if (i < right) det_qsort_descent(objs, i, right);
}

static void det_nms_sorted_bboxes(std::vector<DetObject> &objs, std::vector<int> &picked,
                                  float nms_threshold)
{
    picked.clear();
    const int n = (int)objs.size();
    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
        areas[i] = objs[i].w * objs[i].h;

    for (int i = 0; i < n; i++) {
        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++) {
            float inter = det_intersection_area(objs[i], objs[picked[j]]);
            float uni = areas[i] + areas[picked[j]] - inter;
            if (uni > 0 && inter / uni > nms_threshold)
                keep = 0;
        }
        if (keep) picked.push_back(i);
    }
}

/* insightface scrfd anchor_generator 移植（官方示例原样） */
static ncnn::Mat det_generate_anchors(int base_size, const ncnn::Mat &ratios,
                                      const ncnn::Mat &scales)
{
    int num_ratio = ratios.w;
    int num_scale = scales.w;

    ncnn::Mat anchors;
    anchors.create(4, num_ratio * num_scale);

    const float cx = 0, cy = 0;
    for (int i = 0; i < num_ratio; i++) {
        float ar = ratios[i];
        int r_w = (int)round(base_size / sqrt(ar));
        int r_h = (int)round(r_w * ar);
        for (int j = 0; j < num_scale; j++) {
            float scale = scales[j];
            float *anchor = anchors.row(i * num_scale + j);
            anchor[0] = cx - r_w * scale * 0.5f;
            anchor[1] = cy - r_h * scale * 0.5f;
            anchor[2] = cx + r_w * scale * 0.5f;
            anchor[3] = cy + r_h * scale * 0.5f;
        }
    }
    return anchors;
}

/* anchor + bbox 回归 → proposal（官方示例原样） */
static void det_generate_proposals(const ncnn::Mat &anchors, int feat_stride,
                                   const ncnn::Mat &score_blob, const ncnn::Mat &bbox_blob,
                                   float prob_threshold, std::vector<DetObject> &faceobjects)
{
    int w = score_blob.w;
    int h = score_blob.h;
    const int num_anchors = anchors.h;

    for (int q = 0; q < num_anchors; q++) {
        const float *anchor = anchors.row(q);
        const ncnn::Mat score = score_blob.channel(q);
        const ncnn::Mat bbox = bbox_blob.channel_range(q * 4, 4);

        float anchor_y = anchor[1];
        float anchor_w = anchor[2] - anchor[0];
        float anchor_h = anchor[3] - anchor[1];

        for (int i = 0; i < h; i++) {
            float anchor_x = anchor[0];
            for (int j = 0; j < w; j++) {
                int index = i * w + j;
                float prob = score[index];
                if (prob >= prob_threshold) {
                    float dx = bbox.channel(0)[index] * feat_stride;
                    float dy = bbox.channel(1)[index] * feat_stride;
                    float dw = bbox.channel(2)[index] * feat_stride;
                    float dh = bbox.channel(3)[index] * feat_stride;

                    float cx = anchor_x + anchor_w * 0.5f;
                    float cy = anchor_y + anchor_h * 0.5f;
                    DetObject obj;
                    obj.x = cx - dx;
                    obj.y = cy - dy;
                    obj.w = cx + dw - obj.x + 1;
                    obj.h = cy + dh - obj.y + 1;
                    obj.prob = prob;
                    faceobjects.push_back(obj);
                }
                anchor_x += feat_stride;
            }
            anchor_y += feat_stride;
        }
    }
}

/* SCRFD 前向（官方示例 detect_scrfd 移植；输入 BGR，stride=行字节数） */
static void scrfd_detect(const unsigned char *bgr, int width, int height, int stride,
                         std::vector<DetObject> &faceobjects)
{
    const int target_size = 160;      /* 模型输入短边（08-29 实测 320→1.3-2s；160 → ÷4 计算量。
                                         检出距离缩短，近/中距离覆盖；若范围不足可回 240 折中） */
    const float prob_threshold = 0.3f;
    const float nms_threshold = 0.45f;

    int w = width, h = height;
    float scale = 1.f;
    if (w > h) { scale = (float)target_size / w; w = target_size; h = (int)(h * scale); }
    else       { scale = (float)target_size / h; h = target_size; w = (int)(w * scale); }

    ncnn::Mat in = ncnn::Mat::from_pixels_resize(bgr, ncnn::Mat::PIXEL_BGR2RGB,
                                                 width, height, stride, w, h);

    int wpad = (w + 31) / 32 * 32 - w;
    int hpad = (h + 31) / 32 * 32 - h;
    ncnn::Mat in_pad;
    ncnn::copy_make_border(in, in_pad, hpad / 2, hpad - hpad / 2,
                           wpad / 2, wpad - wpad / 2, ncnn::BORDER_CONSTANT, 0.f);

    const float mean_vals[3] = {127.5f, 127.5f, 127.5f};
    const float norm_vals[3] = {1 / 128.f, 1 / 128.f, 1 / 128.f};
    in_pad.substract_mean_normalize(mean_vals, norm_vals);

    ncnn::Extractor ex = s_scrfd->create_extractor();
    ex.input("input.1", in_pad);

    std::vector<DetObject> faceproposals;

    {   /* stride 8（blob 名为本模型 param 的数值层名） */
        ncnn::Mat score_blob, bbox_blob;
        ex.extract("412", score_blob);
        ex.extract("415", bbox_blob);
        const int base_size = 16, feat_stride = 8;
        ncnn::Mat ratios(1); ratios[0] = 1.f;
        ncnn::Mat scales(2); scales[0] = 1.f; scales[1] = 2.f;
        ncnn::Mat anchors = det_generate_anchors(base_size, ratios, scales);
        det_generate_proposals(anchors, feat_stride, score_blob, bbox_blob,
                               prob_threshold, faceproposals);
    }
    {   /* stride 16 */
        ncnn::Mat score_blob, bbox_blob;
        ex.extract("474", score_blob);
        ex.extract("477", bbox_blob);
        const int base_size = 64, feat_stride = 16;
        ncnn::Mat ratios(1); ratios[0] = 1.f;
        ncnn::Mat scales(2); scales[0] = 1.f; scales[1] = 2.f;
        ncnn::Mat anchors = det_generate_anchors(base_size, ratios, scales);
        det_generate_proposals(anchors, feat_stride, score_blob, bbox_blob,
                               prob_threshold, faceproposals);
    }
    {   /* stride 32 */
        ncnn::Mat score_blob, bbox_blob;
        ex.extract("536", score_blob);
        ex.extract("539", bbox_blob);
        const int base_size = 256, feat_stride = 32;
        ncnn::Mat ratios(1); ratios[0] = 1.f;
        ncnn::Mat scales(2); scales[0] = 1.f; scales[1] = 2.f;
        ncnn::Mat anchors = det_generate_anchors(base_size, ratios, scales);
        det_generate_proposals(anchors, feat_stride, score_blob, bbox_blob,
                               prob_threshold, faceproposals);
    }

    det_qsort_descent(faceproposals, 0, (int)faceproposals.size() - 1);

    std::vector<int> picked;
    det_nms_sorted_bboxes(faceproposals, picked, nms_threshold);

    int face_count = (int)picked.size();
    faceobjects.resize(face_count);
    for (int i = 0; i < face_count; i++) {
        DetObject &o = faceobjects[i];
        o = faceproposals[picked[i]];
        /* 去除 padding 偏移，映射回原始帧坐标 */
        float x0 = (o.x - (wpad / 2)) / scale;
        float y0 = (o.y - (hpad / 2)) / scale;
        float x1 = (o.x + o.w - (wpad / 2)) / scale;
        float y1 = (o.y + o.h - (hpad / 2)) / scale;
        o.x = x0 < 0 ? 0 : (x0 > width - 1 ? width - 1 : x0);
        o.y = y0 < 0 ? 0 : (y0 > height - 1 ? height - 1 : y0);
        o.w = (x1 > width - 1 ? width - 1 : x1) - o.x;
        o.h = (y1 > height - 1 ? height - 1 : y1) - o.y;
        if (o.w < 0) o.w = 0;
        if (o.h < 0) o.h = 0;
    }

    /* 发布检出框（画布坐标系；单写多读快照，UI 侧 3s 保持窗） */
    int pub = face_count < DET_PUB_BOX_MAX ? face_count : DET_PUB_BOX_MAX;
    for (int i = 0; i < pub; i++) {
        s_pub_boxes[i].x = faceobjects[i].x;
        s_pub_boxes[i].y = faceobjects[i].y;
        s_pub_boxes[i].w = faceobjects[i].w;
        s_pub_boxes[i].h = faceobjects[i].h;
        s_pub_boxes[i].prob = faceobjects[i].prob;
    }
    s_pub_count = pub;
    s_pub_tick_ms = det_now_ms();
}

/* ---- 段1：本地 SCRFD 人脸检测（检出框发布/画布叠加不变） ---- */
static int run_local_detect(const uint8_t *rgb565, int w, int h)
{
    if (ncnn_load_once() != 0)
        return DET_RESULT_NO_FACE;   /* 模型缺失：按"有运动未检出人脸"语义 */
    if (w > DET_BGR_W) w = DET_BGR_W;
    if (h > DET_BGR_H) h = DET_BGR_H;

    /* RGB565 → BGR888（约 3-5ms） */
    for (int y = 0; y < h; y++) {
        const uint16_t *s = (const uint16_t *)(rgb565 + y * w * 2);
        unsigned char *d = s_det_bgr + y * DET_BGR_W * 3;
        for (int x = 0; x < w; x++) {
            uint16_t px = s[x];
            d[x*3+0] = (unsigned char)((px & 0x1F) << 3);          /* B */
            d[x*3+1] = (unsigned char)(((px >> 5) & 0x3F) << 2);   /* G */
            d[x*3+2] = (unsigned char)(((px >> 11) & 0x1F) << 3);  /* R */
        }
    }

    struct timespec b0, b1;
    std::vector<DetObject> faces;
    clock_gettime(CLOCK_MONOTONIC, &b0);
    scrfd_detect(s_det_bgr, w, h, DET_BGR_W * 3, faces);   /* stride=640×3（行对齐） */
    clock_gettime(CLOCK_MONOTONIC, &b1);
    long ms = (b1.tv_sec - b0.tv_sec) * 1000 + (b1.tv_nsec - b0.tv_nsec) / 1000000;

    LOGD("DET", "faces=%d (%ld ms)", (int)faces.size(), ms);

    if (!faces.empty())
        return DET_RESULT_FACE_UNKNOWN;   /* 本地不做白名单比对：检出人脸=陌生人 */
    return DET_RESULT_NO_FACE;
}

/* ---- 段2：云端复核（YOLOv8 人员/类型 + face_recognition 白名单比对） ----
 * @param type 出参：云端入侵类型词（"人员/动物/物体"，无人/不可达为空，可 NULL）
 * @return FACE_KNOWN(白名单命中)/FACE_UNKNOWN(陌生人)/OTHER(无人)/NONE(不可达) */
static int run_cloud_review(const uint8_t *rgb565, int w, int h,
                            char *type, int type_n)
{
    if (type && type_n > 0) type[0] = 0;
    int r = cloud_detect_query((const uint16_t *)rgb565, w, h, type, type_n);
    if (r < 0) {
        LOGW("DET", "cloud=unreachable");
        return DET_RESULT_NONE;          /* 无云端结论：dev_bridge 按"维持/升级本地"定案 */
    }
    if (r == 2) {
        LOGI("DET", "cloud=known (%s)", type && type[0] ? type : "-");
        return DET_RESULT_FACE_KNOWN;    /* 白名单命中 → 在告警则自动消警 */
    }
    if (r == 1) {
        LOGW("DET", "cloud=stranger (%s)", type && type[0] ? type : "-");
        return DET_RESULT_FACE_UNKNOWN;  /* 陌生人 → 不在告警则升级告警 */
    }
    LOGI("DET", "cloud=none");
    return DET_RESULT_OTHER;             /* 无人 → 在告警则自动消警（本地误报） */
}

static void detector_report(int zone, int result, int stage, const char *name);   /* 前置声明 */

/* 等初步结论被 dev_bridge 消费（防最终结论覆盖初步结论；最多 ~1s） */
static void wait_result_consumed(void)
{
    for (int i = 0; i < 20; i++) {
        pthread_mutex_lock(&s_result_lock);
        int pending = s_result_pending;
        pthread_mutex_unlock(&s_result_lock);
        if (!pending)
            break;
        struct timespec ts = { 0, 50 * 1000 * 1000 };   /* 50ms */
        nanosleep(&ts, NULL);
    }
}

static void *worker_main(void *arg)
{
    (void)arg;
    /* 低优先级：显示/采集线程优先抢 CPU，推理用碎片算力
     * （代价：结论延迟在系统繁忙时拉长；收益：预览不因推理卡顿） */
    setpriority(PRIO_PROCESS, 0, 10);

    while (!s_quit) {
        sem_wait(&s_job_sem);
        if (s_quit) break;

        pthread_mutex_lock(&s_job_lock);
        int slot = s_job_ids[s_job_head];
        s_job_head = (s_job_head + 1) % DET_QUEUE_DEPTH;
        s_job_count--;
        pthread_mutex_unlock(&s_job_lock);

        const uint8_t *frame = s_job_frames[slot];
        int zone = s_job_zone[slot];

        /* 段1：本地人脸检测 → 初步结论（dev_bridge：检出=立即 STRANGER 告警；
         * 未检出=画面变动轻提醒，等云端定论） */
        int pre = run_local_detect(frame, DET_W, DET_H);
        detector_report(zone, pre, DET_STAGE_INITIAL, NULL);
        wait_result_consumed();

        /* 段2：云端复核（设置→云端复核 开；开关关时初步结论即最终，由 dev_bridge
         * 按"纯本地"语义处置）。不可达也发 FINAL 定案（dev_bridge 决定维持/升级） */
        if (config_get()->cloud_review) {
            char type[16];
            int fin = run_cloud_review(frame, DET_W, DET_H, type, sizeof(type));
            detector_report(zone, fin, DET_STAGE_FINAL, type);
        }

        pthread_mutex_lock(&s_job_lock);
        s_free_slots[s_free_top++] = slot;   /* 归还槽位 */
        pthread_mutex_unlock(&s_job_lock);
    }
    return NULL;
}

void detector_init(void)
{
    if (s_worker_started)
        return;

    sem_init(&s_job_sem, 0, 0);

    pthread_mutex_lock(&s_job_lock);
    s_free_top = 0;
    for (int i = 0; i < DET_QUEUE_DEPTH; i++)
        s_free_slots[s_free_top++] = i;
    s_job_head = 0;
    s_job_count = 0;
    s_drop_count = 0;
    pthread_mutex_unlock(&s_job_lock);

    if (thread_start(&s_worker, 2048 * 1024, worker_main, NULL) == 0)
        s_worker_started = 1;
    else
        LOGE("DET", "worker thread create failed");

    LOGI("DET", "worker started, queue depth=%d", DET_QUEUE_DEPTH);
}

/* T5-B：停止工作线程并 join（云端复核最长 2.5s，join 可能等这么久） */
void detector_stop(void)
{
    if (!s_worker_started)
        return;

    s_quit = 1;
    sem_post(&s_job_sem);
    pthread_join(s_worker, NULL);
    s_worker_started = 0;
    sem_destroy(&s_job_sem);
    LOGI("DET", "worker stopped (hits=%d dropped=%d)",
         __atomic_load_n(&s_hits, __ATOMIC_RELAXED), s_drop_count);
}

int detector_model_ready(void)
{
    /* 开机预加载 SCRFD 模型（幂等：s_ncnn_ok 已就绪则直接返回 0）。
     * 避免"布防时识别尚未开启"——首次真正检测前就完成加载，
     * 让启动流程能在时间轴准确上报"本地人脸识别就绪/未就绪（降级）"。 */
    return ncnn_load_once();
}

void detector_set_verifier(detector_verifier_fn fn)
{
    s_verifier = fn;
}

int detector_submit(int zone)
{
    __atomic_fetch_add(&s_hits, 1, __ATOMIC_RELAXED);

    /* 1. 取一个空闲槽位；队列满则丢弃本次（不阻塞 UI 线程） */
    pthread_mutex_lock(&s_job_lock);
    if (s_free_top == 0) {
        s_drop_count++;
        int drops = s_drop_count;
        pthread_mutex_unlock(&s_job_lock);
        LOGW("DET", "job queue full, drop (total=%d)", drops);
        return 0;
    }
    int slot = s_free_slots[--s_free_top];
    pthread_mutex_unlock(&s_job_lock);

    /* 2. 帧快照（此时槽位独占，不会被其他提交改写） */
    if (cam_feed_copy_frame(s_job_frames[slot]) != 0) {
        pthread_mutex_lock(&s_job_lock);
        s_free_slots[s_free_top++] = slot;
        pthread_mutex_unlock(&s_job_lock);
        return 0;                     /* 无帧可抓（采集未就绪） */
    }
    s_job_zone[slot] = zone;

    /* 3. 入队并唤醒工作线程 */
    pthread_mutex_lock(&s_job_lock);
    s_job_ids[(s_job_head + s_job_count) % DET_QUEUE_DEPTH] = slot;
    s_job_count++;
    pthread_mutex_unlock(&s_job_lock);

    sem_post(&s_job_sem);
    return 1;                         /* 已入队精判（结论异步经轮询分发） */
}

static void detector_report(int zone, int result, int stage, const char *name)
{
    pthread_mutex_lock(&s_result_lock);
    s_result_zone = zone;
    s_result = result;
    s_result_stage = stage;
    if (name && name[0]) {
        strncpy(s_result_name, name, sizeof(s_result_name) - 1);
        s_result_name[sizeof(s_result_name) - 1] = 0;
    } else {
        s_result_name[0] = 0;
    }
    s_result_pending = 1;             /* 发布（dev_bridge 轮询消费后清零） */
    pthread_mutex_unlock(&s_result_lock);
}

int detector_poll_result(int *zone, int *result, int *stage, char *name, int name_n)
{
    pthread_mutex_lock(&s_result_lock);
    if (!s_result_pending) {
        pthread_mutex_unlock(&s_result_lock);
        return 0;
    }
    if (zone)   *zone = s_result_zone;
    if (result) *result = s_result;
    if (stage)  *stage = s_result_stage;
    if (name && name_n > 0) {
        strncpy(name, s_result_name, name_n - 1);
        name[name_n - 1] = 0;
    }
    s_result_pending = 0;
    pthread_mutex_unlock(&s_result_lock);
    return 1;
}

int detector_get_info(char *buf, int n)
{
    if (!buf || n <= 0) return -1;
    return snprintf(buf, n, "精判:%s 命中:%d",
                    s_verifier ? "引擎" : "桩",
                    __atomic_load_n(&s_hits, __ATOMIC_RELAXED));
}

int detector_get_boxes(detector_box_t *out, int max)
{
    if (!out || max <= 0) return 0;
    /* 保持窗：检出后 3s 内有效（过期返回 0，UI 不再叠加显示） */
    if (det_now_ms() - s_pub_tick_ms > DET_PUB_HOLD_MS) return 0;
    int cnt = s_pub_count < max ? s_pub_count : max;
    for (int i = 0; i < cnt; i++)
        out[i] = s_pub_boxes[i];
    return cnt;
}
