/**
 * @file gfx_draw.c
 * @author Alex Hoffman
 * @date 27 August 2019
 * @brief A SDL2 based library to implement work queue based drawing of graphical
 * elements. Allows for drawing using SDL2 from multiple threads.
 *
 * @verbatim
   ----------------------------------------------------------------------
    Copyright (C) Alexander Hoffman, 2019
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
#include <linux/limits.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL2_gfxPrimitives.h>
#include <SDL2/SDL_image.h>

#include <pthread.h>

#include "gfx_draw.h"
#include "gfx_font.h"
#include "gfx_utils.h"
#include "gfx_print.h"

#define ONE_BYTE 8
#define TWO_BYTES 16
#define THREE_BYTES 24
#define MAX_8_BIT 255
#define ALPHA_SOLID MAX_8_BIT
#define FIRST_BYTE 0x000000ff
#define SECOND_BYTE 0x0000ff00
#define THIRD_BYTE 0x00ff0000
#define FOURTH_BYTE 0xff000000
#define RED_PORTION(COLOUR) (COLOUR & 0xFF0000) >> TWO_BYTES
#define GREEN_PORTION(COLOUR) (COLOUR & 0x00FF00) >> ONE_BYTE
#define BLUE_PORTION(COLOUR) (COLOUR & 0x0000FF)
#define ZERO_ALPHA 0

typedef enum {
    DRAW_NONE = 0,
    DRAW_CLEAR,
    DRAW_ARC,
    DRAW_ELLIPSE,
    DRAW_TEXT,
    DRAW_RECT,
    DRAW_FILLED_RECT,
    DRAW_CIRCLE,
    DRAW_LINE,
    DRAW_POLY,
    DRAW_TRIANGLE,
    DRAW_IMAGE,
    DRAW_LOADED_IMAGE,
    DRAW_LOADED_IMAGE_CROP,
    DRAW_SCALED_IMAGE,
    DRAW_ARROW,
} draw_job_type_t;

typedef struct loaded_image {
    char *filename;
    FILE *file;
    SDL_Texture *tex;
    SDL_RWops *ops;
    SDL_Surface *surf;
    int w;
    int h;
    float scale;
    //TODO make this atomic
    unsigned int ref_count;
    unsigned char pending_free;

    struct loaded_image *next;
} loaded_image_t;

typedef struct loaded_image_crop {
    loaded_image_t *image;
    int x;
    int y;
    int c_x;
    int c_y;
    int c_w;
    int c_h;
} loaded_image_crop_t;

typedef struct spritesheet_sequence {
    char *name;
    unsigned start_row;
    unsigned start_col;
    enum sprite_sequence_direction direction;
    unsigned frames;
    struct spritesheet_sequence *next;
} spritesheet_sequence_t;

typedef struct spritesheet {
    loaded_image_t *image;
    unsigned x;
    unsigned y;
    unsigned width;
    unsigned height;
    unsigned sprite_width;
    unsigned sprite_height;
    unsigned sprite_cols;
    unsigned sprite_rows;
    unsigned padding_x;
    unsigned padding_y;
} spritesheet_t;

typedef struct animated_image {
    spritesheet_t *spritesheet;
    spritesheet_sequence_t *sequences;
} animated_image_t;

typedef struct animated_sequence_instance {
    unsigned frame_period_ms;
    signed current_frame;
    unsigned prev_frame_timestamp;
    unsigned cur_frame_timestamp;
    animated_image_t *image;
    spritesheet_sequence_t *sequence;
} animated_sequence_instance_t;

typedef struct clear_data {
    unsigned int colour;
} clear_data_t;

typedef struct arc_data {
    signed short x;
    signed short y;
    signed short radius;
    signed short start;
    signed short end;
    unsigned int colour;
} arc_data_t;

typedef struct ellipse_data {
    signed short x;
    signed short y;
    signed short rx;
    signed short ry;
    unsigned int colour;
} ellipse_data_t;

typedef struct rect_data {
    signed short x;
    signed short y;
    signed short w;
    signed short h;
    unsigned int colour;
} rect_data_t;

typedef struct circle_data {
    signed short x;
    signed short y;
    signed short radius;
    unsigned int colour;
} circle_data_t;

typedef struct line_data {
    signed short x1;
    signed short y1;
    signed short x2;
    signed short y2;
    unsigned char thickness;
    unsigned int colour;
} line_data_t;

typedef struct poly_data {
    coord_t *points;
    unsigned int n;
    unsigned int colour;
} poly_data_t;

typedef struct triangle_data {
    coord_t *points;
    unsigned int colour;
} triangle_data_t;

typedef struct image_data {
    char *filename;
    SDL_Texture *tex;
    signed short x;
    signed short y;
} image_data_t;

typedef struct loaded_image_data {
    loaded_image_t *img;
    signed short x;
    signed short y;
} loaded_image_data_t;

typedef struct scaled_image_data {
    image_data_t image;
    float scale;
} scaled_image_data_t;

typedef struct text_data {
    char *str;
    signed short x;
    signed short y;
    unsigned int colour;
    TTF_Font *font;
} text_data_t;

typedef struct arrow_data {
    signed short x1;
    signed short y1;
    signed short x2;
    signed short y2;
    signed short head_length;
    unsigned char thickness;
    unsigned int colour;
} arrow_data_t;

union data_u {
    clear_data_t clear;
    arc_data_t arc;
    ellipse_data_t ellipse;
    rect_data_t rect;
    circle_data_t circle;
    line_data_t line;
    poly_data_t poly;
    triangle_data_t triangle;
    image_data_t image;
    loaded_image_data_t loaded_image;
    loaded_image_crop_t loaded_image_crop;
    scaled_image_data_t scaled_image;
    text_data_t text;
    arrow_data_t arrow;
};

typedef struct draw_job {
    draw_job_type_t type;
    union data_u *data;

    struct draw_job *next;
} draw_job_t;

pthread_mutex_t job_list_lock = PTHREAD_MUTEX_INITIALIZER;
draw_job_t job_list_head = { 0 };

struct global_offsets {
    int x;
    int y;
    pthread_mutex_t lock;
};

struct global_offsets global_offset = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

pthread_mutex_t loaded_images_lock = PTHREAD_MUTEX_INITIALIZER;
loaded_image_t loaded_images_list = { 0 };

const int screen_height = SCREEN_HEIGHT;
const int screen_width = SCREEN_WIDTH;

SDL_Window *window = NULL;
SDL_Renderer *renderer = NULL;
SDL_GLContext context = NULL;

char *error_message = NULL;

static uint32_t swapBytes(unsigned int x)
{
    return ((x & FIRST_BYTE) << THREE_BYTES) +
           ((x & SECOND_BYTE) << ONE_BYTE) +
           ((x & THIRD_BYTE) >> ONE_BYTE) +
           ((x & FOURTH_BYTE) >> THREE_BYTES);
}

void _setErrorMessage(char *msg)
{
    if (error_message) {
        free(error_message);
    }

    error_message = calloc(strlen(msg) + 1, sizeof(char));
    if (error_message == NULL) {
        return;
    }
    strcpy(error_message, msg);
}

#define PRINT_SDL_ERROR(msg, ...)                                              \
    PRINT_ERROR("[SDL Error] %s\n" #msg, (char *)SDL_GetError(),           \
                ##__VA_ARGS__)

static draw_job_t *_pushDrawJob(void)
{
    draw_job_t *iterator;
    draw_job_t *job = calloc(1, sizeof(draw_job_t));
    if (job == NULL) {
        return NULL;
    }

    pthread_mutex_lock(&job_list_lock);

    for (iterator = &job_list_head; iterator->next;
         iterator = iterator->next)
        ;

    iterator->next = job;

    pthread_mutex_unlock(&job_list_lock);

    return job;
}

static int _waitingDrawJobs(void)
{
    int ret = 1;

    pthread_mutex_lock(&job_list_lock);

    if (job_list_head.next == NULL) {
        ret = 0;
    }

    pthread_mutex_unlock(&job_list_lock);

    return ret;
}

static draw_job_t *popDrawJob(void)
{
    draw_job_t *ret = job_list_head.next;

    if (ret) {
        pthread_mutex_lock(&job_list_lock);

        if (ret->next) {
            job_list_head.next = ret->next;
        }
        else {
            job_list_head.next = NULL;
        }

        pthread_mutex_unlock(&job_list_lock);
    }

    return ret;
}

static int _clearDisplay(unsigned int colour)
{
    SDL_SetRenderDrawColor(renderer, (colour >> 16) & 0xFF,
                           (colour >> 8) & 0xFF, colour & 0xFF,
                           ALPHA_SOLID);
    SDL_RenderClear(renderer);

    return 0;
}

static int _drawRectangle(signed short x, signed short y, signed short w,
                          signed short h, unsigned int colour)
{
    rectangleColor(renderer, x + w, y, x, y + h,
                   swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static int _drawFilledRectangle(signed short x, signed short y, signed short w,
                                signed short h, unsigned int colour)
{
    boxColor(renderer, x + w, y, x, y + h,
             swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static int _drawArc(signed short x, signed short y, signed short radius,
                    signed short start, signed short end, unsigned int colour)
{
    arcColor(renderer, x, y, radius, start, end,
             swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static int _drawEllipse(signed short x, signed short y, signed short rx,
                        signed short ry, unsigned int colour)
{
    ellipseColor(renderer, x, y, rx, ry,
                 swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static int _drawCircle(signed short x, signed short y, signed short radius,
                       unsigned int colour)
{
    filledCircleColor(renderer, x, y, radius,
                      swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static int _drawLine(signed short x1, signed short y1, signed short x2,
                     signed short y2, unsigned char thickness,
                     unsigned int colour)
{
    thickLineColor(renderer, x1, y1, x2, y2, thickness,
                   swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static int _drawPoly(coord_t *points, unsigned int n, int x_offset,
                     int y_offset, signed short colour)
{
    signed short *x_coords = calloc(1, sizeof(signed short) * n);
    signed short *y_coords = calloc(1, sizeof(signed short) * n);
    unsigned int i;

    for (i = 0; i < n; i++) {
        x_coords[i] = points[i].x + x_offset;
        y_coords[i] = points[i].y + y_offset;
    }

    polygonColor(renderer, x_coords, y_coords, n,
                 swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    free(x_coords);
    free(y_coords);

    return 0;
}

static int _drawTriangle(coord_t *points, int x_offset, int y_offset,
                         unsigned int colour)
{
    filledTrigonColor(renderer, points[0].x + x_offset,
                      points[0].y + y_offset, points[1].x + x_offset,
                      points[1].y + y_offset, points[2].x + x_offset,
                      points[2].y + y_offset,
                      swapBytes((colour << ONE_BYTE) | ALPHA_SOLID));

    return 0;
}

static SDL_Texture *_loadImage(char *filename, SDL_Renderer *ren)
{
    SDL_Texture *tex =
        IMG_LoadTexture(ren, gfxUtilFindResourcePath(filename));

    return tex;
}

static int _renderCroppedImage(SDL_Texture *tex, SDL_Renderer *ren,
                               signed short x, signed short y, signed short c_x,
                               signed short c_y, int w, int h)
{
    SDL_Rect src, dst;
    src.x = c_x;
    src.y = c_y;
    src.w = w;
    src.h = h;
    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
    return SDL_RenderCopy(ren, tex, &src, &dst);
}

static int _renderScaledImage(SDL_Texture *tex, SDL_Renderer *ren,
                              signed short x, signed short y, int w, int h)
{
    SDL_Rect dst;
    dst.w = w;
    dst.h = h;
    dst.x = x;
    dst.y = y;
    return SDL_RenderCopy(ren, tex, NULL, &dst);
}

static int _getImageSize(char *filename, int *w, int *h)
{
    SDL_Texture *tex = _loadImage(filename, renderer);
    if (tex == NULL) {
        return -1;
    }
    SDL_QueryTexture(tex, NULL, NULL, w, h);
    SDL_DestroyTexture(tex);

    return 0;
}

gfx_animation_handle_t
gfxDrawAnimationCreate(gfx_spritesheet_handle_t spritesheet)
{
    if (spritesheet == NULL) {
        PRINT_ERROR("Creating animation requires a valid spritesheet");
        goto err;
    }

    animated_image_t *ret = calloc(1, sizeof(animated_image_t));

    if (ret == NULL) {
        PRINT_ERROR("Allocating animation failed");
        goto err;
    }

    ret->spritesheet = spritesheet;

    return (gfx_animation_handle_t)ret;

err:
    return NULL;
}

int gfxDrawAnimationAddSequence(
    gfx_animation_handle_t animation, char *name, unsigned start_row,
    unsigned start_col,
    enum sprite_sequence_direction sprite_step_direction, unsigned frames)
{
    if (animation == NULL) {
        PRINT_ERROR("Animation handle is not valid");
        goto err;
    }

    if (name == NULL) {
        PRINT_ERROR("Sequence requires a valid name");
        goto err;
    }

    animated_image_t *anim = (animated_image_t *)animation;

    spritesheet_sequence_t *seq = calloc(1, sizeof(spritesheet_sequence_t));
    if (seq == NULL) {
        PRINT_ERROR("Could not allocate animation sequence");
        goto err;
    }

    seq->name = strdup(name);
    if (seq->name == NULL) {
        PRINT_ERROR("Could not allocate sequence name");
        goto err_name;
    }

    seq->start_row = start_row;
    seq->start_col = start_col;
    seq->direction = sprite_step_direction;
    seq->frames = frames;

    if (anim->sequences == NULL) {
        anim->sequences = seq;
    }
    else {
        spritesheet_sequence_t *iterator = anim->sequences;

        for (; iterator->next; iterator = iterator->next)
            ;
        iterator->next = seq;
    }

    return 0;

err_name:
    free(seq);
err:
    return -1;
}

gfx_sequence_handle_t
gfxDrawAnimationSequenceInstantiate(gfx_animation_handle_t animation,
                                    char *sequence_name,
                                    unsigned frame_period_ms)
{
    if (animation == NULL) {
        PRINT_ERROR(
            "Animation provided for sequence instantiation was invalid");
        goto err;
    }

    if (sequence_name == NULL) {
        PRINT_ERROR("Sequence name is invalid");
        goto err;
    }

    if (frame_period_ms == 0) {
        PRINT_ERROR("Sequence frame period cannot be zero");
        goto err;
    }

    animated_sequence_instance_t *ret =
        calloc(1, sizeof(animated_sequence_instance_t));
    if (ret == NULL) {
        PRINT_ERROR("Could not create sequence '%s' instance",
                    sequence_name);
        goto err;
    }

    ret->image = (animated_image_t *)animation;

    spritesheet_sequence_t *iterator;

    for (iterator = ret->image->sequences; iterator;
         iterator = iterator->next)
        if (!strcmp(iterator->name, sequence_name)) {
            ret->sequence = iterator;
            break;
        }

    if (ret->sequence == NULL) {
        PRINT_ERROR("Could not find sequence '%s'", sequence_name);
        goto err_sequence;
    }

    ret->frame_period_ms = frame_period_ms;

    return ret;

err_sequence:
    free(ret);
err:
    return NULL;
}

static int freeLoadedImage(loaded_image_t **img)
{
    int ret = -1;

    pthread_mutex_lock(&loaded_images_lock);
    loaded_image_t *iterator = &loaded_images_list;
    loaded_image_t *delete;

    for (; iterator->next; iterator = iterator->next)
        if (iterator->next == *img) {
            break;
        }

    if (iterator->next == *img) {
        delete = iterator->next;

        if (!iterator->next->next) {
            iterator->next = NULL;
        }
        else {
            iterator->next = delete->next;
        }

        SDL_FreeSurface(delete->surf);
        SDL_RWclose(delete->ops);
        SDL_DestroyTexture(delete->tex);
        free(delete->filename);
        free(delete);
        *img = (loaded_image_t *)NULL;

        ret = 0;
    }
    pthread_mutex_unlock(&loaded_images_lock);

    return ret;
}

static void vPutLoadedImage(gfx_image_handle_t img)
{
    loaded_image_t *loaded_img = (loaded_image_t *)img;

    loaded_img->ref_count--;

    if (loaded_img->pending_free && !loaded_img->ref_count) {
        freeLoadedImage((loaded_image_t **)&img);
    }
}

int xDrawLoadedImageCropped(loaded_image_t *img, SDL_Renderer *ren,
                            signed short x, signed short y, signed short c_x,
                            signed short c_y, signed short c_w,
                            signed short c_h)
{
    return _renderCroppedImage(img->tex, ren, x, y, c_x, c_y, c_w, c_h);
}

int xDrawLoadedImage(loaded_image_t *img, SDL_Renderer *ren, signed short x,
                     signed short y)
{
    return _renderScaledImage(img->tex, ren, x, y, img->w * img->scale,
                              img->h * img->scale);
}

static int _drawScaledImage(SDL_Texture *tex, SDL_Renderer *ren, signed short x,
                            signed short y, float scale)
{
    int w, h;
    SDL_QueryTexture(tex, NULL, NULL, &w, &h);
    if (!w || !h) {
        return -1;
    }

    if (_renderScaledImage(tex, ren, x, y, w * scale, h * scale)) {
        return -1;
    }

    SDL_DestroyTexture(tex);

    return 0;
}

static int _drawImage(SDL_Texture *tex, SDL_Renderer *ren, signed short x,
                      signed short y)
{
    return _drawScaledImage(tex, ren, x, y, 1);
}

static int _drawText(char *string, signed short x, signed short y,
                     unsigned int colour, TTF_Font *font)
{
    SDL_Color color = { RED_PORTION(colour), GREEN_PORTION(colour),
                        BLUE_PORTION(colour), ZERO_ALPHA
                      };
    SDL_Surface *surface = TTF_RenderText_Solid(font, string, color);
    gfxFontPutFont(font);
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_Rect dst = { 0 };
    SDL_QueryTexture(texture, NULL, NULL, &dst.w, &dst.h);
    dst.x = x;
    dst.y = y;
    SDL_RenderCopy(renderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);

    return 0;
}

static int _getTextSize(char *string, int *width, int *height)
{
    SDL_Color color = { 0 };
    TTF_Font *font = gfxFontGetCurFont();
    SDL_Surface *surface = TTF_RenderText_Solid(font, string, color);
    gfxFontPutFont(font);
    if (surface == NULL) {
        goto err_surface;
    }

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture == NULL) {
        goto err_texture;
    }

    if (SDL_QueryTexture(texture, NULL, NULL, width, height)) {
        goto err_query;
    }

    SDL_DestroyTexture(texture);
    SDL_FreeSurface(surface);

    return 0;

err_query:
    SDL_DestroyTexture(texture);
err_texture:
    SDL_FreeSurface(surface);
err_surface:
    return -1;
}

static int _drawArrow(signed short x1, signed short y1, signed short x2,
                      signed short y2, signed short head_length,
                      unsigned char thickness, unsigned int colour)
{
    // Line vector
    unsigned short dx = x2 - x1;
    unsigned short dy = y2 - y1;

    // Normalize
    float length = sqrt(dx * dx + dy * dy);
    signed short unit_dx = (signed short)(dx / length);
    signed short unit_dy = (signed short)(dy / length);

    signed short head_x1 =
        roundf(x2 - unit_dx * head_length - unit_dy * head_length);
    signed short head_y1 =
        roundf(y2 - unit_dy * head_length + unit_dx * head_length);

    signed short head_x2 =
        roundf(x2 - unit_dx * head_length + unit_dy * head_length);
    signed short head_y2 =
        roundf(y2 - unit_dy * head_length - unit_dx * head_length);

    if (thickLineColor(renderer, x1, y1, x2, y2, thickness,
                       swapBytes((colour << ONE_BYTE) | ALPHA_SOLID))) {
        return -1;
    }
    if (thickLineColor(renderer, head_x1, head_y1, x2, y2, thickness,
                       swapBytes((colour << ONE_BYTE) | ALPHA_SOLID))) {
        return -1;
    }
    if (thickLineColor(renderer, head_x2, head_y2, x2, y2, thickness,
                       swapBytes((colour << ONE_BYTE) | ALPHA_SOLID))) {
        return -1;
    }

    return 0;
}

static int vHandleDrawJob(draw_job_t *job)
{
    int ret = 0;
    static int x_offset = 0;
    static int y_offset = 0;
    ;
    if (!pthread_mutex_unlock(&global_offset.lock)) {
        x_offset = global_offset.x;
        y_offset = global_offset.y;
    }
    else {
        return -1;
    }

    if (job == NULL) {
        return -1;
    }

    if (job->data == NULL) {
        return -1;
    }

    switch (job->type) {
        case DRAW_CLEAR:
            ret = _clearDisplay(job->data->clear.colour);
            break;
        case DRAW_ARC:
            ret = _drawArc(job->data->arc.x + x_offset,
                           job->data->arc.y + y_offset,
                           job->data->arc.radius, job->data->arc.start,
                           job->data->arc.end, job->data->arc.colour);
            break;
        case DRAW_ELLIPSE:
            ret = _drawEllipse(job->data->ellipse.x + x_offset,
                               job->data->ellipse.y, job->data->ellipse.rx,
                               job->data->ellipse.ry,
                               job->data->ellipse.colour);
            break;
        case DRAW_TEXT:
            ret = _drawText(job->data->text.str,
                            job->data->text.x + x_offset,
                            job->data->text.y + y_offset,
                            job->data->text.colour, job->data->text.font);
            free(job->data->text.str);
            break;
        case DRAW_RECT:
            ret = _drawRectangle(job->data->rect.x + x_offset,
                                 job->data->rect.y + y_offset,
                                 job->data->rect.w, job->data->rect.h,
                                 job->data->rect.colour);
            break;
        case DRAW_FILLED_RECT:
            ret = _drawFilledRectangle(job->data->rect.x + x_offset,
                                       job->data->rect.y + y_offset,
                                       job->data->rect.w, job->data->rect.h,
                                       job->data->rect.colour);
            break;
        case DRAW_CIRCLE:
            ret = _drawCircle(job->data->circle.x + x_offset,
                              job->data->circle.y + y_offset,
                              job->data->circle.radius,
                              job->data->circle.colour);
            break;
        case DRAW_LINE:
            ret = _drawLine(job->data->line.x1 + x_offset,
                            job->data->line.y1 + y_offset,
                            job->data->line.x2 + x_offset,
                            job->data->line.y2 + y_offset,
                            job->data->line.thickness,
                            job->data->line.colour);
            break;
        case DRAW_POLY:
            ret = _drawPoly(job->data->poly.points, job->data->poly.n,
                            x_offset, y_offset, job->data->poly.colour);
            break;
        case DRAW_TRIANGLE:
            ret = _drawTriangle(job->data->triangle.points, x_offset,
                                y_offset, job->data->triangle.colour);
            break;
        case DRAW_IMAGE:
            job->data->image.tex =
                _loadImage(job->data->image.filename, renderer);
            ret = _drawImage(job->data->image.tex, renderer,
                             job->data->image.x + x_offset,
                             job->data->image.y + y_offset);
            break;
        case DRAW_LOADED_IMAGE:
            ret = xDrawLoadedImage(job->data->loaded_image.img, renderer,
                                   job->data->loaded_image.x + x_offset,
                                   job->data->loaded_image.y + y_offset);
            vPutLoadedImage(job->data->loaded_image.img);
            break;
        case DRAW_LOADED_IMAGE_CROP:
            ret = xDrawLoadedImageCropped(
                      job->data->loaded_image_crop.image, renderer,
                      job->data->loaded_image_crop.x + x_offset,
                      job->data->loaded_image_crop.y + y_offset,
                      job->data->loaded_image_crop.c_x,
                      job->data->loaded_image_crop.c_y,
                      job->data->loaded_image_crop.c_w,
                      job->data->loaded_image_crop.c_h);
            vPutLoadedImage(job->data->loaded_image_crop.image);
            break;
        case DRAW_SCALED_IMAGE:
            job->data->scaled_image.image.tex = _loadImage(
                                                    job->data->scaled_image.image.filename, renderer);
            ret = _drawScaledImage(
                      job->data->scaled_image.image.tex, renderer,
                      job->data->scaled_image.image.x + x_offset,
                      job->data->scaled_image.image.y + y_offset,
                      job->data->scaled_image.scale);
            free(job->data->scaled_image.image.filename);
            break;
        case DRAW_ARROW:
            ret = _drawArrow(job->data->arrow.x1 + x_offset,
                             job->data->arrow.y1 + y_offset,
                             job->data->arrow.x2 + x_offset,
                             job->data->arrow.y2 + y_offset,
                             job->data->arrow.head_length,
                             job->data->arrow.thickness,
                             job->data->arrow.colour);
        default:
            break;
    }
    free(job->data);

    return ret;
}

#define INIT_JOB(JOB, TYPE)                                                    \
    draw_job_t *JOB = _pushDrawJob();                                      \
    if (!JOB)                                                              \
        return -1;                                                     \
    union data_u *data = calloc(1, sizeof(union data_u));                  \
    if (data == NULL)                                                      \
        logCriticalError("job->data alloc");                           \
    JOB->data = data;                                                      \
    JOB->type = TYPE;

static void logCriticalError(char *msg)
{
    printf("[ERROR] %s\n", msg);
    exit(-1);
}

#define NS_IN_SECOND 1000000000.0
#define MS_IN_SECOND 1000.0
#define NS_IN_MS 1000000.0

#if (configFPS_LIMIT == 1)
static float timespecDiffMilli(struct timespec *start, struct timespec *stop)
{
    if ((stop->tv_nsec - start->tv_nsec) < 0)
        return (stop->tv_sec - start->tv_sec - 1) * MS_IN_SECOND +
               (stop->tv_nsec - start->tv_nsec + NS_IN_SECOND) /
               NS_IN_MS;

    return (stop->tv_sec - start->tv_sec) * MS_IN_SECOND +
           (stop->tv_nsec - start->tv_nsec) / NS_IN_MS;
}

#ifdef configFPS_LIMIT_RATE
#define FRAMELIMIT configFPS_LIMIT_RATE
#else
#define FRAMELIMIT 50
#endif //configFPS_LIMIT_RATE
#define FRAMELIMIT_PERIOD 1000.0 / FRAMELIMIT
#endif //configFPS_LIMIT

int gfxDrawUpdateScreen(void)
{
    gfxDrawBindThread(); // Setup Rendering handle with correct GL context

    if (!gfxUtilIsCurGLThread()) {
        PRINT_ERROR(
            "Updating screen from thread that does not hold GL context");
        goto err;
    }

#if (configFPS_LIMIT == 1)
    static struct timespec last_time = { 0 }, cur_time = { 0 };

    if (clock_gettime(CLOCK_MONOTONIC, &cur_time)) {
        PRINT_ERROR("Failed to get monotonic clock");
        goto err;
    }

    if (timespecDiffMilli(&last_time, &cur_time) <
        (float)FRAMELIMIT_PERIOD) {
        goto no_jobs;
    }

    memcpy(&last_time, &cur_time, sizeof(struct timespec));
#endif //configFPS_LIMIT

    if (!_waitingDrawJobs()) {
        goto no_jobs;
    }

    draw_job_t *tmp_job;

    while ((tmp_job = popDrawJob()) != NULL) {
        if (!tmp_job->data) {
            goto err;
        }
        if (vHandleDrawJob(tmp_job) == -1) {
            goto draw_error;
        }
        free(tmp_job);
    }

    pthread_mutex_unlock(&job_list_lock);

    SDL_RenderPresent(renderer);

    return 0;

draw_error:
    free(tmp_job);
err:
    pthread_mutex_unlock(&job_list_lock);
    return -1;
no_jobs:
    pthread_mutex_unlock(&job_list_lock);
    return 0;
}

char *gfxGetErrorMessage(void)
{
    return error_message;
}

int gfxDrawInit(char *path) // Should be called from the Thread running main()
{
    /* Relevant for Docker-based toolchain */
