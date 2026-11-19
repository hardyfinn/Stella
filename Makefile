# Path to the Rack SDK (override on the command line: make RACK_DIR=...)
RACK_DIR ?= ../Rack-SDK

FLAGS +=
SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

ifneq ($(MAKECMDGOALS),test)
include $(RACK_DIR)/plugin.mk
endif

# Analysis and picker check, no Rack SDK needed:  make test DIR=/path/to/samples
test:
	$(CXX) -O2 -Wall -Wextra -std=c++17 test/map_test.cpp -o test/map_test && test/map_test $(DIR) > /dev/null
.PHONY: test
