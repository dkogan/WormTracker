#include <unistd.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <time.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <string>
using namespace std;

#include <FL/Fl.H>
#include <Fl/fl_ask.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Value_Slider.H>
#include "Fl_Rotated_Text/Fl_Rotated_Text.H"
#include "cartesian/Cartesian.H"

#include "cvFltkWidget.hh"
#include "ffmpegInterface.hh"
#include "cameraSource.hh"

extern "C"
{
#include "wormProcessing.h"
}

#define DATA_FRAME_RATE_FPS     1 /* I collect at 1 frame per second */
#define PREVIEW_FRAME_RATE_FPS  15
#define VIDEO_ENCODING_FPS      15
#define CIRCLE_RADIUS           50
#define CIRCLE_COLOR            CV_RGB(0xFF, 0, 0)
#define POINTED_CIRCLE_COLOR    CV_RGB(0, 0xFF, 0)


#define FRAME_W        480
#define FRAME_H        480
#define CROP_RECT      cvRect(80, 0, FRAME_W, FRAME_H)
#define WINDOW_W       1200
#define WINDOW_H       (FRAME_H + PLOT_H + X_AXIS_HEIGHT + AXIS_EXTRA_SPACE)
#define BUTTON_W       320
#define BUTTON_H       60
#define PLOT_W         (WINDOW_W - (Y_AXIS_WIDTH + AXIS_EXTRA_SPACE))
#define PLOT_H         400
#define X_AXIS_HEIGHT  30
#define Y_AXIS_WIDTH   80
#define ACCUM_W        180
#define ACCUM_H        BUTTON_H
#define PARAM_SLIDER_W 180
#define PARAM_SLIDER_H 25


// due to a bug (most likely), the axis aren't drawn completely inside their box. Thus I leave a bit
// of extra space to see the labels
#define AXIS_EXTRA_SPACE 40

#define AM_READING_CAMERA (dynamic_cast<CameraSource*>(source) != NULL)

static FFmpegEncoder videoEncoder;

static FrameSource*     source;
static CvFltkWidget*    widgetImage;
static Fl_Button*       goResetButton;
static Fl_Button*       chdirButton;
static Fl_Value_Slider* duration;
static Fl_Input*        experimentName;
static Ca_Canvas*       plot = NULL;
static Ca_X_Axis*       Xaxis;
static Ca_Y_Axis*       Yaxis;

// the vision parameters
static Fl_Value_Slider* param_presmoothing_w;
static Fl_Value_Slider* param_detrend_w;
static Fl_Value_Slider* param_detrend_scale;
static Fl_Value_Slider* param_adaptive_threshold_kernel;
static Fl_Value_Slider* param_adaptive_threshold;
static Fl_Value_Slider* param_morphologic_depth;

static Fl_Output* leftAccum;
static Fl_Output* rightAccum;
static double     leftAccumValue  = 0.0;
static double     rightAccumValue = 0.0;

// the analysis could be idle, running, or idle examining data (STOPPED)
static enum { RESET, RUNNING, STOPPED } analysisState;

static int           numPoints;
static Ca_LinePoint* lastLeftPoint       = NULL;
static Ca_LinePoint* lastRightPoint      = NULL;
static CvPoint       leftCircleCenter    = cvPoint(-1, -1);
static CvPoint       rightCircleCenter   = cvPoint(-1, -1);
static CvPoint       pointedCircleCenter = cvPoint(-1, -1);

FILE* plotPipe = NULL;
static string baseFilename;

#define HAVE_LEFT_CIRCLE    (leftCircleCenter .x > 0 && leftCircleCenter .y > 0)
#define HAVE_RIGHT_CIRCLE   (rightCircleCenter.x > 0 && rightCircleCenter.y > 0)
#define HAVE_CIRCLES        (HAVE_LEFT_CIRCLE && HAVE_RIGHT_CIRCLE)
#define HAVE_POINTED_CIRCLE (pointedCircleCenter.x > 0 && pointedCircleCenter.y > 0)