#ifdef DOCKER
#ifndef HOST_OS
#warning "HOST_OS undefined! Assuming 'linux'..."
#elif HOST_OS != linux
    setenv("LIBGL_ALWAYS_INDIRECT", "1",
           1); // speed up drawings a little bit
    setenv("SDL_VIDEO_X11_VISUALID", "",
           1); // required on windows and macos
#elif HOST_OS == linux
    // nothing
#else
#error "Unexpected value of HOST_OS!"
#endif /* HOST_OS */
#endif /* DOCKER */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        PRINT_SDL_ERROR("SDL_Init failed");
        goto err_sdl;
    }
    if (TTF_Init()) {
        PRINT_ERROR("TTF_Init failed");
        goto err_ttf;
    }

    if (gfxFontInit(path)) {
        PRINT_ERROR("GFX Font init failed");
        goto err_gfx_font;
    }

    window = SDL_CreateWindow(WINDOW_TITLE, SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, screen_width,
                              screen_height, SDL_WINDOW_OPENGL);

    if (window == NULL) {
        PRINT_SDL_ERROR("Failed to create %d x %d window '%s'",
                        screen_width, screen_height, WINDOW_TITLE);
        goto err_window;
    }

    context = SDL_GL_CreateContext(window);

    if (context == NULL) {
        PRINT_SDL_ERROR("Failed to create context");
        goto err_create_context;
    }

    if (SDL_GL_MakeCurrent(window, context) < 0) {
        PRINT_SDL_ERROR("Claiming current context failed");
        goto err_make_current;
    }

    if (SDL_GL_MakeCurrent(window, NULL) < 0) {
        PRINT_SDL_ERROR("Releasing current context failed");
        goto err_make_current;
    }

    gfxDrawBindThread();

    atexit(SDL_Quit);

    return 0;

