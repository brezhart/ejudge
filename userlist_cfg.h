/* -*- c -*- */
/* $Id$ */

#ifndef __USERLIST_CFG_H__
#define __USERLIST_CFG_H__ 1

/* Copyright (C) 2002 Alexander Chernov <cher@ispras.ru> */

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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>

struct userlist_cfg
{
  unsigned char *socket_path;
  unsigned char *db_path;
};

struct userlist_cfg *userlist_cfg_parse(char const *);
struct userlist_cfg *userlist_cfg_free(struct userlist_cfg *);
void userlist_cfg_unparse(struct userlist_cfg *, FILE *);

#endif /* __USERLIST_CFG_H__ */