static void setStoppedAnalysis(void);

static void forceStopAnalysis(void)
{
    goResetButton->value(0);
    setStoppedAnalysis();
}

static bool gotNewFrame(IplImage* buffer, uint64_t timestamp_us)
{
    if(buffer == NULL)
    {
        if(!AM_READING_CAMERA)
        {
            // error ocurred reading the stored video. I likely reached the end of the file. I stop
            // the analysis if I'm running it and rewind the stream
            source->restartStream();
            if(analysisState == RUNNING)
                forceStopAnalysis();

            return true;
        }
        return false;
    }

    cvMerge(buffer, buffer, buffer, NULL, *widgetImage);

    visionParameters_t params;
    Fl::lock();
    {
        params.presmoothing_w            = param_presmoothing_w           ->value();
        params.detrend_w                 = param_detrend_w                ->value();
        params.detrend_scale             = param_detrend_scale            ->value();
        params.adaptive_threshold_kernel = param_adaptive_threshold_kernel->value();
        params.adaptive_threshold        = param_adaptive_threshold       ->value();
        params.morphologic_depth         = param_morphologic_depth        ->value();
    }
    Fl::unlock();
    // these must be odd
    params.presmoothing_w            |= 1;
    params.detrend_w                 |= 1;
    params.adaptive_threshold_kernel |= 1;

    const CvMat* result = isolateWorms(buffer, &params);
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
        static uint64_t nextDataTimestamp_us = 0ull;
        // when using the camera, I get frames much faster than I use them to keep the program
        // looking visually responsive. Here I limit my data collection rate
        if( analysisState == RUNNING && (!AM_READING_CAMERA || timestamp_us > nextDataTimestamp_us) )
        {
            if(nextDataTimestamp_us == 0ull)
                nextDataTimestamp_us = timestamp_us;
            nextDataTimestamp_us += 1e6/DATA_FRAME_RATE_FPS;

            if(videoEncoder) videoEncoder.writeFrameGrayscale(buffer);

            double minutes = (double)numPoints / DATA_FRAME_RATE_FPS / 60.0;
            double leftOccupancy, rightOccupancy;
            computeWormOccupancy(result, &leftCircleCenter, &rightCircleCenter,
                                 CIRCLE_RADIUS,
                                 &leftOccupancy, &rightOccupancy);

            Yaxis->rescale(CA_WHEN_MAX, fmax(leftOccupancy, rightOccupancy) );

            lastLeftPoint  = new Ca_LinePoint(lastLeftPoint,
                                              minutes,
                                              leftOccupancy,  1,FL_RED,   CA_NO_POINT);
            lastRightPoint = new Ca_LinePoint(lastRightPoint,
                                              minutes,
                                              rightOccupancy, 1,FL_GREEN, CA_NO_POINT);
            if(plotPipe)
                fprintf(plotPipe, "%f %f %f\n", minutes, leftOccupancy, rightOccupancy);

            Xaxis->maximum(minutes);
            numPoints++;

            leftAccumValue  += leftOccupancy  / DATA_FRAME_RATE_FPS;
            rightAccumValue += rightOccupancy / DATA_FRAME_RATE_FPS;
            char results[128];
            snprintf(results, sizeof(results), "%.3f", leftAccumValue);
            leftAccum->value(results);
            snprintf(results, sizeof(results), "%.3f", rightAccumValue);
            rightAccum->value(results);

            if(minutes > duration->value())
                forceStopAnalysis();
        }

        widgetImage->redrawNewFrame();
    }

    if(!AM_READING_CAMERA && analysisState != RUNNING)
    {
        Fl::unlock();

        // reading from a video file and not actually running the analysis yet. In this case I
        // rewind back to the beginning and delay, to force a reasonable refresh rate
        source->restartStream();

        struct timespec tv;
        tv.tv_sec  = 0;
        tv.tv_nsec = 1e9 / PREVIEW_FRAME_RATE_FPS;
        nanosleep(&tv, NULL);
    }
    else
        Fl::unlock();

    return true;
}