err_make_current:
    SDL_GL_DeleteContext(context);
err_create_context:
    SDL_DestroyWindow(window);
err_window:
    gfxFontExit();
err_gfx_font:
    TTF_Quit();
err_ttf:
    SDL_Quit();
err_sdl:
    return -1;
}

int gfxDrawBindThread(void) // Should be called from the Drawing Thread
{
    if (!gfxUtilIsCurGLThread() || !renderer) {
        if (SDL_GL_MakeCurrent(window, context) < 0) {
            PRINT_SDL_ERROR("Releasing current context failed");
            goto err_make_current;
        }

        if (renderer) {
            SDL_DestroyRenderer(renderer);
            renderer = NULL;
        }

        renderer =
            SDL_CreateRenderer(window, -1,
                               SDL_RENDERER_ACCELERATED |
                               SDL_RENDERER_TARGETTEXTURE |
                               SDL_RENDERER_PRESENTVSYNC);

        if (renderer == NULL) {
            PRINT_SDL_ERROR("Failed to create renderer");
            goto err_renderer;
        }

        SDL_SetRenderDrawColor(renderer, MAX_8_BIT, MAX_8_BIT,
                               MAX_8_BIT, ALPHA_SOLID);

        SDL_RenderClear(renderer);

        pthread_mutex_lock(&loaded_images_lock);
        loaded_image_t *iterator = &loaded_images_list;

        for (; iterator; iterator = iterator->next)
            if (iterator->tex) {
                SDL_DestroyTexture(iterator->tex);
                iterator->tex = SDL_CreateTextureFromSurface(
                                    renderer, iterator->surf);
            }

        pthread_mutex_unlock(&loaded_images_lock);

        gfxUtilSetGLThread();
    }

    return 0;

err_renderer:
    SDL_DestroyWindow(window);
err_make_current:
    SDL_GL_DeleteContext(context);
    TTF_Quit();
    SDL_Quit();
    return -1;
}

