/*
 * Copyright (C) 2018-2020 Alibaba Group Holding Limited
 */

#include <yoc_config.h>
#include "avutil/common.h"
#include "avutil/av_typedef.h"
#include "stream/stream.h"
#include "avformat/avformat.h"
#include "avcodec/avcodec.h"
#include "output/output.h"
#include "avfilter/avfilter.h"
#include "avfilter/avfilter_all.h"
#include "player.h"

#define TAG                    "player"

//#define FPGA_ENABLE

enum player_status {
    PLAYER_STATUS_STOPED,
    PLAYER_STATUS_PREPARING,
    PLAYER_STATUS_PLAYING,
    PLAYER_STATUS_PAUSED,
};

#define player_lock()   (aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER))
#define player_unlock() (aos_mutex_unlock(&player->lock))

#define EVENT_CALL(player, type, data, len) \
	do { \
		if (player && player->event_cb) \
			player->event_cb(player, type, data, len); \
	} while(0)

#define PLAYER_TASK_QUIT_EVT  (0x01)
struct player_cb {
    char                         *url;
    stream_cls_t                 *s;
    demux_cls_t                  *demuxer;
    ad_cls_t                     *ad;
    ao_cls_t                     *ao;
    char                         *ao_name;      ///< ao name

    uint32_t                     cache_size;    ///< size of the web cache. 0 use default
    uint32_t                     cache_percent; ///< (0~100)start read for player when up to cache_percent. 0 use default
    uint32_t                     period_ms;     ///< period cache size(ms) for audio out. 0 means use default
    uint32_t                     period_num;    ///< number of period_ms. total cache size for ao is (period_num * period_ms * (rate / 1000) * 2 * (16/8)). 0 means use default
    uint32_t                     resample_rate; ///< none zereo means need to resample
    uint8_t                      vol_en;        ///< soft vol scale enable
    uint8_t                      vol_index;     ///< soft vol scale index (0~255)
    uint8_t                      *aef_conf;     ///< config data for aef
    size_t                       aef_conf_size; ///< size of the config data for aef
    uint8_t                      eq_en;         ///< used for equalizer config
    uint8_t                      eq_segments;   ///< used for equalizer config
    eqfp_t                       *eq_params;
    get_decrypt_cb_t             get_dec_cb;    ///< used for get decrypt info

    int64_t                      cur_pts;
    uint8_t                      status;
    aos_event_t                  evt;
    uint8_t                      evt_status;
    player_event_t               event_cb;
    uint8_t                      need_quit;

    aos_mutex_t                  lock;
    uint32_t                     rcv_timeout;

    struct {
        uint32_t  ao_write_size;
        uint32_t  run_loop;
        uint32_t  run_loop_valid;
        uint32_t  ao_full;
    }                            stat;
};

/**
 * @brief  init the player before once play
 * @param  [in] player
 * @return
 */
static void _player_once_init(player_t *player)
{
    player->url        = NULL;
    player->s          = NULL;
    player->demuxer    = NULL;
    player->ad         = NULL;
    player->ao         = NULL;
    player->need_quit  = 0;
    player->cur_pts    = 0;
    player->status     = PLAYER_STATUS_STOPED;
    player->evt_status = PLAYER_EVENT_UNKNOWN;

    aos_event_set(&player->evt, ~PLAYER_TASK_QUIT_EVT, AOS_EVENT_AND);
    memset(&player->stat, 0, sizeof(player->stat));
}

/**
 * @brief  init player module
 * @return 0/-1
 */
int player_init()
{
    static int inited = 0;

    if (!inited) {
        resample_register();
        eqx_register();
        aefx_register();

        stream_register_all();
        demux_register_all();
        ad_register_all();
        ao_register_alsa();

        inited = 1;
    }

    return inited ? 0 : -1;
}

/**
 * @brief  new a player obj
 * @param  [in] plyh
 * @return NULL on error
 */