static void goResetButton_handleActivation(void)
{
    if( !experimentName->value() || experimentName->value()[0] == '\0' || !HAVE_CIRCLES)
        goResetButton->deactivate();
    else
        goResetButton->activate();
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

    goResetButton_handleActivation();
}

static void createBaseOutputFilename(void)
{
    char timestamp[128];
    time_t tnow = time(NULL);
    struct tm* tm = localtime(&tnow);
    strftime(timestamp, sizeof(timestamp), "%F_%H-%M-%S", tm);

    // using quotes because the name can have spaces
    baseFilename = timestamp;
    baseFilename += "_";
    baseFilename += experimentName->value();
}

static void openPlotPipe(void)
{
    // I want a PDF, but this PDF needs to contain analysis results that I do
    // not yet have. I thus create a postscript file, then modify the data
    // in-place, and convert to PDF. I'd do this with the PDF directly, but it
    // doesn't store its data in plain ASCII

    string command("feedGnuplot.pl --lines --domain "
                   "--xlabel Minutes --ylabel \"Occupancy ratio\" "
                   "--le \"Left circle occupancy total 888.88888 ratio-seconds\" "
                   "--le \"Right circle occupancy total 888.88888 ratio-seconds\" "
                   "--title \"Worm occupancy for ");
    command += experimentName->value();
    command += "\" --hardcopy \"";
    command += baseFilename;
    command += ".ps\"";

    plotPipe = popen(command.c_str(), "w");
    if(plotPipe == NULL)
        fl_alert("Couldn't start the plotting pipe. No plot will be generated");
}

static void finalizePlot(void)
{
    string leftLegend("Left circle occupancy (total ");
    leftLegend += leftAccum->value();
    leftLegend += " ratio-seconds)";

    string rightLegend("Right circle occupancy (total ");
    rightLegend += rightAccum->value();
    rightLegend += " ratio-seconds)";

    string command;

    command = "perl -p -i -e 's/Left circle occupancy total 888.88888 ratio-seconds/";
    command += leftLegend + "/' \"" + baseFilename + ".ps\"";
    system(command.c_str());

    command = "perl -p -i -e 's/Right circle occupancy total 888.88888 ratio-seconds/";
    command += rightLegend + "/' \"" + baseFilename + ".ps\"";
    system(command.c_str());

    command = "ps2pdf \"";
    command += baseFilename + ".ps\" \"" + baseFilename + ".pdf\"";
    system(command.c_str());

    command = "rm \"";
    command += baseFilename + ".ps\"";
    system(command.c_str());
}

static void setResetAnalysis(void)
{
    goResetButton->labelfont(FL_HELVETICA_BOLD);
    goResetButton->labelcolor(FL_RED);
    goResetButton->type(FL_TOGGLE_BUTTON);
    goResetButton->label("Analyze");
    experimentName->activate();
    duration      ->activate();
    chdirButton   ->activate();

    numPoints       = 0;
    if(plot) plot->clear();
    lastLeftPoint   = lastRightPoint = NULL; 
    leftAccumValue  = 0.0;
    rightAccumValue = 0.0;
    leftAccum ->value("0.0");
    rightAccum->value("0.0");

    if(!AM_READING_CAMERA)
        source->restartStream();

    analysisState = RESET;
}