void gfxDrawExit(void)
{
    if (window) {
        SDL_DestroyWindow(window);
    }

    if (renderer) {
        SDL_DestroyRenderer(renderer);
    }

    TTF_Quit();
    SDL_Quit();

    exit(EXIT_SUCCESS);
}

int gfxDrawText(char *str, signed short x, signed short y, unsigned int colour)
{
    if (strcmp(str, "") == 0) {
        return -1;
    }

    INIT_JOB(job, DRAW_TEXT);

    job->data->text.str = (char *)calloc(strlen(str) + 1, sizeof(char));

    if (job->data->text.str == NULL) {
        printf("Error allocating buffer in gfxDrawText\n");
        return -1;
    }

    strcpy(job->data->text.str, str);
    job->data->text.font = gfxFontGetCurFont();
    job->data->text.x = x;
    job->data->text.y = y;
    job->data->text.colour = colour;

    return 0;
}

int gfxGetTextSize(char *str, int *width, int *height)
{
    if (str == NULL) {
        return -1;
    }
    return _getTextSize(str, width, height);
}

int gfxDrawCenteredText(char *str, signed short x, signed short y,
                        unsigned int colour)
{
    int width, height;
    if (gfxGetTextSize(str, &width, &height)) {
        return -1;
    }

    return gfxDrawText(str, x - width / 2, y - height / 2, colour);
}

