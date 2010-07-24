#ifndef __WORM_PROCESSING_H__
#define __WORM_PROCESSING_H__

#include "cvlib.hh"

void processingInit(int w, int h);
void processingCleanup(void);
const CvMat* isolateWorms(const IplImage* input);
void findCircles(const IplImage* input, CvPoint* left, CvPoint* right);
void computeWormOccupancy(const CvMat* isolatedWorms,
                          const CvPoint* leftCircle, const CvPoint* rightCircle,
                          int circleRadius,
                          double* left, double* right);

#endif
