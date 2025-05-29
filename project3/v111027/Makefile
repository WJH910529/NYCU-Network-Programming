CXX = g++
CXXFLAGS = -std=c++11 -Wall -Wextra

ifeq ($(DEBUG),1)
	CXXFLAGS += -DDEBUG
endif

TARGETS = np_multi_proc

all: $(TARGETS)

np_multi_proc: np_multi_proc.cpp
	$(CXX) $(CXXFLAGS) -o np_multi_proc np_multi_proc.cpp

clean:
	rm -f $(TARGETS)