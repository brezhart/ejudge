/* -*- mode: c; c-basic-offset: 4 -*- */

/* Copyright (C) 2022 Alexander Chernov <cher@ejudge.ru> */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "ejudge/config.h"
#include "ejudge/agent_client.h"
#include "ejudge/prepare.h"
#include "ejudge/xalloc.h"
#include "ejudge/errlog.h"
#include "ejudge/osdeps.h"

#include <stdlib.h>
#include "ejudge/cJSON.h"

#include <stdio.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/eventfd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/time.h>

struct FDChunk
{
    unsigned char *data;
    int size;
};

struct Future
{
    int serial;

    int ready;
    pthread_mutex_t m;
    pthread_cond_t c;

    cJSON *value;
};

struct AgentClientSsh
{
    struct AgentClient b;

    pthread_t tid;

    unsigned char *id;
    unsigned char *endpoint;
    unsigned char *name;
    int mode;

    // read buffer
    unsigned char *rd_data;
    int rd_size, rd_rsrv;

    unsigned char *wr_data;
    int wr_size, wr_pos;

    pthread_mutex_t rchunkm;
    pthread_cond_t rchunkc;
    struct FDChunk *rchunks;
    int rchunku;
    int rchunka;

    pthread_mutex_t wchunkm;
    struct FDChunk *wchunks;
    int wchunku;
    int wchunka;
    uint32_t wevents;

    pthread_mutex_t futurem;
    struct Future **futures;
    int futureu;
    int futurea;

    int efd;
    int pid;                    /* ssh pid */
    int from_ssh;               /* pipe to ssh */
    int to_ssh;                 /* pipe from ssh */
    int vfd;                    /* to wake up connect thread */

    _Bool need_cleanup;         /* if read/write failed, clean-up */
    _Atomic _Bool stop_request;
    _Atomic _Bool is_stopped;
    pthread_mutex_t stop_m;
    pthread_cond_t stop_c;

    int serial;
};

static void future_init(struct Future *f, int serial)
{
    memset(f, 0, sizeof(*f));
    f->serial = serial;
    pthread_mutex_init(&f->m, NULL);
    pthread_cond_init(&f->c, NULL);
}

static void future_fini(struct Future *f)
{
    pthread_mutex_destroy(&f->m);
    pthread_cond_destroy(&f->c);
    if (f->value) cJSON_Delete(f->value);
}

static void future_wait(struct Future *f)
{
    pthread_mutex_lock(&f->m);
    while (!f->ready) {
        pthread_cond_wait(&f->c, &f->m);
    }
    pthread_mutex_unlock(&f->m);
}

static struct AgentClient *
destroy_func(struct AgentClient *ac)
{
    struct AgentClientSsh *acs = (struct AgentClientSsh *) ac;
    if (!acs) return NULL;

    pthread_mutex_destroy(&acs->futurem);
    free(acs->futures);
    pthread_mutex_destroy(&acs->stop_m);
    pthread_cond_destroy(&acs->stop_c);
    pthread_mutex_destroy(&acs->wchunkm);
    for (int i = 0; i < acs->wchunku; ++i) {
        free(acs->wchunks[i].data);
    }
    free(acs->wchunks);
    free(acs->wr_data);
    pthread_mutex_destroy(&acs->rchunkm);
    pthread_cond_destroy(&acs->rchunkc);
    for (int i = 0; i < acs->rchunku; ++i) {
        free(acs->rchunks[i].data);
    }
    free(acs->rchunks);
    free(acs->rd_data);
    if (acs->pid > 0) {
        kill(acs->pid, SIGKILL);
        waitpid(acs->pid, NULL, 0);
    }
    if (acs->efd >= 0) close(acs->efd);
    if (acs->from_ssh >= 0) close(acs->from_ssh);
    if (acs->to_ssh >= 0) close(acs->to_ssh);
    if (acs->vfd >= 0) close(acs->vfd);
    free(acs);
    return NULL;
}

static int
init_func(
        struct AgentClient *ac,
        const unsigned char *id,
        const unsigned char *endpoint,
        const unsigned char *name,
        int mode)
{
    struct AgentClientSsh *acs = (struct AgentClientSsh *) ac;
    acs->id = xstrdup(id);
    acs->endpoint = xstrdup(endpoint);
    if (name) {
        acs->name = xstrdup(name);
    }
    acs->mode = mode;
    return 0;
}

