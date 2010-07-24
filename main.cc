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
#include "Fl_PlotXY/Fl_PlotXY.H"

#include "cvFltkWidget.hh"
#include "ffmpegInterface.hh"
#include "cameraSource.hh"

extern "C"
{
#include "wormProcessing.h"
}

#define FRAME_RATE_FPS       15
#define CIRCLE_RADIUS        60
#define CIRCLE_COLOR         CV_RGB(0xFF, 0, 0)
#define POINTED_CIRCLE_COLOR CV_RGB(0, 0xFF, 0)


#define FRAME_W   480
#define FRAME_H   480
#define CROP_RECT cvRect(80, 0, FRAME_W, FRAME_H)
#define WINDOW_W  800
#define WINDOW_H  (FRAME_H + BUTTON_H + PLOT_H)
#define BUTTON_W  100
#define BUTTON_H  30
#define PLOT_W    WINDOW_W
#define PLOT_H    400



static FrameSource*  source;
static CvFltkWidget* widgetImage;
static Fl_Button*    goResetButton;
static Fl_PlotXY*    plot;

// Analysis currently running
#define RUNNING_ANALYSIS (goResetButton->type() == FL_TOGGLE_BUTTON && !goResetButton->value())

// Analysis not running. Looking at the data I just got
#define STOPPED_ANALYSIS (goResetButton->type() == FL_NORMAL_BUTTON)

// Analysis not running. Don't have data
#define RESET_ANALYSIS   (goResetButton->type() == FL_TOGGLE_BUTTON &&  goResetButton->value())

static int     leftPlotIndex, rightPlotIndex;
static CvPoint leftCircleCenter    = cvPoint(-1, -1);
static CvPoint rightCircleCenter   = cvPoint(-1, -1);
static CvPoint pointedCircleCenter = cvPoint(-1, -1);

#define HAVE_LEFT_CIRCLE    (leftCircleCenter .x > 0 && leftCircleCenter .y > 0)
#define HAVE_RIGHT_CIRCLE   (rightCircleCenter.x > 0 && rightCircleCenter.y > 0)
#define HAVE_CIRCLES        (HAVE_LEFT_CIRCLE && HAVE_RIGHT_CIRCLE)
#define HAVE_POINTED_CIRCLE (pointedCircleCenter.x > 0 && pointedCircleCenter.y > 0)

void gotNewFrame(IplImage* buffer, uint64_t timestamp_us __attribute__((unused)))
{
    cvMerge(buffer, buffer, buffer, NULL, *widgetImage);

    const CvMat* result = isolateWorms(buffer);
    cvSetImageCOI(*widgetImage, 1);
    cvCopy(result, *widgetImage);
    cvSetImageCOI(*widgetImage, 0);

    if(HAVE_LEFT_CIRCLE)
        cvCircle(*widgetImage, leftCircleCenter,    CIRCLE_RADIUS, CIRCLE_COLOR, 1, 8);

    if(HAVE_RIGHT_CIRCLE)
        cvCircle(*widgetImage, rightCircleCenter,   CIRCLE_RADIUS, CIRCLE_COLOR, 1, 8);

    if(HAVE_POINTED_CIRCLE)
        cvCircle(*widgetImage, pointedCircleCenter, CIRCLE_RADIUS, POINTED_CIRCLE_COLOR, 1, 8);

    double leftOccupancy, rightOccupancy;
    computeWormOccupancy(result, &leftCircleCenter, &rightCircleCenter,
                         CIRCLE_RADIUS,
                         &leftOccupancy, &rightOccupancy);

    Fl::lock();
    {
        widgetImage->redrawNewFrame();
    }
    Fl::unlock();
}

static void widgetImageCallback(Fl_Widget* widget __attribute__((unused)), void* cookie __attribute__((unused)))
{
    bool onLeftHalf = (Fl::event_x() - widget->x()) < FRAME_W/2;
    switch(Fl::event())
    {
    case FL_MOVE:
        pointedCircleCenter.x = Fl::event_x() - widget->x();
        pointedCircleCenter.y = Fl::event_y() - widget->y();
        return;

    case FL_LEAVE:
        pointedCircleCenter.x = pointedCircleCenter.y = -1;
        break;

    case FL_PUSH:
        if(onLeftHalf)
        {
            leftCircleCenter.x = Fl::event_x() - widget->x();
            leftCircleCenter.y = Fl::event_y() - widget->y();
        }
        else
        {
            rightCircleCenter.x = Fl::event_x() - widget->x();
            rightCircleCenter.y = Fl::event_y() - widget->y();
        }

        pointedCircleCenter.x = pointedCircleCenter.y = -1;
        break;

    default: ;
    }
}

static void goResetButton_setResetAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA_BOLD);
    goResetButton->labelcolor(FL_RED);
    goResetButton->type(FL_TOGGLE_BUTTON);
    goResetButton->label("Analyze");
}

static void goResetButton_setRunningAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA);
    goResetButton->labelcolor(FL_BLACK);
    goResetButton->type(FL_TOGGLE_BUTTON);
    goResetButton->label("Stop analysis");
}

static void goResetButton_setStoppedAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA);
    goResetButton->labelcolor(FL_BLACK);
    goResetButton->type(FL_NORMAL_BUTTON);
    goResetButton->label("Reset analysis data");
}

static void pressedGoReset(Fl_Widget* widget __attribute__((unused)), void* cookie __attribute__((unused)))
{
    if     (RUNNING_ANALYSIS) goResetButton_setStoppedAnalysis();
    else if(STOPPED_ANALYSIS) goResetButton_setResetAnalysis();
    else                      goResetButton_setRunningAnalysis();
}

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

    widgetImage->callback(widgetImageCallback);

    goResetButton = new Fl_Button( 0, source->h(), BUTTON_W, BUTTON_H);
    goResetButton_setResetAnalysis();
    goResetButton->callback(pressedGoReset);
    goResetButton->deactivate();

    plot = new Fl_PlotXY( 0, goResetButton->y() + goResetButton->h(), PLOT_W, PLOT_H,
                          "Worm occupancy");
    leftPlotIndex  = plot->newline(0,0,0,0, FL_PLOTXY_AUTO,FL_BLACK, "Left occupancy %");
    rightPlotIndex = plot->newline(0,0,0,0, FL_PLOTXY_AUTO,FL_BLACK, "Right occupancy %");

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
