# selects opencv 2.1 or 2.0
OPENCV_VERSION = 210

FLAGS += -g -O3 -Wall -Wextra -pedantic -MMD
FLAGS += -DOPENCV_VERSION=$(OPENCV_VERSION)
FLAGS += -I../fltkVisionUtils/

CXXFLAGS = $(FLAGS)
CFLAGS = $(FLAGS) --std=gnu99

LDFLAGS  += -g
LDLIBS   += -lX11 -lXft -lXinerama

ifeq ($(OPENCV_VERSION), 210)
  OPENCV_LIBS = -lopencv_core -lopencv_imgproc -lopencv_highgui
else
  OPENCV_LIBS = -lcv
endif

FFMPEG_LIBS = -lavformat -lavcodec -lswscale -lavutil
LDLIBS += -lfltk $(OPENCV_LIBS) -lpthread -ldc1394 $(FFMPEG_LIBS) ../fltkVisionUtils/fltkVisionUtils.a

all: worm3

SOURCE_WILDCARD = *.cc *.c *.cpp
SOURCES = $(wildcard $(SOURCE_WILDCARD) $(patsubst %,cartesian/%, $(SOURCE_WILDCARD)))

SOURCE_OBJECTS = $(addsuffix .o, $(basename $(SOURCES)))

worm3: $(SOURCE_OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@


clean:
	rm -f $(SOURCE_OBJECTS) *.d worm3

-include *.d
