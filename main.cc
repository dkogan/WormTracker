#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <time.h>
#include <string.h>
using namespace std;

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/Fl_File_Chooser.H>

#include "cvFltkWidget.hh"
#include "ffmpegInterface.hh"
#include "cameraSource.hh"

extern "C"
{
#include "wormProcessing.h"
}

#define CIRCLE_RADIUS 60
#define CIRCLE_COLOR CV_RGB(0xFF, 0, 0)
#define FRAME_RATE_FPS       15

static FrameSource*  source;
static CvFltkWidget* widgetImage;
static Fl_Output*    counts;

static double accumLeft  = 0;
static double accumRight = 0;

static CvPoint leftCircleCenter  = cvPoint(-1, -1);
static CvPoint rightCircleCenter = cvPoint(-1, -1);

#define HAVE_CIRCLES                            \
    (leftCircleCenter .x > 0 && leftCircleCenter .y > 0 && \
     rightCircleCenter.x > 0 && rightCircleCenter.y > 0)

void gotNewFrame(IplImage* buffer, uint64_t timestamp_us __attribute__((unused)))
{
    if(!HAVE_CIRCLES)
        findCircles(buffer, &leftCircleCenter, &rightCircleCenter);
        
    cvMerge(buffer, buffer, buffer, NULL, *widgetImage);

    const CvMat* result = isolateWorms(buffer);
    cvSetImageCOI(*widgetImage, 1);
    cvCopy(result, *widgetImage);
    cvSetImageCOI(*widgetImage, 0);

    if(HAVE_CIRCLES)
    {
        cvCircle(*widgetImage, leftCircleCenter,  CIRCLE_RADIUS, CIRCLE_COLOR, 1, 8);
        cvCircle(*widgetImage, rightCircleCenter, CIRCLE_RADIUS, CIRCLE_COLOR, 1, 8);
    }

    double leftOccupancy, rightOccupancy;
    computeWormOccupancy(result, &leftCircleCenter, &rightCircleCenter,
                         CIRCLE_RADIUS,
                         &leftOccupancy, &rightOccupancy);

    accumLeft  += leftOccupancy;
    accumRight += rightOccupancy;

    char countsString[128];
    snprintf(countsString, sizeof(countsString), "%.3f %.3f",
             leftOccupancy, rightOccupancy);

    Fl::lock();
    {
        widgetImage->redrawNewFrame();
        counts->value(countsString);
    }
    Fl::unlock();
}

#define CROP_RECT cvRect(80, 0, 480, 480)
#define WINDOW_W  500
#define WINDOW_H  500

int main(int argc, char* argv[])
{
    Fl::lock();
    Fl::visual(FL_RGB);

    // open the first source. If there's an argument, assume it's an input video. Otherwise, try
    // reading a camera
    if(argc >= 2) source = new FFmpegDecoder(argv[1], FRAMESOURCE_GRAYSCALE, false, CROP_RECT);
    else          source = new CameraSource (FRAMESOURCE_GRAYSCALE, false, 0, CROP_RECT);

    if(! *source)
    {
        fprintf(stderr, "couldn't open source\n");
        delete source;
        return 0;
    }

    IplImage* buffer = cvCreateImage(cvSize(source->w(), source->h()), IPL_DEPTH_8U, 1);

    Fl_Double_Window* window =
        new Fl_Double_Window(WINDOW_W, WINDOW_H, "Wormtracker 3");
    widgetImage = new CvFltkWidget(0, 0, source->w(), source->h(),
                                   WIDGET_COLOR);


    counts = new Fl_Output(0, source->h(), 200, 30);


    window->resizable(window);
    window->end();
    window->show();

    processingInit(source->w(), source->h());

    // I read the data with a tiny delay. This makes sure that I skip old frames (only an issue if I
    // can't keep up with the data rate), but yet got as fast as I can
    source->startSourceThread(&gotNewFrame, 1e6/FRAME_RATE_FPS, buffer);

    while (Fl::wait())
    {
    }

    Fl::unlock();

    delete source;
    delete window;
    cvReleaseImage(&buffer);

    processingCleanup();
    return 0;
}
