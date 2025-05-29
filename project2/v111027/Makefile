CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra

ifeq ($(DEBUG),1)
	CXXFLAGS += -DDEBUG
endif

TARGETS = np_simple np_single_proc

all: $(TARGETS)

npshell: npshell.cpp
	$(CXX) $(CXXFLAGS) -o np_simple np_simple.cpp

np_single_proc: np_single_proc.cpp
	$(CXX) $(CXXFLAGS) -o np_single_proc np_single_proc.cpp

clean:
	rm -f $(TARGETS)