int gfxDrawEllipse(signed short x, signed short y, signed short rx,
                   signed short ry, unsigned int colour)
{
    INIT_JOB(job, DRAW_ELLIPSE);

    job->data->ellipse.x = x;
    job->data->ellipse.y = y;
    job->data->ellipse.rx = rx;
    job->data->ellipse.ry = ry;
    job->data->ellipse.colour = colour;

    return 0;
}

int gfxDrawArc(signed short x, signed short y, signed short radius,
               signed short start, signed short end, unsigned int colour)
{
    INIT_JOB(job, DRAW_ARC);

    job->data->arc.x = x;
    job->data->arc.y = y;
    job->data->arc.radius = radius;
    job->data->arc.start = start;
    job->data->arc.end = end;
    job->data->arc.colour = colour;

    return 0;
}

int gfxDrawFilledBox(signed short x, signed short y, signed short w,
                     signed short h, unsigned int colour)
{
    INIT_JOB(job, DRAW_FILLED_RECT);

    job->data->rect.x = x;
    job->data->rect.y = y;
    job->data->rect.w = w;
    job->data->rect.h = h;
    job->data->rect.colour = colour;

    return 0;
}

int gfxDrawBox(signed short x, signed short y, signed short w, signed short h,
               unsigned int colour)
{
    INIT_JOB(job, DRAW_RECT);

    job->data->rect.x = x;
    job->data->rect.y = y;
    job->data->rect.w = w;
    job->data->rect.h = h;
    job->data->rect.colour = colour;

    return 0;
}

void gfxDrawDuplicateBuffer(void)
{
    SDL_Surface *screen_shot =
        SDL_CreateRGBSurface(0, SCREEN_WIDTH, SCREEN_HEIGHT, 32,
                             0x00ff0000, 0x0000ff00, 0x000000ff,
                             0xff000000);
    SDL_RenderReadPixels(renderer, NULL, 0, screen_shot->pixels,
                         screen_shot->pitch);
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, screen_shot);
    SDL_RenderClear(renderer);
    SDL_Rect dest = { .w = SCREEN_WIDTH, .h = SCREEN_HEIGHT };
    SDL_RenderCopy(renderer, tex, NULL, &dest);
    SDL_RenderPresent(renderer);
}

int gfxDrawClear(unsigned int colour)
{
    INIT_JOB(job, DRAW_CLEAR);

    job->data->clear.colour = colour;

    return 0;
}

int gfxDrawCircle(signed short x, signed short y, signed short radius,
                  unsigned int colour)
{
    INIT_JOB(job, DRAW_CIRCLE);

    job->data->circle.x = x;
    job->data->circle.y = y;
    job->data->circle.radius = radius;
    job->data->circle.colour = colour;

    return 0;
}

int gfxDrawLine(signed short x1, signed short y1, signed short x2,
                signed short y2, unsigned char thickness, unsigned int colour)
{
    INIT_JOB(job, DRAW_LINE);

    job->data->line.x1 = x1;
    job->data->line.y1 = y1;
    job->data->line.x2 = x2;
    job->data->line.y2 = y2;
    job->data->line.thickness = thickness;
    job->data->line.colour = colour;

    return 0;
}

