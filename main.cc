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
#include "cartesian/Cartesian.H"

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


#define FRAME_W       480
#define FRAME_H       480
#define CROP_RECT     cvRect(80, 0, FRAME_W, FRAME_H)
#define WINDOW_W      800
#define WINDOW_H      (FRAME_H + BUTTON_H + PLOT_H + X_AXIS_HEIGHT)
#define BUTTON_W      100
#define BUTTON_H      30
#define PLOT_W        WINDOW_W
#define PLOT_H        400
#define X_AXIS_HEIGHT 30
#define Y_AXIS_WIDTH  30


static FrameSource*  source;
static CvFltkWidget* widgetImage;
static Fl_Button*    goResetButton;
static Ca_Canvas*    plot;

// the analysis could be idle, running, or idle examining data (STOPPED)
static enum { RESET, RUNNING, STOPPED } analysisState;

static int           numPoints;
static Ca_LinePoint* lastLeftPoint       = NULL;
static Ca_LinePoint* lastRightPoint      = NULL;
static CvPoint       leftCircleCenter    = cvPoint(-1, -1);
static CvPoint       rightCircleCenter   = cvPoint(-1, -1);
static CvPoint       pointedCircleCenter = cvPoint(-1, -1);

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

    // This critical section is likely larger than it needs to be, but this keeps me safe. The
    // analysis state can change in the FLTK thread, so I err on the side of safety
    Fl::lock();
    {
        if(analysisState == RUNNING)
        {
            double leftOccupancy, rightOccupancy;
            computeWormOccupancy(result, &leftCircleCenter, &rightCircleCenter,
                                 CIRCLE_RADIUS,
                                 &leftOccupancy, &rightOccupancy);

            lastLeftPoint  = new Ca_LinePoint(lastLeftPoint,
                                              numPoints, leftOccupancy, 1,FL_RED,   CA_ROUND|CA_BORDER);
            lastRightPoint = new Ca_LinePoint(lastRightPoint,
                                              numPoints, leftOccupancy, 1,FL_GREEN, CA_ROUND|CA_BORDER);
            plot->redraw();
            numPoints++;
        }

        widgetImage->redrawNewFrame();
    }
    Fl::unlock();
}

static void widgetImageCallback(Fl_Widget* widget __attribute__((unused)), void* cookie __attribute__((unused)))
{
    if(Fl::event() == FL_LEAVE)
    {
        // I want to make sure to never miss this, so I handle it unconditionally
        pointedCircleCenter.x = pointedCircleCenter.y = -1;
        return;
    }

    if(analysisState == RUNNING)
    {
        pointedCircleCenter.x = pointedCircleCenter.y = -1;
        return;
    }

    bool onLeftHalf = (Fl::event_x() - widget->x()) < FRAME_W/2;
    switch(Fl::event())
    {
    case FL_MOVE:
        pointedCircleCenter.x = Fl::event_x() - widget->x();
        pointedCircleCenter.y = Fl::event_y() - widget->y();
        break;

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

    if(HAVE_CIRCLES && analysisState == RESET)
        goResetButton->activate();
}

static void setResetAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA_BOLD);
    goResetButton->labelcolor(FL_RED);
    goResetButton->type(FL_TOGGLE_BUTTON);
    goResetButton->label("Analyze");

    analysisState = RESET;
}

static void setRunningAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA);
    goResetButton->labelcolor(FL_BLACK);
    goResetButton->type(FL_TOGGLE_BUTTON);
    goResetButton->label("Stop analysis");

    pointedCircleCenter.x = pointedCircleCenter.y = -1;
    numPoints = 0;
    plot->clearall();

    analysisState = RUNNING;
}

static void setStoppedAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA);
    goResetButton->labelcolor(FL_BLACK);
    goResetButton->type(FL_NORMAL_BUTTON);
    goResetButton->label("Reset analysis data");

    analysisState = STOPPED;
}

static void pressedGoReset(Fl_Widget* widget __attribute__((unused)), void* cookie __attribute__((unused)))
{
    if     (analysisState == RUNNING) setStoppedAnalysis();
    else if(analysisState == STOPPED) setResetAnalysis();
    else                              setRunningAnalysis();
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
    setResetAnalysis();
    goResetButton->callback(pressedGoReset);
    goResetButton->deactivate();

    plot = new Ca_Canvas( Y_AXIS_WIDTH, goResetButton->y() + goResetButton->h(), PLOT_W, PLOT_H,
                          "Worm occupancy");
    Ca_X_Axis* Xaxis = new Ca_X_Axis(plot->x(), plot->y() + plot->h(), plot->w(), X_AXIS_HEIGHT, "Points");
    Xaxis->align(FL_ALIGN_BOTTOM);
//     Xaxis->scale(CA_LOG);
    Xaxis->minimum(0);
    Xaxis->maximum(100);
    Xaxis->label_format("%g");
    Xaxis->minor_grid_color(fl_gray_ramp(20));
    Xaxis->major_grid_color(fl_gray_ramp(15));
    Xaxis->label_grid_color(fl_gray_ramp(10));
    Xaxis->grid_visible(CA_MINOR_GRID|CA_MAJOR_GRID|CA_LABEL_GRID);
    Xaxis->major_step(10);
    Xaxis->label_step(10);
    Xaxis->axis_color(FL_BLACK);
    Xaxis->axis_align(CA_BOTTOM|CA_LINE);

    Ca_Y_Axis* Yaxis = new Ca_Y_Axis(0, plot->y(), Y_AXIS_WIDTH, plot->h(), "occupancy ratio");
    Yaxis->align(FL_ALIGN_RIGHT|FL_ALIGN_TOP);
    Yaxis->minor_grid_style(FL_DASH);
    Yaxis->axis_align(CA_LEFT);
    Yaxis->axis_color(FL_BLACK);

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
