#
# FILE            makefile
#
# AUTHOR          Ken Zangelin
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# corDdsBridge is a PLUGIN, and it is built on its own - never from the corLibs
# umbrella. The eProsima stack it links is 21.4 MiB across nine libraries,
# roughly five times the whole broker, and putting it in COR_DIRS would make all
# of that a hard dependency of every coraine clone. That is the precise thing
# the plugin decision exists to prevent.
#
# It is also the only C++ in the stack, for a reason that is not stylistic: the
# DDS Enabler's API takes std::string and std::shared_ptr, which no amount of
# declaring mangled symbols reaches from C.
#
PLUGIN        = dds.so
CXX           = g++
COR_LIBS     ?= $(HOME)/git
PLUGIN_DIR   ?= /opt/seamware/plugins/bridge

INCLUDE       = -I$(COR_LIBS) -I/usr/local/include
CXXFLAGS      = -std=c++17 -O2 -Wall -Werror -fPIC $(INCLUDE) -MMD -MP

#
# The broker is linked rdynamic, so ktrace and the rest resolve from the running
# process at dlopen. Only the transport's own libraries are linked here.
#
LDFLAGS       = -L/usr/local/lib
LIBS          = -lddsenabler -lddsenabler_participants -lddsenabler_yaml \
                -lddspipe_core -lddspipe_participants -lddspipe_yaml \
                -lcpp_utils -lfastdds -lfastcdr -lpthread

SOURCES       = ddsBridge.cpp ddsRegister.cpp
OBJS          = $(SOURCES:.cpp=.o)
DEPS          = $(SOURCES:.cpp=.d)

all: $(PLUGIN)

$(PLUGIN): $(OBJS)
	$(CXX) -shared $(OBJS) -o $(PLUGIN) $(LDFLAGS) $(LIBS) -Wl,-rpath,/usr/local/lib

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

install: all
	mkdir -p $(PLUGIN_DIR)
	cp -p $(PLUGIN) $(PLUGIN_DIR)/

di: install
ci: clean install

clean:
	rm -f *.o *.d *.so *~

-include $(DEPS)
