/*
 * This file is part of cyanrip.
 *
 * cyanrip is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * cyanrip is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with cyanrip; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

/* A minimal reader for web service replies. Values are addressed by pointers
 * into the text, which runs up to end and need not be NUL-terminated. Every
 * function takes a NULL value, and returns NULL or -1 for it, so lookups can
 * be chained. */

/* Value of member key of the object at p, NULL if p is not an object or
 * has no such member */
const char *crip_json_get(const char *p, const char *end, const char *key);

/* Number of elements of the array at p, -1 if p is not an array */
int crip_json_count(const char *p, const char *end);

/* Unescaped copy of the string at p, to be freed with av_free(),
 * NULL if p is not a string */
char *crip_json_str(const char *p, const char *end);
