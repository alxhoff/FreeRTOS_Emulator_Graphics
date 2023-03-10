/**
 * @file gfx_font.c
 * @author Alex Hoffman
 * @date 30 April 2020
 * @brief Manages fonts used in GFX Draw
 *
 * @verbatim
 ----------------------------------------------------------------------
 Copyright (C) Alexander Hoffman, 2020
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 any later version.
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ----------------------------------------------------------------------
 @endverbatim
 */

#include <stdlib.h>
#include <pthread.h>

#include "gfx_font.h"
#include "gfx_utils.h"
#include "gfx_print.h"

#define PRINT_TTF_ERROR(msg, ...)                                              \
    PRINT_ERROR("[TTF Error] %s\n" #msg, (char *)TTF_GetError(),           \
                ##__VA_ARGS__)

struct gfx_font_ref {
    TTF_Font *font;
    unsigned ref_count;
    unsigned pending_free;
};

typedef struct gfx_font {
    char *path;
    char *name;
    struct gfx_font_ref font;
    unsigned size;
    struct gfx_font *next;
} gfx_font_t;

pthread_mutex_t list_lock = PTHREAD_MUTEX_INITIALIZER;
static struct gfx_font font_list = { 0 };

static const char *fonts_dir;
static struct gfx_font *cur_default_font = NULL;

static char *_getFontPath(char *font_name)
{
    unsigned font_dir_len = strlen(fonts_dir);
    unsigned font_len = strlen(font_name);

    if ((font_dir_len + strlen(font_name)) > MAX_FONT_NAME_LENGTH) {
        return NULL;
    }

    char *ret = (char *)calloc(font_dir_len + font_len + 1, sizeof(char));
    if (ret == NULL) {
        return NULL;
    }

    strcpy(ret, fonts_dir);
    strcpy(ret + font_dir_len, font_name);

    return ret;
}

static struct gfx_font *_loadFont(char *font_name, ssize_t size)
{
    struct gfx_font *ret;

    ret = calloc(1, sizeof(struct gfx_font));
    if (ret == NULL) {
        goto err_alloc_def_font;
    }

    ret->path = _getFontPath(font_name);
    if (ret->path == NULL) {
        goto err_get_font_path;
    }

    ret->name = ret->path + strlen(fonts_dir);
    ret->size = size;

    ret->font.font = TTF_OpenFont(ret->path, ret->size);
    if (ret->font.font == NULL) {
        PRINT_TTF_ERROR("Failed to load default font");
        goto err_font_open;
    }

    ret->font.ref_count = 0;
    ret->font.pending_free = 0;

    return ret;

err_font_open:
    free(ret->path);
err_get_font_path:
    free(ret);
err_alloc_def_font:
    return NULL;
}

int gfxFontInit(char *path)
{
    const char *resource_path = gfxUtilFindResourceDirectory();
    fonts_dir = gfxUtilPrependPath(resource_path, FONTS_DIR);

    font_list.next = _loadFont(DEFAULT_FONT, DEFAULT_FONT_SIZE);
    if (font_list.next == NULL) {
        return -1;
    }

    cur_default_font = font_list.next;

    return 0;
}

void gfxFontDeleteFont(struct gfx_font *font)
{
    free(font->path);
    TTF_CloseFont(font->font.font);
    free(font);
}

void gfxFontExit(void)
{
    pthread_mutex_lock(&list_lock);
    struct gfx_font *iterator = font_list.next;
    struct gfx_font *delete = NULL;

    while (iterator) {
        delete = iterator;
        iterator = iterator->next;

        gfxFontDeleteFont(delete);
    }

    pthread_mutex_unlock(&list_lock);
}

void gfxFontPutFontHandle(font_handle_t font)
{
    pthread_mutex_lock(&list_lock);
    struct gfx_font *iterator = &font_list;
    struct gfx_font *delete = NULL;

    for (; iterator; iterator = iterator->next) {
        if (iterator == font) {
            iterator->font.ref_count--;
            if (iterator->font.ref_count == 0 &&
                iterator->font.pending_free) {
                if (iterator->next) {
                    if (delete) {
                        delete->next = iterator->next;
                    }
                    else {
                        font_list.next = iterator->next;
                    }
                }
                gfxFontDeleteFont(iterator);
            }
            pthread_mutex_unlock(&list_lock);
            return;
        }
        delete = iterator;
    }
    pthread_mutex_unlock(&list_lock);
}

void gfxFontPutFont(const TTF_Font *font)
{
    pthread_mutex_lock(&list_lock);
    struct gfx_font *iterator = &font_list;
    struct gfx_font *delete = NULL;

    for (; iterator; iterator = iterator->next) {
        if (iterator->font.font == font) {
            iterator->font.ref_count--;
            if (iterator->font.ref_count == 0 &&
                iterator->font.pending_free) {
                if (iterator->next) {
                    if (delete) {
                        delete->next = iterator->next;
                    }
                    else {
                        font_list.next = iterator->next;
                    }
                }
                gfxFontDeleteFont(iterator);
            }
            pthread_mutex_unlock(&list_lock);
            return;
        }
        delete = iterator;
    }
    pthread_mutex_unlock(&list_lock);
}

TTF_Font *gfxFontGetCurFont(void)
{
    TTF_Font *ret;

    pthread_mutex_lock(&list_lock);

    cur_default_font->font.ref_count++;
    ret = cur_default_font->font.font;

    pthread_mutex_unlock(&list_lock);

    return ret;
}

ssize_t gfxFontGetCurFontSize(void)
{
    pthread_mutex_lock(&list_lock);
    ssize_t ret = cur_default_font->size;
    pthread_mutex_unlock(&list_lock);
    return ret;
}

char *gfxFontGetCurFontName(void)
{
    pthread_mutex_lock(&list_lock);
    char *ret = strdup(cur_default_font->name);
    pthread_mutex_unlock(&list_lock);
    return ret;
}

font_handle_t gfxFontGetCurFontHandle(void)
{
    pthread_mutex_lock(&list_lock);
    cur_default_font->font.ref_count++;
    font_handle_t ret = cur_default_font;
    pthread_mutex_unlock(&list_lock);
    return ret;
}

static struct gfx_font *_appendFont(char *font_name, ssize_t size)
{
    struct gfx_font *iterator = &font_list;

    for (; iterator->next; iterator = iterator->next)
        ;

    iterator->next = _loadFont(font_name, size);
    if (iterator->next == NULL) {
        return NULL;
    }

    return iterator->next;
}

int gfxFontLoadFont(char *font_name, ssize_t size)
{
    int ret = 0;

    pthread_mutex_lock(&list_lock);

    if (_appendFont(font_name, (size) ? size : DEFAULT_FONT_SIZE) ==
        NULL) {
        ret = -1;
    }

    pthread_mutex_unlock(&list_lock);

    return ret;
}

int gfxFontSelectFontFromName(char *font_name)
{
    pthread_mutex_lock(&list_lock);
    struct gfx_font *iterator = &font_list;

    for (; iterator; iterator = iterator->next)
        if (iterator->name)
            if (!strcmp(iterator->name, font_name)) {
                cur_default_font = iterator;
                pthread_mutex_unlock(&list_lock);
                return 0;
            }

    pthread_mutex_unlock(&list_lock);

    return -1;
}

int gfxFontSelectFontFromHandle(font_handle_t font_handle)
{
    pthread_mutex_lock(&list_lock);
    struct gfx_font *iterator = &font_list;

    for (; iterator; iterator = iterator->next)
        if (iterator == font_handle) {
            cur_default_font = iterator;
            pthread_mutex_unlock(&list_lock);
            return 0;
        }

    pthread_mutex_unlock(&list_lock);

    return -1;
}

int gfxFontSetSize(ssize_t font_size)
{
    if (cur_default_font == NULL) {
        goto err_;
    }

    pthread_mutex_lock(&list_lock);

    if (cur_default_font->size == font_size) {
        pthread_mutex_unlock(&list_lock);
        return 0;
    }

    if (!cur_default_font->font.ref_count) {
        TTF_CloseFont(cur_default_font->font.font);
        TTF_Font *new_font =
            TTF_OpenFont(cur_default_font->path, font_size);

        if (new_font == NULL) {
            goto err_;
        }

        cur_default_font->font.font = new_font;
        cur_default_font->size = font_size;
    }
    else {
        cur_default_font->font.pending_free = 1;
        cur_default_font =
            _appendFont(cur_default_font->name, font_size);
        if (!cur_default_font) {
            goto err_;
        }
    }

    pthread_mutex_unlock(&list_lock);

    return 0;
err_:
    pthread_mutex_unlock(&list_lock);
    return -1;
}
