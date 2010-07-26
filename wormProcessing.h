#ifndef __WORM_PROCESSING_H__
#define __WORM_PROCESSING_H__

#include "cvlib.hh"

typedef struct
{
    unsigned int presmoothing_r;
    unsigned int detrend_r;
    double       detrend_scale;
    unsigned int adaptive_threshold_kernel;
    unsigned int adaptive_threshold;
    unsigned int morphologic_depth;
} visionParameters_t;

void processingInit(int w, int h);
void processingCleanup(void);
const CvMat* isolateWorms(const IplImage* input,
                          visionParameters_t* params);
void computeWormOccupancy(const CvMat* isolatedWorms,
                          const CvPoint* leftCircle, const CvPoint* rightCircle,
                          int circleRadius,
                          double* left, double* right);

#endif