player_t* player_new(const plyh_t *plyh)
{
    player_t *player = NULL;

    CHECK_PARAM(plyh && plyh->ao_name, NULL);
    LOGI(TAG, "%s, %d enter.", __FUNCTION__, __LINE__);
    player = (struct player_cb*)aos_zalloc(sizeof(struct player_cb));
    CHECK_RET_TAG_WITH_RET(player, NULL);
    player->ao_name = strdup(plyh->ao_name);
    CHECK_RET_TAG_WITH_GOTO(player->ao_name, err);

#if 1
    if (plyh->eq_segments && plyh->aef_conf && plyh->aef_conf_size) {
        LOGE(TAG, "param faild, eq & aef can't both enabled");
        goto err;
    }
#endif
    if (plyh->eq_segments) {
        player->eq_params = aos_zalloc(sizeof(eqfp_t) * plyh->eq_segments);
        CHECK_RET_TAG_WITH_GOTO(player->eq_params, err);
    }
    if (plyh->aef_conf && plyh->aef_conf_size) {
        player->aef_conf = aos_malloc(plyh->aef_conf_size);
        CHECK_RET_TAG_WITH_GOTO(player->aef_conf, err);
        memcpy(player->aef_conf, plyh->aef_conf, plyh->aef_conf_size);
        player->aef_conf_size = plyh->aef_conf_size;
    }

    player->event_cb      = plyh->event_cb;
    player->get_dec_cb    = plyh->get_dec_cb;
    player->resample_rate = plyh->resample_rate;
    player->vol_en        = plyh->vol_en;
    player->vol_index     = plyh->vol_index;
    player->rcv_timeout   = plyh->rcv_timeout ? plyh->rcv_timeout : SRCV_TIMEOUT_DEFAULT;
    player->eq_segments   = plyh->eq_segments;
    player->cache_size    = plyh->cache_size ? plyh->cache_size : SCACHE_SIZE_DEFAULT;
    player->cache_percent = plyh->cache_percent ? plyh->cache_percent : SCACHE_PERCENT_DEFAULT;
    player->period_ms     = plyh->period_ms ? plyh->period_ms : AO_ONE_PERIOD_MS;
    player->period_num    = plyh->period_num ? plyh->period_num : AO_TOTAL_PERIOD_NUM;
    player->status        = PLAYER_STATUS_STOPED;
    aos_event_new(&player->evt, 0);
    aos_mutex_new(&player->lock);
    _player_once_init(player);

    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    return player;
err:
    if (player) {
        aos_free(player->ao_name);
        aos_free(player->eq_params);
        aos_free(player->aef_conf);
        aos_free(player);
    }
    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    return NULL;
}

/**
 * @brief  control/config the player by command
 * @PARAM  [in] player
 * @PARAM  [in] cmd    : PLAYER_CMD_XXX
 * @param  [in] ...
 * @return
 */
