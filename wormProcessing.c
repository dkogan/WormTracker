#include <stdint.h>
#include "wormProcessing.h"

static int width, height;

static CvMat*         workImage0;
static CvMat*         workImage1;
static CvMat*         workImageInt;

#define PRESMOOTHING_R            9
#define DETREND_R                 25
#define DETREND_SCALE             180
#define ADAPTIVE_THRESHOLD_KERNEL 21
#define ADAPTIVE_THRESHOLD        5
#define MORPHOLOGIC_DEPTH         2

#define PRESMOOTHING_W (1 + 2*PRESMOOTHING_R)
#define DETREND_W      (1 + 2*DETREND_R)

void processingInit(int w, int h)
{
    width  = w;
    height = h;

    workImage0   = cvCreateMat(h, w, CV_32FC1);
    workImage1   = cvCreateMat(h, w, CV_32FC1);
    workImageInt = cvCreateMat(h, w, CV_8UC1);
}

void processingCleanup(void)
{
    cvReleaseMat(&workImage0);
    cvReleaseMat(&workImage1);
    cvReleaseMat(&workImageInt);
}

const CvMat* isolateWorms(const IplImage* input,
                          visionParameters_t* params)
{
    unsigned int presmoothing_w = 1 + 2*params->presmoothing_r;
    unsigned int detrend_w      = 1 + 2*params->detrend_r;

    cvConvert(input, workImage0);
    cvSmooth(workImage0, workImage0, CV_GAUSSIAN, presmoothing_w, presmoothing_w, 0, 0);
    cvSmooth(workImage0, workImage1, CV_GAUSSIAN, detrend_w,      detrend_w,      0, 0);
    cvDiv(workImage0, workImage1, workImage0, params->detrend_scale);

    cvConvert(workImage0, workImageInt);

    cvAdaptiveThreshold(workImageInt, workImageInt,
                        255,CV_ADAPTIVE_THRESH_MEAN_C,
                        CV_THRESH_BINARY_INV,
                        params->adaptive_threshold_kernel, params->adaptive_threshold);

    cvErode (workImageInt, workImageInt, NULL, params->morphologic_depth);
    cvDilate(workImageInt, workImageInt, NULL, params->morphologic_depth);

    return workImageInt;
}

static double computeOccupancySingleCircle(const CvMat* isolatedWorms,
                                           const CvPoint* circle, int circleRadius)
{
    int numInCircle = 0;
    int numWormsInCircle = 0;

    for(int y = MAX(0,        circle->y - circleRadius);
        y <     MIN(height-1, circle->y + circleRadius);
        y++)
    {
        int dy = y - circle->y;

        const uint8_t* data = (const uint8_t*)(isolatedWorms->data.ptr + y * isolatedWorms->step);

        for(int x = MAX(0,        circle->x - circleRadius);
            x <     MIN(width-1,  circle->x + circleRadius);
            x++)
        {
            int dx = x - circle->x;
            if(dx*dx + dy*dy <= circleRadius*circleRadius)
            {
                numInCircle++;

                if(data[x] != 0)
                    numWormsInCircle++;
            }
        }
    }

    return (double)numWormsInCircle / (double)numInCircle;
}


void computeWormOccupancy(const CvMat* isolatedWorms,
                          const CvPoint* leftCircle, const CvPoint* rightCircle,
                          int circleRadius,
                          double* left, double* right)
{
    *left  = computeOccupancySingleCircle(isolatedWorms, leftCircle,  circleRadius);
    *right = computeOccupancySingleCircle(isolatedWorms, rightCircle, circleRadius);
}