static void
add_rchunk(struct AgentClientSsh *acs, const unsigned char *data, int size)
{
    pthread_mutex_lock(&acs->rchunkm);
    if (acs->rchunka == acs->rchunku) {
        if (!(acs->rchunka *= 2)) acs->rchunka = 4;
        XREALLOC(acs->rchunks, acs->rchunka);
    }
    struct FDChunk *c = &acs->rchunks[acs->rchunku++];
    c->data = malloc(size + 1);
    memcpy(c->data, data, size);
    c->data[size] = 0;
    c->size = size;
    if (acs->rchunku == 1) {
        pthread_cond_signal(&acs->rchunkc);
    }
    pthread_mutex_unlock(&acs->rchunkm);
}

static void
add_wchunk_move(struct AgentClientSsh *acs, unsigned char *data, int size)
{
    pthread_mutex_lock(&acs->wchunkm);
    if (acs->wchunka == acs->wchunku) {
        if (!(acs->wchunka *= 2)) acs->wchunka = 4;
        XREALLOC(acs->wchunks, acs->wchunka);
    }
    struct FDChunk *c = &acs->wchunks[acs->wchunku++];
    c->data = data;
    c->size = size;
    pthread_mutex_unlock(&acs->wchunkm);
    uint64_t v = 1;
    write(acs->vfd, &v, sizeof(v));
}

static void
add_future(struct AgentClientSsh *acs, struct Future *f)
{
    pthread_mutex_lock(&acs->futurem);
    if (acs->futurea == acs->futureu) {
        if (!(acs->futurea *= 2)) acs->futurea = 4;
        XREALLOC(acs->futures, acs->futurea);
    }
    acs->futures[acs->futureu++] = f;
    pthread_mutex_unlock(&acs->futurem);
}

static struct Future *
get_future(struct AgentClientSsh *acs, int serial)
{
    struct Future *result = NULL;
    pthread_mutex_lock(&acs->futurem);
    for (int i = 0; i < acs->futureu; ++i) {
        if (acs->futures[i]->serial == serial) {
            result = acs->futures[i];
            if (i < acs->futureu - 1) {
                memcpy(&acs->futures[i], &acs->futures[i + 1],
                       (acs->futureu - i - 1) * sizeof(acs->futures[0]));
            }
            --acs->futureu;
            break;
        }
    }
    pthread_mutex_unlock(&acs->futurem);
    return result;
}

static void
do_pipe_read(struct AgentClientSsh *acs)
{
    char buf[65536];
    while (1) {
        errno = 0;
        int r = read(acs->from_ssh, buf, sizeof(buf));
        if (r < 0 && errno == EAGAIN) {
            break;
        }
        if (r < 0) {
            err("pipe_read_func: read failed: %s", os_ErrorMsg());
            acs->need_cleanup = 1;
            return;
        }
        if (!r) {
            acs->need_cleanup = 1;
            return;
        }
        int exp_size = acs->rd_size + r + 1;
        if (exp_size >= acs->rd_rsrv) {
            int exp_rsrv = acs->rd_rsrv * 2;
            if (!exp_rsrv) exp_rsrv = 32;
            while (exp_rsrv < exp_size) exp_rsrv *= 2;
            acs->rd_data = xrealloc(acs->rd_data, exp_rsrv);
            acs->rd_rsrv = exp_rsrv;
        }
        memcpy(acs->rd_data + acs->rd_size, buf, r);
        acs->rd_size += r;
        acs->rd_data[acs->rd_size] = 0;
    }
    if (acs->rd_size >= 2) {
        int s = 0;
        for (int i = 1; i < acs->rd_size; ++i) {
            if (acs->rd_data[i] == '\n' && acs->rd_data[i - 1] == '\n') {
                add_rchunk(acs, &acs->rd_data[s], i - s + 1);
                s = i + 1;
            }
        }
        if (s > 0) {
            acs->rd_size -= s;
            memcpy(acs->rd_data, acs->rd_data + s, acs->rd_size);
        }
    }
}