int gfxDrawPoly(coord_t *points, int n, unsigned int colour)
{
    INIT_JOB(job, DRAW_POLY);

    coord_t *points_cpy = (coord_t *)calloc(n, sizeof(coord_t));
    if (!points_cpy) {
        return -1;
    }

    memcpy(points_cpy, points, sizeof(coord_t) * n);

    job->data->poly.points = points_cpy;
    job->data->poly.n = n;
    job->data->poly.colour = colour;

    return 0;
}

int gfxDrawTriangle(coord_t *points, unsigned int colour)
{
    INIT_JOB(job, DRAW_TRIANGLE);

    coord_t *points_cpy = (coord_t *)calloc(3, sizeof(coord_t));
    if (!points_cpy) {
        return -1;
    }

    memcpy(points_cpy, points, sizeof(coord_t) * 3);

    job->data->triangle.points = points_cpy;
    job->data->triangle.colour = colour;

    return 0;
}

gfx_image_handle_t gfxDrawLoadScaledImage(char *filename, float scale)
{
    if (!renderer || !gfxUtilIsCurGLThread()) {
        gfxDrawBindThread();
        if (!renderer) {
            goto err_renderer;
        }
    }

    loaded_image_t *ret = calloc(1, sizeof(loaded_image_t));
    if (ret == NULL) {
        PRINT_ERROR("Failed to allocate loaded image");
        goto err_alloc;
    }

    ret->filename = strdup(filename);
    if (ret->filename == NULL) {
        PRINT_ERROR("Failed to duplicate filename");
        goto err_filename;
    }

    ret->file = gfxUtilFindResource(filename, "rb");
    if (ret->file == NULL) {
        PRINT_ERROR("Failed to open file '%s'", filename);
        goto err_file_open;
    }

    ret->ops = SDL_RWFromFP(ret->file, SDL_TRUE);
    if (ret->ops == NULL) {
        PRINT_SDL_ERROR("Failed open from FP");
        goto err_ops;
    }

    ret->surf = IMG_Load_RW(ret->ops, 0);
    if (ret->surf == NULL) {
        PRINT_SDL_ERROR("Failed to load image");
        goto err_surf;
    }

    ret->tex = SDL_CreateTextureFromSurface(renderer, ret->surf);
    if (ret->tex == NULL) {
        PRINT_SDL_ERROR("Failed to create texture from surface");
        goto err_tex;
    }

    SDL_QueryTexture(ret->tex, NULL, NULL, &ret->w, &ret->h);

    ret->scale = scale;

    pthread_mutex_lock(&loaded_images_lock);

    loaded_image_t *iterator = &loaded_images_list;
    for (; iterator->next; iterator = iterator->next)
        ;
    iterator->next = ret;

    pthread_mutex_unlock(&loaded_images_lock);

    return ret;

err_tex:
    SDL_FreeSurface(ret->surf);
err_surf:
    SDL_RWclose(ret->ops);
err_ops:
    fclose(ret->file);
err_file_open:
    free(ret->filename);
err_filename:
    free(ret);
err_alloc:
err_renderer:
    return NULL;
}

gfx_image_handle_t gfxDrawLoadImage(char *filename)
{
    return gfxDrawLoadScaledImage(filename, 1);
}

int gfxDrawFreeLoadedImage(gfx_image_handle_t *img)
{
    int ret = 0;
    loaded_image_t **loaded_img = (loaded_image_t **)img;

    if (!(*loaded_img)->ref_count) {
        ret = freeLoadedImage(loaded_img);
    }
    else {
        (*loaded_img)->pending_free = 1;
    }

    return ret;
}

int gfxDrawLoadedImage(gfx_image_handle_t img, signed short x, signed short y)
{
    if (img == NULL) {
        return -1;
    }

    INIT_JOB(job, DRAW_LOADED_IMAGE);

    ((loaded_image_t *)img)->ref_count++;
    job->data->loaded_image.img = img;
    job->data->loaded_image.x = x;
    job->data->loaded_image.y = y;

    return 0;
}

int gfxDrawSetLoadedImageScale(gfx_image_handle_t img, float scale)
{
    if (img == NULL) {
        return -1;
    }

    ((loaded_image_t *)img)->scale = scale;

    return 0;
}

float gfxDrawGetLoadedImageScale(gfx_image_handle_t img)
{
    if (img == NULL) {
        return -1;
    }

    return ((loaded_image_t *)img)->scale;
}

int gfxDrawGetLoadedImageWidth(gfx_image_handle_t img)
{
    if (img == NULL) {
        return -1;
    }

    return ((loaded_image_t *)img)->w * ((loaded_image_t *)img)->scale;
}

int gfxDrawGetLoadedImageHeight(gfx_image_handle_t img)
{
    if (img == NULL) {
        return -1;
    }

    return ((loaded_image_t *)img)->h * ((loaded_image_t *)img)->scale;
}

int gfxDrawGetLoadedImageSize(gfx_image_handle_t img, int *w, int *h)
{
    if (img == NULL) {
        return -1;
    }

    *w = gfxDrawGetLoadedImageWidth(img);
    *h = gfxDrawGetLoadedImageHeight(img);

    if (*w == -1 || *h == -1) {
        return -1;
    }

    return 0;
}

int __attribute_deprecated__ gfxDrawImage(char *filename, signed short x,
        signed short y)
{
    INIT_JOB(job, DRAW_IMAGE);

    char abs_path[PATH_MAX + 1];

    if (realpath(filename, (char *)abs_path) == NULL) {
        return -1;
    }

    job->data->image.filename = calloc(strlen(abs_path) + 1, sizeof(char));
    strcpy(job->data->image.filename, abs_path);
    job->data->image.x = x;
    job->data->image.y = y;

    return 0;
}

spritesheet_t *gfxDrawInitSpritesheet(gfx_image_handle_t img)
{
    if (img == NULL) {
        PRINT_ERROR("No spritesheet provided to load");
        return NULL;
    }

    spritesheet_t *ret = calloc(1, sizeof(spritesheet_t));

    if (ret == NULL) {
        PRINT_ERROR("Could not allocate spritesheet");
        return NULL;
    }

    ret->image = img;
    ret->width = ((loaded_image_t *)img)->w;
    ret->height = ((loaded_image_t *)img)->h;

    return ret;
}

void gfxDrawSpritesheetSetBoundingBox(spritesheet_t *spritesheet,
                                      unsigned bounding_x, unsigned bounding_y,
                                      unsigned bounding_w, unsigned bounding_h)
{
    spritesheet->x = bounding_x;
    spritesheet->y = bounding_y;
    spritesheet->width = bounding_w;
    spritesheet->height = bounding_h;
}

void gfxDrawSpritesheetSetPadding(spritesheet_t *spritesheet,
                                  unsigned padding_x, unsigned padding_y)
{
    spritesheet->padding_x = padding_x;
    spritesheet->padding_y = padding_y;
}

/**
 * @brief Sets the number of cols and rows and in turn the sprite
 * width and height. Should be called AFTER setting the padding
 * and bounding box
 *
 * @param spritesheet Sprite sheet to be set
 * @param cols Number of columns on the spritesheet
 * @param rows Number of rows on the spritesheet
 */
