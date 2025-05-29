CXX=g++
CXXFLAGS=-std=c++14 -Wall -pedantic -pthread -lboost_system
CXX_INCLUDE_DIRS=/usr/local/include
CXX_INCLUDE_PARAMS=$(addprefix -I , $(CXX_INCLUDE_DIRS))
CXX_LIB_DIRS=/usr/local/lib
CXX_LIB_PARAMS=$(addprefix -L , $(CXX_LIB_DIRS))

.PHONY: all part1 part2 clean

all: part1 part2

part1: http_server console.cgi

http_server: http_server.cpp
	$(CXX) http_server.cpp -o http_server $(CXX_INCLUDE_PARAMS) $(CXX_LIB_PARAMS) $(CXXFLAGS)

console.cgi: console.cpp
	$(CXX) console.cpp -o console.cgi $(CXX_INCLUDE_PARAMS) $(CXX_LIB_PARAMS) $(CXXFLAGS)

part2: cgi_server.exe

cgi_server.exe: cgi_server.cpp
	$(CXX) $< -o $@ -lws2_32 -lwsock32 -lboost_system -std=c++14

clean:
	rm -f http_server console.cgi cgi_server.exe