static void
do_pipe_write(struct AgentClientSsh *acs)
{
    while (1) {
        int wsz = acs->wr_size - acs->wr_pos;
        if (wsz < 0) abort();
        if (!wsz) {
            unsigned char *data = NULL;
            int size = 0;
            pthread_mutex_lock(&acs->wchunkm);
            if (acs->wchunku > 0) {
                data = acs->wchunks[0].data;
                size = acs->wchunks[0].size;
                if (acs->wchunku > 1) {
                    memcpy(&acs->wchunks[0], &acs->wchunks[1],
                           (acs->wchunku - 1) * sizeof(acs->wchunks[0]));
                }
                --acs->wchunku;
            }
            pthread_mutex_unlock(&acs->wchunkm);
            if (!size) {
                free(data);
                free(acs->wr_data); acs->wr_data = NULL;
                acs->wr_size = 0; acs->wr_pos = 0;
                epoll_ctl(acs->efd, EPOLL_CTL_DEL, acs->to_ssh, NULL);
                acs->wevents = 0;
                return;
            }
            free(acs->wr_data);
            acs->wr_data = data;
            acs->wr_size = size;
            acs->wr_pos = 0;
            continue;
        }
        errno = 0;
        int r = write(acs->to_ssh, acs->wr_data + acs->wr_pos, wsz);
        if (r < 0 && errno == EAGAIN) {
            break;
        }
        if (r < 0) {
            err("pipe_write_func: write failed: %s", os_ErrorMsg());
            acs->need_cleanup = 1;
            return;
        }
        if (!r) {
            err("pipe_write_func: write returned 0");
            acs->need_cleanup = 1;
            return;
        }
        acs->wr_pos += r;
    }
}

static void
do_notify_read(struct AgentClientSsh *acs)
{
    uint64_t value = 0;
    int r;
    if ((r = read(acs->vfd, &value, sizeof(value))) < 0) {
        err("notify_read: read error: %s", os_ErrorMsg());
        acs->need_cleanup = 1;
    }
    if (r == 0) {
        err("notify_read: unexpected EOF");
        acs->need_cleanup = 1;
    }
    if (acs->stop_request) return;
    if (acs->to_ssh < 0) return;

    if (!(acs->wevents & EPOLLOUT)) {
        struct epoll_event ev = { .events = EPOLLOUT, .data.fd = acs->to_ssh };
        epoll_ctl(acs->efd, EPOLL_CTL_ADD, acs->to_ssh, &ev);
        acs->wevents |= EPOLLOUT;
    }
}

static void
handle_rchunks(struct AgentClientSsh *acs)
{
    pthread_mutex_lock(&acs->rchunkm);
    if (acs->rchunku <= 0) goto done1;

    for (int i = 0; i < acs->rchunku; ++i) {
        struct FDChunk *c = &acs->rchunks[i];
        cJSON *j = cJSON_Parse(c->data);
        if (!j) {
            err("JSON parse error");
        } else {
            cJSON *js = cJSON_GetObjectItem(j, "s");
            if (!js || js->type != cJSON_Number) {
                err("invalid JSON");
            } else {
                int serial = js->valuedouble;
                struct Future *f = get_future(acs, serial);
                if (f) {
                    f->value = j; j = NULL;
                    pthread_mutex_lock(&f->m);
                    f->ready = 1;
                    pthread_cond_signal(&f->c);
                    pthread_mutex_unlock(&f->m);
                }
            }
            if (j) cJSON_Delete(j);
        }
        free(c->data); c->data = NULL; c->size = 0;
    }
    acs->rchunku = 0;

done1:
    pthread_mutex_unlock(&acs->rchunkm);
}

