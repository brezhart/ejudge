/* -*- mode: c; c-basic-offset: 4 -*- */

#ifndef __OAUTH_H__
#define __OAUTH_H__

/* Copyright (C) 2021 Alexander Chernov <cher@ejudge.ru> */

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

unsigned char *
oauth_get_redirect_url(
        const struct ejudge_cfg *config,
        const unsigned char *provider,
        const unsigned char *cookie,
        int contest_id,
        const unsigned char *extra_data);

// callback called when this fd is ready for reading
typedef void (*oauth_fd_ready_callback_func_t)(void *data, int fd);

// function for registering callback
typedef void (*oauth_register_fd_func_t)(void *register_data, int fd, oauth_fd_ready_callback_func_t cb, void *data);

void
oauth_set_register_fd_func(oauth_register_fd_func_t func, void *register_data);

unsigned char *
oauth_server_callback(
        const struct ejudge_cfg *config,
        const unsigned char *provider,
        const unsigned char *state_id,
        const unsigned char *code);

struct OAuthLoginResult
{
    int status; // 0, 1 - progress; 2 - fail, 3 - success
    unsigned char *provider;
    unsigned char *cookie;
    unsigned char *extra_data;
    unsigned char *email;
    unsigned char *name;
    unsigned char *access_token;
    unsigned char *id_token;
    unsigned char *error_message;
    int contest_id;
};

struct OAuthLoginResult
oauth_get_result(
        const struct ejudge_cfg *config,
        const unsigned char *provider,
        const unsigned char *request_id);

void
oauth_free_result(struct OAuthLoginResult *res);

#endif /* __OAUTH_H__ */