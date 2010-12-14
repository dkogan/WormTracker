# debian places the headers and libraries in a different (much more sensible) location than the
# upstream opencv builds. This variable selects this
OPENCV_DEBIAN_PACKAGES = 1

FLAGS += -g -Wall -Wextra -MMD
FLAGS += -I../fltkVisionUtils/

CXXFLAGS = $(FLAGS)
CFLAGS = $(FLAGS) --std=gnu99

LDFLAGS  += -g
LDLIBS   += -lX11 -lXft -lXinerama

ifeq ($(OPENCV_DEBIAN_PACKAGES), 0)
  OPENCV_LIBS = -lopencv_core -lopencv_imgproc -lopencv_highgui
else
  CXXFLAGS += -DOPENCV_DEBIAN_PACKAGES
  CFLAGS += -DOPENCV_DEBIAN_PACKAGES
  OPENCV_LIBS = -lcv -lhighgui
endif

FFMPEG_LIBS = -lavformat -lavcodec -lswscale -lavutil
LDLIBS += -lfltk $(OPENCV_LIBS) -lpthread -ldc1394 $(FFMPEG_LIBS) ../fltkVisionUtils/fltkVisionUtils.a

all: worm3

SOURCE_WILDCARD = *.cc *.c *.cpp
SOURCES = $(wildcard $(SOURCE_WILDCARD) $(patsubst %,cartesian/%, $(SOURCE_WILDCARD)) $(patsubst %,Fl_Rotated_Text/%, $(SOURCE_WILDCARD)))

SOURCE_OBJECTS = $(addsuffix .o, $(basename $(SOURCES)))

worm3: $(SOURCE_OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@


clean:
	rm -f $(SOURCE_OBJECTS) *.d worm3

-include *.d