static void *
thread_func(void *ptr)
{
    sigset_t ss;
    sigfillset(&ss);
    pthread_sigmask(SIG_BLOCK, &ss, NULL);

    struct AgentClientSsh *acs = (struct AgentClientSsh *) ptr;

    while (1) {
        struct epoll_event evs[16];
        errno = 0;
        int n = epoll_wait(acs->efd, evs, 16, -1);
        if (n < 0 && errno == EINTR) {
            info("epoll_wait interrupted by a signal");
            continue;
        }
        if (n < 0) {
            err("epoll_wait failed: %s", os_ErrorMsg());
            break;
        }
        if (!n) {
            err("epoll_wait returned 0");
            break;
        }
        for (int i = 0; i < n; ++i) {
            struct epoll_event *ev = &evs[i];
            if (ev->data.fd == acs->vfd) {
                if ((ev->events & (EPOLLIN | EPOLLHUP)) != 0) {
                    do_notify_read(acs);
                } else {
                    err("spurious wake-up on read from ssh");
                }
            }
            if (ev->data.fd == acs->from_ssh) {
                if ((ev->events & (EPOLLIN | EPOLLHUP)) != 0) {
                    do_pipe_read(acs);
                } else {
                    err("spurious wake-up on read from ssh");
                }
            }
            if (acs->to_ssh >= 0 && ev->data.fd == acs->to_ssh) {
                if ((ev->events & (EPOLLOUT | EPOLLERR)) != 0) {
                    do_pipe_write(acs);
                } else {
                    err("spurious wake-up on write from ssh");
                }
            }
        }
        if (acs->stop_request) {
            // forcefully close write fd
            if ((acs->wevents & EPOLLOUT) != 0) {
                epoll_ctl(acs->efd, EPOLL_CTL_DEL, acs->to_ssh, NULL);
                acs->wevents = 0;
            }
            if (acs->to_ssh >= 0) {
                close(acs->to_ssh);
                acs->to_ssh = -1;
            }
        }
        if (acs->need_cleanup) {
            // what else to do?
            break;
        }
        handle_rchunks(acs);
    }

    if (acs->pid > 0) {
        kill(acs->pid, SIGKILL);
        waitpid(acs->pid, NULL, 0);
        acs->pid = -1;
    }

    pthread_mutex_lock(&acs->stop_m);
    acs->is_stopped = 1;
    pthread_cond_signal(&acs->stop_c);
    pthread_mutex_unlock(&acs->stop_m);

    return NULL;
}

static int
connect_func(struct AgentClient *ac)
{
    struct AgentClientSsh *acs = (struct AgentClientSsh *) ac;
    int tossh[2] = { -1, -1 }, fromssh[2] = { -1, -1 };

    pipe2(tossh, O_CLOEXEC);
    pipe2(fromssh, O_CLOEXEC);

    acs->pid = fork();
    if (acs->pid < 0) {
        err("fork failed: %s", os_ErrorMsg());
        goto fail;
    }
    if (!acs->pid) {
        // ssh -aTx ENDPOINT -i ID -n NAME -m MODE EJUDGE/ej-agent 2>>LOG
        char *cmd_s = NULL;
        size_t cmd_z = 0;
        FILE *cmd_f = open_memstream(&cmd_s, &cmd_z);
        fprintf(cmd_f, "exec %s/ej-agent", EJUDGE_SERVER_BIN_PATH);
        if (acs->id && acs->id[0]) {
            fprintf(cmd_f, " -i '%s'", acs->id);
        }
        if (acs->name) {
            fprintf(cmd_f, " -n '%s'", acs->name);
        }
        if (acs->mode == PREPARE_COMPILE) {
            fprintf(cmd_f, " -m compile");
        } else if (acs->mode == PREPARE_RUN) {
            fprintf(cmd_f, " -m run");
        }
        fprintf(cmd_f, " 2>>%s/var/ej-agent.log", EJUDGE_CONTESTS_HOME_DIR);
        fclose(cmd_f); cmd_f = NULL;

        dup2(tossh[0], 0); close(tossh[0]); close(tossh[1]);
        dup2(fromssh[1], 1); close(fromssh[0]); close(fromssh[1]);

        char *args[] =
        {
            "ssh",
            "-aTx",
            acs->endpoint,
            cmd_s,
            NULL,
        };

        /*
        int fd = open("/dev/null", O_WRONLY);
        dup2(fd, 2); close(fd);
        */

        execvp("ssh", args);
        _exit(1);
    }

    close(tossh[0]); tossh[0] = -1;
    close(fromssh[1]); fromssh[1] = -1;
    acs->from_ssh = fromssh[0]; fromssh[0] = -1;
    acs->to_ssh = tossh[1]; tossh[1] = -1;
    fcntl(acs->from_ssh, F_SETFL, fcntl(acs->from_ssh, F_GETFL) | O_NONBLOCK);
    fcntl(acs->to_ssh, F_SETFL, fcntl(acs->to_ssh, F_GETFL) | O_NONBLOCK);

    if ((acs->vfd = eventfd(0, EFD_CLOEXEC)) < 0) {
        err("eventfd create failed: %s", os_ErrorMsg());
        goto fail;
    }

    if ((acs->efd = epoll_create1(EPOLL_CLOEXEC)) < 0) {
        err("epoll_create failed: %s", os_ErrorMsg());
        goto fail;
    }

    {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = acs->vfd };
        epoll_ctl(acs->efd, EPOLL_CTL_ADD, acs->vfd, &ev);
    }
    {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = acs->from_ssh };
        epoll_ctl(acs->efd, EPOLL_CTL_ADD, acs->from_ssh, &ev);
    }

    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, 1024 * 1024);
    pthread_attr_setdetachstate(&pa, PTHREAD_CREATE_DETACHED);
    int e = pthread_create(&acs->tid, &pa, thread_func, acs);
    if (e) {
        err("pthread_create failed: %s", strerror(e));
        goto fail;
    }
    pthread_attr_destroy(&pa);

    return 0;