void gfxDrawSpritesheetSetDivisions(spritesheet_t *spritesheet, unsigned cols,
                                    unsigned rows)
{
    unsigned total_x_padding_pixels =
        spritesheet->padding_x * (cols - 1) * 2;
    unsigned total_y_padding_pixels =
        spritesheet->padding_y * (rows - 1) * 2;
    spritesheet->sprite_cols = cols;
    spritesheet->sprite_rows = rows;
    spritesheet->sprite_width =
        (spritesheet->width - total_x_padding_pixels) / cols;
    spritesheet->sprite_height =
        (spritesheet->height - total_y_padding_pixels) / rows;
}

gfx_spritesheet_handle_t gfxDrawLoadSpritesheetFromEntireImageUnpadded(
    gfx_image_handle_t img, unsigned sprite_cols, unsigned sprite_rows)
{
    spritesheet_t *ret = NULL;
    if (!(ret = gfxDrawInitSpritesheet(img))) {
        return NULL;
    }

    gfxDrawSpritesheetSetDivisions(ret, sprite_cols, sprite_rows);

    return (gfx_spritesheet_handle_t)ret;
}

gfx_spritesheet_handle_t gfxDrawLoadSpritesheetFromEntireImagePadded(
    gfx_image_handle_t img, unsigned sprite_cols, unsigned sprite_rows,
    unsigned sprite_padding_x, unsigned sprite_padding_y)
{
    spritesheet_t *ret = NULL;
    if (!(ret = gfxDrawInitSpritesheet(img))) {
        return NULL;
    }

    gfxDrawSpritesheetSetPadding(ret, sprite_padding_x, sprite_padding_y);
    gfxDrawSpritesheetSetDivisions(ret, sprite_cols, sprite_rows);

    return (gfx_spritesheet_handle_t)ret;
}

gfx_spritesheet_handle_t gfxDrawLoadSpritesheetFromEntireImagePaddedSpacing(
    gfx_image_handle_t img, unsigned sprite_cols, unsigned sprite_rows,
    unsigned sprite_spacing_x, unsigned sprite_spacing_y)
{
    spritesheet_t *ret = NULL;
    if (!(ret = gfxDrawInitSpritesheet(img))) {
        return NULL;
    }

    gfxDrawSpritesheetSetPadding(ret, sprite_spacing_x / 2,
                                 sprite_spacing_y / 2);
    gfxDrawSpritesheetSetDivisions(ret, sprite_cols, sprite_rows);

    return (gfx_spritesheet_handle_t)ret;
}

gfx_spritesheet_handle_t gfxDrawLoadSpritesheetFromPortionOfImageUnpadded(
    gfx_image_handle_t img, unsigned sprite_cols, unsigned sprite_rows,
    unsigned sprite_width, unsigned sprite_height,
    unsigned bounding_box_left_x_pixel, unsigned bounding_box_top_y_pixel)
{
    spritesheet_t *ret = NULL;
    if (!(ret = gfxDrawInitSpritesheet(img))) {
        return NULL;
    }

    gfxDrawSpritesheetSetBoundingBox(ret, bounding_box_left_x_pixel,
                                     bounding_box_top_y_pixel,
                                     sprite_width * sprite_cols,
                                     sprite_height * sprite_rows);
    gfxDrawSpritesheetSetDivisions(ret, sprite_cols, sprite_rows);

    return (gfx_spritesheet_handle_t)ret;
}

gfx_spritesheet_handle_t gfxDrawLoadSpritesheetFromPortionOfImagePadded(
    gfx_image_handle_t img, unsigned sprite_cols, unsigned sprite_rows,
    unsigned sprite_width, unsigned sprite_height,
    unsigned sprite_padding_x, unsigned sprite_padding_y,
    unsigned bounding_box_left_x_pixel, unsigned bounding_box_top_y_pixel)
{
    spritesheet_t *ret = NULL;
    if (!(ret = gfxDrawInitSpritesheet(img))) {
        return NULL;
    }

    // We do not have padding before first sprite and last sprite for both
    // width and height, thus sprite_rows/cols - 1 * (padding * 2) total
    // padding
    gfxDrawSpritesheetSetBoundingBox(
        ret, bounding_box_left_x_pixel, bounding_box_top_y_pixel,
        sprite_cols * sprite_width +
        (sprite_cols - 1) * sprite_padding_x * 2,
        sprite_rows * sprite_height +
        (sprite_rows - 1) * sprite_padding_y * 2);
    gfxDrawSpritesheetSetPadding(ret, sprite_padding_x, sprite_padding_y);
    gfxDrawSpritesheetSetDivisions(ret, sprite_cols, sprite_rows);

    return (gfx_spritesheet_handle_t)ret;
}

gfx_spritesheet_handle_t gfxDrawLoadSpritesheetFromPortionOfImagePaddedSpacing(
    gfx_image_handle_t img, unsigned sprite_cols, unsigned sprite_rows,
    unsigned sprite_width, unsigned sprite_height,
    unsigned sprite_spacing_x, unsigned sprite_spacing_y,
    unsigned bounding_box_left_x_pixel, unsigned bounding_box_top_y_pixel)
{
    spritesheet_t *ret = NULL;
    if (!(ret = gfxDrawInitSpritesheet(img))) {
        return NULL;
    }

    gfxDrawSpritesheetSetBoundingBox(
        ret, bounding_box_left_x_pixel, bounding_box_top_y_pixel,
        sprite_cols * sprite_width +
        (sprite_cols - 1) * sprite_spacing_x,
        sprite_rows * sprite_height +
        (sprite_rows - 1) * sprite_spacing_y);
    gfxDrawSpritesheetSetPadding(ret, sprite_spacing_x / 2,
                                 sprite_spacing_y / 2);
    gfxDrawSpritesheetSetDivisions(ret, sprite_cols, sprite_rows);

    return (gfx_spritesheet_handle_t)ret;
}

int gfxDrawSprite(gfx_spritesheet_handle_t spritesheet, char column, char row,
                  signed short x, signed short y)
{
    if (spritesheet == NULL) {
        PRINT_ERROR("No spritesheet given to draw from");
        goto err;
    }

    if (column < 0 ||
        column > ((spritesheet_t *)spritesheet)->sprite_cols) {
        PRINT_ERROR("Spritesheet column not valid");
        goto err;
    }

    if (row < 0 || row > ((spritesheet_t *)spritesheet)->sprite_rows) {
        PRINT_ERROR("Spritesheet row not valid");
        goto err;
    }

    INIT_JOB(job, DRAW_LOADED_IMAGE_CROP);

    ((spritesheet_t *)spritesheet)->image->ref_count++;
    job->data->loaded_image_crop.image =
        ((spritesheet_t *)spritesheet)->image;
    job->data->loaded_image_crop.x = x;
    job->data->loaded_image_crop.y = y;
    job->data->loaded_image_crop.c_w =
        ((spritesheet_t *)spritesheet)->sprite_width;
    job->data->loaded_image_crop.c_h =
        ((spritesheet_t *)spritesheet)->sprite_height;

    // X and Y need to incorporate row or column * 2 + 1 instances of the
    // sprite's padding
    job->data->loaded_image_crop.c_x =
        column * (((spritesheet_t *)spritesheet)->sprite_width +
                  ((spritesheet_t *)spritesheet)->padding_x * 2) +
        ((spritesheet_t *)spritesheet)->padding_x;
    job->data->loaded_image_crop.c_y =
        row * (((spritesheet_t *)spritesheet)->sprite_height +
               ((spritesheet_t *)spritesheet)->padding_y * 2) +
        ((spritesheet_t *)spritesheet)->padding_y;

    return 0;

err:
    return -1;
}

