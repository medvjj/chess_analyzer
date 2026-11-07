CXX := g++
TARGET := chess_analyzer
SOURCE := main.cpp

CXXFLAGS := -std=c++17 -O2 -pthread $(shell fltk-config --cxxflags)
LDFLAGS := $(shell fltk-config --ldstaticflags)
LDLIBS := -lexpat -lxcb -lXrender -lfreetype \
          -lpng -lbrotlidec -lbrotlicommon \
          -lXau -lXdmcp -lz

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SOURCE)
	$(CXX) $(CXXFLAGS) $(SOURCE) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -f $(TARGET)
