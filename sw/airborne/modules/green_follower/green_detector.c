#include "modules/green_follower/green_detector.h"
#include "modules/computer_vision/cv.h"
#include "modules/core/abi.h"
#include "std.h"

#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include "pthread.h"

#ifndef GREENFILTER_FPS
#define GREENFILTER_FPS 0       ///< Default FPS (zero means run at camera fps)
#endif
PRINT_CONFIG_VAR(COLORFILTER_FPS)

#define GREEN_DETECTOR_VERBOSE TRUE

#define PRINT(string,...) fprintf(stderr, "[green_follower->%s()] " string,__FUNCTION__ , ##__VA_ARGS__)
#if GREEN_DETECTOR_VERBOSE
#define VERBOSE_PRINT PRINT
#else
#define VERBOSE_PRINT(...)
#endif

// Filter Settings
uint8_t gd_lum_min = 60;
uint8_t gd_lum_max = 130;
uint8_t gd_cb_min = 75;
uint8_t gd_cb_max = 110;
uint8_t gd_cr_min = 120;
uint8_t gd_cr_max = 140;

static pthread_mutex_t mutex;

struct heading_object_t {
    float best_heading;
    float safe_length;
    uint32_t green_pixels;
    bool updated;
};
struct heading_object_t global_heading_object;

void apply_threshold(struct image_t *img, uint32_t *green_pixels,
                     uint8_t lum_min, uint8_t lum_max,
                     uint8_t cb_min, uint8_t cb_max,
                     uint8_t cr_min, uint8_t cr_max);

float get_radial(struct image_t *img, float angle, uint8_t radius);

void get_direction(struct image_t *img, uint8_t resolution, float* best_heading, float* safe_length);

/*
 * object_detector
 * @param img - input image to process
 * @return img
 */
static struct image_t *green_heading_finder(struct image_t *img)
{
    uint8_t lum_min, lum_max;
    uint8_t cb_min, cb_max;
    uint8_t cr_min, cr_max;

    float best_heading, safe_length;

    uint32_t green_pixels;

    lum_min = gd_lum_min;
    lum_max = gd_lum_max;
    cb_min = gd_cb_min;
    cb_max = gd_cb_max;
    cr_min = gd_cr_min;
    cr_max = gd_cr_max;
    uint8_t scan_resolution = 50;

    // Filter the image so that all green pixels have a y value of 255 and all others a y value of 0
    apply_threshold(img, &green_pixels, lum_min, lum_max, cb_min, cb_max, cr_min, cr_max);
    // Scan in radials from the centre bottom of the image to find the direction with the most green pixels
    get_direction(img, scan_resolution, &best_heading, &safe_length);

    pthread_mutex_lock(&mutex);
    global_heading_object.best_heading = best_heading;
    global_heading_object.safe_length = safe_length;
    global_heading_object.green_pixels = green_pixels;
    global_heading_object.updated = true;
    pthread_mutex_unlock(&mutex);
    return img;
}

struct image_t *green_heading_finder1(struct image_t *img, uint8_t camera_id);
struct image_t *green_heading_finder1(struct image_t *img, uint8_t camera_id __attribute__((unused)))
{
    return green_heading_finder(img);
}

void green_detector_init(void) {
    memset(&global_heading_object, 0, sizeof(struct heading_object_t));
    pthread_mutex_init(&mutex, NULL);

    #ifdef GREEN_DETECTOR_LUM_MIN
        gd_lum_min = GREEN_DETECTOR_LUM_MIN;
        gd_lum_max = GREEN_DETECTOR_LUM_MAX;
        gd_cb_min = GREEN_DETECTOR_CB_MIN;
        gd_cb_max = GREEN_DETECTOR_CB_MAX;
        gd_cr_min = GREEN_DETECTOR_CR_MIN;
        gd_cr_max = GREEN_DETECTOR_CR_MAX;
    #endif

    cv_add_to_device(&GREENFILTER_CAMERA, green_heading_finder1, GREENFILTER_FPS, 0);
}

void green_detector_periodic(void) {
    static struct heading_object_t local_heading_object;
    pthread_mutex_lock(&mutex);
    memcpy(&local_heading_object, &global_heading_object, sizeof(struct heading_object_t));
    pthread_mutex_unlock(&mutex);

    if(local_heading_object.updated){
        AbiSendMsgGREEN_DETECTION(GREEN_DETECTION_ID, local_heading_object.best_heading, local_heading_object.safe_length, local_heading_object.green_pixels);
        local_heading_object.updated = false;
    }
}


void apply_threshold(struct image_t *img, uint32_t* green_pixels,
                     uint8_t lum_min, uint8_t lum_max,
                     uint8_t cb_min, uint8_t cb_max,
                     uint8_t cr_min, uint8_t cr_max)
{
  uint8_t *buffer = img->buf;
  uint32_t local_green_pixels = 0;

  // Go through all the pixels
  for (uint16_t y = 0; y < img->h; y++) {
    for (uint16_t x = 0; x < img->w; x ++) {
      // Check if the color is inside the specified values
      uint8_t *yp, *up, *vp;
      if (x % 2 == 0) {
        // Even x
        up = &buffer[y * 2 * img->w + 2 * x];      // U
        yp = &buffer[y * 2 * img->w + 2 * x + 1];  // Y1
        vp = &buffer[y * 2 * img->w + 2 * x + 2];  // V
        //yp = &buffer[y * 2 * img->w + 2 * x + 3]; // Y2
      } else {
        // Uneven x
        up = &buffer[y * 2 * img->w + 2 * x - 2];  // U
        //yp = &buffer[y * 2 * img->w + 2 * x - 1]; // Y1
        vp = &buffer[y * 2 * img->w + 2 * x];      // V
        yp = &buffer[y * 2 * img->w + 2 * x + 1];  // Y2
      }
      if ( (*yp >= lum_min) && (*yp <= lum_max) &&
           (*up >= cb_min ) && (*up <= cb_max ) &&
           (*vp >= cr_min ) && (*vp <= cr_max )) {
            local_green_pixels++;
            *yp = 255;  // make pixel white
        }
      else {
        *yp = 0; // make pixel black
      }
    }
  }
  *green_pixels = local_green_pixels;
}

float get_radial(struct image_t *img, float angle, uint8_t radius) {
    uint8_t *buffer = img->buf;

    uint32_t sum = 0;
    uint16_t x, y;

    for (double i = 0; i < radius; i++) {
        y = (uint16_t)((double)img->h - i * sin(angle));
        x = (uint16_t)((double)img->w / 2 + i * cos(angle));
        if (buffer[y * 2 * img->w + 2 * x + 1] == 255) {
            sum++;
        }
    }

    return (float)sum; // * (sin(angle) + 0.2) ;
}

void get_direction(struct image_t *img, uint8_t resolution, float* best_heading, float* safe_length) {

    float step_size = M_PI / (float)resolution;
    *best_heading = 0;
    *safe_length = 0;

    for (float angle = 0.001; angle < M_PI; angle += step_size) {
        float radial = get_radial(img, angle, img->h - 20);

        // VERBOSE_PRINT("GF: Radial %f is %f\n", angle, radial);


        if (radial >= *safe_length) {
            *best_heading = angle;
            *safe_length = radial;
        }
    }

    *best_heading = M_PI/2 - *best_heading;
}
