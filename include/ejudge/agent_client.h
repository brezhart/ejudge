/* -*- mode: c; c-basic-offset: 4 -*- */
#ifndef __AGENT_CLIENT_H__
#define __AGENT_CLIENT_H__

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

#include <stdlib.h>

struct AgentClient;

struct AgentClientOps
{
    struct AgentClient *(*destroy)(struct AgentClient *ac);
    int (*init)(
        struct AgentClient *ac,
        const unsigned char *id,
        const unsigned char *endpoint,
        const unsigned char *name,
        int mode);
    int (*connect)(struct AgentClient *ac);
    void (*close)(struct AgentClient *ac);
    int (*is_closed)(struct AgentClient *ac);

    int (*poll_queue)(
        struct AgentClient *ac,
        unsigned char *pkt_name,
        size_t pkt_len);
};

struct AgentClient
{
    const struct AgentClientOps *ops;
};

struct AgentClient *agent_client_ssh_create(void);

#endif /* __AGENT_CLIENT_H__ */