int __attribute_deprecated__ gfxGetImageSize(char *filename, int *w, int *h)
{
    char full_filename[PATH_MAX + 1];
    realpath(filename, full_filename);
    return _getImageSize(full_filename, w, h);
}

int __attribute_deprecated__ gfxDrawScaledImage(char *filename, signed short x,
        signed short y, float scale)
{
    INIT_JOB(job, DRAW_SCALED_IMAGE);

    char abs_path[PATH_MAX + 1];

    if (realpath(filename, (char *)abs_path) == NULL) {
        return -1;
    }

    job->data->scaled_image.image.filename =
        calloc(strlen(abs_path) + 1, sizeof(char));
    strcpy(job->data->scaled_image.image.filename, abs_path);
    job->data->scaled_image.image.x = x;
    job->data->scaled_image.image.y = y;
    job->data->scaled_image.scale = scale;

    return 0;
}

int gfxDrawArrow(signed short x1, signed short y1, signed short x2,
                 signed short y2, signed short head_length,
                 unsigned char thickness, unsigned int colour)
{
    INIT_JOB(job, DRAW_ARROW);

    job->data->arrow.x1 = x1;
    job->data->arrow.y1 = y1;
    job->data->arrow.x2 = x2;
    job->data->arrow.y2 = y2;
    job->data->arrow.head_length = head_length;
    job->data->arrow.thickness = thickness;
    job->data->arrow.colour = colour;

    return 0;
}

void gfxDrawAnimationReset(gfx_sequence_handle_t sequence)
{
    animated_sequence_instance_t *anim =
        (animated_sequence_instance_t *)sequence;

    anim->prev_frame_timestamp = 0;
    anim->cur_frame_timestamp = 0;

    if (anim->sequence->direction ==
        SPRITE_SEQUENCE_HORIZONTAL_POS ||
        anim->sequence->direction == SPRITE_SEQUENCE_HORIZONTAL_NEG) {
        anim->current_frame = anim->sequence->start_col;
    }
    else if (anim->sequence->direction ==
             SPRITE_SEQUENCE_VERTICAL_POS ||
             anim->sequence->direction == SPRITE_SEQUENCE_VERTICAL_NEG) {
        anim->current_frame = anim->sequence->start_row;
    }
}

int gfxDrawAnimationDrawFrame(gfx_sequence_handle_t sequence,
                              unsigned ms_timestep, int x, int y)
{
    if (sequence == NULL) {
        PRINT_ERROR("Trying to draw invalid sequence");
        goto err;
    }

    animated_sequence_instance_t *anim =
        (animated_sequence_instance_t *)sequence;

    anim->cur_frame_timestamp += ms_timestep;

    if (anim->cur_frame_timestamp >
        (anim->prev_frame_timestamp + anim->frame_period_ms)) {

        if (anim->sequence->direction ==
            SPRITE_SEQUENCE_HORIZONTAL_POS ||
            anim->sequence->direction == SPRITE_SEQUENCE_VERTICAL_POS) {
            anim->current_frame += ((anim->cur_frame_timestamp -
                                     anim->prev_frame_timestamp) /
                                    anim->frame_period_ms);
            anim->current_frame %= anim->sequence->frames;
        }
        else if (anim->sequence->direction ==
                 SPRITE_SEQUENCE_HORIZONTAL_NEG ||
                 anim->sequence->direction ==
                 SPRITE_SEQUENCE_VERTICAL_NEG) {
            anim->current_frame -= ((anim->cur_frame_timestamp -
                                     anim->prev_frame_timestamp) /
                                    anim->frame_period_ms);
            if (anim->current_frame == -1)
                anim->current_frame =
                    anim->sequence->frames - 1;
        }

        anim->prev_frame_timestamp += (((anim->cur_frame_timestamp -
                                         anim->prev_frame_timestamp) /
                                        anim->frame_period_ms) *
                                       anim->frame_period_ms);
    }

    INIT_JOB(job, DRAW_LOADED_IMAGE_CROP);

    anim->image->spritesheet->image->ref_count++;
    job->data->loaded_image_crop.image = anim->image->spritesheet->image;
    job->data->loaded_image_crop.x = x;
    job->data->loaded_image_crop.y = y;
    job->data->loaded_image_crop.c_w =
        anim->image->spritesheet->sprite_width;
    job->data->loaded_image_crop.c_h =
        anim->image->spritesheet->sprite_height;

    switch (anim->sequence->direction) {
        case SPRITE_SEQUENCE_HORIZONTAL_POS:
        case SPRITE_SEQUENCE_HORIZONTAL_NEG: {
            unsigned cur_frame_index_offset =
                (anim->current_frame + anim->sequence->start_col) %
                anim->sequence->frames;
            job->data->loaded_image_crop.c_x =
                anim->image->spritesheet->x +
                cur_frame_index_offset *
                (anim->image->spritesheet->sprite_width +
                 anim->image->spritesheet->padding_x * 2);
            job->data->loaded_image_crop.c_y =
                anim->image->spritesheet->y +
                anim->sequence->start_row *
                (anim->image->spritesheet->sprite_height +
                 anim->image->spritesheet->padding_y * 2);
        } break;
        case SPRITE_SEQUENCE_VERTICAL_POS:
        case SPRITE_SEQUENCE_VERTICAL_NEG: {
            unsigned cur_frame_index_offset =
                (anim->current_frame + anim->sequence->start_row) %
                anim->sequence->frames;
            job->data->loaded_image_crop.c_x =
                anim->image->spritesheet->x +
                anim->sequence->start_col *
                (anim->image->spritesheet->sprite_width +
                 anim->image->spritesheet->padding_x * 2);
            job->data->loaded_image_crop.c_y =
                anim->image->spritesheet->y +
                cur_frame_index_offset *
                (anim->image->spritesheet->sprite_height +
                 anim->image->spritesheet->padding_y * 2);
        } break;
        default:
            break;
    }

    return 0;
err:
    return -1;
}

int gfxDrawSetGlobalXOffset(int offset)
{
    int ret;

    if (!(ret = pthread_mutex_lock(&global_offset.lock))) {
        global_offset.x = offset;
        pthread_mutex_unlock(&global_offset.lock);
    }
    else {
        PRINT_ERROR("Could not set global X offset");
    }

    return ret;
}

int gfxDrawSetGlobalYOffset(int offset)
{
    int ret;

    if (!(ret = pthread_mutex_lock(&global_offset.lock))) {
        global_offset.y = offset;
        pthread_mutex_unlock(&global_offset.lock);
    }
    else {
        PRINT_ERROR("Could not set global Y offset");
    }

    return ret;
}

int gfxDrawGetGlobalXOffset(int *offset)
{
    int ret;

    if (!(ret = pthread_mutex_lock(&global_offset.lock))) {
        *offset = global_offset.x;
        pthread_mutex_unlock(&global_offset.lock);
    }
    else {
        PRINT_ERROR("Could not get global X offset");
    }

    return ret;
}

int gfxDrawGetGlobalYOffset(int *offset)
{
    int ret;

    if (!(ret = pthread_mutex_lock(&global_offset.lock))) {
        *offset = global_offset.y;
        pthread_mutex_unlock(&global_offset.lock);
    }
    else {
        PRINT_ERROR("Could not get global Y offset");
    }

    return ret;
}