int player_ioctl(player_t *player, int cmd, ...)
{
    int rc = -1;
    size_t size;
    va_list ap;

    CHECK_PARAM(player, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGD(TAG, "%s, %d enter. player = %p, cmd = %d", __FUNCTION__, __LINE__, player, cmd);
    switch (cmd) {
    case PLAYER_CMD_SET_RESAMPLE_RATE:
    case PLAYER_CMD_SET_RCVTO:
    case PLAYER_CMD_SET_CACHE_SIZE: {
        uint32_t val;
        va_start(ap, cmd);
        val = va_arg(ap, uint32_t);
        va_end(ap);
        if (cmd == PLAYER_CMD_SET_RESAMPLE_RATE)
            player->resample_rate = val;
        else if (cmd == PLAYER_CMD_SET_RCVTO)
            player->rcv_timeout = val ? val : SRCV_TIMEOUT_DEFAULT;
        else
            player->cache_size = val ? val : SCACHE_SIZE_DEFAULT;
        rc = 0;
    }
    break;
    case PLAYER_CMD_EQ_ENABLE: {
        peq_seten_t *val;
        va_start(ap, cmd);
        val = va_arg(ap, peq_seten_t*);
        va_end(ap);
        CHECK_RET_TAG_WITH_RET(val, -1);
        if (player->ao) {
            oeq_seten_t para;

            size = sizeof(para);
            para.enable = val->enable;
            rc = ao_control(player->ao, AO_CMD_EQ_ENABLE, (void*)&para, &size);
        } else {
            rc = 0;
        }
        player->eq_en = (rc == 0) ? val->enable : player->eq_en;
    }
    break;
    case PLAYER_CMD_EQ_SET_PARAM: {
        eqfp_t *param;
        peq_setpa_t *val;
        va_start(ap, cmd);
        val = va_arg(ap, peq_setpa_t*);
        va_end(ap);
        CHECK_RET_TAG_WITH_RET(val, -1);

        if (!((val->segid < player->eq_segments) && player->eq_params)) {
            LOGE(TAG, "param faild. segid = %u, eq_segments = %u", val->segid, player->eq_segments);
            break;
        }
        param = &val->param;
        if (!(param->gain >= -24.0 && param->gain <= 18.0
              && param->q >= 0.1 && param->q <= 50.0
              && param->type > EQF_TYPE_UNKNOWN && param->type < EQF_TYPE_MAX)) {
            LOGE(TAG, "eq param error");
            break;
        }

        if (player->ao) {
            oeq_setpa_t para;
            size       = sizeof(para);
            para.segid = val->segid;
            memcpy(&para.param, &val->param, sizeof(eqfp_t));
            rc = ao_control(player->ao, AO_CMD_EQ_SET_PARAM, (void*)&para, &size);
        } else {
            rc = 0;
        }
        if (rc == 0) {
            memcpy(&player->eq_params[val->segid], &val->param, sizeof(eqfp_t));
        }
    }
    break;
    default:
        break;
    }
    LOGD(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return rc;
}

static int _interrupt(void *arg)
{
    player_t *player = arg;

    return player->need_quit;
}

static ao_cls_t* _player_ao_new(player_t *player, sf_t ao_sf)
{
    int i, rc;
    aoh_t aoh;
    size_t size;
    ao_cls_t *ao;

    memset(&aoh, 0, sizeof(aoh_t));
    aoh.sf            = ao_sf;
    aoh.name          = player->ao_name;
    aoh.eq_segments   = player->eq_segments;
    aoh.resample_rate = player->resample_rate;
    aoh.aef_conf      = player->aef_conf;
    aoh.aef_conf_size = player->aef_conf_size;
    aoh.vol_en        = player->vol_en;
    aoh.vol_index     = player->vol_index;
    aoh.period_ms     = player->period_ms;
    aoh.period_num    = player->period_num;
    ao = ao_open(&aoh);
    CHECK_RET_TAG_WITH_RET(ao, NULL);

    if (player->eq_params) {
        oeq_seten_t eqen;
        oeq_setpa_t eqpa;

        size = sizeof(oeq_setpa_t);
        for (i = 0; i < player->eq_segments; i++) {
            if (player->eq_params[i].type != EQF_TYPE_UNKNOWN) {
                eqpa.segid = i;
                memcpy(&eqpa.param, (void*)&player->eq_params[i], sizeof(eqfp_t));
                rc = ao_control(ao, AO_CMD_EQ_SET_PARAM, (void*)&eqpa, &size);
                CHECK_RET_TAG_WITH_GOTO(rc == 0, err);
            }
        }

        size        = sizeof(oeq_seten_t);
        eqen.enable = player->eq_en;
        rc          = ao_control(ao, AO_CMD_EQ_ENABLE, (void*)&eqen, &size);
        CHECK_RET_TAG_WITH_GOTO(rc == 0, err);
    }

    return ao;
err:
    ao_close(ao);
    return NULL;
}

static int _player_prepare(player_t *player)
{
    int rc;
    sf_t sf;
    sth_t sth;
    stream_cls_t  *s       = NULL;
    demux_cls_t   *demuxer = NULL;
    ad_cls_t      *ad      = NULL;
    ao_cls_t      *ao      = NULL;

    memset(&sth, 0, sizeof(sth_t));
    sth.mode          = STREAM_READ;
    sth.url           = player->url;
    sth.get_dec_cb    = player->get_dec_cb;
    sth.cache_size    = player->cache_size;
    sth.cache_percent = player->cache_percent;
    sth.irq.arg       = player;
    sth.irq.handler   = _interrupt;
    s = stream_open(&sth);
    CHECK_RET_TAG_WITH_GOTO(s, err);
    demuxer = demux_open(s);
    CHECK_RET_TAG_WITH_GOTO(demuxer, err);

    ad = ad_open(&demuxer->ash);
    CHECK_RET_TAG_WITH_GOTO(ad, err);

    /* FIXME: sf of the demuxer may be inaccurate */
    sf = ad->ash.sf ? ad->ash.sf : demuxer->ash.sf;
    demuxer->ash.sf = sf;

#ifndef FPGA_ENABLE
    ao = _player_ao_new(player, sf);
    CHECK_RET_TAG_WITH_GOTO(ao, err);
    rc = ao_start(ao);
    CHECK_RET_TAG_WITH_GOTO(rc == 0, err);
#endif
    player->s       = s;
    player->demuxer = demuxer;
    player->ad      = ad;
    player->ao      = ao;

    return 0;
err:
#ifndef FPGA_ENABLE
    ao_close(ao);
#endif
    ad_close(ad);
    demux_close(demuxer);
    stream_close(s);
    return -1;
}

static void _ptask(void *arg)
{
    avpacket_t pkt;
    uint8_t play_first = 1;
    avframe_t *dframe;
    ad_cls_t *ad;
    demux_cls_t *demuxer;
    player_t *player = arg;
    int rc = -1, got_frame = 0;

    avpacket_init(&pkt);
    dframe = avframe_alloc();
    CHECK_RET_TAG_WITH_GOTO(dframe, quit);

    player_lock();
    rc = _player_prepare(player);
    if (rc < 0) {
        goto quit;
    }
    player->status = PLAYER_STATUS_PLAYING;
    player_unlock();
    ad      = player->ad;
    demuxer = player->demuxer;

    EVENT_CALL(player, PLAYER_EVENT_START, NULL, 0);
#ifdef FPGA_ENABLE
    char back[13] = {0};
    long long t = aos_now_ms();
    printf("\r\n====>pcm time             ");
    memset(back, '\b', 10);
#endif
    player_lock();
    for (;;) {
        player_unlock();
        player_lock();
        player->stat.run_loop++;
        if (PLAYER_STATUS_PAUSED == player->status) {
            aos_msleep(200);
            continue;
        } else if (PLAYER_STATUS_STOPED == player->status) {
            break;
        }

        rc = demux_read_packet(demuxer, &pkt);
        if (rc < 0) {
            LOGE(TAG, "read packet fail, rc = %d", rc);
            goto quit;
        } else if (rc == 0) {
            break;
        }

#if 0
        printf("====>>>size = %d\n", pkt.len);
#else
        player->cur_pts = pkt.pts;
        rc = ad_decode(ad, dframe, &got_frame, &pkt);
        if (rc <= 0) {
            LOGE(TAG, "ad decode fail, rc = %d", rc);
            goto quit;
        }
        if (!got_frame) {
            continue;
        }

#ifdef FPGA_ENABLE
        aos_msleep(10);
        if ((aos_now_ms() - t) > (120000)) {
            goto quit;
        }
        printf("%s%10ld", back, aos_now_ms() - t);
#else
        rc = ao_write(player->ao, dframe->data[0], dframe->linesize[0]);
        if (rc >= 0) {
            player->stat.ao_write_size += rc;
            player->stat.run_loop_valid++;
            if (play_first == 1) {
                play_first = 0;
                LOGI(TAG, "play first");
            }
        } else {
            LOGE(TAG, "ao write fail, rc = %d, pcm_size = %d", rc, dframe->linesize[0]);
            goto quit;
        }
#endif
#endif
    }
    rc = 0;
quit:
    player_unlock();
    LOGI(TAG, "cb run task quit");
    avpacket_free(&pkt);
    avframe_free(&dframe);
    aos_event_set(&player->evt, PLAYER_TASK_QUIT_EVT, AOS_EVENT_OR);
    if (player->status != PLAYER_STATUS_STOPED) {
        player->evt_status = (rc < 0) ? PLAYER_EVENT_ERROR : PLAYER_EVENT_FINISH;
        EVENT_CALL(player, player->evt_status, NULL, 0);
    }
    return;
}

/**
 * @brief  player play interface
 * @param  [in] player
 * @param  [in] url : example: http://ip:port/xx.mp3
 * @return 0/-1
 */
int player_play(player_t *player, const char *url)
{
    int rc = -1;
    aos_task_t ptask;

    if (!(player && url && strlen(url))) {
        LOGE(TAG, "param err, %s", __FUNCTION__);
        return -1;
    }

    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGI(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    if (player->status != PLAYER_STATUS_STOPED) {
        LOGE(TAG, "the player: %p is not stopped!", player);
        goto quit;
    }
    _player_once_init(player);
    player->url = strdup(url);
    CHECK_RET_TAG_WITH_GOTO(player->url, quit);
    player->status = PLAYER_STATUS_PREPARING;
    rc = aos_task_new_ext(&ptask, "player_task", _ptask, (void *)player, 96*1024, AOS_DEFAULT_APP_PRI - 2);
    if (rc != 0) {
        aos_freep(&player->url);
        LOGE(TAG, "player_task new create faild, may be oom, rc = %d", rc);
        goto quit;
    }

quit:
    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);
    return rc;
}

/**
 * @brief  pause the player
 * @param  [in] player
 * @return 0/-1
 */
int player_pause(player_t *player)
{
    int ret = -1;

    CHECK_PARAM(player, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGI(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    if (player->status == PLAYER_STATUS_PLAYING) {
        player->status = PLAYER_STATUS_PAUSED;
#ifndef FPGA_ENABLE
        ao_stop(player->ao);
#endif
        ret = 0;
    }
    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return ret;
}

/**
 * @brief  resume the player
 * @param  [in] player
 * @return 0/-1
 */
int player_resume(player_t *player)
{
    int ret = -1;

    CHECK_PARAM(player, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGI(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    if (player->status == PLAYER_STATUS_PAUSED) {
#ifndef FPGA_ENABLE
        ao_start(player->ao);
#endif
        player->status = PLAYER_STATUS_PLAYING;
        ret = 0;
    }
    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return ret;
}

static int _player_stop(player_t *player)
{
    int ret = -1;

#ifndef FPGA_ENABLE
    if (player->ao) {
        if ((demux_is_eof(player->demuxer) == 1) && (player->evt_status == PLAYER_EVENT_FINISH)) {
            ao_drain(player->ao);
        }
        ao_stop(player->ao);
        ao_close(player->ao);
        player->ao = NULL;
    }
#endif
    if (player->ad) {
        ad_close(player->ad);
        player->ad = NULL;
    }
    if (player->demuxer) {
        demux_close(player->demuxer);
        player->demuxer = NULL;
    }
    if (player->s) {
        stream_close(player->s);
        player->s = NULL;
    }

    ret = 0;
    aos_freep(&player->url);

    return ret;
}

/**
 * @brief  stop the player
 * @param  [in] player
 * @return 0/-1
 */
int player_stop(player_t *player)
{
    int ret = 0;
    unsigned int flag;

    CHECK_PARAM(player, -1);
    player->need_quit = 1;
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGI(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    if (player->status != PLAYER_STATUS_STOPED) {
        player->status = PLAYER_STATUS_STOPED;
        aos_mutex_unlock(&player->lock);
        aos_event_get(&player->evt, PLAYER_TASK_QUIT_EVT, AOS_EVENT_OR_CLEAR, &flag, AOS_WAIT_FOREVER);
        ret = _player_stop(player);
        _player_once_init(player);
    } else {
        aos_mutex_unlock(&player->lock);
    }

    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    return ret;
}

/**
 * @brief  free/destroy the player obj
 * @param  [in] player
 * @return 0/-1
 */
int player_free(player_t *player)
{
    int rc = 0;

    LOGI(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    CHECK_PARAM(player, -1);

    if (player->status != PLAYER_STATUS_STOPED) {
        rc = player_stop(player);
    }

    aos_event_free(&player->evt);
    aos_mutex_free(&player->lock);
    aos_free(player->ao_name);
    aos_free(player->aef_conf);
    aos_free(player);

    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    return rc;
}

/**
 * @brief  seek to the time
 * @param  [in] player
 * @param  [in] timestamp : seek time
 * @return 0/-1
 */
int player_seek(player_t *player, uint64_t timestamp)
{
    int rc = -1;
    demux_cls_t *demuxer;

    CHECK_PARAM(player, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGI(TAG, "%s, %d enter. player = %p, timestamp = %llu", __FUNCTION__, __LINE__, player, timestamp);
    demuxer = player->demuxer;
    if (player->status == PLAYER_STATUS_PLAYING || player->status == PLAYER_STATUS_PAUSED) {
        //FIXME: atleast 2 second at tail
        if (timestamp + 2000 < demuxer->duration) {
            ao_stop(player->ao);
            rc = demux_seek(demuxer, timestamp);
            if (rc == 0) {
                ad_reset(player->ad);
            }
            ao_start(player->ao);
        }
    }
    LOGI(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return rc;
}

/**
 * @brief  get current play time
 * @param  [in] player
 * @param  [in/out] ptime
 * @return 0/-1
 */
int player_get_cur_ptime(player_t *player, play_time_t *ptime)
{
    int rc = -1;
    demux_cls_t *demuxer;

    CHECK_PARAM(player && ptime, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGD(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    demuxer = player->demuxer;
    if (player->status == PLAYER_STATUS_PLAYING || player->status == PLAYER_STATUS_PAUSED) {
        ptime->curtime  = demuxer->time_scale ? player->cur_pts * 1000 / demuxer->time_scale : 0;
        ptime->duration = demuxer->duration;
        rc              = 0;
    }
    LOGD(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return rc;
}

/**
 * @brief  get media info
 * @param  [in] player
 * @param  [in/out] minfo
 * @return 0/-1
 */
int player_get_media_info(player_t *player, media_info_t *minfo)
{
    int rc = -1;
    demux_cls_t *demuxer;

    CHECK_PARAM(player && minfo, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGD(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    demuxer = player->demuxer;
    if (player->status == PLAYER_STATUS_PLAYING || player->status == PLAYER_STATUS_PAUSED) {
        minfo->tracks   = demuxer->tracks;
        minfo->size     = stream_get_size(player->s);
        //TODO: just one track now for audio player
        minfo->bps      = demuxer->bps;
        minfo->duration = demuxer->duration;
        rc              = 0;
    }
    LOGD(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return rc;
}

/**
 * @brief  get soft vol index of the player
 * @param  [in] player
 * @param  [in/out] vol : vol scale index(0~255)
 * @return 0/-1
 */
int player_get_vol(player_t *player, uint8_t *vol)
{
    int rc = -1;

    CHECK_PARAM(player && vol, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGD(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    if (player->vol_en) {
        *vol = player->vol_index;
        rc = 0;
    }
    LOGD(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return rc;
}

/**
 * @brief  set soft vol index of the player
 * @param  [in] player
 * @param  [in/out] vol : vol scale index(0~255)
 * @return 0/-1
 */
int player_set_vol(player_t *player, uint8_t vol)
{
    int rc = -1;

    CHECK_PARAM(player, -1);
    aos_mutex_lock(&player->lock, AOS_WAIT_FOREVER);
    LOGD(TAG, "%s, %d enter. player = %p", __FUNCTION__, __LINE__, player);
    if (player->vol_en) {
        if (player->ao) {
            size_t size;
            ovol_set_t para;

            size = sizeof(para);
            para.vol_index = vol;
            rc = ao_control(player->ao, AO_CMD_VOL_SET, (void*)&para, &size);
            player->vol_index = rc < 0 ? player->vol_index : vol;
        } else {
            player->vol_index = vol;
            rc = 0;
        }
    }
    LOGD(TAG, "%s, %d leave. player = %p", __FUNCTION__, __LINE__, player);
    aos_mutex_unlock(&player->lock);

    return rc;
}