fail:
    if (tossh[0] >= 0) close(tossh[0]);
    if (tossh[1] >= 0) close(tossh[1]);
    if (fromssh[0] >= 0) close(fromssh[0]);
    if (fromssh[1] >= 0) close(fromssh[1]);
    return -1;
}

static void
close_func(struct AgentClient *ac)
{
    struct AgentClientSsh *acs = (struct AgentClientSsh *) ac;
    pthread_mutex_lock(&acs->stop_m);
    acs->stop_request = 1;
    uint64_t value = 1;
    write(acs->vfd, &value, sizeof(value));
    while (!acs->is_stopped) {
        pthread_cond_wait(&acs->stop_c, &acs->stop_m);
    }
    pthread_mutex_unlock(&acs->stop_m);
}

static int
is_closed_func(struct AgentClient *ac)
{
    struct AgentClientSsh *acs = (struct AgentClientSsh *) ac;
    return acs->is_stopped;
}

static void
add_wchunk_json(
        struct AgentClientSsh *acs,
        cJSON *json)
{
    char *str = cJSON_Print(json);
    int len = strlen(str);
    str = realloc(str, len + 2);
    str[len++] = '\n';
    str[len++] = '\n';
    str[len] = 0;
    add_wchunk_move(acs, str, len);
}

static int
poll_queue_func(
        struct AgentClient *ac,
        unsigned char *pkt_name,
        size_t pkt_len)
{
    struct AgentClientSsh *acs = (struct AgentClientSsh *) ac;
    cJSON *jq = cJSON_CreateObject();
    int serial = ++acs->serial;
    struct Future f;
    future_init(&f, serial);
    add_future(acs, &f);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long current_time_us = tv.tv_sec * 1000LL + tv.tv_usec / 1000;

    cJSON_AddNumberToObject(jq, "t", (double) current_time_us);
    cJSON_AddNumberToObject(jq, "s", (double) serial);
    cJSON_AddStringToObject(jq, "q", "poll");
    add_wchunk_json(acs, jq);
    cJSON_Delete(jq); jq = NULL;

    future_wait(&f);

    future_fini(&f);
    return 0;
}

static const struct AgentClientOps ops_ssh =
{
    destroy_func,
    init_func,
    connect_func,
    close_func,
    is_closed_func,
    poll_queue_func,
};

struct AgentClient *
agent_client_ssh_create(void)
{
    struct AgentClientSsh *acs;

    XCALLOC(acs, 1);
    acs->b.ops = &ops_ssh;

    acs->efd = -1;
    acs->pid = -1;
    acs->from_ssh = -1;
    acs->to_ssh = -1;
    acs->vfd = -1;

    pthread_mutex_init(&acs->rchunkm, NULL);
    pthread_cond_init(&acs->rchunkc, NULL);
    pthread_mutex_init(&acs->wchunkm, NULL);
    pthread_mutex_init(&acs->stop_m, NULL);
    pthread_cond_init(&acs->stop_c, NULL);
    pthread_mutex_init(&acs->futurem, NULL);

    return &acs->b;
}