static void setRunningAnalysis(void)
{
    createBaseOutputFilename();

    if(AM_READING_CAMERA)
    {
        string videoFilename = baseFilename + ".avi";
        videoEncoder.close();
        videoEncoder.open(videoFilename.c_str(), source->w(), source->h(), VIDEO_ENCODING_FPS, FRAMESOURCE_GRAYSCALE);

        if(!videoEncoder)
            fl_alert("Couldn't start video recording. Video will NOT be written");
    }

    openPlotPipe();

    goResetButton->labelfont(FL_HELVETICA);
    goResetButton->labelcolor(FL_BLACK);
    goResetButton->type(FL_TOGGLE_BUTTON);
    goResetButton->label("Stop analysis");
    experimentName->deactivate();
    duration      ->deactivate();
    chdirButton   ->deactivate();

    pointedCircleCenter.x = pointedCircleCenter.y = -1;

    analysisState = RUNNING;
}

static void setStoppedAnalysis(void)
{
    videoEncoder.close();
    if(plotPipe)
    {
        pclose(plotPipe);
        plotPipe = NULL;

        finalizePlot();
    }

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

static void updateChdirButtonLabel(void)
{
    char* path = getcwd(NULL, 0);
    if(path == NULL)
    {
        fl_alert("Couldn't getcwd for some reason...");
        return;
    }

    string label("Change directory. Current:\n");
    label += path;
    chdirButton->copy_label(label.c_str());

    free(path);
}

static void pressedChdir(Fl_Widget* widget __attribute__((unused)), void* cookie __attribute__((unused)))
{
    char* dir = fl_dir_chooser("Choose new output directory", "", 0);
    if(dir != NULL)
    {
        if(chdir(dir) == 0)
            updateChdirButtonLabel();
        else
            fl_alert("Couldn't switch to selected directory %s", dir);
    }
}

static void changedExperimentName(Fl_Widget* widget __attribute__((unused)), void* cookie __attribute__((unused)))
{
    if( !experimentName->value() || experimentName->value()[0] == '\0' )
        experimentName->color(FL_RED);
    else
        experimentName->color(FL_WHITE);

    goResetButton_handleActivation();
    experimentName->redraw();
}

static void setupVisionParameters(void)
{
    param_presmoothing_w            = new Fl_Value_Slider(rightAccum->x(), rightAccum->y() + rightAccum->h(),
                                                          PARAM_SLIDER_W, PARAM_SLIDER_H,
                                                          "Preesmoothing width");
    param_detrend_w                 = new Fl_Value_Slider(rightAccum->x(), param_presmoothing_w->y() + param_presmoothing_w->h(),
                                                          PARAM_SLIDER_W, PARAM_SLIDER_H,
                                                          "Detrend width");
    param_detrend_scale             = new Fl_Value_Slider(rightAccum->x(), param_detrend_w->y() + param_detrend_w->h(),
                                                          PARAM_SLIDER_W, PARAM_SLIDER_H,
                                                          "Detrend scaling");
    param_adaptive_threshold_kernel = new Fl_Value_Slider(rightAccum->x(), param_detrend_scale->y() + param_detrend_scale->h(),
                                                          PARAM_SLIDER_W, PARAM_SLIDER_H,
                                                          "Adaptive threshold kernel");
    param_adaptive_threshold        = new Fl_Value_Slider(rightAccum->x(), param_adaptive_threshold_kernel->y() + param_adaptive_threshold_kernel->h(),
                                                          PARAM_SLIDER_W, PARAM_SLIDER_H,
                                                          "Adaptive threshold");
    param_morphologic_depth         = new Fl_Value_Slider(rightAccum->x(), param_adaptive_threshold->y() + param_adaptive_threshold->h(),
                                                          PARAM_SLIDER_W, PARAM_SLIDER_H,
                                                          "Morphologic depth");
    param_presmoothing_w           ->align(FL_ALIGN_RIGHT);
    param_detrend_w                ->align(FL_ALIGN_RIGHT);
    param_detrend_scale            ->align(FL_ALIGN_RIGHT);
    param_adaptive_threshold_kernel->align(FL_ALIGN_RIGHT);
    param_adaptive_threshold       ->align(FL_ALIGN_RIGHT);
    param_morphologic_depth        ->align(FL_ALIGN_RIGHT);

    param_presmoothing_w           ->type(FL_HOR_SLIDER);
    param_detrend_w                ->type(FL_HOR_SLIDER);
    param_detrend_scale            ->type(FL_HOR_SLIDER);
    param_adaptive_threshold_kernel->type(FL_HOR_SLIDER);
    param_adaptive_threshold       ->type(FL_HOR_SLIDER);
    param_morphologic_depth        ->type(FL_HOR_SLIDER);

    visionParameters_t params;
    getDefaultParameters(&params);
    param_presmoothing_w           ->bounds(params.presmoothing_w            / 3, params.presmoothing_w            * 3);
    param_detrend_w                ->bounds(params.detrend_w                 / 3, params.detrend_w                 * 3);
    param_detrend_scale            ->bounds(params.detrend_scale             / 3, params.detrend_scale             * 3);
    param_adaptive_threshold_kernel->bounds(params.adaptive_threshold_kernel / 3, params.adaptive_threshold_kernel * 3);
    param_adaptive_threshold       ->bounds(params.adaptive_threshold        / 3, params.adaptive_threshold        * 3);
    param_morphologic_depth        ->bounds(params.morphologic_depth         / 3, params.morphologic_depth         * 3);

    param_presmoothing_w           ->value(params.presmoothing_w);
    param_detrend_w                ->value(params.detrend_w);
    param_detrend_scale            ->value(params.detrend_scale);
    param_adaptive_threshold_kernel->value(params.adaptive_threshold_kernel);
    param_adaptive_threshold       ->value(params.adaptive_threshold);
    param_morphologic_depth        ->value(params.morphologic_depth);

    param_presmoothing_w           ->precision(0); // integers
    param_detrend_w                ->precision(0); // integers
    param_detrend_scale            ->precision(1); // accurate to 0.1
    param_adaptive_threshold_kernel->precision(0); // integers
    param_adaptive_threshold       ->precision(0); // integers
    param_morphologic_depth        ->precision(0); // integers
}

int main(int argc, char* argv[])
{
    Fl::lock();
    Fl::visual(FL_RGB);

    // To load a video file, the last cmdline argument must be the file.
    // To read a camera, the last cmdline argument must be 0x..., we use it as the camera GUID
    // Otherwise we try to load any camera
    if(argc < 2)
        source = new CameraSource (FRAMESOURCE_GRAYSCALE, false, 0, CROP_RECT);
    else if(strncmp(argv[argc-1], "0x", 2) == 0)
    {
        assert(sizeof(long long unsigned int) == sizeof(uint64_t));

        uint64_t guid;
        sscanf(&argv[argc-1][2], "%llx", (long long unsigned int*)&guid);
        source = new CameraSource(FRAMESOURCE_GRAYSCALE, false, guid, CROP_RECT);
    }
    else
        source = new FFmpegDecoder(argv[argc-1], FRAMESOURCE_GRAYSCALE, false, CROP_RECT);

    if(! *source)
    {
        fprintf(stderr, "couldn't open frame source\n");
        fl_alert("couldn't open frame source");
        delete source;
        return 0;
    }

    Fl_Double_Window* window =
        new Fl_Double_Window(WINDOW_W, WINDOW_H, "Wormtracker 3");
    widgetImage = new CvFltkWidget(0, 0, source->w(), source->h(),
                                   WIDGET_COLOR);

    widgetImage->callback(widgetImageCallback);

    chdirButton = new Fl_Button( widgetImage->x() + widgetImage->w(), widgetImage->y(),
                                 BUTTON_W, BUTTON_H);
    chdirButton->callback(pressedChdir);
    updateChdirButtonLabel();

    duration = new Fl_Value_Slider( chdirButton->x() + chdirButton->w(), chdirButton->y(),
                                    BUTTON_W, BUTTON_H, "Duration (min)" );
    duration->bounds(1, 300);
    duration->precision(0); // integers
    duration->value(20);
    duration->type(FL_HOR_SLIDER);

    experimentName = new Fl_Input( widgetImage->x() + widgetImage->w(), chdirButton->y() + chdirButton->h(),
                                   BUTTON_W, BUTTON_H, "Experiment name");
    experimentName->align(FL_ALIGN_RIGHT);
    experimentName->callback(changedExperimentName);
    experimentName->when(FL_WHEN_CHANGED);

    goResetButton = new Fl_Button( widgetImage->x() + widgetImage->w(), experimentName->y() + experimentName->h(),
                                   2 * BUTTON_W, BUTTON_H);
    goResetButton->callback(pressedGoReset);

    plot = new Ca_Canvas( Y_AXIS_WIDTH + AXIS_EXTRA_SPACE, widgetImage->y() + widgetImage->h(),
                          PLOT_W, PLOT_H,
                          "Worm occupancy");
    plot->align(FL_ALIGN_TOP);

    // This is extremely important for some reason. Without it the plots do not refresh property and
    // there're artifacts every time the plot is resized
    plot->box(FL_DOWN_BOX);

    Xaxis = new Ca_X_Axis(plot->x(), plot->y() + plot->h(), plot->w(), X_AXIS_HEIGHT, "Minutes");
    Xaxis->align(FL_ALIGN_BOTTOM);
    Xaxis->minimum(0);
    Xaxis->maximum(1);
    Xaxis->label_format("%g");
    Xaxis->major_step(10);
    Xaxis->label_step(10);
    Xaxis->axis_color(FL_BLACK);
    Xaxis->axis_align(CA_BOTTOM | CA_LINE);

    Yaxis = new Ca_Y_Axis(AXIS_EXTRA_SPACE, plot->y(), Y_AXIS_WIDTH, plot->h());
    Fl_Rotated_Text YaxisLabel("occupancy ratio", FL_HELVETICA, 14, 0, 1);
    Yaxis->image(&YaxisLabel);
    Yaxis->minimum(0);
    Yaxis->maximum(0.01);
    Yaxis->align(FL_ALIGN_LEFT);
    Yaxis->axis_align(CA_LEFT | CA_LINE);
    Yaxis->axis_color(FL_BLACK);

    leftAccum  = new Fl_Output(widgetImage->x() + widgetImage->w(), goResetButton->y() + goResetButton->h(),
                              ACCUM_W, ACCUM_H, "Left accumulator (ratio-seconds)");
    rightAccum = new Fl_Output(leftAccum->x(), leftAccum->y() + leftAccum->h(),
                              ACCUM_W, ACCUM_H, "Right accumulator (ratio-seconds)");
    leftAccum ->align(FL_ALIGN_RIGHT);
    rightAccum->align(FL_ALIGN_RIGHT);
    leftAccum ->labelcolor(FL_RED);
    rightAccum->labelcolor(FL_GREEN);

    setupVisionParameters();

    window->end();
    window->show();

    processingInit(source->w(), source->h());

    changedExperimentName(NULL, NULL);
    setResetAnalysis();
    goResetButton_handleActivation();

    // I read the data with a tiny delay. This makes sure that I skip old frames (only an issue if I
    // can't keep up with the data rate), but yet got as fast as I can
    IplImage* buffer = cvCreateImage(cvSize(source->w(), source->h()), IPL_DEPTH_8U, 1);

    // If reading from a stored video file, go as fast as possible
    if(AM_READING_CAMERA) source->startSourceThread(&gotNewFrame, 1e6/PREVIEW_FRAME_RATE_FPS, buffer);
    else                  source->startSourceThread(&gotNewFrame, 0,                          buffer);

    Fl::run();
    Fl::unlock();

    delete source;
    delete window;
    cvReleaseImage(&buffer);

    processingCleanup();
    return 0;
}
