#ifndef __WORM_PROCESSING_H__
#define __WORM_PROCESSING_H__

#include "cvlib.hh"

typedef struct
{
    unsigned int presmoothing_w;
    unsigned int detrend_w;
    double       detrend_scale;
    unsigned int adaptive_threshold_kernel;
    unsigned int adaptive_threshold;
    unsigned int morphologic_depth;
} visionParameters_t;

void processingInit(int w, int h);
void processingCleanup(void);
void getDefaultParameters(visionParameters_t* params);
const CvMat* isolateWorms(const IplImage* input,
                          visionParameters_t* params);
void computeWormOccupancy(const CvMat* isolatedWorms,
                          const CvPoint* leftCircle, const CvPoint* rightCircle,
                          int circleRadius,
                          double* left, double* right);

#endif
